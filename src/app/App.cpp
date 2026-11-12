#include "app/App.h"

#ifdef _WIN32
#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <windowsx.h>
#endif

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#include "app/DarkMenu.h"
#else
#include "app/linux/Window.h"
#endif
#include "merge_view.h"

namespace verso {

namespace {

#ifdef _WIN32
constexpr wchar_t kClassName[] = L"VersoMainWindow";
#endif
constexpr UINT_PTR kTimerBlink = 1;
constexpr UINT_PTR kTimerSession = 3;

}  // namespace

#ifdef _WIN32  // Linux: linux/AppLinux.cpp (its window and UI state are complete there)
App::App() = default;

App::~App() { index_.stop(); }
#endif

// Results posted by background work (git status, the file index, PDF pages, Markdown
// images, the terminal and AI tools). Returns false for other messages.
bool App::onAppMessage(UINT msg, WPARAM wp) {
  switch (msg) {
    case WM_APP_GIT_READY:
      onGitResults();
      return true;
    case WM_APP_TERMINAL:
      onTerminalMessage(wp);
      return true;
    case WM_APP_MCP:
      mcp_.runPending([this](const std::string& tool, const json::Value& args) { return mcpCall(tool, args); });
      return true;
    case WM_APP_PDF_READY:
      onPdfReady();
      return true;
    case WM_APP_MD_IMAGES:
      if (preview_.images().takeResults()) invalidate();
      return true;
    case WM_APP_INDEX_READY:
      indexSnapshot_ = index_.snapshot();
      if (overlay_ == Overlay::Files) refreshOverlay();
      invalidate();
      return true;
    default:
      return false;
  }
}

void App::onMenuCommand(UINT id) {
  if (id >= 50000 && id < 50100) {
    size_t i = id - 50000;
    if (i < recentFiles_.size()) openFile(recentFiles_[i]);
    return;
  }
  if (id >= 51000 && id < 51100) {
    size_t i = id - 51000;
    if (i < recentFolders_.size()) openFolder(recentFolders_[i], true);
    return;
  }
  auto it = menuCommands_.find(id);
  if (it != menuCommands_.end()) runCommand(it->second);
}

App::CursorKind App::cursorAt(float x, float y) {
  if (overlay_ != Overlay::None) return overlayInputRect().contains(x, y) ? CursorKind::IBeam : CursorKind::Arrow;
  if (drag_ == Drag::Splitter || (!lay_.splitter.empty() && lay_.splitter.contains(x, y))) return CursorKind::SizeWE;
  if (!lay_.preview.empty() && lay_.preview.contains(x, y)) {
    if (Document* pd = doc(); pd && !preview_.linkAt(x, y, lay_.preview, pd->previewScroll).url.empty())
      return CursorKind::Hand;
    return CursorKind::Arrow;
  }
  if (lay_.text.contains(x, y) && doc()) return CursorKind::IBeam;
  if (findMode_ != FindMode::Hidden && lay_.find.contains(x, y)) {
    FindLayout fl = findLayout();
    if (fl.findBox.contains(x, y) || (findMode_ == FindMode::Replace && fl.replaceBox.contains(x, y)))
      return CursorKind::IBeam;
  }
  return CursorKind::Arrow;
}

#ifdef _WIN32
int App::run(HINSTANCE instance, int show, const std::vector<std::wstring>& args) {
  inst_ = instance;
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
  if (!newWindow) {
    if (HWND other = FindWindowW(kClassName, nullptr)) {
      std::wstring payload;
      for (const auto& p : paths) payload += p + L'\n';
      COPYDATASTRUCT cds{1, static_cast<DWORD>((payload.size() + 1) * sizeof(wchar_t)),
                         const_cast<wchar_t*>(payload.c_str())};
      DWORD_PTR result = 0;
      SendMessageTimeoutW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 3000, &result);
      if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
      SetForegroundWindow(other);
      return 0;
    }
  }

