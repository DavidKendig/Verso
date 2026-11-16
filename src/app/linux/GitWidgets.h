#pragma once
// Linux git mode: the controls the Windows git view gets from Win32 (list views, a tree
// view, edit controls, owner-drawn lists), drawn by Verso with Gfx/Cairo/Pango instead.
// GitView.cpp arranges them and runs the git commands. Coordinates are DIPs in the git
// view's area; input arrives already in those coordinates.

#include <windows.h>  // src/app/linux/win32/windows.h

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "app/Gfx.h"
#include "app/Theme.h"
#include "git.h"
#include "graph.h"

typedef struct _PangoLayout PangoLayout;

namespace verso::gitui {

// Git mode's colors, derived from Verso's theme so the view follows light and dark.
struct Palette {
  Color bg, panel, side, input, border, text, dim, accent, select, selectDim, hover;
  Color addBg, addGutter, addText, delBg, delGutter, delText, hunkBg, hunkText, fileBg;
  Color button, buttonHover, buttonDown, buttonOn, buttonText, warn;
  Color badgeLocal, badgeCurrent, badgeRemote, badgeTag, badgeHead;
  Color scrollThumb, scrollThumbHot, inputBorder, inputBorderFocus, placeholder, inputSelection;
  Color overlayBg, overlayBorder, shadow;
  Color added, modified, deleted;
};
Palette makePalette(const Theme& t);
Color laneColor(int lane);
Color statusColor(wchar_t c, const Palette& p);

struct Fonts {
  IDWriteTextFormat* ui = nullptr;       // list text, buttons
  IDWriteTextFormat* bold = nullptr;     // headers, the working directory row
  IDWriteTextFormat* caption = nullptr;  // section labels, ref badges
  IDWriteTextFormat* mono = nullptr;     // diffs, hashes, details, the console
  float monoW = 8, monoH = 16;
};

enum class Align { Left, Center, Right };
enum class Elide { None, End, Middle };  // Middle keeps both ends, like DT_PATH_ELLIPSIS

// One line of text vertically centered in `r`, cut with an ellipsis when it does not fit.
void drawText(Gfx& g, std::wstring_view s, const RectF& r, Color c, IDWriteTextFormat* f,
              Align a = Align::Left, Elide e = Elide::End);
// Text from (x, y) with no clipping or eliding (the caller clips).
void drawRaw(Gfx& g, std::wstring_view s, float x, float y, Color c, IDWriteTextFormat* f);
float textHeight(IDWriteTextFormat* f);
void strokeCircle(Gfx& g, float cx, float cy, float r, Color c, float width);

// A scroll bar drawn as a thin thumb; positions are in the owner's units (rows or DIPs).
struct ScrollBar {
  bool vertical = true;
  RectF track;
  float total = 0, page = 0, pos = 0;
  bool dragging = false;
  float grab = 0;

  float maxPos() const { return total > page ? total - page : 0; }
  bool visible() const { return total > page && !track.empty(); }
  RectF thumb() const;
  void paint(Gfx& g, const Palette& p, float mx, float my) const;
  // A press in the track: grabs the thumb or pages toward the point. False when outside.
  bool press(float x, float y);
  void dragTo(float x, float y);
};

// Double-click detection for the drawn controls (Windows sends WM_LBUTTONDBLCLK).
class ClickCounter {
 public:
  int press(float x, float y, const void* target);

 private:
  uint64_t time_ = 0;
  float x_ = -100, y_ = -100;
  const void* target_ = nullptr;
  int count_ = 0;
};

// A wrapping multi-line text field (the commit message), or read-only selectable text
// (commit details). Laid out by Pango; positions are wchar_t indices.
class TextArea {
 public:
  TextArea();
  ~TextArea();
  TextArea(const TextArea&) = delete;
  TextArea& operator=(const TextArea&) = delete;

  bool readOnly = false;
  IDWriteTextFormat* font = nullptr;
  std::wstring placeholder;
  RectF rect;

  const std::wstring& text() const { return text_; }
  void setText(const std::wstring& t);  // caret at the start; clears undo
  void paint(Gfx& g, const Palette& p, bool focused, bool caretOn);
  enum class Key { NotHandled, Handled, Changed };
  Key key(unsigned vk, bool ctrl, bool shift);
  bool typeChar(wchar_t c);
  void press(float x, float y, bool shift, int clicks);
  void dragTo(float x, float y);
  void release() { dragging_ = false; sb_.dragging = false; }
  bool dragging() const { return dragging_ || sb_.dragging; }
  void wheel(float notches);
  void selectAll();
  std::wstring selectedText() const;

