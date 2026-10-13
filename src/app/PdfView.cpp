#include "app/PdfView.h"

#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cwchar>
#include <mutex>
#include <thread>

namespace verso {

namespace wdp = winrt::Windows::Data::Pdf;
namespace wgi = winrt::Windows::Graphics::Imaging;
namespace wss = winrt::Windows::Storage::Streams;

namespace {

constexpr float kPadX = 24, kPadY = 20, kGap = 16;
constexpr float kFitMaxWidth = 1100;  // fit-to-width stops growing here (DIPs)
constexpr int kMaxRenderWidth = 4096;  // pixels; wider views are upscaled

}  // namespace

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
  ID2D1RenderTarget* rt = g.target();
  if (g.targetGeneration() != generation_) {  // bitmaps belong to the previous target
    for (auto& p : pages_) {
      if (p.bitmap) p.bitmapW = 0;
      p.bitmap.Reset();
    }
    generation_ = g.targetGeneration();
  }
  clampScroll(area);
  const float s = scale(area);
  const float cw = contentWidth(area);
  const float left = cw <= area.w() ? area.l + (area.w() - cw) / 2 : area.l - scrollX;
  const float dpiScale = g.dpi() / 96.0f;
  const int maxBitmap = static_cast<int>(std::min<UINT32>(rt->GetMaximumBitmapSize(), 1u << 20));

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
