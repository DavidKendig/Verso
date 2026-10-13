#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/TextBuffer.h"

namespace verso {

// One visual row of a (possibly wrapped) logical line.
struct VisualRow {
  size_t line = 0;      // logical line
  size_t start = 0;     // buffer offset of the row's first character
  size_t end = 0;       // buffer offset just past the row's last character
  size_t startCol = 0;  // visual column of `start` within the logical line
  size_t indent = 0;    // hanging indent (columns) for continuation rows
};

// Maps logical lines to visual rows. Without wrapping there is exactly one
// row per line and nothing is stored.
class WrapMap {
 public:
  // `wrapCols` is the usable width in columns; lines break after spaces when
  // possible and continuation rows keep the line's indentation.
  void build(const TextBuffer& buf, int tabSize, size_t wrapCols, bool wrap);

  bool wrapping() const { return wrap_; }
  size_t rowCount() const;
  VisualRow row(size_t r) const;
  size_t rowOf(size_t pos) const;
  size_t firstRowOfLine(size_t line) const;
  // `pos` clamped onto visual row `r`. A wrapped row's end offset belongs to the next
  // row, so the caret stays before the row's last character (a whole surrogate pair).
  size_t caretOnRow(size_t r, size_t pos) const;

 private:
  const TextBuffer* buf_ = nullptr;
  bool wrap_ = false;
  std::vector<VisualRow> rows_;
  std::vector<size_t> lineFirstRow_;
};

}  // namespace verso