 private:
  void ensureLayout();
  void replaceSelection(const std::wstring& s, bool typing);
  void pushUndo(bool typing);
  void revealCaret();
  void setCaret(size_t pos, bool extend);
  size_t indexAt(float x, float y);
  int byteOf(size_t pos) const;
  size_t posOf(int byte) const;
  size_t wordLeft(size_t p) const;
  size_t wordRight(size_t p) const;
  float contentHeight();
  RectF textRect() const;

  std::wstring text_;
  size_t caret_ = 0, anchor_ = 0;
  float scrollY_ = 0, preferredX_ = -1;
  bool dragging_ = false;
  PangoLayout* layout_ = nullptr;
  std::vector<int> offsets_;
  float layoutW_ = -1;
  IDWriteTextFormat* layoutFont_ = nullptr;
  bool dirty_ = true;
  struct Snapshot {
    std::wstring text;
    size_t caret;
  };
  std::vector<Snapshot> undo_, redo_;
  bool lastTyping_ = false;
  ScrollBar sb_;
};

// A file list: a colored status letter and a path per row, with multi-selection
// (the Windows view's report-style ListView).
class FileList {
 public:
  struct Row {
    wchar_t status = L' ';
    std::wstring text;
    std::string key;  // the path, for keeping the selection across refreshes
  };
  RectF rect;
  static constexpr float kRowH = 22;

  void setRows(std::vector<Row> rows, const std::set<std::string>& keep);
  const std::vector<Row>& rows() const { return rows_; }
  std::vector<int> selection() const;
  bool anySelected() const;
  void clearSelection();
  void selectOnly(int row);
  int caret() const { return caret_; }
  void paint(Gfx& g, const Palette& p, const Fonts& f, bool focused, float mx, float my);
  int rowAt(float x, float y) const;
  // Click handling; true when the selection changed. A right click keeps a selection
  // that already holds the row, like the ListView.
  bool press(float x, float y, bool ctrl, bool shift, bool right);
  bool key(unsigned vk, bool ctrl, bool shift, bool* changed);
  void wheel(float notches);
  void dragTo(float x, float y);  // drags the scroll bar's thumb
  void release() { sb_.dragging = false; }

 private:
  void reveal(int row);
  int visibleRows() const;
  std::vector<Row> rows_;
  std::vector<char> sel_;
  int caret_ = -1, anchor_ = -1, top_ = 0;
  ScrollBar sb_;
};

// The side bar's tree of branches, remotes, tags and stashes.
class SideTree {
 public:
  struct Node {
    std::wstring text;
    int depth = 0;
    int parent = -1;
    bool expandable = false;
    bool bold = false;
    int item = -1;          // the owner's item index
    std::wstring collapseKey;
  };
  RectF rect;
  static constexpr float kRowH = 24;

  // `collapsed` remembers which headers and remotes are folded (keyed by their text).
  void setNodes(std::vector<Node> nodes, std::map<std::wstring, bool>* collapsed, int selectNode);
  const std::vector<Node>& nodes() const { return nodes_; }
  int selected() const { return selected_; }  // node index, or -1
  int selectedItem() const { return selected_ >= 0 ? nodes_[static_cast<size_t>(selected_)].item : -1; }
  void paint(Gfx& g, const Palette& p, const Fonts& f, bool focused, float mx, float my);
  int nodeAt(float x, float y) const;
  // Returns true when the selection changed; `toggled` when a node was folded/unfolded.
  bool press(float x, float y, int clicks, bool* toggled);
  bool key(unsigned vk, bool* changed);
  void wheel(float notches);
  void dragTo(float x, float y);
  void release() { sb_.dragging = false; }
  void select(int node);
  void toggle(int node);

 private:
  bool expanded(int node) const;
  void rebuildVisible();
  void reveal(int node);
  int visibleRows() const;
  std::vector<Node> nodes_;
  std::vector<int> visible_;  // node indices in display order
  std::map<std::wstring, bool>* collapsed_ = nullptr;
  int selected_ = -1, top_ = 0;
  ScrollBar sb_;
};

// The commit list with its graph lanes and ref badges (Windows: commit_list.cpp).
// Items: -1 = the working directory row, >= 0 = an index into the commits.
class CommitListPane {
 public:
  static constexpr int kNone = -2;
  static constexpr int kWorkdir = -1;
  RectF rect;
  std::function<void()> onSelChange;