  // Only the first window keeps the hot-exit session; others would overwrite it.
  ownsSession_ = !FindWindowW(kClassName, nullptr);

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = 0;
  wc.lpfnWndProc = &App::wndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wc.hIconSm = wc.hIcon;
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);

  keymap_.loadDefaults();
  loadSettings(false);
  loadKeymap(false);
  registerCommands();

  hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, kClassName, L"Verso", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                          CW_USEDEFAULT, 1280, 820, nullptr, nullptr, instance, this);
  if (!hwnd_) return 1;
  if (!gfx_.init(hwnd_)) {
    MessageBoxW(nullptr, L"Direct2D/DirectWrite initialization failed.", L"Verso", MB_ICONERROR);
    return 1;
  }
  applyFont();
  applyTheme();
  buildMenu();

  git_.start(hwnd_, WM_APP_GIT_READY);
  preview_.images().setNotify(hwnd_, WM_APP_MD_IMAGES);
  preview_.images().setRemoteAllowed(settings_.markdownRemoteImages);
  startMcp();  // before any terminal starts, so its shells can reach the server
  bool restored = restoreSession();
  openPaths(paths);
  for (auto& d : docs_) requestGitBaseline(*d);
  gitRefresh();
  if (docs_.empty()) newDocument();
  computeLayout();
  updateTitle();
  if (!restored) ShowWindow(hwnd_, show);
  UpdateWindow(hwnd_);

  SetTimer(hwnd_, kTimerBlink, 530, nullptr);
  SetTimer(hwnd_, kTimerSession, 15000, nullptr);
  if (startInGitMode_) setGitMode(true);
  if (startInTerminalMode_ && !gitMode_) setTerminalMode(true);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    // In git mode the git view's shortcuts (Ctrl+Enter commit, F5, Ctrl+Shift+G...) apply.
    if (gitMode_ && merge_ && merge_->translateAccelerator(&msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  index_.stop();
  CoUninitialize();
  return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    app->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  }
  if (!app) return DefWindowProcW(hwnd, msg, wp, lp);
  return app->handle(msg, wp, lp);
}

