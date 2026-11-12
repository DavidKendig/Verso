#include "app/PdfView.h"

#ifdef _WIN32
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#else
// Linux: poppler's pdfinfo and pdftoppm run as separate programs (poppler-utils), so
// Verso links no PDF engine; pages arrive as PPM images on the program's stdout.
#include <cairo.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "core/TextUtil.h"
#endif

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cwchar>
#include <mutex>
#include <thread>

namespace verso {

#ifdef _WIN32
namespace wdp = winrt::Windows::Data::Pdf;
namespace wgi = winrt::Windows::Graphics::Imaging;
namespace wss = winrt::Windows::Storage::Streams;
#endif

namespace {

constexpr float kPadX = 24, kPadY = 20, kGap = 16;
constexpr float kFitMaxWidth = 1100;  // fit-to-width stops growing here (DIPs)
constexpr int kMaxRenderWidth = 4096;  // pixels; wider views are upscaled

#ifndef _WIN32
// Runs a program and collects its stdout (stderr into `err`); returns the exit code,
// or -1 when it could not run.
int capture(const std::vector<std::string>& args, std::string& out, std::string* err) {
  int o[2], e[2];
  if (pipe2(o, O_CLOEXEC) < 0) return -1;
  if (pipe2(e, O_CLOEXEC) < 0) {
    close(o[0]);
    close(o[1]);
    return -1;
  }
  std::vector<char*> argv;
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = fork();
  if (pid == 0) {
    int null = open("/dev/null", O_RDONLY);
    if (null >= 0) dup2(null, 0);
    dup2(o[1], 1);
    dup2(e[1], 2);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(o[1]);
  close(e[1]);
  if (pid < 0) {
    close(o[0]);
    close(e[0]);
    return -1;
  }
  std::string errText;
  std::thread errReader([&] {
    char buf[4096];
    ssize_t n;
    while ((n = read(e[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR))
      if (n > 0 && errText.size() < 65536) errText.append(buf, static_cast<size_t>(n));
  });
  char buf[65536];
  ssize_t n;
  while ((n = read(o[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR))
    if (n > 0) out.append(buf, static_cast<size_t>(n));
  errReader.join();
  close(o[0]);
  close(e[0]);
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
  }
  if (err) *err = std::move(errText);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// A binary PPM (P6, maxval 255) as premultiplied BGRA rows; false when malformed.
bool decodePpm(const std::string& d, int& w, int& h, std::vector<uint8_t>& px) {
  size_t i = 0;
  auto token = [&]() -> long {
    for (;;) {
      while (i < d.size() && std::isspace(static_cast<unsigned char>(d[i]))) ++i;
      if (i < d.size() && d[i] == '#') {
        while (i < d.size() && d[i] != '\n') ++i;
        continue;
      }
      break;
    }
    long v = 0;
    bool any = false;
    while (i < d.size() && std::isdigit(static_cast<unsigned char>(d[i]))) {
      v = v * 10 + (d[i++] - '0');
      any = true;
      if (v > 1000000) return -1;
    }
    return any ? v : -1;
  };
  if (d.size() < 2 || d[0] != 'P' || d[1] != '6') return false;
  i = 2;
  long pw = token(), ph = token(), maxv = token();
  if (pw <= 0 || ph <= 0 || maxv != 255 || i >= d.size()) return false;
  ++i;  // the single whitespace after maxval
  const size_t need = static_cast<size_t>(pw) * static_cast<size_t>(ph) * 3;
  if (d.size() - i < need) return false;
  w = static_cast<int>(pw);
  h = static_cast<int>(ph);
  px.resize(static_cast<size_t>(w) * h * 4);
  const unsigned char* s = reinterpret_cast<const unsigned char*>(d.data() + i);
  for (size_t p = 0, n = static_cast<size_t>(w) * h; p < n; ++p) {
    px[p * 4 + 0] = s[p * 3 + 2];
    px[p * 4 + 1] = s[p * 3 + 1];
    px[p * 4 + 2] = s[p * 3 + 0];
    px[p * 4 + 3] = 255;
  }
  return true;
}

// The first line of a poppler error, made readable ("Command Line Error: ..." etc.).
std::wstring popplerError(const std::string& err) {
  std::istringstream in(err);
  std::string line, last;
  while (std::getline(in, line)) {
    if (line.find("Error") != std::string::npos || line.find("error") != std::string::npos) return utf8ToWide(line);
    if (!line.empty()) last = line;
  }
  return utf8ToWide(last);
}
#endif

}  // namespace

#ifndef _WIN32
void SurfaceRef::Reset() {
  if (s_) cairo_surface_destroy(s_);
  s_ = nullptr;
}
#endif

bool isPdfData(std::string_view bytes) { return bytes.rfind("%PDF-", 0) == 0; }

// State shared with the worker thread. The thread holds its own reference, so a
// view can be closed while a page is still rendering.
struct PdfView::Worker {
  std::wstring path;
  HWND notify = nullptr;
  UINT message = 0;

  std::mutex m;
  std::condition_variable cv;
  bool stop = false;
  std::vector<std::pair<int, int>> queue;  // (page, pixel width), most wanted first
  std::pair<int, int> inFlight{-1, 0};

  // Results, guarded by m.
  bool loaded = false, failed = false;
  std::wstring error;
  std::vector<std::pair<float, float>> sizes;
  struct Rendered {
    int page = 0, w = 0, h = 0;
    std::vector<uint8_t> pixels;
    bool failed = false;
  };
  std::vector<Rendered> done;

  void post() { PostMessageW(notify, message, 0, 0); }

  bool stopping() {
    std::lock_guard<std::mutex> l(m);
    return stop;
  }

#ifndef _WIN32
  static void run(std::shared_ptr<Worker> self) { self->work(); }

  void work() {
    const std::string file = wideToUtf8(path);
    std::vector<std::pair<float, float>> sz;
    {
      std::string out, err;
      int rc = capture({"pdfinfo", "-f", "1", "-l", "2000000000", "--", file}, out, &err);
      if (rc == 0) {
        // "Page    N size:  612 x 792 pts" then "Page    N rot:   90" for every page.
        std::istringstream in(out);
        std::string line;
        while (std::getline(in, line)) {
          int page = 0, rot = 0;
          double w = 0, h = 0;
          if (sscanf(line.c_str(), "Page %d size: %lf x %lf", &page, &w, &h) == 3 && page == (int)sz.size() + 1) {
            sz.emplace_back(std::max(1.0f, static_cast<float>(w)), std::max(1.0f, static_cast<float>(h)));
          } else if (sscanf(line.c_str(), "Page %d rot: %d", &page, &rot) == 2 && page == (int)sz.size() &&
                     ((rot % 180 + 180) % 180) == 90) {
            std::swap(sz.back().first, sz.back().second);
          }
        }
      }
      std::lock_guard<std::mutex> l(m);
      if (rc == 0) {
        sizes = sz;
        loaded = true;
      } else {
        failed = true;
        if (rc == 127 || rc < 0)
          error = L"Showing PDFs needs poppler-utils (pdfinfo, pdftoppm), which is not installed.";
        else if (err.find("assword") != std::string::npos)
          error = L"The file is password-protected.";
        else {
          std::wstring why = popplerError(err);
          error = L"The file may be damaged" + (why.empty() ? std::wstring(L".") : L" (" + why + L").");
        }
      }
    }
    post();
    if (sz.empty()) return;

    for (;;) {
      std::pair<int, int> job;
      {
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] { return stop || !queue.empty(); });
        if (stop) return;
        job = queue.front();
        queue.erase(queue.begin());
        inFlight = job;
      }
      Rendered r;
      r.page = job.first;
      if (job.first >= 0 && job.first < static_cast<int>(sz.size())) {
        // DPI for the wanted pixel width; independent of the page's rotation.
        const double dpi = job.second * 72.0 / sz[static_cast<size_t>(job.first)].first;
        char dpiText[32];
        snprintf(dpiText, sizeof dpiText, "%.3f", std::max(1.0, dpi));
        const std::string page = std::to_string(job.first + 1);
        std::string out;
        int rc = capture({"pdftoppm", "-f", page, "-l", page, "-r", dpiText, "-aa", "yes", "-aaVector", "yes", "--",
                          file},
                         out, nullptr);
        r.failed = rc != 0 || !decodePpm(out, r.w, r.h, r.pixels);
      } else {
        r.failed = true;
      }
      {
        std::lock_guard<std::mutex> l(m);
        inFlight = {-1, 0};
        done.push_back(std::move(r));
      }
      post();
    }
  }
#else
  static void run(std::shared_ptr<Worker> self) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    self->work();
    winrt::uninit_apartment();
  }

  void work() {
    wdp::PdfDocument doc{nullptr};
    try {
      auto file = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(path).get();
      doc = wdp::PdfDocument::LoadFromFileAsync(file).get();
      std::vector<std::pair<float, float>> sz;
      uint32_t n = doc.PageCount();
      sz.reserve(n);
      for (uint32_t i = 0; i < n; ++i) {
        if ((i & 63) == 0 && stopping()) return;
        auto s = doc.GetPage(i).Size();
        sz.emplace_back(std::max(1.0f, s.Width), std::max(1.0f, s.Height));
      }
      std::lock_guard<std::mutex> l(m);
      sizes = std::move(sz);
      loaded = true;
    } catch (const winrt::hresult_error& e) {
      std::lock_guard<std::mutex> l(m);
      failed = true;
      // The Windows PDF engine's HRESULTs usually have no message text, so show the code.
      wchar_t code[16];
      swprintf_s(code, L"%08X", static_cast<uint32_t>(e.code().value));
      error = L"The file may be damaged or password-protected (Windows error 0x" + std::wstring(code) + L").";
    } catch (...) {
      std::lock_guard<std::mutex> l(m);
      failed = true;
      error = L"The file could not be read as a PDF.";
    }
    post();
    if (!doc) return;

    for (;;) {
      std::pair<int, int> job;
      {
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] { return stop || !queue.empty(); });
        if (stop) return;
        job = queue.front();
        queue.erase(queue.begin());
        inFlight = job;
      }
      Rendered r;
      r.page = job.first;
      try {
        auto page = doc.GetPage(static_cast<uint32_t>(job.first));
        wss::InMemoryRandomAccessStream stream;
        wdp::PdfPageRenderOptions opts;
        opts.DestinationWidth(static_cast<uint32_t>(job.second));
        opts.BitmapEncoderId(wgi::BitmapEncoder::BmpEncoderId());
        page.RenderToStreamAsync(stream, opts).get();
        stream.Seek(0);
        auto decoder = wgi::BitmapDecoder::CreateAsync(stream).get();
        auto bmp = decoder.GetSoftwareBitmapAsync(wgi::BitmapPixelFormat::Bgra8, wgi::BitmapAlphaMode::Premultiplied).get();
        r.w = bmp.PixelWidth();
        r.h = bmp.PixelHeight();
        wss::Buffer buf(static_cast<uint32_t>(r.w) * static_cast<uint32_t>(r.h) * 4);
        bmp.CopyToBuffer(buf);
        r.pixels.assign(buf.data(), buf.data() + buf.Length());
      } catch (...) {
        r.failed = true;
      }
      {
        std::lock_guard<std::mutex> l(m);
        inFlight = {-1, 0};
        done.push_back(std::move(r));
      }
      post();
    }
  }
#endif
};

PdfView::PdfView(const std::wstring& path, HWND notify, UINT message) : worker_(std::make_shared<Worker>()) {
  worker_->path = path;
  worker_->notify = notify;
  worker_->message = message;
  std::thread(&Worker::run, worker_).detach();
}

PdfView::~PdfView() {
  {
    std::lock_guard<std::mutex> l(worker_->m);
    worker_->stop = true;
  }
  worker_->cv.notify_all();
}

bool PdfView::takeResults() {
  bool changed = false;
  std::lock_guard<std::mutex> l(worker_->m);
  if (state_ == State::Loading) {
    if (worker_->loaded) {
      pages_.resize(worker_->sizes.size());
      maxPageW_ = 1;
      for (size_t i = 0; i < pages_.size(); ++i) {
        pages_[i].w = worker_->sizes[i].first;
        pages_[i].h = worker_->sizes[i].second;
        maxPageW_ = std::max(maxPageW_, pages_[i].w);
      }
      state_ = State::Ready;
      layoutDirty_ = changed = true;
    } else if (worker_->failed) {
      error_ = worker_->error;
      state_ = State::Failed;
      changed = true;
    }
  }
  for (auto& r : worker_->done) {
    if (r.page < 0 || r.page >= pageCount()) continue;
    Page& p = pages_[static_cast<size_t>(r.page)];
    changed = true;
    if (r.failed || r.w <= 0 || r.h <= 0) {
      p.failed = true;
      continue;
    }
    // The rendered bitmap is authoritative for the page's shape (rotation, crop box).
    float aspect = static_cast<float>(r.h) / static_cast<float>(r.w);
    if (std::fabs(aspect - p.h / p.w) > 0.01f) {
      p.h = p.w * aspect;
      layoutDirty_ = true;
    }
    p.pixels = std::move(r.pixels);
    p.bitmapW = r.w;
    p.pixelsH = r.h;
    p.bitmap.Reset();
    p.failed = false;
  }
  worker_->done.clear();
  return changed;
}

float PdfView::scale(const RectF& area) const {
  float fit = std::min(std::max(100.0f, area.w() - 2 * kPadX), kFitMaxWidth);
  return fit / maxPageW_ * zoom;
}

float PdfView::contentWidth(const RectF& area) const { return maxPageW_ * scale(area) + 2 * kPadX; }

void PdfView::layout(const RectF& area) {
  if (!layoutDirty_ && layoutAreaW_ == area.w() && layoutZoom_ == zoom) return;
  const float s = scale(area);
  float y = kPadY;
  for (auto& p : pages_) {
    p.top = y;
    y += p.h * s + kGap;
  }
  contentH_ = pages_.empty() ? 0 : y - kGap + kPadY;
  layoutAreaW_ = area.w();
  layoutZoom_ = zoom;
  layoutDirty_ = false;
}

void PdfView::clampScroll(const RectF& area) {
  layout(area);
  scrollY = std::clamp(scrollY, 0.0f, std::max(0.0f, contentH_ - area.h()));
  scrollX = std::clamp(scrollX, 0.0f, std::max(0.0f, contentWidth(area) - area.w()));
}

void PdfView::scrollBy(const RectF& area, float dx, float dy) {
  scrollX += dx;
  scrollY += dy;
  clampScroll(area);
}

int PdfView::currentPage(const RectF& area) {
  if (pages_.empty()) return 0;
  layout(area);
  float y = scrollY + area.h() / 2;
  auto it = std::upper_bound(pages_.begin(), pages_.end(), y, [](float v, const Page& p) { return v < p.top; });
  return std::max(0, static_cast<int>(it - pages_.begin()) - 1);
}

void PdfView::setZoom(const RectF& area, float newZoom, float anchorY) {
  newZoom = std::clamp(newZoom, 0.25f, 8.0f);
  if (pages_.empty() || newZoom == zoom) {
    zoom = newZoom;
    return;
  }
  layout(area);
  const float oldScale = scale(area);
  float docY = scrollY + anchorY;
  auto it = std::upper_bound(pages_.begin(), pages_.end(), docY, [](float v, const Page& p) { return v < p.top; });
  size_t i = static_cast<size_t>(std::max<ptrdiff_t>(0, (it - pages_.begin()) - 1));
  float frac = (docY - pages_[i].top) / (pages_[i].h * oldScale);
  float centerX = scrollX + area.w() / 2;
  zoom = newZoom;
  layout(area);
  const float newScale = scale(area);
  scrollY = pages_[i].top + frac * pages_[i].h * newScale - anchorY;
  scrollX = centerX * (newScale / oldScale) - area.w() / 2;
  clampScroll(area);
}

void PdfView::paint(Gfx& g, const Theme& theme, const RectF& area) {
  g.fill(area, theme.chrome);
  if (state_ == State::Loading) {
    g.textIn(L"Opening PDF…", area, theme.statusText, g.uiFormat(), true);
    return;
  }
  if (state_ == State::Failed) {
    float cy = area.t + area.h() / 2;
    g.textIn(L"Unable to display this PDF", {area.l, cy - 26, area.r, cy}, theme.tabTextActive, g.uiFormat(), true);
    g.textIn(error_, {area.l + 20, cy + 4, area.r - 20, cy + 30}, theme.statusText, g.uiSmallFormat(), true);
    return;
  }
  if (pages_.empty()) {
    g.textIn(L"This PDF has no pages.", area, theme.statusText, g.uiFormat(), true);
    return;
  }
#ifdef _WIN32
  ID2D1RenderTarget* rt = g.target();
  if (g.targetGeneration() != generation_) {  // bitmaps belong to the previous target
    for (auto& p : pages_) {
      if (p.bitmap) p.bitmapW = 0;
      p.bitmap.Reset();
    }
    generation_ = g.targetGeneration();
  }
#endif
  clampScroll(area);
  const float s = scale(area);
  const float cw = contentWidth(area);
  const float left = cw <= area.w() ? area.l + (area.w() - cw) / 2 : area.l - scrollX;
  const float dpiScale = g.dpi() / 96.0f;
#ifdef _WIN32
  const int maxBitmap = static_cast<int>(std::min<UINT32>(rt->GetMaximumBitmapSize(), 1u << 20));
#else
  const int maxBitmap = 16384;  // cairo image surfaces stay well inside their 32767 limit
#endif

  auto first = std::upper_bound(pages_.begin(), pages_.end(), scrollY, [&](float v, const Page& p) {
    return v < p.top + p.h * s;
  });
  const int firstVis = std::min(static_cast<int>(first - pages_.begin()), pageCount() - 1);
  int lastVis = firstVis;
  while (lastVis + 1 < pageCount() && pages_[static_cast<size_t>(lastVis + 1)].top < scrollY + area.h()) ++lastVis;

  // Pages to rasterize: visible ones first, then one on each side.
  std::vector<std::pair<int, int>> want;
  auto consider = [&](int i) {
    if (i < 0 || i >= pageCount()) return;
    Page& p = pages_[static_cast<size_t>(i)];
    if (p.failed) return;
    int need = static_cast<int>(std::lround(p.w * s * dpiScale));
    need = std::min({need, kMaxRenderWidth, maxBitmap, static_cast<int>(static_cast<float>(maxBitmap) * p.w / p.h)});
    need = std::max(need, 16);
    if (p.bitmapW == 0 || std::abs(p.bitmapW - need) > need / 8) want.emplace_back(i, need);
  };
  for (int i = firstVis; i <= lastVis; ++i) consider(i);
  consider(lastVis + 1);
  consider(firstVis - 1);
  if (want != lastRequest_) {
    {
      std::lock_guard<std::mutex> l(worker_->m);
      worker_->queue.clear();
      for (const auto& w : want)
        if (w != worker_->inFlight) worker_->queue.push_back(w);
    }
    worker_->cv.notify_one();
    lastRequest_ = want;
  }

#ifndef _WIN32
  g.pushClip(area);
  cairo_t* cr = g.cairo();
  for (int i = firstVis; i <= lastVis; ++i) {
    Page& p = pages_[static_cast<size_t>(i)];
    if (!p.pixels.empty()) {
      cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, p.bitmapW, p.pixelsH);
      if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
        cairo_surface_flush(surf);
        unsigned char* dst = cairo_image_surface_get_data(surf);
        const int stride = cairo_image_surface_get_stride(surf);
        for (int y = 0; y < p.pixelsH; ++y)
          memcpy(dst + static_cast<size_t>(y) * stride, p.pixels.data() + static_cast<size_t>(y) * p.bitmapW * 4,
                 static_cast<size_t>(p.bitmapW) * 4);
        cairo_surface_mark_dirty(surf);
        p.bitmap.Attach(surf);
      } else {
        cairo_surface_destroy(surf);
        p.bitmap.Reset();
        p.failed = true;
      }
      std::vector<uint8_t>().swap(p.pixels);
    }
    float pw = p.w * s, ph = p.h * s;
    float x = std::floor(left + kPadX + (maxPageW_ * s - pw) / 2);
    float y = std::floor(area.t + p.top - scrollY);
    RectF pr{x, y, x + pw, y + ph};
    g.fill({pr.l + 2, pr.t + 3, pr.r + 2, pr.b + 3}, Color::hex(0x000000, 0.35f));
    g.fill(pr, Color::hex(0xFFFFFF));
    if (p.bitmap && cr) {
      cairo_save(cr);
      cairo_rectangle(cr, pr.l, pr.t, pr.w(), pr.h());
      cairo_clip(cr);
      cairo_translate(cr, pr.l, pr.t);
      cairo_scale(cr, pw / p.bitmapW, ph / p.pixelsH);
      cairo_set_source_surface(cr, p.bitmap.Get(), 0, 0);
      cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
      cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
      cairo_paint(cr);
      cairo_restore(cr);
    } else {
      std::wstring label = (p.failed ? L"Unable to render page " : L"Loading page ") + std::to_wstring(i + 1) +
                           (p.failed ? L"" : L"…");
      g.textIn(label, pr, Color::hex(0x8A8F98), g.uiFormat(), true);
    }
  }
  g.popClip();