  void setModel(const std::vector<Commit>* commits, const std::vector<GraphRow>* graph, std::vector<int> rows,
                bool showGraph, bool workdirRow, const std::string& headHash);
  void setWorkdirText(const std::wstring& text, bool dirty);
  void setBanner(const std::wstring& text);
  int selected() const { return selected_; }
  void select(int item, bool notify);
  int itemAt(float x, float y) const;
  void paint(Gfx& g, const Palette& p, const Fonts& f, bool focused);
  bool press(float x, float y);  // selects the row under the point
  bool key(unsigned vk);
  void wheel(float notches);
  void dragTo(float x, float y);
  void release() { sb_.dragging = false; }

 private:
  void paintGraph(Gfx& g, const Palette& p, const RectF& row, int item);
  int rowCount() const { return (workdir_ ? 1 : 0) + static_cast<int>(rows_.size()); }
  int itemOfRow(int row) const;
  int rowOfItem(int item) const;
  float bannerHeight() const { return banner_.empty() ? 0 : kRowH; }
  int visibleRows() const;
  void ensureVisible(int row);
  void scrollTo(int top);
  void selectRow(int row, bool notify);

  static constexpr float kRowH = 26, kLaneW = 14;
  const std::vector<Commit>* commits_ = nullptr;
  const std::vector<GraphRow>* graph_ = nullptr;
  std::vector<int> rows_;
  bool showGraph_ = true, workdir_ = true, workdirDirty_ = false;
  std::wstring workdirText_, banner_;
  std::string headHash_;
  int selected_ = kNone, top_ = 0;
  ScrollBar sb_;
};

// Unified diff viewer with line selection and patch building for hunk and line staging;
// also plain text (blame output, the git console). Windows: diff_view.cpp.
class DiffPane {
 public:
  RectF rect;
  bool console = false;  // plain text that follows its end as lines are appended

  void setDiff(const std::string& diff, bool keepScroll);
  void setText(const std::wstring& text);
  void appendText(const std::wstring& text);
  void setPlaceholder(const std::wstring& message);
  bool scrollToFile(const std::string& path);
  bool cursorInHunk() const;
  bool hasMultiLineSelection() const;
  std::wstring selectedText() const;
  std::string cursorFilePath() const;
  std::string buildPatch(bool reverse, bool wholeHunks) const;
  void scrollToEnd();

  void paint(Gfx& g, const Palette& p, const Fonts& f, bool focused);
  bool press(float x, float y, bool shift, bool right);
  void dragTo(float x, float y);
  void release() { dragging_ = false; vsb_.dragging = hsb_.dragging = false; }
  bool dragging() const { return dragging_ || vsb_.dragging || hsb_.dragging; }
  bool key(unsigned vk, bool ctrl, bool shift);
  void wheel(float notches, bool horizontal);

 private:
  enum class Kind : uint8_t { Context, Add, Del, Hunk, FileHeader, Meta, NoNewline, Text };
  struct Line {
    Kind kind = Kind::Text;
    bool hidden = false;
    int oldNo = 0, newNo = 0;
    int file = -1;
    std::string raw;
    std::wstring text;
  };
  struct FileInfo {
    int header = 0, end = 0;
    std::string path, origPath;
    bool combined = false, binary = false, added = false, deleted = false;
    std::wstring modeChange;
  };
  void reset();
  void finishParse();
  int visibleRows() const;
  int rowAt(float y) const;
  float gutterWidth() const;
  float contentWidth() const;
  void scrollTo(int top);
  void scrollH(float x);
  void ensureVisible(int row);
  void setCursor(int row, bool extend);
  void selectionLines(int& a, int& b) const;
  void widenToHunks(int& a, int& b) const;

  std::vector<Line> lines_;
  std::vector<int> rows_;
  std::vector<FileInfo> files_;
  std::wstring placeholder_;
  bool diffMode_ = false;
  int top_ = 0;
  float hscroll_ = 0;
  float lineH_ = 17, charW_ = 8;
  size_t maxChars_ = 0;
  int anchor_ = -1, cursor_ = -1;
  bool dragging_ = false;
  ScrollBar vsb_, hsb_{false};
};

}  // namespace verso::gitui
