// The Linux front end for App: the counterpart of App::run and the Win32 window procedure
// (App.cpp), plus what Windows provides and Linux does not: a menu bar and popup menus
// drawn by Verso, message boxes, file dialogs (zenity), single-instance handoff and the
// bundled fonts. Everything else is the shared App code, driven through the same methods.
#include <cairo.h>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "app/App.h"
#include "app/linux/Menus.h"
#include "app/linux/Window.h"
#include "core/TextUtil.h"
#include "merge_view.h"

namespace verso::linux_fonts {
struct Blob {
  const uint8_t* data;
  size_t size;
};
extern const Blob kFonts[];
extern const size_t kFontCount;
extern const char* const kOflText;
}  // namespace verso::linux_fonts

namespace verso {

namespace {

App* gApp = nullptr;
constexpr float kMenuBarH = 26.0f;
constexpr float kMenuRowH = 26.0f;
constexpr float kMenuSepH = 9.0f;
constexpr UINT_PTR kTimerBlink = 1;
constexpr UINT_PTR kTimerSession = 3;

std::string runtimeDir() {
  const char* r = getenv("XDG_RUNTIME_DIR");
  if (r && *r) return r;
  return "/tmp";
}

// The bundled JetBrains Mono Slashed: written once to the runtime folder and registered
// with fontconfig before Pango first looks for fonts (Windows loads them from resources).
void registerEmbeddedFonts() {
  std::string dir = runtimeDir() + "/verso-fonts";
  mkdir(dir.c_str(), 0700);
  for (size_t i = 0; i < linux_fonts::kFontCount; ++i) {
    std::string path = dir + "/JetBrainsMonoSlashed-" + std::to_string(i) + ".ttf";
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || static_cast<size_t>(st.st_size) != linux_fonts::kFonts[i].size) {
      std::string tmp = path + ".part";
      if (FILE* f = fopen(tmp.c_str(), "wb")) {
        fwrite(linux_fonts::kFonts[i].data, 1, linux_fonts::kFonts[i].size, f);
        fclose(f);
        rename(tmp.c_str(), path.c_str());
      }
    }
    FcConfigAppFontAddFile(FcConfigGetCurrent(), reinterpret_cast<const FcChar8*>(path.c_str()));
  }
}

std::string instanceSocketPath() { return runtimeDir() + "/verso-" + std::to_string(getuid()) + ".sock"; }

// Hands `paths` to a Verso that is already running; false when there is none.
bool sendToRunningInstance(const std::vector<std::wstring>& paths) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::string p = instanceSocketPath();
  strncpy(addr.sun_path, p.c_str(), sizeof addr.sun_path - 1);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    close(fd);
    return false;
  }
  std::string payload;
  for (const auto& path : paths) payload += wideToUtf8(path) + "\n";
  size_t off = 0;
  while (off < payload.size()) {
    ssize_t n = write(fd, payload.data() + off, payload.size() - off);
    if (n <= 0) break;
    off += static_cast<size_t>(n);
  }
  close(fd);
  return true;
}

std::wstring stripMnemonic(const std::wstring& s, wchar_t* mnemonic = nullptr) {
  std::wstring out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == L'&' && i + 1 < s.size()) {
      if (s[i + 1] == L'&') {
        out += L'&';
      } else if (mnemonic && !*mnemonic) {
        *mnemonic = static_cast<wchar_t>(std::towupper(s[i + 1]));
      }
      if (s[i + 1] == L'&') ++i;
      continue;
    }
    out += s[i];
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// UI state that only the Linux front end has.
// ---------------------------------------------------------------------------------------
struct App::LinuxUi {
  // menu bar
  struct BarItem {
    RectF r;
    HMENU sub = nullptr;
    std::wstring label;
    wchar_t mnemonic = 0;
  };
  std::vector<BarItem> bar;
  int barHover = -1, barOpen = -1, barSwitchTo = -1;
  // open popup menus (a cascade: [0] is the top one)
  struct Popup {
    HMENU menu = nullptr;
    RectF r;
    std::vector<RectF> rows;  // one per item
    int hover = -1;
    int openSub = -1;
  };
  std::vector<Popup> popups;
  bool popupActive = false;
  UINT popupResult = 0;
  // message box
  struct Dialog {
    std::wstring text, caption;
    std::vector<std::pair<std::wstring, int>> buttons;
    std::vector<RectF> buttonRects;
    int def = 0, hover = -1, result = 0;
    bool open = false;
  };
  Dialog dialog;
  float mouseX = -1, mouseY = -1;  // surface coordinates
  int listenFd = -1;
};

App::App() : lx_(std::make_unique<LinuxUi>()) { gApp = this; }

App::~App() {
  index_.stop();
  if (lx_ && lx_->listenFd >= 0) {
    close(lx_->listenFd);
    unlink(instanceSocketPath().c_str());
  }
  if (gApp == this) gApp = nullptr;
}

App* App::current() { return gApp; }

namespace platform {
extern float gPointerX, gPointerY;  // Win32Compat.cpp (GetCursorPos)
float menuBarHeight() {
  App* app = App::current();
  return app && windowMenu() && app->menuBarShown() ? kMenuBarH : 0;
}
}  // namespace platform

