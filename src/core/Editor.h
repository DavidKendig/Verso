#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/Search.h"
#include "core/Selection.h"
#include "core/TextBuffer.h"
#include "core/TextUtil.h"

namespace verso {

struct EditorConfig {
  int tabSize = 4;
  bool translateTabs = false;
  bool autoMatch = true;
  bool autoIndent = true;
  bool trimAutoWhitespace = true;
  std::wstring wordSeparators{kDefaultWordSeparators};
};

struct CommentStyle {
  std::wstring line;
  std::wstring blockStart;
  std::wstring blockEnd;
  bool indentAfterColon = false;
};

enum class EditKind { Other, Typing, DeleteBack, DeleteForward };

struct EditRecord {
  size_t pos = 0;
  std::wstring removed;
  std::wstring inserted;
};

struct Transaction {
  std::vector<EditRecord> edits;  // in application order
  std::vector<Selection> before, after;
  size_t beforePrimary = 0, afterPrimary = 0;
  EditKind kind = EditKind::Other;
  uint64_t id = 0;
};

enum class MoveBy {
  Chars,
  Words,
  WordEnds,
  Subwords,
  SubwordEnds,
  Lines,
  Pages,
  LineBegin,  // smart: first non-blank, then column 0
  LineEnd,
  DocBegin,
  DocEnd,
  Brackets,
};

enum class CaseMode { Upper, Lower, Title, Swap };
enum class LineOrder { Sort, SortCaseSensitive, Reverse, Unique };

struct ClipboardText {
  std::wstring text;
  bool wholeLines = false;
};

// Editing engine for one buffer: multiple selections, undo history and all
// text commands. It has no knowledge of rendering or windowing.
class Editor {
 public:
  struct Change {
    size_t pos = 0;
    size_t len = 0;
    std::wstring text;
  };

  Editor();

  EditorConfig config;
  CommentStyle comments;

  const TextBuffer& buffer() const { return buf_; }
  const SelectionSet& selections() const { return sels_; }
  void setSelections(std::vector<Selection> sels, size_t primary);
  void setText(std::wstring text);           // resets history and selection
  void replaceAllText(std::wstring text);    // undoable whole-buffer replacement
  std::wstring textOf(const Selection& s) const;

  // History
  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  void undo();
  void redo();
  uint64_t stateId() const { return undo_.empty() ? 0 : undo_.back().id; }
  void markSaved() { savedId_ = stateId(); }
  void markUnsaved() { savedId_ = UINT64_MAX; }
  bool isDirty() const { return stateId() != savedId_; }
  uint64_t selectionVersion() const { return selVersion_; }
  // Increments on every buffer change, including setText (for layout caches).
  uint64_t revision() const { return revision_; }
  // Earliest line whose content changed since the previous call (SIZE_MAX if none).
  size_t takeFirstChangedLine();

  // Geometry in visual columns (tabs expanded, wide characters count twice).
  size_t columnAt(size_t pos) const;
  size_t posAtColumn(size_t line, double col, bool round = true) const;
  std::pair<size_t, size_t> wordAt(size_t pos) const;
  size_t matchBracket(size_t pos) const;  // pos must be at a bracket; npos when unmatched

  // Typing and deletion
  void insertText(std::wstring_view s, bool typed = true);
  void newline();
  void insertLineAfter();
  void insertLineBefore();
  void backspace();
  void deleteForward();
  void deleteWord(bool forward);
  void deleteToLineBoundary(bool forward);
  void tabKey();
  void indent();
  void unindent();

  // Movement and selection
  void move(MoveBy by, bool forward, bool extend, int pageLines = 1);
  void selectAll();
  void singleSelection();
  void splitSelectionIntoLines();
  void addCursorLine(bool down);
  void expandToLine();
  void expandToWord();
  void expandToBrackets();
  void setCaret(size_t pos, bool extend, bool add);
  void setColumnSelection(size_t anchorLine, double anchorCol, size_t line, double col);

  // Find-under-cursor family (Ctrl+D etc.)
  void findUnderExpand();
  void findUnderExpandSkip();
  void findAllUnder();
  bool findUnder(bool forward);

  // Line operations
  void swapLines(bool up);
  void duplicate();
  void deleteLine();
  void joinLines();
  void toggleComment(bool block);
  void transformCase(CaseMode mode);
  void permuteLines(LineOrder order);
  void transpose();
  void trimTrailingWhitespace();
  void ensureNewlineAtEof();
  void convertIndentation(bool toSpaces);

  // Clipboard
  ClipboardText copy() const;
  ClipboardText cut();
  void paste(std::wstring_view text, bool wholeLines);

  // Search
  bool findNext(const Searcher& s, bool forward, bool wrap = true);
  size_t selectAllMatches(const Searcher& s);
  bool replaceNext(const Searcher& s, std::wstring_view format, bool wrap = true);
  size_t replaceAll(const Searcher& s, std::wstring_view format);

  // Low-level: apply sorted, non-overlapping changes as one undoable step.
  void commit(std::vector<Change> changes, std::vector<Selection> after, size_t afterPrimary, EditKind kind);
  void commitMapped(std::vector<Change> changes, EditKind kind);

 private:
  std::wstring indentUnit() const;
  size_t moveTarget(size_t pos, MoveBy by, bool forward) const;
  size_t wordBoundary(size_t pos, bool forward) const;
  size_t subwordBoundary(size_t pos, bool forward) const;
  size_t nextCharPos(size_t pos) const;
  size_t prevCharPos(size_t pos) const;
  std::vector<std::pair<size_t, size_t>> selectedLineBlocks() const;
  void deleteRanges(std::vector<std::pair<size_t, size_t>> ranges, EditKind kind);
  void applyRaw(size_t pos, size_t eraseLen, std::wstring_view insert);
  void pushTransaction(Transaction t);
  void touchSelection() { ++selVersion_; }
  static size_t mapPos(size_t pos, const std::vector<Change>& changes, bool right);
  Selection mapSelection(const Selection& s, const std::vector<Change>& changes) const;
  std::vector<Match> occurrences(const std::wstring& needle, bool wholeWord) const;

  TextBuffer buf_;
  SelectionSet sels_;
  std::vector<Transaction> undo_, redo_;
  uint64_t nextId_ = 0;
  uint64_t savedId_ = 0;
  uint64_t selVersion_ = 0;
  uint64_t revision_ = 0;
  uint64_t wholeWordVersion_ = UINT64_MAX;  // selection version for which Ctrl+D uses whole words
  bool groupBroken_ = false;
  size_t firstChanged_ = 0;
};

}  // namespace verso