#else
  rt->PushAxisAlignedClip(D2D1::RectF(area.l, area.t, area.r, area.b), D2D1_ANTIALIAS_MODE_ALIASED);
  for (int i = firstVis; i <= lastVis; ++i) {
    Page& p = pages_[static_cast<size_t>(i)];
    if (!p.pixels.empty()) {
      D2D1_BITMAP_PROPERTIES props =
          D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
      p.bitmap.Reset();
      if (FAILED(rt->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(p.bitmapW), static_cast<UINT32>(p.pixelsH)),
                                  p.pixels.data(), static_cast<UINT32>(p.bitmapW) * 4, props,
                                  p.bitmap.GetAddressOf())))
        p.failed = true;
      std::vector<uint8_t>().swap(p.pixels);
    }
    float pw = p.w * s, ph = p.h * s;
    float x = std::floor(left + kPadX + (maxPageW_ * s - pw) / 2);
    float y = std::floor(area.t + p.top - scrollY);
    RectF pr{x, y, x + pw, y + ph};
    g.fill({pr.l + 2, pr.t + 3, pr.r + 2, pr.b + 3}, Color::hex(0x000000, 0.35f));
    g.fill(pr, Color::hex(0xFFFFFF));
    if (p.bitmap) {
      rt->DrawBitmap(p.bitmap.Get(), D2D1::RectF(pr.l, pr.t, pr.r, pr.b), 1.0f,
                     D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
      std::wstring label = (p.failed ? L"Unable to render page " : L"Loading page ") + std::to_wstring(i + 1) +
                           (p.failed ? L"" : L"…");
      g.textIn(label, pr, Color::hex(0x8A8F98), g.uiFormat(), true);
    }
  }
  rt->PopAxisAlignedClip();
#endif

  // Release pages well outside the viewport.
  for (int i = 0; i < pageCount(); ++i) {
    if (i >= firstVis - 3 && i <= lastVis + 3) continue;
    Page& p = pages_[static_cast<size_t>(i)];
    if (p.bitmap || !p.pixels.empty()) {
      p.bitmap.Reset();
      std::vector<uint8_t>().swap(p.pixels);
      p.bitmapW = 0;
    }
  }

  if (contentH_ > area.h()) {
    float maxY = contentH_ - area.h();
    float th = std::max(30.0f, area.h() * area.h() / contentH_);
    float ty = area.t + (area.h() - th) * (scrollY / maxY);
    g.fillRounded({area.r - 9, ty, area.r - 3, ty + th}, 3, theme.scrollThumb);
  }
}

}  // namespace verso