LRESULT App::handle(UINT msg, WPARAM wp, LPARAM lp) {
  if (theme_.dark && menu_) {
    auto ref = [](Color c) {
      return RGB(static_cast<int>(c.r * 255 + 0.5f), static_cast<int>(c.g * 255 + 0.5f),
                 static_cast<int>(c.b * 255 + 0.5f));
    };
    MenuColors colors{ref(theme_.chrome), ref(theme_.sidebarText), ref(theme_.sidebarTextDim),
                      ref(theme_.buttonHover), ref(theme_.overlaySelected)};
    LRESULT result = 0;
    if (handleDarkMenuBar(hwnd_, msg, wp, lp, colors, result)) return result;
  }
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd_, &ps);
      paint();
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      gfx_.resize();
      computeLayout();
      layoutMergeView();
      if (Document* d = doc()) clampScroll(*d);
      invalidate();
      return 0;
    case WM_DPICHANGED: {
      gfx_.setDpi(static_cast<float>(HIWORD(wp)));
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      computeLayout();
      layoutMergeView();
      invalidate();
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
      mmi->ptMinTrackSize = {420, 300};
      return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      bool handled = false;
      onKeyDown(static_cast<UINT>(wp), handled);
      if (handled) return 0;
      if (msg == WM_SYSKEYDOWN) break;
      return 0;
    }
    case WM_CHAR:
      onChar(static_cast<wchar_t>(wp));
      return 0;
    case WM_SYSCHAR:
      if (suppressChar_) {
        suppressChar_ = false;
        return 0;
      }
      if (terminalMode_ && !gitMode_ && overlay_ == Overlay::None && focus_ == Focus::Editor && wp != L' ') {
        terminalChar(static_cast<wchar_t>(wp), true);  // Alt+key: ESC prefix, not the menu
        return 0;
      }
      break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: {
      SetFocus(hwnd_);
      int button = msg == WM_LBUTTONDOWN ? 0 : (msg == WM_RBUTTONDOWN ? 1 : 2);
      onMouseDown(button, gfx_.pxToDip(GET_X_LPARAM(lp)), gfx_.pxToDip(GET_Y_LPARAM(lp)));
      return 0;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
      int button = msg == WM_LBUTTONUP ? 0 : (msg == WM_RBUTTONUP ? 1 : 2);
      onMouseUp(button, gfx_.pxToDip(GET_X_LPARAM(lp)), gfx_.pxToDip(GET_Y_LPARAM(lp)));
      return 0;
    }
    case WM_MOUSEMOVE:
      if (!trackingMouse_) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
        trackingMouse_ = true;
      }
      onMouseMove(gfx_.pxToDip(GET_X_LPARAM(lp)), gfx_.pxToDip(GET_Y_LPARAM(lp)));
      return 0;
    case WM_MOUSELEAVE:
      trackingMouse_ = false;
      if (drag_ == Drag::None) {
        mouseX_ = mouseY_ = -1;
        invalidate();
      }
      return 0;
    case WM_CAPTURECHANGED:
      if (reinterpret_cast<HWND>(lp) != hwnd_) {
        drag_ = Drag::None;
        KillTimer(hwnd_, kTimerAutoscroll);
      }
      return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd_, &pt);
      float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
      bool horizontal = msg == WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT);
      if (msg == WM_MOUSEHWHEEL) notches = -notches;
      onMouseWheel(notches, horizontal, gfx_.pxToDip(pt.x), gfx_.pxToDip(pt.y));
      return 0;
    }
    case WM_SETCURSOR:
      if (reinterpret_cast<HWND>(wp) != hwnd_) break;  // git view controls set their own cursors
      if (LOWORD(lp) == HTCLIENT) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        LPCWSTR cursor = IDC_ARROW;
        switch (cursorAt(gfx_.pxToDip(pt.x), gfx_.pxToDip(pt.y))) {
          case CursorKind::IBeam: cursor = IDC_IBEAM; break;
          case CursorKind::SizeWE: cursor = IDC_SIZEWE; break;
          case CursorKind::Hand: cursor = IDC_HAND; break;
          default: break;
        }
        SetCursor(LoadCursorW(nullptr, cursor));
        return TRUE;
      }
      break;
    case WM_TIMER:
      onTimer(wp);
      return 0;
    case WM_COMMAND: {
      if (gitMode_ && merge_ && lp == 0) {  // the git menu bar is showing
        merge_->onMenuCommand(wp, lp);
        return 0;
      }
      onMenuCommand(LOWORD(wp));
      return 0;
    }
    case WM_INITMENUPOPUP:
      if (!gitMode_) onInitMenuPopup(reinterpret_cast<HMENU>(wp));
      return 0;
    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(wp);
      UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      std::vector<std::wstring> files;
      for (UINT i = 0; i < count; ++i) {
        UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring f(len, L'\0');
        DragQueryFileW(drop, i, f.data(), len + 1);
        files.push_back(f);
      }
      DragFinish(drop);
      openPaths(files);
      return 0;
    }
    case WM_ACTIVATEAPP:
      onActivateApp(wp != 0);
      return 0;
    case WM_SETFOCUS:
      resetBlink();
      return 0;
    case WM_KILLFOCUS:
      pendingChord_.reset();
      invalidate();
      return 0;
    case WM_COPYDATA: {
      auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
      if (cds && cds->dwData == 1 && cds->lpData) {
        std::wstring payload(static_cast<const wchar_t*>(cds->lpData));
        std::vector<std::wstring> files;
        size_t start = 0;
        for (size_t i = 0; i < payload.size(); ++i) {
          if (payload[i] == L'\n') {
            if (i > start) files.push_back(payload.substr(start, i - start));
            start = i + 1;
          }
        }
        openPaths(files);
      }
      return TRUE;
    }
    case WM_IME_STARTCOMPOSITION:
      updateImePosition();
      break;
    case WM_APP_GIT_READY:
    case WM_APP_TERMINAL:
    case WM_APP_MCP:
    case WM_APP_PDF_READY:
    case WM_APP_MD_IMAGES:
    case WM_APP_INDEX_READY:
      onAppMessage(msg, wp);
      return 0;
    case WM_XBUTTONDOWN:
      // Mouse back/forward buttons follow the Markdown link history.
      if (mdMode_ && !gitMode_) {
        navigateMarkdown(GET_XBUTTON_WPARAM(wp) == XBUTTON1);
        return TRUE;
      }
      break;
    case WM_CLOSE:
      requestClose();
      return 0;
    case WM_QUERYENDSESSION:
      saveSession();
      return TRUE;
    case WM_ENDSESSION:
      if (wp) saveSession();
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd_, kTimerBlink);
      KillTimer(hwnd_, kTimerSession);
      KillTimer(hwnd_, kTimerAutoscroll);
      git_.stop();
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}
#endif  // _WIN32

