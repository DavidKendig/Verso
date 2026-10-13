#include "core/VtScreen.h"

#include <algorithm>

#include "core/TextUtil.h"

namespace verso {

int vtCharWidth(char32_t c) {
  if (c == 0x200B || c == 0x200C || c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF))
    return 0;  // zero-width space/joiners and variation selectors
  if ((c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) ||
      (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F))
    return 0;  // combining marks
  if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) ||
      (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
      (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
      (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
      (c >= 0x1F680 && c <= 0x1F6FF) || (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x1FA70 && c <= 0x1FAFF) ||
      (c >= 0x20000 && c <= 0x3FFFD))
    return 2;
  return 1;
}

namespace {

void appendUtf16(std::wstring& out, char32_t c) {
  if (c >= 0x10000) {
    c -= 0x10000;
    out += static_cast<wchar_t>(0xD800 + (c >> 10));
    out += static_cast<wchar_t>(0xDC00 + (c & 0x3FF));
  } else {
    out += static_cast<wchar_t>(c);
  }
}

void appendUtf8(std::string& out, char32_t c) {
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else if (c < 0x800) {
    out += static_cast<char>(0xC0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += static_cast<char>(0xE0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (c >> 18));
    out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
}

constexpr int kMaxParam = 100000;

}  // namespace

VtScreen::VtScreen(int cols, int rows, size_t scrollbackLimit)
    : cols_(std::max(1, cols)), rows_(std::max(1, rows)), scrollbackLimit_(scrollbackLimit) {
  lines_.assign(static_cast<size_t>(rows_), blankLine());
  scrollBottom_ = rows_ - 1;
  tabStops_.assign(static_cast<size_t>(cols_), false);
  for (int i = 0; i < cols_; i += 8) tabStops_[static_cast<size_t>(i)] = true;
}

VtCell VtScreen::blankCell() const {
  VtCell c;
  c.bg = pen_.bg;  // erased cells take the current background (as xterm does)
  return c;
}

VtLine VtScreen::blankLine() const {
  VtLine l;
  l.cells.assign(static_cast<size_t>(cols_), blankCell());
  return l;
}

const VtLine& VtScreen::line(size_t index) const {
  const size_t sb = scrollbackSize();
  if (index < sb) return scrollback_[index];
  const auto& s = screen();
  return s[std::min(index - sb, s.size() - 1)];
}

std::string VtScreen::takeReplies() {
  std::string r;
  r.swap(replies_);
  return r;
}

int VtScreen::param(size_t i, int def) const {
  if (i >= params_.size() || params_[i] < 0) return def;
  return params_[i];
}

// --- Input ------------------------------------------------------------------------

void VtScreen::write(std::string_view data) {
  if (data.empty()) return;
  ++revision_;
  std::string joined;
  if (!utf8_.empty()) {
    joined = utf8_ + std::string(data);
    data = joined;
    utf8_.clear();
  }
  const size_t n = data.size();
  size_t i = 0;
  while (i < n) {
    const auto b = static_cast<unsigned char>(data[i]);
    char32_t cp;
    size_t len;
    if (b < 0x80) {
      cp = b;
      len = 1;
    } else if ((b & 0xE0) == 0xC0) {
      cp = b & 0x1F;
      len = 2;
    } else if ((b & 0xF0) == 0xE0) {
      cp = b & 0x0F;
      len = 3;
    } else if ((b & 0xF8) == 0xF0) {
      cp = b & 0x07;
      len = 4;
    } else {
      feed(0xFFFD);
      ++i;
      continue;
    }
    if (i + len > n) {  // completed by the next write
      utf8_.assign(data.substr(i));
      break;
    }
    bool ok = true;
    for (size_t k = 1; k < len; ++k) {
      const auto cb = static_cast<unsigned char>(data[i + k]);
      if ((cb & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (cb & 0x3F);
    }
    if (!ok || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
      feed(0xFFFD);
      ++i;
      continue;
    }
    feed(cp);
    i += len;
  }
}

void VtScreen::feed(char32_t c) {
  switch (state_) {
    case State::Ground:
      if (c == 0x1B) {
        state_ = State::Escape;
        intermediates_.clear();
      } else if (c < 0x20 || c == 0x7F) {
        control(c);
      } else if (c < 0x80 || c >= 0xA0) {
        print(c);
      }  // 8-bit C1 controls are not used by ConPTY and are ignored
      return;
    case State::Escape:
      if (c == '[') {
        state_ = State::Csi;
        params_.clear();
        paramStarted_ = false;
        prefix_ = 0;
        intermediates_.clear();
      } else if (c == ']') {
        state_ = State::Osc;
        oscText_.clear();
      } else if (c == 'P' || c == 'X' || c == '^' || c == '_') {
        state_ = State::String;  // DCS, SOS, PM, APC: consumed and ignored
      } else if (c >= 0x20 && c <= 0x2F) {
        intermediates_ += static_cast<char>(c);
        state_ = State::EscapeIntermediate;
      } else if (c < 0x20) {
        control(c);
      } else {
        state_ = State::Ground;
        escape(c);
      }
      return;
    case State::EscapeIntermediate:
      if (c >= 0x20 && c <= 0x2F) {
        intermediates_ += static_cast<char>(c);
      } else if (c < 0x20) {
        control(c);
      } else {
        state_ = State::Ground;
        if (intermediates_ == "#" && c == '8') {  // DECALN: fill the screen with E
          VtCell e;
          e.ch = U'E';
          for (auto& l : screen())
            for (auto& cell : l.cells) cell = e;
        }
        // Character set designations (ESC ( B, ...) need no action: output is UTF-8.
      }
      return;
    case State::Csi:
      if (c >= '0' && c <= '9') {
        if (!paramStarted_) {
          params_.push_back(0);
          paramStarted_ = true;
        }
        params_.back() = std::min(params_.back() * 10 + static_cast<int>(c - '0'), kMaxParam);
      } else if (c == ';' || c == ':') {
        if (!paramStarted_) params_.push_back(-1);
        paramStarted_ = false;
      } else if (c == '?' || c == '>' || c == '=' || c == '<') {
        prefix_ = static_cast<char>(c);
      } else if (c >= 0x20 && c <= 0x2F) {
        intermediates_ += static_cast<char>(c);
      } else if (c >= 0x40 && c <= 0x7E) {
        state_ = State::Ground;
        csi(c);
      } else if (c == 0x1B) {
        state_ = State::Escape;
        intermediates_.clear();
      } else if (c < 0x20) {
        control(c);
      } else {
        state_ = State::Ground;  // malformed sequence
      }
      if (params_.size() > 64) state_ = State::Ground;
      return;
    case State::Osc:
      if (c == 0x07) {
        state_ = State::Ground;
        osc();
      } else if (c == 0x1B) {
        state_ = State::OscEscape;
      } else if (oscText_.size() < 4096) {
        appendUtf8(oscText_, c);
      }
      return;
    case State::OscEscape:
      osc();
      if (c == '\\') {
        state_ = State::Ground;
      } else {  // ESC began a new sequence
        state_ = State::Escape;
        intermediates_.clear();
        feed(c);
      }
      return;
    case State::String:
      if (c == 0x1B)
        state_ = State::StringEscape;
      else if (c == 0x07)
        state_ = State::Ground;
      return;
    case State::StringEscape:
      state_ = c == '\\' ? State::Ground : State::String;
      return;
  }
}

// --- Characters and controls ---------------------------------------------------------

void VtScreen::print(char32_t c) {
  const int w = vtCharWidth(c);
  if (w == 0) return;  // combining marks and joiners are not drawn separately
  auto& s = screen();
  if (pendingWrap_) {
    pendingWrap_ = false;
    if (autowrap_) {
      s[static_cast<size_t>(cy_)].wrapped = true;
      cx_ = 0;
      lineFeed();
    }
  }
  if (w == 2 && cx_ == cols_ - 1) {  // a wide character does not fit in the last column
    if (autowrap_ && cols_ > 1) {
      s[static_cast<size_t>(cy_)].cells[static_cast<size_t>(cx_)] = blankCell();
      s[static_cast<size_t>(cy_)].wrapped = true;
      cx_ = 0;
      lineFeed();
    } else if (cols_ > 1) {
      cx_ = cols_ - 2;
    } else {
      return;
    }
  }
  auto& cells = s[static_cast<size_t>(cy_)].cells;
  if (insertMode_) {
    std::rotate(cells.begin() + cx_, cells.end() - w, cells.end());
    for (int k = 0; k < w; ++k) cells[static_cast<size_t>(cx_ + k)] = blankCell();
  }
  // Writing over half of a wide character clears its other half.
  auto clearPair = [&](int col) {
    if (col < 0 || col >= cols_) return;
    VtCell& cell = cells[static_cast<size_t>(col)];
    if (cell.width == 0 && col > 0) cells[static_cast<size_t>(col - 1)] = blankCell();
    if (cell.width == 2 && col + 1 < cols_) cells[static_cast<size_t>(col + 1)] = blankCell();
  };
  clearPair(cx_);
  if (w == 2) clearPair(cx_ + 1);
  VtCell cell = pen_;
  cell.ch = c;
  cell.width = static_cast<uint8_t>(w);
  cells[static_cast<size_t>(cx_)] = cell;
  if (w == 2) {
    VtCell cont = pen_;
    cont.ch = 0;
    cont.width = 0;
    cells[static_cast<size_t>(cx_ + 1)] = cont;
  }
  lastPrinted_ = c;
  cx_ += w;
  if (cx_ >= cols_) {
    cx_ = cols_ - 1;
    pendingWrap_ = true;
  }
}

void VtScreen::control(char32_t c) {
  switch (c) {
    case 0x08:  // BS
      if (cx_ > 0) --cx_;
      pendingWrap_ = false;
      break;
    case 0x09: {  // HT
      int x = cx_ + 1;
      while (x < cols_ - 1 && !tabStops_[static_cast<size_t>(x)]) ++x;
      cx_ = std::min(x, cols_ - 1);
      pendingWrap_ = false;
      break;
    }
    case 0x0A:
    case 0x0B:
    case 0x0C:  // LF, VT, FF
      lineFeed();
      if (newlineMode_) cx_ = 0;
      break;
    case 0x0D:  // CR
      cx_ = 0;
      pendingWrap_ = false;
      break;
    default:
      break;  // BEL and the rest
  }
}

void VtScreen::lineFeed() {
  pendingWrap_ = false;
  if (cy_ == scrollBottom_)
    scrollUp(scrollTop_, scrollBottom_, 1);
  else if (cy_ < rows_ - 1)
    ++cy_;
}

void VtScreen::scrollUp(int top, int bottom, int n) {
  auto& s = screen();
  n = std::min(n, bottom - top + 1);
  for (int i = 0; i < n; ++i) {
    // A full-screen scroll of the primary screen moves the top line into history.
    if (!alt_ && top == 0 && bottom == rows_ - 1) {
      scrollback_.push_back(std::move(s[0]));
      ++linesScrolled_;
      if (scrollback_.size() > scrollbackLimit_) scrollback_.pop_front();
    }
    s.erase(s.begin() + top);
    s.insert(s.begin() + bottom, blankLine());
  }
}

void VtScreen::scrollDown(int top, int bottom, int n) {
  auto& s = screen();
  n = std::min(n, bottom - top + 1);
  for (int i = 0; i < n; ++i) {
    s.erase(s.begin() + bottom);
    s.insert(s.begin() + top, blankLine());
  }
}

void VtScreen::eraseCells(int row, int from, int to) {
  if (row < 0 || row >= rows_) return;
  auto& cells = screen()[static_cast<size_t>(row)].cells;
  from = std::clamp(from, 0, cols_);
  to = std::clamp(to, 0, cols_);
  // Erasing half of a wide character erases all of it.
  if (from > 0 && from < cols_ && cells[static_cast<size_t>(from)].width == 0) --from;
  if (to < cols_ && to > 0 && cells[static_cast<size_t>(to)].width == 0) ++to;
  for (int x = from; x < to; ++x) cells[static_cast<size_t>(x)] = blankCell();
  if (to >= cols_) screen()[static_cast<size_t>(row)].wrapped = false;
}

void VtScreen::clampCursor() {
  cx_ = std::clamp(cx_, 0, cols_ - 1);
  cy_ = std::clamp(cy_, 0, rows_ - 1);
}

void VtScreen::escape(char32_t c) {
  switch (c) {
    case '7':
      saved_ = {cx_, cy_, pen_, originMode_};
      break;
    case '8':
      cx_ = saved_.x;
      cy_ = saved_.y;
      pen_ = saved_.pen;
      originMode_ = saved_.originMode;
      pendingWrap_ = false;
      clampCursor();
      break;
    case 'D':  // IND
      lineFeed();
      break;
    case 'E':  // NEL
      cx_ = 0;
      lineFeed();
      break;
    case 'M':  // RI
      pendingWrap_ = false;
      if (cy_ == scrollTop_)
        scrollDown(scrollTop_, scrollBottom_, 1);
      else if (cy_ > 0)
        --cy_;
      break;
    case 'H':  // HTS
      tabStops_[static_cast<size_t>(cx_)] = true;
      break;
    case 'c': {  // RIS
      const int cols = cols_, rows = rows_;
      const size_t limit = scrollbackLimit_;
      const uint64_t rev = revision_, scrolled = linesScrolled_;
      *this = VtScreen(cols, rows, limit);
      revision_ = rev;
      linesScrolled_ = scrolled;
      break;
    }
    default:
      break;  // keypad modes (= >), ST (\)
  }
}

// --- Control sequences -------------------------------------------------------------------

void VtScreen::csi(char32_t final) {
  const int n1 = std::max(1, param(0, 1));
  auto& s = screen();
  if (!intermediates_.empty()) {
    if (intermediates_ == " " && final == 'q') cursorShape_ = param(0, 0);  // DECSCUSR
    if (intermediates_ == "!" && final == 'p') softReset();                // DECSTR
    return;
  }
  if (prefix_ == '?' && final != 'h' && final != 'l') return;  // private queries (DECRQM etc.)
  if (prefix_ == '>' || prefix_ == '=') {
    if (final == 'c' && prefix_ == '>') replies_ += "\x1b[>0;10;1c";  // secondary device attributes
    return;
  }
  const int regionTop = originMode_ ? scrollTop_ : 0;
  switch (final) {
    case '@': {  // ICH
      auto& cells = s[static_cast<size_t>(cy_)].cells;
      const int n = std::min(n1, cols_ - cx_);
      std::rotate(cells.begin() + cx_, cells.end() - n, cells.end());
      for (int k = 0; k < n; ++k) cells[static_cast<size_t>(cx_ + k)] = blankCell();
      break;
    }
    case 'A': {  // CUU
      const int top = cy_ >= scrollTop_ ? scrollTop_ : 0;
      cy_ = std::max(top, cy_ - n1);
      break;
    }
    case 'B':
    case 'e': {  // CUD, VPR
      const int bottom = cy_ <= scrollBottom_ ? scrollBottom_ : rows_ - 1;
      cy_ = std::min(bottom, cy_ + n1);
      break;
    }
    case 'C':
    case 'a':  // CUF, HPR
      cx_ = std::min(cols_ - 1, cx_ + n1);
      break;
    case 'D':  // CUB
      cx_ = std::max(0, cx_ - n1);
      break;
    case 'E':  // CNL
      cy_ = std::min(rows_ - 1, cy_ + n1);
      cx_ = 0;
      break;
    case 'F':  // CPL
      cy_ = std::max(0, cy_ - n1);
      cx_ = 0;
      break;
    case 'G':
    case '`':  // CHA, HPA
      cx_ = std::clamp(param(0, 1) - 1, 0, cols_ - 1);
      break;
    case 'H':
    case 'f':  // CUP, HVP
      cy_ = std::clamp(regionTop + param(0, 1) - 1, 0, rows_ - 1);
      cx_ = std::clamp(param(1, 1) - 1, 0, cols_ - 1);
      break;
    case 'd':  // VPA
      cy_ = std::clamp(regionTop + param(0, 1) - 1, 0, rows_ - 1);
      break;
    case 'I':  // CHT
      for (int k = 0; k < n1; ++k) control(0x09);
      break;
    case 'Z':  // CBT
      for (int k = 0; k < n1 && cx_ > 0; ++k) {
        --cx_;
        while (cx_ > 0 && !tabStops_[static_cast<size_t>(cx_)]) --cx_;
      }
      break;
    case 'J': {  // ED
      const int mode = param(0, 0);
      if (mode == 0) {
        eraseCells(cy_, cx_, cols_);
        for (int y = cy_ + 1; y < rows_; ++y) eraseCells(y, 0, cols_);
      } else if (mode == 1) {
        for (int y = 0; y < cy_; ++y) eraseCells(y, 0, cols_);
        eraseCells(cy_, 0, cx_ + 1);
      } else {
        for (int y = 0; y < rows_; ++y) eraseCells(y, 0, cols_);
        if (mode == 3 && !alt_) scrollback_.clear();
      }
      break;
    }
    case 'K': {  // EL
      const int mode = param(0, 0);
      if (mode == 0)
        eraseCells(cy_, cx_, cols_);
      else if (mode == 1)
        eraseCells(cy_, 0, cx_ + 1);
      else
        eraseCells(cy_, 0, cols_);
      break;
    }
    case 'L':  // IL
      if (cy_ >= scrollTop_ && cy_ <= scrollBottom_) {
        scrollDown(cy_, scrollBottom_, n1);
        cx_ = 0;
      }
      break;
    case 'M':  // DL (deleted lines do not go to history)
      if (cy_ >= scrollTop_ && cy_ <= scrollBottom_) {
        const bool full = scrollTop_ == 0 && scrollBottom_ == rows_ - 1 && cy_ == 0;
        if (full) {
          for (int k = 0; k < std::min(n1, rows_); ++k) {
            s.erase(s.begin());
            s.insert(s.begin() + scrollBottom_, blankLine());
          }
        } else {
          scrollUp(cy_, scrollBottom_, n1);
        }
        cx_ = 0;
      }
      break;
    case 'P': {  // DCH
      auto& cells = s[static_cast<size_t>(cy_)].cells;
      const int n = std::min(n1, cols_ - cx_);
      std::rotate(cells.begin() + cx_, cells.begin() + cx_ + n, cells.end());
      for (int k = cols_ - n; k < cols_; ++k) cells[static_cast<size_t>(k)] = blankCell();
      break;
    }
    case 'S':  // SU
      for (int k = 0; k < n1; ++k) {
        s.erase(s.begin() + scrollTop_);
        s.insert(s.begin() + scrollBottom_, blankLine());
      }
      break;
    case 'T':  // SD
      scrollDown(scrollTop_, scrollBottom_, n1);
      break;
    case 'X':  // ECH
      eraseCells(cy_, cx_, cx_ + n1);
      break;
    case 'b':  // REP
      if (lastPrinted_)
        for (int k = 0; k < std::min(n1, cols_ * rows_); ++k) print(lastPrinted_);
      break;
    case 'c':  // DA
      replies_ += "\x1b[?1;2c";
      break;
    case 'g':  // TBC
      if (param(0, 0) == 0)
        tabStops_[static_cast<size_t>(cx_)] = false;
      else if (param(0, 0) == 3)
        std::fill(tabStops_.begin(), tabStops_.end(), false);
      break;
    case 'h':
      setMode(true);
      break;
    case 'l':
      setMode(false);
      break;
    case 'm':
      sgr();
      break;
    case 'n':  // DSR
      if (param(0, 0) == 5) {
        replies_ += "\x1b[0n";
      } else if (param(0, 0) == 6) {
        replies_ += "\x1b[" + std::to_string(cy_ - regionTop + 1) + ";" + std::to_string(cx_ + 1) + "R";
      }
      break;
    case 'r': {  // DECSTBM
      const int top = param(0, 1) - 1, bottom = param(1, rows_) - 1;
      if (top >= 0 && bottom < rows_ && top < bottom) {
        scrollTop_ = top;
        scrollBottom_ = bottom;
        cx_ = 0;
        cy_ = originMode_ ? scrollTop_ : 0;
      }
      break;
    }
    case 's':
      saved_ = {cx_, cy_, pen_, originMode_};
      break;
    case 'u':
      cx_ = saved_.x;
      cy_ = saved_.y;
      pen_ = saved_.pen;
      clampCursor();
      break;
    default:
      break;  // window manipulation (t) and others
  }
  pendingWrap_ = false;
}

void VtScreen::setMode(bool on) {
  for (size_t i = 0; i < std::max<size_t>(1, params_.size()); ++i) {
    const int m = param(i, 0);
    if (prefix_ == '?') {
      switch (m) {
        case 1: appCursor_ = on; break;
        case 6:
          originMode_ = on;
          cx_ = 0;
          cy_ = on ? scrollTop_ : 0;
          break;
        case 7: autowrap_ = on; break;
        case 25: cursorVisible_ = on; break;
        case 47:
        case 1047: setAltScreen(on, false); break;
        case 1049: setAltScreen(on, true); break;
        case 2004: bracketedPaste_ = on; break;
        default: break;  // mouse reporting, focus events, win32-input-mode: not used
      }
    } else if (m == 4) {
      insertMode_ = on;
    } else if (m == 20) {
      newlineMode_ = on;
    }
  }
}

void VtScreen::setAltScreen(bool on, bool saveCursor) {
  if (on == alt_) return;
  if (on) {
    if (saveCursor) savedPrimary_ = {cx_, cy_, pen_, originMode_};
    alt_ = true;
    altLines_.assign(static_cast<size_t>(rows_), blankLine());
  } else {
    alt_ = false;
    altLines_.clear();
    if (saveCursor) {
      cx_ = savedPrimary_.x;
      cy_ = savedPrimary_.y;
      pen_ = savedPrimary_.pen;
      originMode_ = savedPrimary_.originMode;
    }
  }
  scrollTop_ = 0;
  scrollBottom_ = rows_ - 1;
  pendingWrap_ = false;
  clampCursor();
}

void VtScreen::softReset() {
  pen_ = VtCell{};
  cursorVisible_ = true;
  autowrap_ = true;
  originMode_ = false;
  insertMode_ = false;
  appCursor_ = false;
  scrollTop_ = 0;
  scrollBottom_ = rows_ - 1;
  saved_ = {};
}

void VtScreen::sgr() {
  if (params_.empty()) {
    pen_.fg = pen_.bg = VtColor{};
    pen_.attrs = 0;
    return;
  }
  auto extended = [&](size_t& i, VtColor& out) {
    // 38;5;n or 38;2;r;g;b (also with ':' separators and an empty color-space id).
    const int kind = param(i + 1, -1);
    if (kind == 5 && i + 2 < params_.size()) {
      out = {VtColor::Palette, static_cast<uint8_t>(std::clamp(param(i + 2, 0), 0, 255)), 0};
      i += 2;
    } else if (kind == 2) {
      size_t first = i + 2;
      if (i + 5 < params_.size() && params_[first] < 0) ++first;  // 38:2::r:g:b
      if (first + 2 < params_.size()) {
        const auto r = static_cast<uint32_t>(std::clamp(param(first, 0), 0, 255));
        const auto g = static_cast<uint32_t>(std::clamp(param(first + 1, 0), 0, 255));
        const auto b = static_cast<uint32_t>(std::clamp(param(first + 2, 0), 0, 255));
        out = {VtColor::Rgb, 0, (r << 16) | (g << 8) | b};
      }
      i = first + 2;
    } else {
      i += 1;
    }
  };
  for (size_t i = 0; i < params_.size(); ++i) {
    const int p = param(i, 0);
    if (p == 0) {
      pen_.fg = pen_.bg = VtColor{};
      pen_.attrs = 0;
    } else if (p == 1) {
      pen_.attrs |= VtBold;
    } else if (p == 2) {
      pen_.attrs |= VtDim;
    } else if (p == 3) {
      pen_.attrs |= VtItalic;
    } else if (p == 4 || p == 21) {
      pen_.attrs |= VtUnderline;
    } else if (p == 7) {
      pen_.attrs |= VtInverse;
    } else if (p == 8) {
      pen_.attrs |= VtHidden;
    } else if (p == 9) {
      pen_.attrs |= VtStrike;
    } else if (p == 22) {
      pen_.attrs &= static_cast<uint8_t>(~(VtBold | VtDim));
    } else if (p == 23) {
      pen_.attrs &= static_cast<uint8_t>(~VtItalic);
    } else if (p == 24) {
      pen_.attrs &= static_cast<uint8_t>(~VtUnderline);
    } else if (p == 27) {
      pen_.attrs &= static_cast<uint8_t>(~VtInverse);
    } else if (p == 28) {
      pen_.attrs &= static_cast<uint8_t>(~VtHidden);
    } else if (p == 29) {
      pen_.attrs &= static_cast<uint8_t>(~VtStrike);
    } else if (p >= 30 && p <= 37) {
      pen_.fg = {VtColor::Palette, static_cast<uint8_t>(p - 30), 0};
    } else if (p == 38) {
      extended(i, pen_.fg);
    } else if (p == 39) {
      pen_.fg = VtColor{};
    } else if (p >= 40 && p <= 47) {
      pen_.bg = {VtColor::Palette, static_cast<uint8_t>(p - 40), 0};
    } else if (p == 48) {
      extended(i, pen_.bg);
    } else if (p == 49) {
      pen_.bg = VtColor{};
    } else if (p == 58) {
      VtColor ignored;  // underline color: parsed so its arguments are not misread
      extended(i, ignored);
    } else if (p >= 90 && p <= 97) {
      pen_.fg = {VtColor::Palette, static_cast<uint8_t>(p - 90 + 8), 0};
    } else if (p >= 100 && p <= 107) {
      pen_.bg = {VtColor::Palette, static_cast<uint8_t>(p - 100 + 8), 0};
    }
  }
}

void VtScreen::osc() {
  const size_t semi = oscText_.find(';');
  if (semi == std::string::npos) return;
  const std::string code = oscText_.substr(0, semi);
  if (code == "0" || code == "2") title_ = utf8ToWide(oscText_.substr(semi + 1));
}

// --- Size and text ------------------------------------------------------------------------

void VtScreen::resize(int cols, int rows) {
  cols = std::max(1, cols);
  rows = std::max(1, rows);
  if (cols == cols_ && rows == rows_) return;
  ++revision_;
  auto fitWidth = [&](VtLine& l) {
    l.cells.resize(static_cast<size_t>(cols), VtCell{});
    if (!l.cells.empty() && l.cells.back().width == 2) l.cells.back() = VtCell{};  // half a wide char
  };
  for (auto& l : lines_) fitWidth(l);
  for (auto& l : altLines_) fitWidth(l);
  for (auto& l : scrollback_) fitWidth(l);
  cols_ = cols;
  auto fitHeight = [&](std::vector<VtLine>& s, bool primary) {
    if (s.empty()) return;
    while (static_cast<int>(s.size()) > rows) {
      // Keep the cursor on screen: drop lines above it (into history) before blank ones below.
      if (cy_ >= rows) {
        if (primary) {
          scrollback_.push_back(std::move(s.front()));
          ++linesScrolled_;
          if (scrollback_.size() > scrollbackLimit_) scrollback_.pop_front();
        }
        s.erase(s.begin());
        --cy_;
      } else {
        s.pop_back();
      }
    }
    while (static_cast<int>(s.size()) < rows) {
      VtLine l;
      l.cells.assign(static_cast<size_t>(cols), VtCell{});
      s.push_back(std::move(l));
    }
  };
  const int cy = cy_;
  fitHeight(lines_, true);
  if (alt_) {
    cy_ = cy;
    fitHeight(altLines_, false);
  }
  rows_ = rows;
  tabStops_.assign(static_cast<size_t>(cols_), false);
  for (int i = 0; i < cols_; i += 8) tabStops_[static_cast<size_t>(i)] = true;
  scrollTop_ = 0;
  scrollBottom_ = rows_ - 1;
  pendingWrap_ = false;
  clampCursor();
}

std::wstring VtScreen::text(size_t line0, int col0, size_t line1, int col1) const {
  std::wstring out;
  const size_t count = lineCount();
  if (count == 0) return out;
  line1 = std::min(line1, count - 1);
  for (size_t l = line0; l <= line1; ++l) {
    const VtLine& ln = line(l);
    const int from = l == line0 ? std::max(0, col0) : 0;
    const int to = l == line1 ? std::min(col1, static_cast<int>(ln.cells.size())) : static_cast<int>(ln.cells.size());
    std::wstring row;
    for (int x = from; x < to; ++x) {
      const VtCell& c = ln.cells[static_cast<size_t>(x)];
      if (c.width == 0) continue;
      appendUtf16(row, c.ch ? c.ch : U' ');
    }
    if (!(ln.wrapped && l != line1)) {
      while (!row.empty() && row.back() == L' ') row.pop_back();
    }
    out += row;
    if (l != line1 && !ln.wrapped) out += L"\r\n";
  }
  return out;
}

}  // namespace verso