// ---------------------------------------------------------------------------------------
// Startup and the event loop
// ---------------------------------------------------------------------------------------
int App::run(HINSTANCE, int, const std::vector<std::wstring>& args) {
  bool newWindow = false;
  std::vector<std::wstring> paths;
  for (const auto& a : args) {
    if (a == L"-n" || a == L"--new-window") {
      newWindow = true;
      continue;
    }
    paths.push_back(fullPathOf(a));
  }
  // Hand files to an already-running window so one session owns hot exit.
  if (!newWindow && sendToRunningInstance(paths)) return 0;
  ownsSession_ = !newWindow || !sendToRunningInstance({});

  registerEmbeddedFonts();
  window_ = std::make_unique<platform::Window>();
  hwnd_ = reinterpret_cast<HWND>(window_.get());
  platform::bindWin32(
      window_.get(), hwnd_,
      [](void* ctx, UINT msg, WPARAM wp, LPARAM) {
        App* app = static_cast<App*>(ctx);
        if (msg == WM_COMMAND) {
          app->onMenuCommand(static_cast<UINT>(LOWORD(wp)));
          return;
        }
        if (msg == WM_CLOSE) {  // File: Exit
          app->requestClose();
          return;
        }
        app->onAppMessage(msg, wp);
      },
      [](void* ctx, UINT_PTR id) { static_cast<App*>(ctx)->onTimer(id); }, this);
  platform::setMenuPresenter(
      [](void* ctx, HMENU menu, int x, int y, UINT flags) { return static_cast<App*>(ctx)->linuxPopup(menu, x, y, flags); },
      [](void* ctx) { static_cast<App*>(ctx)->invalidate(); }, this);

  keymap_.loadDefaults();
  loadSettings(false);
  loadKeymap(false);
  registerCommands();

  if (!window_->create("Verso", "verso", 1280, 820)) {
    fprintf(stderr, "verso: cannot open a window (is a Wayland compositor running?)\n");
    return 1;
  }
  gfx_.init();
  applyFont();
  applyTheme();
  buildMenu();

  window_->onPaint = [this](cairo_t* cr, int w, int h) { linuxPaint(cr, w, h); };
  window_->onResize = [this](int, int) {
    gfx_.resize();
    computeLayout();
    layoutMergeView();
    if (Document* d = doc()) clampScroll(*d);
    invalidate();
  };
  window_->onKeyDown = [this](unsigned vk, uint32_t, bool repeat) { linuxKeyDown(vk, repeat); };
  window_->onChar = [this](char32_t c) { linuxChar(c); };
  window_->onMouseButton = [this](int b, bool down, float x, float y, uint32_t) { linuxMouseButton(b, down, x, y); };
  window_->onMouseMove = [this](float x, float y, uint32_t) { linuxMouseMove(x, y); };
  window_->onMouseLeave = [this] {
    lx_->barHover = -1;
    if (drag_ == Drag::None) {
      mouseX_ = mouseY_ = -1;
      invalidate();
    }
  };
  window_->onWheel = [this](float n, bool h, float x, float y, uint32_t) { linuxWheel(n, h, x, y); };
  window_->onFocus = [this](bool active) {
    if (active) {
      resetBlink();
    } else {
      pendingChord_.reset();
      invalidate();
    }
    onActivateApp(active);
  };
  window_->onClose = [this] {
    if (lx_->dialog.open || lx_->popupActive || (merge_ && merge_->modal())) return;
    requestClose();
  };
  window_->onDropFiles = [this](const std::vector<std::string>& files) {
    std::vector<std::wstring> w;
    for (const auto& f : files) w.push_back(utf8ToWide(f));
    openPaths(w);
  };

  // Single instance: later launches hand their files over this socket.
  if (ownsSession_) {
    std::string sp = instanceSocketPath();
    unlink(sp.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sp.c_str(), sizeof addr.sun_path - 1);
    if (fd >= 0 && bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0 && listen(fd, 4) == 0) {
      chmod(sp.c_str(), 0600);
      lx_->listenFd = fd;
      window_->addWatch(fd, [this, fd] {
        int c = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) return;
        std::string data;
        char buf[4096];
        for (;;) {
          pollfd p{c, POLLIN, 0};
          if (poll(&p, 1, 1000) <= 0) break;
          ssize_t n = read(c, buf, sizeof buf);
          if (n <= 0) break;
          data.append(buf, static_cast<size_t>(n));
        }
        close(c);
        std::vector<std::wstring> files;
        size_t start = 0;
        for (size_t i = 0; i < data.size(); ++i)
          if (data[i] == '\n') {
            if (i > start) files.push_back(utf8ToWide(data.substr(start, i - start)));
            start = i + 1;
          }
        openPaths(files);
      });
    } else if (fd >= 0) {
      close(fd);
    }
  }

  git_.start(hwnd_, WM_APP_GIT_READY);
  preview_.images().setNotify(hwnd_, WM_APP_MD_IMAGES);
  preview_.images().setRemoteAllowed(settings_.markdownRemoteImages);
  startMcp();
  restoreSession();
  openPaths(paths);
  for (auto& d : docs_) requestGitBaseline(*d);
  gitRefresh();
  if (docs_.empty()) newDocument();
  computeLayout();
  updateTitle();

  SetTimer(hwnd_, kTimerBlink, 530, nullptr);
  SetTimer(hwnd_, kTimerSession, 15000, nullptr);
  if (startInGitMode_) setGitMode(true);
  if (startInTerminalMode_ && !gitMode_) setTerminalMode(true);

  int rc = window_->run();
  KillTimer(hwnd_, kTimerBlink);
  KillTimer(hwnd_, kTimerSession);
  KillTimer(hwnd_, kTimerAutoscroll);
  git_.stop();
  index_.stop();
  return rc;
}