void App::invalidate() {
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::updateTitle() {
  // "Verso - <active file>"; git mode names the repository and branch instead.
  std::wstring subject;
  if (gitMode_)
    subject = mergeTitle_.empty() ? L"Git" : L"Git: " + mergeTitle_;
  else if (terminalMode_)
    subject = term_->screen.title().empty() ? L"Terminal" : L"Terminal: " + term_->screen.title();
  else if (Document* d = doc())
    subject = d->path.empty() ? d->title() : fileNameOf(d->path);
  std::wstring title = subject.empty() ? std::wstring(L"Verso") : L"Verso - " + subject;
  SetWindowTextW(hwnd_, title.c_str());
}

void App::computeLayout() {
  RECT rc;
  GetClientRect(hwnd_, &rc);
  width_ = gfx_.pxToDip(rc.right);
  height_ = gfx_.pxToDip(rc.bottom);
  const float statusH = distractionFree_ ? 0.0f : 24.0f;
  const float findH = findMode_ == FindMode::Hidden ? 0.0f : (findMode_ == FindMode::Replace ? 74.0f : 40.0f);
  float bottom = height_;
  lay_.status = {0, bottom - statusH, width_, bottom};
  bottom -= statusH;
  lay_.find = {0, bottom - findH, width_, bottom};
  bottom -= findH;

  bool side = sidebarVisible_ && !distractionFree_;
  float sw = side ? std::clamp(sidebarWidth_, 120.0f, std::max(120.0f, width_ - 300.0f)) : 0.0f;
  lay_.sidebar = {0, 0, sw, bottom};
  lay_.splitter = side ? RectF{sw - 3, 0, sw + 3, bottom} : RectF{};

  const float tabsH = distractionFree_ ? 0.0f : 36.0f;
  lay_.tabs = {sw, 0, width_, tabsH};
  lay_.editor = {sw, tabsH, width_, bottom};

  Document* d = doc();
  float gw = d && !distractionFree_ ? gutterWidth(*d) : 0.0f;
  float mm = (minimapVisible_ && settings_.drawMinimap && !distractionFree_) ? 110.0f : 0.0f;
  const float sb = 12.0f;
  lay_.vscroll = {width_ - sb, tabsH, width_, bottom};
  lay_.minimap = {width_ - sb - mm, tabsH, width_ - sb, bottom};
  lay_.gutter = {sw, tabsH, sw + gw, bottom};
  lay_.text = {sw + gw, tabsH, width_ - sb - mm, bottom};
  if (distractionFree_) {
    float maxW = gfx_.charWidth() * 100 + 40;
    if (lay_.text.w() > maxW) {
      float pad = (lay_.text.w() - maxW) / 2;
      lay_.text.l += pad;
      lay_.text.r -= pad;
    }
    lay_.gutter = {lay_.text.l, tabsH, lay_.text.l, bottom};
  }
  lay_.preview = {};
  if (mdMode_ && !gitMode_ && !distractionFree_) {
    RectF area{sw, tabsH, width_, bottom};
    if (mdView_ == MdView::Read) {
      lay_.preview = area;
      lay_.editor = lay_.gutter = lay_.text = lay_.minimap = lay_.vscroll = RectF{};
    } else if (mdView_ == MdView::Split) {
      float mid = std::floor(area.l + area.w() / 2);
      lay_.preview = {mid, area.t, area.r, area.b};
      lay_.editor = {area.l, area.t, mid, area.b};
      lay_.vscroll = {mid - sb, area.t, mid, area.b};
      lay_.minimap = {};
      lay_.gutter = {area.l, area.t, area.l + gw, area.b};
      lay_.text = {area.l + gw, area.t, mid - sb, area.b};
    } else {
      lay_.minimap = {};  // the writing view uses the full width for text
      lay_.text.r = lay_.vscroll.l;
    }
  }
  lay_.pdf = {};
  if (d && d->pdf && !gitMode_) {
    lay_.pdf = {sw, tabsH, width_, bottom};
    lay_.preview = lay_.gutter = lay_.text = lay_.minimap = lay_.vscroll = RectF{};
  }
  // Terminal mode: the terminal takes the tabs' and editor's space; the side bar stays.
  lay_.terminal = {};
  if (terminalMode_ && !gitMode_) {
    lay_.terminal = {sw, 0, width_, bottom};
    lay_.tabs = lay_.editor = lay_.preview = lay_.pdf = lay_.gutter = lay_.text = lay_.minimap = lay_.vscroll = RectF{};
    layoutTerminal();
  }
}

void App::onTimer(UINT_PTR id) {
  if (id == kTimerBlink) {
    if (settings_.caretBlink && GetFocus() == hwnd_) {
      caretVisible_ = !caretVisible_;
      invalidate();
    }
    if (!status_.empty() && GetTickCount64() > statusUntil_) {
      status_.clear();
      invalidate();
    }
  } else if (id == kTimerAutoscroll) {
    if (drag_ != Drag::Text && drag_ != Drag::Column && drag_ != Drag::Gutter) {
      KillTimer(hwnd_, kTimerAutoscroll);
      return;
    }
    float dy = 0, dx = 0;
    if (mouseY_ < lay_.text.t) dy = mouseY_ - lay_.text.t;
    if (mouseY_ > lay_.text.b) dy = mouseY_ - lay_.text.b;
    if (mouseX_ < lay_.text.l && drag_ != Drag::Gutter) dx = mouseX_ - lay_.text.l;
    if (mouseX_ > lay_.text.r) dx = mouseX_ - lay_.text.r;
    if (dx == 0 && dy == 0) {
      KillTimer(hwnd_, kTimerAutoscroll);
      return;
    }
    scrollBy(dy * 0.6f, dx * 0.6f);
    updateDrag(mouseX_, mouseY_);
  } else if (id == kTimerSession) {
    if (sessionDirty_) saveSession();
  }
}

void App::resetBlink() {
  caretVisible_ = true;
  SetTimer(hwnd_, kTimerBlink, 530, nullptr);
  invalidate();
}

void App::setStatus(std::wstring msg, int ms) {
  status_ = std::move(msg);
  statusUntil_ = GetTickCount64() + static_cast<ULONGLONG>(ms);
  invalidate();
}

#ifdef _WIN32
void App::setFullscreen(bool on) {
  if (on == fullscreen_) return;
  fullscreen_ = on;
  if (on) {
    GetWindowPlacement(hwnd_, &savedPlacement_);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
    SetMenu(hwnd_, nullptr);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
    SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
  } else {
    SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN);
    SetMenu(hwnd_, gitMode_ && merge_ ? merge_->menu() : menu_);
    SetWindowPlacement(hwnd_, &savedPlacement_);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }
  computeLayout();
  invalidate();
}
#endif

