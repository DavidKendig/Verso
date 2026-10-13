#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace verso {

// A terminal color: the default, one of the 256 palette entries, or 24-bit RGB.
struct VtColor {
  enum Kind : uint8_t { Default, Palette, Rgb };
  Kind kind = Default;
  uint8_t index = 0;
  uint32_t rgb = 0;  // 0xRRGGBB
  bool operator==(const VtColor& o) const { return kind == o.kind && index == o.index && rgb == o.rgb; }
};

enum VtAttr : uint8_t { VtBold = 1, VtDim = 2, VtItalic = 4, VtUnderline = 8, VtInverse = 16, VtStrike = 32, VtHidden = 64 };

struct VtCell {
  char32_t ch = U' ';
  VtColor fg, bg;
  uint8_t attrs = 0;
  uint8_t width = 1;  // 2: first cell of a wide character; 0: the cell it covers
  bool sameStyle(const VtCell& o) const { return fg == o.fg && bg == o.bg && attrs == o.attrs; }
};

struct VtLine {
  std::vector<VtCell> cells;
  bool wrapped = false;  // the text continues on the next line (an automatic wrap)
};

// Display width of a character in terminal cells: 0 for combining marks, 2 for
// East Asian wide characters and emoji, 1 otherwise.
int vtCharWidth(char32_t c);

// The screen of a VT-compatible terminal: parses the output of a program (UTF-8
// with ANSI/VT escape sequences, as Windows ConPTY produces) into a grid of cells,
// with scrollback, the alternate screen and the modes that affect input.
class VtScreen {
 public:
  explicit VtScreen(int cols = 80, int rows = 24, size_t scrollbackLimit = 10000);

  void write(std::string_view utf8);
  void resize(int cols, int rows);
  // Answers to queries such as cursor position reports, to be sent to the program.
  std::string takeReplies();

  int cols() const { return cols_; }
  int rows() const { return rows_; }
  // History: scrollback lines first (oldest at 0), then the screen rows.
  size_t scrollbackSize() const { return alt_ ? 0 : scrollback_.size(); }
  size_t lineCount() const { return scrollbackSize() + static_cast<size_t>(rows_); }
  const VtLine& line(size_t index) const;
  // Lines pushed into scrollback so far (also counts lines dropped at the limit).
  uint64_t linesScrolled() const { return linesScrolled_; }

  int cursorX() const { return cx_; }
  int cursorY() const { return cy_; }
  bool cursorVisible() const { return cursorVisible_; }
  int cursorShape() const { return cursorShape_; }  // DECSCUSR: 0/1/2 block, 3/4 underline, 5/6 bar
  bool altScreen() const { return alt_; }
  bool appCursorKeys() const { return appCursor_; }
  bool bracketedPaste() const { return bracketedPaste_; }
  const std::wstring& title() const { return title_; }
  uint64_t revision() const { return revision_; }

  // Text of history positions [line0:col0, line1:col1), joining wrapped lines.
  std::wstring text(size_t line0, int col0, size_t line1, int col1) const;

 private:
  enum class State { Ground, Escape, EscapeIntermediate, Csi, Osc, OscEscape, String, StringEscape };

  std::vector<VtLine>& screen() { return alt_ ? altLines_ : lines_; }
  const std::vector<VtLine>& screen() const { return alt_ ? altLines_ : lines_; }
  void feed(char32_t c);
  VtLine blankLine() const;
  VtCell blankCell() const;
  void print(char32_t c);
  void control(char32_t c);
  void escape(char32_t c);
  void csi(char32_t final);
  void sgr();
  void setMode(bool on);
  void osc();
  void lineFeed();
  void scrollUp(int top, int bottom, int n);
  void scrollDown(int top, int bottom, int n);
  void eraseCells(int row, int from, int to);
  void clampCursor();
  void setAltScreen(bool on, bool saveCursor);
  void softReset();
  int param(size_t i, int def) const;

  int cols_, rows_;
  size_t scrollbackLimit_;
  std::vector<VtLine> lines_, altLines_;
  std::deque<VtLine> scrollback_;
  uint64_t linesScrolled_ = 0;
  bool alt_ = false;

  int cx_ = 0, cy_ = 0;
  bool pendingWrap_ = false;
  VtCell pen_;  // current colors and attributes (ch unused)
  struct Saved {
    int x = 0, y = 0;
    VtCell pen;
    bool originMode = false;
  } saved_, savedPrimary_;
  int scrollTop_ = 0, scrollBottom_ = 0;
  std::vector<bool> tabStops_;
  bool autowrap_ = true, originMode_ = false, insertMode_ = false, cursorVisible_ = true;
  bool appCursor_ = false, bracketedPaste_ = false, newlineMode_ = false;
  int cursorShape_ = 0;
  char32_t lastPrinted_ = 0;

  State state_ = State::Ground;
  std::string utf8_;  // an incomplete UTF-8 sequence carried between writes
  std::vector<int> params_;
  bool paramStarted_ = false;
  char prefix_ = 0;          // '?', '>', '=' or '<' after CSI
  std::string intermediates_;
  std::string oscText_;
  std::wstring title_;
  std::string replies_;
  uint64_t revision_ = 0;
};

}  // namespace verso
