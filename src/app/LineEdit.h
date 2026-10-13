#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace verso {

// Single-line text field used by the find panel and overlays.
class LineEdit {
 public:
  enum class KeyResult { NotHandled, Handled, Changed };

  const std::wstring& text() const { return text_; }
  void setText(std::wstring t, bool selectAll = true);
  void selectAll();
  bool hasSelection() const { return caret_ != anchor_; }
  size_t caret() const { return caret_; }
  size_t anchor() const { return anchor_; }
  void setCaret(size_t pos, bool extend);
  void selectWordAt(size_t pos);
  void insert(std::wstring_view s);

  KeyResult onKey(UINT vk, bool ctrl, bool shift, HWND owner);

  // History navigation (Up/Down in the find field).
  void remember();
  bool historyStep(int dir);

  float scrollX = 0;

 private:
  void eraseSelection();
  size_t wordLeft(size_t p) const;
  size_t wordRight(size_t p) const;

  std::wstring text_;
  size_t caret_ = 0, anchor_ = 0;
  std::vector<std::wstring> history_;
  int historyPos_ = -1;
};

}  // namespace verso