void App::toggleFullscreen() {
  if (distractionFree_) {
    distractionFree_ = false;
    setFullscreen(false);
    return;
  }
  setFullscreen(!fullscreen_);
}

void App::toggleDistractionFree() {
  distractionFree_ = !distractionFree_;
  setFullscreen(distractionFree_);
  computeLayout();
  revealCaret();
  invalidate();
}

void App::onActivateApp(bool active) {
  if (active) {
    if (gitMode_ && merge_) merge_->refresh();
    checkFilesOnDisk();
    gitRefresh();
    if (!tree_.empty()) {
      tree_.refresh();
      if (indexStarted_) rebuildIndex();
    }
    invalidate();
  } else if (settings_.saveOnFocusLost) {
    for (auto& d : docs_)
      if (d->ed.isDirty() && !d->path.empty()) saveDocument(*d, false);
  }
}

void App::openPaths(const std::vector<std::wstring>& paths) {
  for (const auto& p : paths) {
    DWORD attrs = GetFileAttributesW(p.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
      openFolder(p, false);
    else
      openFile(p);
  }
}

void App::requestClose() {
  if (merge_ && merge_->busy() &&
      MessageBoxW(hwnd_, L"A git operation is still running. Quit anyway?", L"Verso",
                  MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
    return;
  if (!settings_.hotExit || !ownsSession_) {
    for (size_t i = 0; i < docs_.size(); ++i) {
      Document& d = *docs_[i];
      if (!d.ed.isDirty()) continue;
      activate(i);
      std::wstring q = L"Save changes to " + d.title() + L" before closing?";
      int r = MessageBoxW(hwnd_, q.c_str(), L"Verso", MB_YESNOCANCEL | MB_ICONWARNING);
      if (r == IDCANCEL) return;
      if (r == IDYES && !saveDocument(d, false)) return;
    }
  }
  saveSession();
#ifdef _WIN32
  SetMenu(hwnd_, nullptr);  // menus are owned by the editor and the git view, not the window
  DestroyWindow(hwnd_);
#else
  window_->quit();
#endif
}

#ifdef _WIN32
void App::applyDarkTitleBar() {
  BOOL dark = theme_.dark ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
  setDarkPopupMenus(theme_.dark);
  if (menu_) DrawMenuBar(hwnd_);
  SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void App::updateImePosition() {
  Document* d = doc();
  if (!d || focus_ != Focus::Editor) return;
  size_t caret = d->ed.selections().primary().caret;
  ensureWrap(*d);
  size_t r = d->wrap.rowOf(caret);
  VisualRow row = d->wrap.row(r);
  float x = textLeft(*d) +
            (static_cast<float>(d->ed.columnAt(caret)) - static_cast<float>(row.startCol) +
             static_cast<float>(row.indent)) * gfx_.charWidth() - d->scrollX;
  float y = lay_.text.t + static_cast<float>(r) * gfx_.lineHeight() - d->scrollY;
  float scale = gfx_.dpi() / 96.0f;
  if (HIMC imc = ImmGetContext(hwnd_)) {
    COMPOSITIONFORM cf{};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos = {static_cast<LONG>(x * scale), static_cast<LONG>(y * scale)};
    ImmSetCompositionWindow(imc, &cf);
    LOGFONTW lf{};
    lf.lfHeight = -static_cast<LONG>(gfx_.lineHeight() * scale * 0.8f);
    wcsncpy_s(lf.lfFaceName, settings_.fontFace.c_str(), _TRUNCATE);
    ImmSetCompositionFontW(imc, &lf);
    ImmReleaseContext(hwnd_, imc);
  }
}
#endif  // _WIN32

}  // namespace verso