// ---------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------
void App::linuxKeyDown(unsigned vk, bool repeat) {
  platform::setMessageTime(static_cast<LONG>(GetTickCount64()));
  if (lx_->dialog.open) {
    auto& d = lx_->dialog;
    if (vk == VK_ESCAPE) {
      for (auto& b : d.buttons)
        if (b.second == IDCANCEL || b.second == IDNO) d.result = b.second;
      if (!d.result && d.buttons.size() == 1) d.result = d.buttons[0].second;
    } else if (vk == VK_RETURN || vk == VK_SPACE) {
      d.result = d.buttons[static_cast<size_t>(std::clamp(d.def, 0, static_cast<int>(d.buttons.size()) - 1))].second;
    } else if (vk == VK_LEFT || (vk == VK_TAB && (GetKeyState(VK_SHIFT) < 0))) {
      d.def = std::max(0, d.def - 1);
    } else if (vk == VK_RIGHT || vk == VK_TAB) {
      d.def = std::min(static_cast<int>(d.buttons.size()) - 1, d.def + 1);
    }
    invalidate();
    return;
  }
  if (lx_->popupActive) {
    popupKey(vk);
    return;
  }
  const bool gitDialog = gitMode_ && merge_ && merge_->modal();
  // F10 opens the menu bar, as on Windows.
  if (vk == VK_F10 && GetKeyState(VK_SHIFT) >= 0 && !lx_->bar.empty() && !gitDialog) {
    openMenuBarItem(0);
    return;
  }
  bool handled = false;
  if (gitMode_ && merge_) {
    // The git view has its own shortcuts; what it leaves may open its menus (Alt+letter).
    handled = merge_->keyDown(vk, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_MENU) < 0);
    if (gitDialog) return;
  } else {
    onKeyDown(vk, handled);
  }
  // Alt+letter opens a menu by its mnemonic when Verso itself did not use the key.
  if (!handled && GetKeyState(VK_MENU) < 0 && GetKeyState(VK_CONTROL) >= 0 && vk >= 'A' && vk <= 'Z' && !terminalMode_) {
    for (size_t i = 0; i < lx_->bar.size(); ++i)
      if (lx_->bar[i].mnemonic == static_cast<wchar_t>(vk)) {
        suppressChar_ = true;
        openMenuBarItem(static_cast<int>(i));
        return;
      }
  }
  (void)repeat;
}

void App::linuxChar(char32_t c) {
  if (lx_->dialog.open || lx_->popupActive) return;
  bool alt = GetKeyState(VK_MENU) < 0 && GetKeyState(VK_CONTROL) >= 0;
  if (gitMode_ && merge_) {
    // Alt+key is a shortcut or a menu mnemonic there (WM_SYSCHAR), never typed text.
    if (!alt) merge_->charTyped(static_cast<wchar_t>(c));
    suppressChar_ = false;
    return;
  }
  if (alt) {
    // WM_SYSCHAR on Windows: Alt+key goes to the terminal (as an ESC prefix), nowhere else.
    if (suppressChar_) {
      suppressChar_ = false;
      return;
    }
    if (terminalMode_ && !gitMode_ && overlay_ == Overlay::None && focus_ == Focus::Editor && c != U' ')
      terminalChar(static_cast<wchar_t>(c), true);
    return;
  }
  onChar(static_cast<wchar_t>(c));
}

