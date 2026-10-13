#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace verso {

// UTF-16 text with '\n' line separators and an incrementally maintained line
// index. Line-ending style and encoding belong to the owning document.
class TextBuffer {
 public:
  TextBuffer();
  explicit TextBuffer(std::wstring text);

  void assign(std::wstring text);
  const std::wstring& text() const { return text_; }
  size_t size() const { return text_.size(); }
  wchar_t at(size_t pos) const { return pos < text_.size() ? text_[pos] : L'\0'; }

  size_t lineCount() const { return starts_.size(); }
  size_t lineStart(size_t line) const;
  size_t lineEnd(size_t line) const;  // position of the line's '\n' or size()
  size_t lineLength(size_t line) const { return lineEnd(line) - lineStart(line); }
  size_t lineOf(size_t pos) const;
  std::wstring_view lineText(size_t line) const;
  std::wstring_view view(size_t pos, size_t len) const;
  size_t longestLineLength() const;

  void insert(size_t pos, std::wstring_view s);
  void erase(size_t pos, size_t len);

 private:
  void rebuild();

  std::wstring text_;
  std::vector<size_t> starts_;
};

}  // namespace verso
