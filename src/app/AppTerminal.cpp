#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "app/App.h"
#include "app/Clipboard.h"
#include "core/TextUtil.h"
#include "verso_version.h"

namespace verso {

namespace {

constexpr float kTermPad = 6.0f;
constexpr size_t kScrollback = 10000;

// Campbell, the Windows Terminal default palette, for the 16 ANSI colors.
constexpr uint32_t kAnsi[16] = {0x0C0C0C, 0xC50F1F, 0x13A10E, 0xC19C00, 0x0037DA, 0x881798, 0x3A96DD, 0xCCCCCC,
                                0x767676, 0xE74856, 0x16C60C, 0xF9F1A5, 0x3B78FF, 0xB4009E, 0x61D6D6, 0xF2F2F2};

uint32_t paletteRgb(uint8_t i) {
  if (i < 16) return kAnsi[i];
  if (i < 232) {  // 6x6x6 color cube
    static const uint8_t level[6] = {0, 95, 135, 175, 215, 255};
    const int n = i - 16;
    return (static_cast<uint32_t>(level[n / 36]) << 16) | (static_cast<uint32_t>(level[(n / 6) % 6]) << 8) |
           level[n % 6];
  }
  const uint32_t g = 8 + 10 * static_cast<uint32_t>(i - 232);  // grayscale ramp
  return (g << 16) | (g << 8) | g;
}

void appendUtf16(std::wstring& out, char32_t c) {
  if (c >= 0x10000) {
    c -= 0x10000;
    out += static_cast<wchar_t>(0xD800 + (c >> 10));
    out += static_cast<wchar_t>(0xDC00 + (c & 0x3FF));
  } else {
    out += static_cast<wchar_t>(c);
  }
}

std::wstring exeDirectory() {
  wchar_t path[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  std::wstring p(path, n);
  return p.substr(0, p.find_last_of(L'\\'));
}

}  // namespace

// --- Mode ---------------------------------------------------------------------------

RectF App::terminalToggleRect() const {
  const float l = gitMode_ ? lay_.status.l + 8 : sidebarToggleRect().r + 4;
  return {l, lay_.status.t + 2, l + 28, lay_.status.b - 2};
}

void App::setTerminalMode(bool on) {
  if (on) {
    if (gitMode_) setGitMode(false);
    if (findMode_ != FindMode::Hidden) hideFind();
    terminalMode_ = true;
    selectTerminal();
  } else {
    terminalMode_ = false;
  }
  focus_ = Focus::Editor;
  computeLayout();
  updateTitle();
  resetBlink();
  sessionDirty_ = true;
  invalidate();
}

std::wstring App::terminalShell() const {
  if (!settings_.terminalShell.empty()) return settings_.terminalShell;
  wchar_t found[MAX_PATH];
  if (SearchPathW(nullptr, L"pwsh.exe", nullptr, MAX_PATH, found, nullptr)) return L"\"" + std::wstring(found) + L"\" -NoLogo";
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  return L"\"" + std::wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoLogo";
}

// Where the work is: the first project folder, else the active file's folder, else home.
std::wstring App::terminalFolder() const {
  if (!tree_.folders().empty()) return tree_.folders().front();
  if (!docs_.empty() && !docs_[active_]->path.empty()) return parentOf(docs_[active_]->path);
  wchar_t home[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
  return n > 0 && n < MAX_PATH ? std::wstring(home, n) : std::wstring();
}

void App::selectTerminal() {
  const std::wstring folder = terminalFolder();
  TerminalView* match = nullptr;
  for (auto& t : terms_)
    if (t->started && samePath(t->folder, folder)) match = t.get();
  // Shells that exited (or never started) in other folders have nothing to return to.
  std::erase_if(terms_, [&](const auto& t) { return t.get() != match && (!t->started || t->exited); });
  if (!match) {
    terms_.push_back(std::make_unique<TerminalView>());
    match = terms_.back().get();
    match->folder = folder;
  }
  term_ = match;
  startTerminal();
  computeLayout();
  updateTitle();
  invalidate();
}

bool App::startTerminal() {
  if (term_->started && !term_->exited) return true;
  computeLayout();
  int cols = 120, rows = 30;
  if (!lay_.terminal.empty()) {
    cols = std::max(20, static_cast<int>((lay_.terminal.w() - 2 * kTermPad - 10) / gfx_.charWidth()));
    rows = std::max(5, static_cast<int>((lay_.terminal.h() - 2 * kTermPad) / gfx_.lineHeight()));
  }
  const std::wstring cwd = term_->folder;
  std::vector<std::pair<std::wstring, std::wstring>> env = {
      {L"TERM_PROGRAM", L"Verso"},
      {L"TERM_PROGRAM_VERSION", VERSO_VERSION_WSTRING},
      {L"COLORTERM", L"truecolor"},
      {L"VERSO_MCP_BRIDGE", exeDirectory() + L"\\verso-mcp.exe"},
      // Verso sets these for its own background git; an interactive shell needs git's defaults.
      {L"GIT_TERMINAL_PROMPT", L""},
      {L"GIT_OPTIONAL_LOCKS", L""},
  };
  // AI tools started here reach this Verso through its MCP pipe.
  if (mcp_.running()) env.push_back({L"VERSO_MCP_PIPE", mcp_.pipeName()});
  term_->screen = VtScreen(cols, rows, kScrollback);
  term_->scrolledBack = 0;
  term_->seenScrolled = 0;
  term_->hasSelection = term_->selecting = false;
  term_->shell = terminalShell();
  std::wstring error;
  term_->started = true;
  term_->exited = !term_->pty.start(term_->shell, cwd, env, cols, rows, hwnd_, WM_APP_TERMINAL, &error);
  if (term_->exited) {
    term_->screen.write("\x1b[31m" + wideToUtf8(error) + "\x1b[0m\r\n");
    setStatus(error, 6000);
    return false;
  }
  return true;
}

void App::onTerminalMessage(WPARAM kind) {
  // Every folder's shell posts here; background shells keep their screens current too.
  for (auto& tp : terms_) {
    TerminalView& t = *tp;
    if (!t.started || t.exited) continue;
    std::string out = t.pty.takeOutput();
    if (!out.empty()) t.screen.write(out);
    if (std::string replies = t.screen.takeReplies(); !replies.empty()) t.pty.write(replies);
    // Keep a scrolled-back view on the same lines while new output arrives.
    const uint64_t scrolled = t.screen.linesScrolled();
    if (t.scrolledBack > 0)
      t.scrolledBack = std::min<size_t>(t.scrolledBack + static_cast<size_t>(scrolled - t.seenScrolled),
                                        t.screen.scrollbackSize());
    t.seenScrolled = scrolled;
    if (kind == 1 && !t.pty.running()) {
      t.exited = true;
      t.exitCode = t.pty.exitCode();
      t.screen.write("\r\n\x1b[2m[The shell exited with code " + std::to_string(t.exitCode) +
                     ". Press Enter to start a new one.]\x1b[0m\r\n");
      t.pty.stop();
    }
  }
  if (terminalMode_) {
    updateTitle();
    invalidate();
  }
}

void App::layoutTerminal() {
  if (lay_.terminal.empty() || !term_->started) return;
  const int cols = std::max(20, static_cast<int>((lay_.terminal.w() - 2 * kTermPad - 10) / gfx_.charWidth()));
  const int rows = std::max(5, static_cast<int>((lay_.terminal.h() - 2 * kTermPad) / gfx_.lineHeight()));
  if (cols == term_->screen.cols() && rows == term_->screen.rows()) return;
  term_->screen.resize(cols, rows);
  term_->pty.resize(cols, rows);
  term_->scrolledBack = std::min(term_->scrolledBack, term_->screen.scrollbackSize());
}

std::wstring App::terminalStatusText() {
  if (term_->exited) return L"The shell exited (code " + std::to_wstring(term_->exitCode) + L"). Press Enter for a new one.";
  std::wstring t = L"Terminal";
  if (const size_t slash = term_->folder.find_last_of(L"\\/"); !term_->folder.empty())
    t += L" in " + term_->folder.substr(slash == std::wstring::npos ? 0 : slash + 1);
  if (!term_->screen.title().empty()) t += L" — " + term_->screen.title();
  if (const auto others = std::count_if(terms_.begin(), terms_.end(),
                                        [&](const auto& o) { return o.get() != term_ && o->started && !o->exited; }))
    t += L" — " + std::to_wstring(others) + (others == 1 ? L" other folder's shell" : L" other folders' shells") +
         L" running";
  if (term_->scrolledBack > 0) t += L" — " + std::to_wstring(term_->scrolledBack) + L" lines up (Shift+End returns)";
  if (int n = mcp_.clientCount(); n > 0)
    t += L" — " + std::to_wstring(n) + (n == 1 ? L" AI tool connected" : L" AI tools connected");
  return t;
}

// --- Drawing ----------------------------------------------------------------------------

void App::paintTerminal() {
  const RectF a = lay_.terminal;
  gfx_.fill(a, theme_.bg);
  if (a.empty()) return;
  const VtScreen& s = term_->screen;
  const float cw = gfx_.charWidth(), lh = gfx_.lineHeight(), ty = gfx_.textOffsetY();
  const float x0 = a.l + kTermPad, y0 = a.t + kTermPad;
  const size_t back = std::min(term_->scrolledBack, s.scrollbackSize());
  const size_t first = s.lineCount() - static_cast<size_t>(s.rows()) - back;
  const Color defFg = theme_.token(TokenKind::Text), defBg = theme_.bg;
  auto color = [&](const VtColor& c, bool fg, bool bold) -> Color {
    if (c.kind == VtColor::Rgb) return Color::hex(c.rgb);
    if (c.kind == VtColor::Palette) return Color::hex(paletteRgb(fg && bold && c.index < 8 ? c.index + 8 : c.index));
    return fg ? defFg : defBg;
  };
  // Selection bounds in (line, column) order.
  size_t sl0 = term_->selAnchorLine, sl1 = term_->selLine;
  int sc0 = term_->selAnchorCol, sc1 = term_->selCol;
  if (sl1 < sl0 || (sl1 == sl0 && sc1 < sc0)) {
    std::swap(sl0, sl1);
    std::swap(sc0, sc1);
  }
  auto selected = [&](size_t line, int col) {
    if (!term_->hasSelection) return false;
    if (line < sl0 || line > sl1) return false;
    if (line == sl0 && col < sc0) return false;
    if (line == sl1 && col >= sc1) return false;
    return true;
  };

  gfx_.pushClip(a);
  for (int r = 0; r < s.rows(); ++r) {
    const size_t li = first + static_cast<size_t>(r);
    const VtLine& ln = s.line(li);
    const float y = y0 + static_cast<float>(r) * lh;
    const int n = static_cast<int>(ln.cells.size());
    // Backgrounds (and the selection), in runs.
    for (int x = 0; x < n;) {
      const VtCell& c = ln.cells[static_cast<size_t>(x)];
      const bool inv = (c.attrs & VtInverse) != 0, sel = selected(li, x);
      int e = x + 1;
      while (e < n && ln.cells[static_cast<size_t>(e)].sameStyle(c) && selected(li, e) == sel) ++e;
      if (sel) {
        gfx_.fill({x0 + x * cw, y, x0 + e * cw, y + lh}, theme_.selection);
      } else if (inv || c.bg.kind != VtColor::Default) {
        gfx_.fill({x0 + x * cw, y, x0 + e * cw, y + lh}, inv ? color(c.fg, true, c.attrs & VtBold) : color(c.bg, false, false));
      }
      x = e;
    }
    // Text, in runs of one style; wide characters are drawn on their own so the grid holds.
    for (int x = 0; x < n;) {
      const VtCell& c = ln.cells[static_cast<size_t>(x)];
      if (c.width == 0) {
        ++x;
        continue;
      }
      std::wstring run;
      int e = x;
      while (e < n) {
        const VtCell& d = ln.cells[static_cast<size_t>(e)];
        if (!d.sameStyle(c) || (e > x && d.width != 1) || d.width == 0) break;
        appendUtf16(run, d.ch ? d.ch : U' ');
        e += d.width == 2 ? 2 : 1;
        if (d.width == 2) break;
      }
      if (e == x) e = x + 1;
      const bool blank = std::all_of(run.begin(), run.end(), [](wchar_t ch) { return ch == L' '; });
      if (!blank && !(c.attrs & VtHidden)) {
        const bool inv = (c.attrs & VtInverse) != 0;
        Color fg = inv ? color(c.bg, false, false) : color(c.fg, true, c.attrs & VtBold);
        if (c.attrs & VtDim) fg = fg.withAlpha(0.6f);
        IDWriteTextFormat* fmt = gfx_.editorFormat((c.attrs & VtBold)     ? FontStyle::Bold
                                                   : (c.attrs & VtItalic) ? FontStyle::Italic
                                                                          : FontStyle::Regular);
        gfx_.text(run, x0 + x * cw, y + ty, fg, fmt);
        if (c.attrs & VtUnderline) gfx_.line(x0 + x * cw, y + lh - 2, x0 + e * cw, y + lh - 2, fg, 1);
        if (c.attrs & VtStrike) gfx_.line(x0 + x * cw, y + lh / 2, x0 + e * cw, y + lh / 2, fg, 1);
      }
      x = e;
    }
  }
  // Cursor: on the live screen only.
  if (back == 0 && s.cursorVisible() && !term_->exited) {
    const float cx = x0 + s.cursorX() * cw, cy = y0 + s.cursorY() * lh;
    const int shape = s.cursorShape();
    const bool focused = GetFocus() == hwnd_ && overlay_ == Overlay::None;
    if (!focused) {
      gfx_.stroke({cx + 0.5f, cy + 0.5f, cx + cw - 0.5f, cy + lh - 0.5f}, theme_.caret, 1);
    } else if (caretVisible_) {
      if (shape == 5 || shape == 6)
        gfx_.fill({cx, cy, cx + 2, cy + lh}, theme_.caret);
      else if (shape == 3 || shape == 4)
        gfx_.fill({cx, cy + lh - 2, cx + cw, cy + lh}, theme_.caret);
      else
        gfx_.fill({cx, cy, cx + cw, cy + lh}, theme_.caret.withAlpha(0.7f));
    }
  }
  // Scrollback position.
  if (s.scrollbackSize() > 0 && back > 0) {
    const float total = static_cast<float>(s.lineCount()), h = a.h();
    const float th = std::max(24.0f, h * static_cast<float>(s.rows()) / total);
    const float t = a.t + (h - th) * static_cast<float>(first) / std::max(1.0f, total - static_cast<float>(s.rows()));
    gfx_.fillRounded({a.r - 9, t, a.r - 3, t + th}, 3, theme_.scrollThumb);
  }
  gfx_.popClip();
}

// --- Input -------------------------------------------------------------------------------

void App::terminalSend(std::string_view bytes) {
  if (bytes.empty() || term_->exited || !term_->started) return;
  term_->pty.write(bytes);
  term_->scrolledBack = 0;  // typing returns to the live screen
  term_->hasSelection = false;
  invalidate();
}

App::TermKey App::terminalKeyDown(UINT vk, bool ctrl, bool shift, bool alt) {
  // Verso's own shortcuts while the terminal has the keyboard.
  if (ctrl && !alt && vk == VK_OEM_3) {  // Ctrl+`
    setTerminalMode(false);
    return TermKey::Consumed;
  }
  if (ctrl && shift && !alt && (vk == 'P' || vk == 'G')) return TermKey::App;  // palette, git mode
  if (vk == VK_F11) return TermKey::App;
  if ((ctrl && shift && !alt && vk == 'C') || (ctrl && !shift && !alt && vk == 'C' && term_->hasSelection)) {
    terminalCopy();
    suppressChar_ = true;  // Ctrl+C would also produce ^C
    return TermKey::Consumed;
  }
  if (ctrl && !alt && vk == 'V') {
    terminalPaste();
    suppressChar_ = true;
    return TermKey::Consumed;
  }
  if (shift && !ctrl && !alt && (vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END) &&
      !term_->screen.altScreen()) {
    const size_t page = static_cast<size_t>(std::max(1, term_->screen.rows() - 1));
    const size_t maxBack = term_->screen.scrollbackSize();
    if (vk == VK_PRIOR) term_->scrolledBack = std::min(maxBack, term_->scrolledBack + page);
    if (vk == VK_NEXT) term_->scrolledBack = term_->scrolledBack > page ? term_->scrolledBack - page : 0;
    if (vk == VK_HOME) term_->scrolledBack = maxBack;
    if (vk == VK_END) term_->scrolledBack = 0;
    invalidate();
    return TermKey::Consumed;
  }
  if (term_->exited) {
    if (vk == VK_RETURN) {
      startTerminal();
      suppressChar_ = true;
      invalidate();
    }
    return TermKey::Consumed;
  }
  // Keys without characters become escape sequences (xterm style, with modifiers).
  const int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
  auto cursorKey = [&](char final) {
    std::string seq = mod == 1 ? std::string(term_->screen.appCursorKeys() ? "\x1bO" : "\x1b[") + final
                               : "\x1b[1;" + std::to_string(mod) + final;
    terminalSend(seq);
    return TermKey::Consumed;
  };
  auto tildeKey = [&](int code) {
    terminalSend(mod == 1 ? "\x1b[" + std::to_string(code) + "~"
                          : "\x1b[" + std::to_string(code) + ";" + std::to_string(mod) + "~");
    return TermKey::Consumed;
  };
  switch (vk) {
    case VK_UP: return cursorKey('A');
    case VK_DOWN: return cursorKey('B');
    case VK_RIGHT: return cursorKey('C');
    case VK_LEFT: return cursorKey('D');
    case VK_HOME: return cursorKey('H');
    case VK_END: return cursorKey('F');
    case VK_INSERT: return tildeKey(2);
    case VK_DELETE: return tildeKey(3);
    case VK_PRIOR: return tildeKey(5);
    case VK_NEXT: return tildeKey(6);
    case VK_F1:
    case VK_F2:
    case VK_F3:
    case VK_F4: {
      const char final = static_cast<char>('P' + (vk - VK_F1));
      terminalSend(mod == 1 ? std::string("\x1bO") + final : "\x1b[1;" + std::to_string(mod) + final);
      return TermKey::Consumed;
    }
    case VK_F5: return tildeKey(15);
    case VK_F6: return tildeKey(17);
    case VK_F7: return tildeKey(18);
    case VK_F8: return tildeKey(19);
    case VK_F9: return tildeKey(20);
    case VK_F10: return tildeKey(21);
    case VK_F12: return tildeKey(24);
    case VK_TAB:
      if (shift && !ctrl && !alt) {
        terminalSend("\x1b[Z");
        suppressChar_ = true;
        return TermKey::Consumed;
      }
      break;
    case VK_SPACE:
      if (ctrl && !alt) {
        terminalSend(std::string(1, '\0'));  // Ctrl+Space
        suppressChar_ = true;
        return TermKey::Consumed;
      }
      break;
    default:
      break;
  }
  return TermKey::Char;  // the character arrives as WM_CHAR (including Ctrl+letters)
}

void App::terminalChar(wchar_t c, bool alt) {
  if (term_->exited) return;
  std::wstring w;
  if (c >= 0xD800 && c <= 0xDBFF) {
    term_->pendingHigh = c;
    return;
  }
  if (c >= 0xDC00 && c <= 0xDFFF) {
    if (!term_->pendingHigh) return;
    w = {term_->pendingHigh, c};
    term_->pendingHigh = 0;
  } else {
    // Backspace sends DEL and Ctrl+Backspace sends BS, as terminals expect.
    w = c == 0x08 ? L"\x7f" : (c == 0x7F ? L"\x08" : std::wstring(1, c));
  }
  std::string bytes = wideToUtf8(w);
  terminalSend(alt ? "\x1b" + bytes : bytes);
}

void App::terminalPaste() {
  std::wstring text = getClipboardText(hwnd_);
  if (text.empty() || term_->exited) return;
  std::wstring norm;  // terminals expect Enter (CR) between pasted lines
  norm.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == L'\r') {
      norm += L'\r';
      if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
    } else if (text[i] == L'\n') {
      norm += L'\r';
    } else {
      norm += text[i];
    }
  }
  std::string bytes = wideToUtf8(norm);
  if (term_->screen.bracketedPaste()) bytes = "\x1b[200~" + bytes + "\x1b[201~";
  terminalSend(bytes);
}

bool App::terminalCopy() {
  if (!term_->hasSelection) return false;
  size_t l0 = term_->selAnchorLine, l1 = term_->selLine;
  int c0 = term_->selAnchorCol, c1 = term_->selCol;
  if (l1 < l0 || (l1 == l0 && c1 < c0)) {
    std::swap(l0, l1);
    std::swap(c0, c1);
  }
  std::wstring text = term_->screen.text(l0, c0, l1, c1);
  if (text.empty()) return false;
  setClipboardText(hwnd_, text);
  term_->hasSelection = false;
  setStatus(L"Copied " + std::to_wstring(text.size()) + L" characters", 1500);
  invalidate();
  return true;
}

bool App::terminalCell(float x, float y, size_t& line, int& col) {
  const RectF a = lay_.terminal;
  if (a.empty()) return false;
  const VtScreen& s = term_->screen;
  const float cw = gfx_.charWidth(), lh = gfx_.lineHeight();
  const int row = std::clamp(static_cast<int>(std::floor((y - a.t - kTermPad) / lh)), 0, s.rows() - 1);
  col = std::clamp(static_cast<int>(std::lround((x - a.l - kTermPad) / cw)), 0, s.cols());
  const size_t back = std::min(term_->scrolledBack, s.scrollbackSize());
  line = s.lineCount() - static_cast<size_t>(s.rows()) - back + static_cast<size_t>(row);
  return true;
}

void App::terminalMouseDown(int button, float x, float y) {
  size_t line = 0;
  int col = 0;
  if (!terminalCell(x, y, line, col)) return;
  if (button == 1) {  // right click: copy a selection, otherwise paste
    if (!terminalCopy()) terminalPaste();
    return;
  }
  if (button == 2) {
    terminalPaste();
    return;
  }
  const DWORD now = GetMessageTime();
  const bool doubleClick = now - lastClickTime_ <= GetDoubleClickTime() && std::fabs(x - lastClickX_) < 5 &&
                           std::fabs(y - lastClickY_) < 5;
  lastClickTime_ = now;
  lastClickX_ = x;
  lastClickY_ = y;
  if (doubleClick) {  // select the word (non-blank run) under the pointer
    const VtLine& ln = term_->screen.line(line);
    auto blank = [&](int c) {
      return c < 0 || c >= static_cast<int>(ln.cells.size()) || ln.cells[static_cast<size_t>(c)].ch == U' ' ||
             ln.cells[static_cast<size_t>(c)].ch == 0;
    };
    int c = std::min(col, static_cast<int>(ln.cells.size()) - 1);
    if (!blank(c)) {
      int b = c, e = c;
      while (!blank(b - 1)) --b;
      while (!blank(e + 1)) ++e;
      term_->selAnchorLine = term_->selLine = line;
      term_->selAnchorCol = b;
      term_->selCol = e + 1;
      term_->hasSelection = true;
      invalidate();
      return;
    }
  }
  term_->selAnchorLine = term_->selLine = line;
  term_->selAnchorCol = term_->selCol = col;
  term_->hasSelection = false;
  term_->selecting = true;
  drag_ = Drag::Terminal;
  SetCapture(hwnd_);
  invalidate();
}

void App::terminalDrag(float x, float y) {
  size_t line = 0;
  int col = 0;
  if (!term_->selecting || !terminalCell(x, y, line, col)) return;
  term_->selLine = line;
  term_->selCol = col;
  term_->hasSelection = line != term_->selAnchorLine || col != term_->selAnchorCol;
  invalidate();
}

void App::terminalWheel(float notches) {
  const VtScreen& s = term_->screen;
  const int lines = static_cast<int>(std::lround(notches * 3));
  if (lines == 0) return;
  if (s.altScreen()) {  // full-screen programs scroll by arrow keys
    const std::string key = std::string(s.appCursorKeys() ? "\x1bO" : "\x1b[") + (lines > 0 ? "A" : "B");
    std::string seq;
    for (int i = 0; i < std::abs(lines); ++i) seq += key;
    term_->pty.write(seq);
    return;
  }
  const long long back = static_cast<long long>(term_->scrolledBack) + lines;
  term_->scrolledBack = static_cast<size_t>(std::clamp<long long>(back, 0, static_cast<long long>(s.scrollbackSize())));
  invalidate();
}

// --- Connecting AI tools ------------------------------------------------------------------

void App::connectAiTool() {
  const std::wstring bridge = exeDirectory() + L"\\verso-mcp.exe";
  if (GetFileAttributesW(bridge.c_str()) == INVALID_FILE_ATTRIBUTES) {
    setStatus(L"verso-mcp.exe was not found next to Verso.exe.", 5000);
    return;
  }
  if (!mcp_.running()) {
    setStatus(L"The MCP server is off (\"mcp_server\": false in Preferences).", 5000);
    return;
  }
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, 1, L"Claude Code");
  AppendMenuW(m, MF_STRING, 2, L"Codex CLI");
  AppendMenuW(m, MF_STRING, 3, L"Gemini CLI");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, 4, L"Copy MCP Configuration (JSON)");
  RectF t = terminalToggleRect();
  UINT choice = trackMenu(m, toScreen(t.l, t.t));
  DestroyMenu(m);
  if (choice == 0) return;
  if (choice == 4) {
    std::wstring escaped;
    for (wchar_t c : bridge) escaped += c == L'\\' ? std::wstring(L"\\\\") : std::wstring(1, c);
    setClipboardText(hwnd_, L"{\n  \"mcpServers\": {\n    \"verso\": {\n      \"command\": \"" + escaped +
                                L"\",\n      \"args\": []\n    }\n  }\n}\n");
    setStatus(L"Copied the MCP server configuration for Verso.", 3000);
    return;
  }
  // Typed at the prompt for the user to review; nothing runs until they press Enter.
  // ('--' is quoted so PowerShell passes it to the program.)
  const std::wstring quoted = L"\"" + bridge + L"\"";
  const std::wstring command = choice == 1   ? L"claude mcp add --scope user verso " + quoted
                               : choice == 2 ? L"codex mcp add verso '--' " + quoted
                                             : L"gemini mcp add --scope user verso " + quoted;
  setTerminalMode(true);
  if (term_->exited) return;
  terminalSend(wideToUtf8(command));
  setStatus(L"Press Enter in the terminal to register Verso with the AI tool (once per tool).", 6000);
}

}  // namespace verso
