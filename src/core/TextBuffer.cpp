#include "core/TextBuffer.h"

#include <algorithm>

namespace verso {

TextBuffer::TextBuffer() { rebuild(); }

TextBuffer::TextBuffer(std::wstring text) : text_(std::move(text)) { rebuild(); }

void TextBuffer::assign(std::wstring text) {
  text_ = std::move(text);
  rebuild();
}

void TextBuffer::rebuild() {
  starts_.clear();
  starts_.push_back(0);
  for (size_t i = 0; i < text_.size(); ++i)
    if (text_[i] == L'\n') starts_.push_back(i + 1);
}

size_t TextBuffer::lineStart(size_t line) const {
  if (line >= starts_.size()) return text_.size();
  return starts_[line];
}

size_t TextBuffer::lineEnd(size_t line) const {
  if (line + 1 >= starts_.size()) return text_.size();
  return starts_[line + 1] - 1;
}

size_t TextBuffer::lineOf(size_t pos) const {
  pos = std::min(pos, text_.size());
  auto it = std::upper_bound(starts_.begin(), starts_.end(), pos);
  return static_cast<size_t>(it - starts_.begin()) - 1;
}

std::wstring_view TextBuffer::lineText(size_t line) const {
  size_t b = lineStart(line);
  return std::wstring_view(text_).substr(b, lineEnd(line) - b);
}

std::wstring_view TextBuffer::view(size_t pos, size_t len) const {
  pos = std::min(pos, text_.size());
  return std::wstring_view(text_).substr(pos, len);
}

size_t TextBuffer::longestLineLength() const {
  size_t best = 0;
  for (size_t i = 0; i < starts_.size(); ++i) best = std::max(best, lineLength(i));
  return best;
}

void TextBuffer::insert(size_t pos, std::wstring_view s) {
  if (s.empty()) return;
  pos = std::min(pos, text_.size());
  size_t line = lineOf(pos);
  text_.insert(pos, s);
  for (size_t i = line + 1; i < starts_.size(); ++i) starts_[i] += s.size();
  std::vector<size_t> added;
  for (size_t i = 0; i < s.size(); ++i)
    if (s[i] == L'\n') added.push_back(pos + i + 1);
  if (!added.empty()) starts_.insert(starts_.begin() + static_cast<ptrdiff_t>(line + 1), added.begin(), added.end());
}

void TextBuffer::erase(size_t pos, size_t len) {
  if (pos >= text_.size() || len == 0) return;
  len = std::min(len, text_.size() - pos);
  size_t line = lineOf(pos);
  size_t newlines = 0;
  for (size_t i = pos; i < pos + len; ++i)
    if (text_[i] == L'\n') ++newlines;
  text_.erase(pos, len);
  auto first = starts_.begin() + static_cast<ptrdiff_t>(line + 1);
  starts_.erase(first, first + static_cast<ptrdiff_t>(newlines));
  for (size_t i = line + 1; i < starts_.size(); ++i) starts_[i] -= len;
}

}  // namespace verso