void App::linuxMouseButton(int button, bool down, float x, float y) {
  platform::setMessageTime(static_cast<LONG>(GetTickCount64()));
  static const int kVk[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
  if (button <= platform::MouseMiddle) platform::setMouseButton(kVk[button], down);
  platform::gPointerX = x;
  platform::gPointerY = y;
  lx_->mouseX = x;
  lx_->mouseY = y;
  if (lx_->dialog.open) {
    if (down && button == platform::MouseLeft) {
      for (size_t i = 0; i < lx_->dialog.buttonRects.size(); ++i)
        if (lx_->dialog.buttonRects[i].contains(x, y)) lx_->dialog.result = lx_->dialog.buttons[i].second;
    }
    return;
  }
  if (lx_->popupActive) {
    popupMouse(button, down, x, y);
    return;
  }
  if (menuBarMouse(button, down, x, y)) return;
  float mb = platform::menuBarHeight();
  float cy = y - mb;
  // The git view takes its area; the status bar below it stays the editor's.
  if (gitMode_ && merge_ && (merge_->mouseButton(button, down, x, cy) || merge_->modal())) return;
  if (button == platform::MouseBack || button == platform::MouseForward) {
    // Mouse back/forward buttons follow the Markdown link history.
    if (down && mdMode_ && !gitMode_) navigateMarkdown(button == platform::MouseBack);
    return;
  }
  if (down) onMouseDown(button, x, cy);
  else onMouseUp(button, x, cy);
}

void App::linuxMouseMove(float x, float y) {
  platform::gPointerX = x;
  platform::gPointerY = y;
  lx_->mouseX = x;
  lx_->mouseY = y;
  if (lx_->dialog.open) {
    int h = -1;
    for (size_t i = 0; i < lx_->dialog.buttonRects.size(); ++i)
      if (lx_->dialog.buttonRects[i].contains(x, y)) h = static_cast<int>(i);
    if (h != lx_->dialog.hover) {
      lx_->dialog.hover = h;
      invalidate();
    }
    window_->setCursor(platform::Cursor::Arrow);
    return;
  }
  if (lx_->popupActive) {
    popupMouse(-1, false, x, y);
    window_->setCursor(platform::Cursor::Arrow);
    return;
  }
  float mb = platform::menuBarHeight();
  if (y < mb) {
    int h = -1;
    for (size_t i = 0; i < lx_->bar.size(); ++i)
      if (lx_->bar[i].r.contains(x, y)) h = static_cast<int>(i);
    if (h != lx_->barHover) {
      lx_->barHover = h;
      invalidate();
    }
    window_->setCursor(platform::Cursor::Arrow);
    return;
  }
  if (lx_->barHover != -1) {
    lx_->barHover = -1;
    invalidate();
  }
  float cy = y - mb;
  if (gitMode_ && merge_) {
    MergeView::Pointer pointer = MergeView::Pointer::Arrow;
    if (merge_->mouseMove(x, cy, &pointer) || merge_->modal()) {
      window_->setCursor(pointer == MergeView::Pointer::IBeam     ? platform::Cursor::IBeam
                         : pointer == MergeView::Pointer::ResizeH ? platform::Cursor::ResizeH
                                                                  : platform::Cursor::Arrow);
      if (mouseX_ >= 0) {  // the status bar's hover highlight follows the pointer out
        mouseX_ = mouseY_ = -1;
        invalidate();
      }
      return;
    }
  }
  onMouseMove(x, cy);
  switch (cursorAt(x, cy)) {
    case CursorKind::IBeam: window_->setCursor(platform::Cursor::IBeam); break;
    case CursorKind::SizeWE: window_->setCursor(platform::Cursor::ResizeH); break;
    case CursorKind::Hand: window_->setCursor(platform::Cursor::Hand); break;
    default: window_->setCursor(platform::Cursor::Arrow); break;
  }
}

void App::linuxWheel(float notches, bool horizontal, float x, float y) {
  if (lx_->dialog.open || lx_->popupActive) return;
  float cy = y - platform::menuBarHeight();
  if (cy < 0) return;
  if (gitMode_ && merge_) {
    merge_->wheel(notches, horizontal, x, cy);
    return;
  }
  if (GetKeyState(VK_SHIFT) < 0) horizontal = true;
  onMouseWheel(notches, horizontal, x, cy);
}

// ---------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------
void App::linuxPaint(cairo_t* cr, int w, int h) {
  float mb = platform::menuBarHeight();
  gfx_.setTarget(cr, static_cast<float>(w), static_cast<float>(h));
  cairo_save(cr);
  cairo_translate(cr, 0, mb);
  cairo_rectangle(cr, 0, 0, w, std::max(0.0f, h - mb));
  cairo_clip(cr);
  paint();
  if (gitMode_ && merge_) {
    RectF area{0, 0, static_cast<float>(w), std::max(0.0f, h - mb - lay_.status.h())};
    merge_->paint(gfx_, theme_, area);
  }
  cairo_restore(cr);
  gfx_.setTarget(cr, static_cast<float>(w), static_cast<float>(h));
  if (mb > 0) paintMenuBar();
  paintPopups();
  paintDialog();
}

void App::paintMenuBar() {
  HMENU bar = platform::windowMenu();
  auto* items = platform::menuItems(bar);
  lx_->bar.clear();
  float w = static_cast<float>(window_->width());
  gfx_.fill(RectF{0, 0, w, kMenuBarH}, theme_.chrome);
  gfx_.fill(RectF{0, kMenuBarH - 1, w, kMenuBarH}, theme_.tabBorder);
  if (!items) return;
  float x = 6;
  for (const auto& it : *items) {
    LinuxUi::BarItem b;
    b.sub = it.sub();
    b.label = stripMnemonic(it.text, &b.mnemonic);
    float tw = gfx_.textWidth(b.label, gfx_.uiFormat());
    b.r = RectF{x, 0, x + tw + 18, kMenuBarH - 1};
    x = b.r.r;
    lx_->bar.push_back(b);
  }
  for (size_t i = 0; i < lx_->bar.size(); ++i) {
    const auto& b = lx_->bar[i];
    bool on = static_cast<int>(i) == lx_->barOpen || (lx_->barOpen < 0 && static_cast<int>(i) == lx_->barHover);
    if (on) gfx_.fillRounded(b.r.inset(2, 3), 4, theme_.buttonHover);
    gfx_.textIn(b.label, b.r, theme_.sidebarText, gfx_.uiFormat(), true);
  }
}

void App::paintPopups() {
  for (const auto& p : lx_->popups) {
    auto* items = platform::menuItems(p.menu);
    if (!items) continue;
    gfx_.fillRounded(RectF{p.r.l + 2, p.r.t + 4, p.r.r + 2, p.r.b + 6}, 7, theme_.shadow);
    gfx_.fillRounded(p.r, 6, theme_.overlayBg);
    gfx_.strokeRounded(p.r, 6, theme_.overlayBorder, 1.0f);
    for (size_t i = 0; i < items->size() && i < p.rows.size(); ++i) {
      const auto& it = (*items)[i];
      RectF row = p.rows[i];
      if (it.separator()) {
        float y = std::floor((row.t + row.b) / 2) + 0.5f;
        gfx_.line(row.l + 8, y, row.r - 8, y, theme_.overlayBorder, 1.0f);
        continue;
      }
      bool hot = static_cast<int>(i) == p.hover || static_cast<int>(i) == p.openSub;
      if (hot && !it.disabled()) gfx_.fillRounded(row.inset(4, 1), 4, theme_.overlaySelected);
      Color fg = it.disabled() ? theme_.overlayDetail : theme_.overlayText;
      std::wstring label = it.text, shortcut;
      size_t tab = label.find(L'\t');
      if (tab != std::wstring::npos) {
        shortcut = label.substr(tab + 1);
        label = label.substr(0, tab);
      }
      label = stripMnemonic(label);
      if (it.checked()) {
        D2D1_POINT_2F tick[] = {D2D1::Point2F(row.l + 11, row.t + 13), D2D1::Point2F(row.l + 14, row.t + 16.5f),
                                D2D1::Point2F(row.l + 20, row.t + 9.5f)};
        gfx_.path(tick, 3, fg, 1.6f, false, false);
      }
      gfx_.textIn(label, RectF{row.l + 28, row.t, row.r - 24, row.b}, fg, gfx_.uiFormat());
      if (!shortcut.empty()) {
        float sw = gfx_.textWidth(shortcut, gfx_.uiFormat());
        gfx_.textIn(shortcut, RectF{row.r - 22 - sw, row.t, row.r - 14, row.b}, theme_.overlayDetail, gfx_.uiFormat());
      }
      if (it.sub()) {
        float cx = row.r - 14, cy = (row.t + row.b) / 2;
        D2D1_POINT_2F chev[] = {D2D1::Point2F(cx - 2, cy - 4), D2D1::Point2F(cx + 2, cy), D2D1::Point2F(cx - 2, cy + 4)};
        gfx_.path(chev, 3, fg, 1.4f, false, false);
      }
    }
  }
}

void App::paintDialog() {
  auto& d = lx_->dialog;
  if (!d.open) return;
  float w = static_cast<float>(window_->width()), h = static_cast<float>(window_->height());
  gfx_.fill(RectF{0, 0, w, h}, Color::hex(0x000000, 0.35f));
  IDWriteTextFormat* f = gfx_.uiFormat();
  // Word-wrap the message to the card width.
  const float cardW = std::min(460.0f, w - 40);
  const float textW = cardW - 48;
  std::vector<std::wstring> lines;
  size_t start = 0;
  while (start <= d.text.size()) {
    size_t nl = d.text.find(L'\n', start);
    std::wstring para = d.text.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
    std::wstring cur;
    size_t i = 0;
    while (i <= para.size()) {
      size_t sp = para.find(L' ', i);
      std::wstring word = para.substr(i, sp == std::wstring::npos ? std::wstring::npos : sp - i);
      std::wstring cand = cur.empty() ? word : cur + L" " + word;
      if (!cur.empty() && gfx_.textWidth(cand, f) > textW) {
        lines.push_back(cur);
        cur = word;
      } else {
        cur = cand;
      }
      if (sp == std::wstring::npos) break;
      i = sp + 1;
    }
    lines.push_back(cur);
    if (nl == std::wstring::npos) break;
    start = nl + 1;
  }
  float lh = gfx_.formatHeight(f) + 3;
  float cardH = 64 + lines.size() * lh + 56;
  RectF card{std::floor((w - cardW) / 2), std::floor((h - cardH) / 2), 0, 0};
  card.r = card.l + cardW;
  card.b = card.t + cardH;
  gfx_.fillRounded(RectF{card.l + 2, card.t + 6, card.r + 2, card.b + 8}, 10, theme_.shadow);
  gfx_.fillRounded(card, 9, theme_.overlayBg);
  gfx_.strokeRounded(card, 9, theme_.overlayBorder, 1.0f);
  gfx_.text(d.caption.empty() ? std::wstring(L"Verso") : d.caption, card.l + 24, card.t + 18, theme_.overlayText,
            gfx_.uiBoldFormat());
  float y = card.t + 48;
  for (const auto& l : lines) {
    gfx_.text(l, card.l + 24, y, theme_.overlayText, f);
    y += lh;
  }
  d.buttonRects.assign(d.buttons.size(), RectF{});
  float bx = card.r - 20;
  for (size_t k = d.buttons.size(); k-- > 0;) {
    float bw = std::max(84.0f, gfx_.textWidth(d.buttons[k].first, f) + 28);
    RectF b{bx - bw, card.b - 44, bx, card.b - 16};
    d.buttonRects[k] = b;
    bool isDef = static_cast<int>(k) == d.def, hot = static_cast<int>(k) == d.hover;
    gfx_.fillRounded(b, 5, isDef ? theme_.accent : (hot ? theme_.buttonHover : theme_.buttonBg));
    gfx_.textIn(d.buttons[k].first, b, isDef ? Color::hex(0xFFFFFF) : theme_.buttonText, f, true);
    bx = b.l - 8;
  }
}

// ---------------------------------------------------------------------------------------
// Menu bar and popup menus
// ---------------------------------------------------------------------------------------
bool App::menuBarMouse(int button, bool down, float x, float y) {
  if (y >= platform::menuBarHeight() || button != platform::MouseLeft) return y < platform::menuBarHeight();
  if (!down) return true;
  for (size_t i = 0; i < lx_->bar.size(); ++i)
    if (lx_->bar[i].r.contains(x, y)) {
      openMenuBarItem(static_cast<int>(i));
      return true;
    }
  return true;
}

void App::openMenuBarItem(int index) {
  int next = index;
  while (next >= 0 && next < static_cast<int>(lx_->bar.size())) {
    lx_->barOpen = next;
    lx_->barSwitchTo = -1;
    const auto& b = lx_->bar[static_cast<size_t>(next)];
    invalidate();
    UINT cmd = b.sub ? linuxPopup(b.sub, static_cast<int>(b.r.l), static_cast<int>(kMenuBarH - 1), 0) : 0;
    next = lx_->barSwitchTo;
    lx_->barOpen = -1;
    invalidate();
    if (cmd) {
      if (gitMode_ && merge_) merge_->onMenuCommand(cmd, 0);
      else onMenuCommand(cmd);
      return;
    }
  }
}

UINT App::linuxPopup(HMENU menu, int x, int y, UINT) {
  if (!platform::menuItems(menu) || lx_->popupActive) return 0;
  if (!gitMode_) onInitMenuPopup(menu);  // WM_INITMENUPOPUP: check marks, recent files
  auto layoutPopup = [this](LinuxUi::Popup& p, float x0, float y0) {
    auto* items = platform::menuItems(p.menu);
    float wmax = 160;
    float h = 6;
    for (const auto& it : *items) {
      if (it.separator()) {
        h += kMenuSepH;
        continue;
      }
      std::wstring label = it.text, shortcut;
      size_t tab = label.find(L'\t');
      if (tab != std::wstring::npos) {
        shortcut = label.substr(tab + 1);
        label = label.substr(0, tab);
      }
      float tw = gfx_.textWidth(stripMnemonic(label), gfx_.uiFormat()) + 28 + 24;
      if (!shortcut.empty()) tw += gfx_.textWidth(shortcut, gfx_.uiFormat()) + 36;
      wmax = std::max(wmax, tw);
      h += kMenuRowH;
    }
    h += 6;
    float W = static_cast<float>(window_->width()), H = static_cast<float>(window_->height());
    if (x0 + wmax > W - 4) x0 = std::max(4.0f, W - 4 - wmax);
    if (y0 + h > H - 4) y0 = std::max(4.0f, H - 4 - h);
    p.r = RectF{x0, y0, x0 + wmax, y0 + h};
    p.rows.clear();
    float yy = y0 + 6;
    for (const auto& it : *items) {
      float rh = it.separator() ? kMenuSepH : kMenuRowH;
      p.rows.push_back(RectF{x0, yy, x0 + wmax, yy + rh});
      yy += rh;
    }
  };
  LinuxUi::Popup top;
  top.menu = menu;
  layoutPopup(top, static_cast<float>(x), static_cast<float>(y));
  lx_->popups = {top};
  lx_->popupActive = true;
  lx_->popupResult = 0;
  invalidate();
  while (lx_->popupActive && window_->step(-1)) {
    // Lay out any newly opened submenu next to its row.
    for (size_t level = 1; level < lx_->popups.size(); ++level) {
      auto& parent = lx_->popups[level - 1];
      auto& child = lx_->popups[level];
      if (child.rows.empty() && parent.openSub >= 0 && static_cast<size_t>(parent.openSub) < parent.rows.size())
        layoutPopup(child, parent.r.r - 4, parent.rows[static_cast<size_t>(parent.openSub)].t - 6);
    }
  }
  lx_->popups.clear();
  invalidate();
  return lx_->popupResult;
}

bool App::popupMouse(int button, bool down, float x, float y) {
  auto& ui = *lx_;
  // The deepest popup under the pointer.
  int level = -1, row = -1;
  for (int l = static_cast<int>(ui.popups.size()) - 1; l >= 0 && level < 0; --l) {
    const auto& p = ui.popups[static_cast<size_t>(l)];
    if (!p.r.contains(x, y)) continue;
    level = l;
    for (size_t i = 0; i < p.rows.size(); ++i)
      if (p.rows[i].contains(x, y)) row = static_cast<int>(i);
  }
  if (button < 0) {  // motion
    if (level >= 0) {
      auto& p = ui.popups[static_cast<size_t>(level)];
      if (p.hover != row) {
        p.hover = row;
        auto* items = platform::menuItems(p.menu);
        if (row >= 0 && items && (*items)[static_cast<size_t>(row)].sub()) {
          if (p.openSub != row) {
            ui.popups.resize(static_cast<size_t>(level) + 1);
            p.openSub = row;
            LinuxUi::Popup child;
            child.menu = (*items)[static_cast<size_t>(row)].sub();
            if (!(*items)[static_cast<size_t>(row)].disabled()) ui.popups.push_back(child);
            else p.openSub = -1;
          }
        } else if (row >= 0 && p.openSub >= 0) {
          ui.popups.resize(static_cast<size_t>(level) + 1);
          p.openSub = -1;
        }
        invalidate();
      }
    } else if (ui.barOpen >= 0 && y < platform::menuBarHeight()) {
      // Sliding along the menu bar switches menus.
      for (size_t i = 0; i < ui.bar.size(); ++i)
        if (ui.bar[i].r.contains(x, y) && static_cast<int>(i) != ui.barOpen) {
          ui.barSwitchTo = static_cast<int>(i);
          ui.popupActive = false;
        }
    }
    return true;
  }
  if (!down) {
    if (button != platform::MouseLeft && button != platform::MouseRight) return true;
    if (level < 0 || row < 0) return true;
    auto* items = platform::menuItems(ui.popups[static_cast<size_t>(level)].menu);
    const auto& it = (*items)[static_cast<size_t>(row)];
    if (it.separator() || it.disabled() || it.sub()) return true;
    ui.popupResult = static_cast<UINT>(it.id);
    ui.popupActive = false;
    return true;
  }
  if (level < 0) {
    // A press outside closes the menus; on the menu bar it may open another one.
    ui.popupActive = false;
    if (ui.barOpen >= 0 && y < platform::menuBarHeight())
      for (size_t i = 0; i < ui.bar.size(); ++i)
        if (ui.bar[i].r.contains(x, y) && static_cast<int>(i) != ui.barOpen) ui.barSwitchTo = static_cast<int>(i);
  }
  return true;
}

bool App::popupKey(unsigned vk) {
  auto& ui = *lx_;
  if (ui.popups.empty()) return false;
  auto& p = ui.popups.back();
  auto* items = platform::menuItems(p.menu);
  if (!items) return false;
  auto step = [&](int dir) {
    int n = static_cast<int>(items->size());
    for (int k = 0, i = p.hover; k < n; ++k) {
      i = (i + dir + n) % n;
      if (!(*items)[static_cast<size_t>(i)].separator() && !(*items)[static_cast<size_t>(i)].disabled()) {
        p.hover = i;
        return;
      }
    }
  };
  switch (vk) {
    case VK_ESCAPE:
      if (ui.popups.size() > 1) {
        ui.popups.pop_back();
        ui.popups.back().openSub = -1;
      } else {
        ui.popupActive = false;
      }
      break;
    case VK_DOWN: step(1); break;
    case VK_UP: step(-1); break;
    case VK_RIGHT:
      if (p.hover >= 0 && (*items)[static_cast<size_t>(p.hover)].sub()) {
        p.openSub = p.hover;
        LinuxUi::Popup child;
        child.menu = (*items)[static_cast<size_t>(p.hover)].sub();
        child.hover = 0;
        ui.popups.push_back(child);
      } else if (ui.barOpen >= 0) {
        ui.barSwitchTo = (ui.barOpen + 1) % static_cast<int>(ui.bar.size());
        ui.popupActive = false;
      }
      break;
    case VK_LEFT:
      if (ui.popups.size() > 1) {
        ui.popups.pop_back();
        ui.popups.back().openSub = -1;
      } else if (ui.barOpen >= 0) {
        ui.barSwitchTo = (ui.barOpen + static_cast<int>(ui.bar.size()) - 1) % static_cast<int>(ui.bar.size());
        ui.popupActive = false;
      }
      break;
    case VK_RETURN:
    case VK_SPACE:
      if (p.hover >= 0) {
        const auto& it = (*items)[static_cast<size_t>(p.hover)];
        if (it.sub()) return popupKey(VK_RIGHT);
        if (!it.disabled() && !it.separator()) {
          ui.popupResult = static_cast<UINT>(it.id);
          ui.popupActive = false;
        }
      }
      break;
    default:
      // Mnemonic letters choose an item.
      if (vk >= 'A' && vk <= 'Z') {
        for (size_t i = 0; i < items->size(); ++i) {
          wchar_t m = 0;
          stripMnemonic((*items)[i].text, &m);
          if (m == static_cast<wchar_t>(vk) && !(*items)[i].disabled()) {
            p.hover = static_cast<int>(i);
            return popupKey(VK_RETURN);
          }
        }
      }
      break;
  }
  invalidate();
  return true;
}

// ---------------------------------------------------------------------------------------
// Message boxes and file dialogs
// ---------------------------------------------------------------------------------------
int App::messageBox(const std::wstring& text, const std::wstring& caption, UINT type) {
  if (!window_ || !window_->running() || lx_->dialog.open) {
    fprintf(stderr, "Verso: %s\n", wideToUtf8(text).c_str());
    return (type & 0xF) == MB_YESNO || (type & 0xF) == MB_YESNOCANCEL ? IDNO : IDOK;
  }
  auto& d = lx_->dialog;
  d = LinuxUi::Dialog{};
  d.text = text;
  d.caption = caption;
  switch (type & 0xF) {
    case MB_OKCANCEL: d.buttons = {{L"OK", IDOK}, {L"Cancel", IDCANCEL}}; break;
    case MB_YESNOCANCEL: d.buttons = {{L"Yes", IDYES}, {L"No", IDNO}, {L"Cancel", IDCANCEL}}; break;
    case MB_YESNO: d.buttons = {{L"Yes", IDYES}, {L"No", IDNO}}; break;
    case MB_RETRYCANCEL: d.buttons = {{L"Retry", IDRETRY}, {L"Cancel", IDCANCEL}}; break;
    case MB_ABORTRETRYIGNORE: d.buttons = {{L"Abort", IDABORT}, {L"Retry", IDRETRY}, {L"Ignore", IDIGNORE}}; break;
    default: d.buttons = {{L"OK", IDOK}}; break;
  }
  d.def = (type & MB_DEFBUTTON3) == MB_DEFBUTTON3 ? 2 : (type & MB_DEFBUTTON2) ? 1 : 0;
  d.def = std::min(d.def, static_cast<int>(d.buttons.size()) - 1);
  d.open = true;
  invalidate();
  while (!d.result && window_->step(-1)) {
  }
  int r = d.result ? d.result : IDCANCEL;
  d.open = false;
  invalidate();
  return r;
}

// Runs zenity (or a compatible picker) and returns the chosen paths; an empty list when
// cancelled. The window keeps painting and handling background work meanwhile.
std::vector<std::wstring> App::runFileDialog(const std::vector<std::string>& args) {
  std::string tool;
  for (const char* t : {"zenity", "ryu-picker"}) {
    std::string cmd = std::string("command -v ") + t + " >/dev/null 2>&1";
    if (system(cmd.c_str()) == 0) {
      tool = t;
      break;
    }
  }
  if (tool.empty()) {
    messageBox(L"Install zenity to choose files with a dialog.", L"Verso", MB_ICONWARNING);
    return {};
  }
  int out[2];
  if (pipe2(out, O_CLOEXEC) < 0) return {};
  pid_t pid = fork();
  if (pid == 0) {
    dup2(out[1], 1);
    std::vector<char*> argv = {const_cast<char*>(tool.c_str())};
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(out[1]);
  if (pid < 0) {
    close(out[0]);
    return {};
  }
  std::string data;
  bool done = false;
  window_->addWatch(out[0], [&] {
    char buf[4096];
    ssize_t n = read(out[0], buf, sizeof buf);
    if (n > 0) data.append(buf, static_cast<size_t>(n));
    else done = true;
  });
  while (!done && window_->step(-1)) {
  }
  window_->removeWatch(out[0]);
  close(out[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  std::vector<std::wstring> paths;
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return paths;
  size_t start = 0;
  for (size_t i = 0; i <= data.size(); ++i)
    if (i == data.size() || data[i] == '\n') {
      if (i > start) paths.push_back(utf8ToWide(data.substr(start, i - start)));
      start = i + 1;
    }
  return paths;
}

std::vector<std::wstring> App::promptOpenFiles() {
  std::string dir;
  if (Document* d = doc(); d && !d->path.empty()) dir = wideToUtf8(parentOf(d->path)) + "/";
  else if (!tree_.empty()) dir = wideToUtf8(tree_.folders().front()) + "/";
  std::vector<std::string> args = {"--file-selection", "--multiple", "--separator=\n", "--title=Open"};
  if (!dir.empty()) args.push_back("--filename=" + dir);
  return runFileDialog(args);
}

std::wstring App::promptSaveFile(const Document& d) {
  std::wstring suggestion;
  if (!d.path.empty()) {
    suggestion = d.path;
  } else {
    std::wstring name = d.title() == L"untitled" ? L"untitled" : d.title();
    for (auto& c : name)
      if (c == L'/') c = L'_';
    if (!d.lang->extensions.empty() && d.lang->mode != LexMode::Plain) name += L"." + d.lang->extensions.front();
    else name += L".txt";
    std::wstring folder = !tree_.empty() ? tree_.folders().front() : utf8ToWide(getenv("HOME") ? getenv("HOME") : "/tmp");
    suggestion = folder + kPathSepStr + name;
  }
  auto r = runFileDialog({"--file-selection", "--save", "--confirm-overwrite", "--title=Save As",
                          "--filename=" + wideToUtf8(suggestion)});
  return r.empty() ? std::wstring() : r.front();
}

std::wstring App::promptFolder() {
  auto r = runFileDialog({"--file-selection", "--directory", "--title=Open Folder"});
  return r.empty() ? std::wstring() : r.front();
}

std::wstring App::dataDir() {
  static std::wstring dir;
  if (!dir.empty()) return dir;
  std::string base;
  if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x) base = x;
  else base = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.config";
  mkdir(base.c_str(), 0755);
  std::string d = base + "/verso";
  mkdir(d.c_str(), 0755);
  dir = utf8ToWide(d);
  return dir;
}

void App::setFullscreen(bool on) {
  if (on == fullscreen_) return;
  fullscreen_ = on;
  window_->setFullscreen(on);
  computeLayout();
  invalidate();
}

void App::applyDarkTitleBar() {}   // the compositor draws the title bar
void App::updateImePosition() {}   // text-input-v3 is not wired up yet

}  // namespace verso

// MessageBoxW (Win32Compat.h): a dialog drawn over Verso's window.
int MessageBoxW(HWND, LPCWSTR text, LPCWSTR caption, UINT type) {
  if (verso::App* app = verso::App::current()) return app->messageBox(text ? text : L"", caption ? caption : L"", type);
  fprintf(stderr, "Verso: %s\n", verso::wideToUtf8(text ? text : L"").c_str());
  return IDOK;
}
