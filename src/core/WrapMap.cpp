#include "core/WrapMap.h"

#include <algorithm>

#include "core/TextUtil.h"

namespace verso {

void WrapMap::build(const TextBuffer& buf, int tabSize, size_t wrapCols, bool wrap) {
  buf_ = &buf;
  wrap_ = wrap;
  rows_.clear();
  lineFirstRow_.clear();
  if (!wrap) return;

  const size_t tab = static_cast<size_t>(std::max(1, tabSize));
  wrapCols = std::max<size_t>(wrapCols, 8);
  const std::wstring& t = buf.text();
  std::vector<size_t> col;  // visual column before each character of the line
  lineFirstRow_.reserve(buf.lineCount());
  for (size_t line = 0; line < buf.lineCount(); ++line) {
    lineFirstRow_.push_back(rows_.size());
    size_t ls = buf.lineStart(line), le = buf.lineEnd(line);
    col.assign(1, 0);
    for (size_t i = ls; i < le; ++i) {
      wchar_t c = t[i];
      size_t c0 = col.back();
      col.push_back(c0 + (c == L'\t' ? tab - c0 % tab : static_cast<size_t>(unitColumns(c))));
    }
    // Continuation rows hang at the line's indentation (capped so text still fits).
    size_t lead = 0;
    while (ls + lead < le && isBlank(t[ls + lead])) ++lead;
    size_t hang = std::min(col[lead], wrapCols / 2);

    size_t rowStart = ls, indent = 0, avail = wrapCols, lastBreak = 0;
    for (size_t i = ls; i < le; ++i) {
      size_t width = col[i + 1 - ls] - col[rowStart - ls];
      if (width > avail && i > rowStart) {
        size_t brk = lastBreak > rowStart ? lastBreak : i;
        // Never split a surrogate pair.
        if (brk > rowStart && brk < le && isLowSurrogate(t[brk])) --brk;
        if (brk <= rowStart) brk = i;
        rows_.push_back({line, rowStart, brk, col[rowStart - ls], indent});
        rowStart = brk;
        indent = hang;
        avail = wrapCols > hang ? wrapCols - hang : wrapCols;
        lastBreak = 0;
        i = brk - 1;  // re-examine from the new row start
        continue;
      }
      if (t[i] == L' ' || t[i] == L'\t') lastBreak = i + 1;
    }
    rows_.push_back({line, rowStart, le, col[rowStart - ls], indent});
  }
}

size_t WrapMap::rowCount() const {
  if (!buf_) return 0;
  return wrap_ ? rows_.size() : buf_->lineCount();
}

VisualRow WrapMap::row(size_t r) const {
  if (wrap_) return rows_[std::min(r, rows_.size() - 1)];
  size_t line = std::min(r, buf_->lineCount() - 1);
  return {line, buf_->lineStart(line), buf_->lineEnd(line), 0, 0};
}

size_t WrapMap::rowOf(size_t pos) const {
  size_t line = buf_->lineOf(pos);
  if (!wrap_) return line;
  size_t first = lineFirstRow_[line];
  size_t last = line + 1 < lineFirstRow_.size() ? lineFirstRow_[line + 1] : rows_.size();
  size_t r = first;
  while (r + 1 < last && rows_[r + 1].start <= pos) ++r;
  return r;
}

size_t WrapMap::caretOnRow(size_t r, size_t pos) const {
  const VisualRow vr = row(r);
  pos = std::clamp(pos, vr.start, vr.end);
  if (pos == vr.end && pos > vr.start && vr.end != buf_->lineEnd(vr.line)) {
    --pos;
    const wchar_t lo = buf_->at(pos), hi = pos > vr.start ? buf_->at(pos - 1) : 0;
    if (lo >= 0xDC00 && lo <= 0xDFFF && hi >= 0xD800 && hi <= 0xDBFF) --pos;
  }
  return pos;
}

size_t WrapMap::firstRowOfLine(size_t line) const {
  if (!wrap_) return line;
  return lineFirstRow_[std::min(line, lineFirstRow_.size() - 1)];
}

}  // namespace verso
