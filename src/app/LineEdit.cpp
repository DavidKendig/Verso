#include "app/LineEdit.h"

#include <algorithm>
#include <cwctype>

#include "app/Clipboard.h"
#include "core/TextUtil.h"

namespace verso {

void LineEdit::setText(std::wstring t, bool selectAllText) {
  text_ = std::move(t);
  caret_ = text_.size();
  anchor_ = selectAllText ? 0 : caret_;
  historyPos_ = -1;
}

void LineEdit::selectAll() {
  anchor_ = 0;
  caret_ = text_.size();
}

void LineEdit::setCaret(size_t pos, bool extend) {
  caret_ = std::min(pos, text_.size());
  if (!extend) anchor_ = caret_;
}

void LineEdit::selectWordAt(size_t pos) {
  pos = std::min(pos, text_.size());
  auto word = [&](size_t i) { return i < text_.size() && (std::iswalnum(text_[i]) || text_[i] == L'_'); };
  size_t b = pos, e = pos;
  while (b > 0 && word(b - 1)) --b;
  while (word(e)) ++e;
  anchor_ = b;
  caret_ = e;
}

void LineEdit::eraseSelection() {
  size_t b = std::min(caret_, anchor_), e = std::max(caret_, anchor_);
  text_.erase(b, e - b);
  caret_ = anchor_ = b;
}

void LineEdit::insert(std::wstring_view s) {
  std::wstring clean;
  for (wchar_t c : s)
    if (c != L'\r' && c != L'\n') clean += c;
    else if (c == L'\n') clean += L' ';
  eraseSelection();
  text_.insert(caret_, clean);
  caret_ += clean.size();
  anchor_ = caret_;
}

size_t LineEdit::wordLeft(size_t p) const {
  while (p > 0 && std::iswspace(text_[p - 1])) --p;
  bool w = p > 0 && (std::iswalnum(text_[p - 1]) || text_[p - 1] == L'_');
  while (p > 0 && !std::iswspace(text_[p - 1]) && ((std::iswalnum(text_[p - 1]) || text_[p - 1] == L'_') == w)) --p;
  return p;
}

size_t LineEdit::wordRight(size_t p) const {
  const size_t n = text_.size();
  while (p < n && std::iswspace(text_[p])) ++p;
  bool w = p < n && (std::iswalnum(text_[p]) || text_[p] == L'_');
  while (p < n && !std::iswspace(text_[p]) && ((std::iswalnum(text_[p]) || text_[p] == L'_') == w)) ++p;
  return p;
}

LineEdit::KeyResult LineEdit::onKey(UINT vk, bool ctrl, bool shift, HWND owner) {
  switch (vk) {
    case VK_LEFT:
      if (!shift && hasSelection() && !ctrl) setCaret(std::min(caret_, anchor_), false);
      else setCaret(ctrl ? wordLeft(caret_) : (caret_ > 0 ? caret_ - 1 : 0), shift);
      return KeyResult::Handled;
    case VK_RIGHT:
      if (!shift && hasSelection() && !ctrl) setCaret(std::max(caret_, anchor_), false);
      else setCaret(ctrl ? wordRight(caret_) : std::min(caret_ + 1, text_.size()), shift);
      return KeyResult::Handled;
    case VK_HOME:
      setCaret(0, shift);
      return KeyResult::Handled;
    case VK_END:
      setCaret(text_.size(), shift);
      return KeyResult::Handled;
    case VK_BACK:
      if (!hasSelection()) {
        if (caret_ == 0) return KeyResult::Handled;
        anchor_ = ctrl ? wordLeft(caret_) : caret_ - 1;
      }
      eraseSelection();
      return KeyResult::Changed;
    case VK_DELETE:
      if (shift && !ctrl) {
        if (hasSelection()) {
          size_t b = std::min(caret_, anchor_), e = std::max(caret_, anchor_);
          setClipboardText(owner, text_.substr(b, e - b));
          eraseSelection();
          return KeyResult::Changed;
        }
        return KeyResult::Handled;
      }
      if (!hasSelection()) {
        if (caret_ >= text_.size()) return KeyResult::Handled;
        anchor_ = ctrl ? wordRight(caret_) : caret_ + 1;
      }
      eraseSelection();
      return KeyResult::Changed;
    default:
      break;
  }
  if (ctrl && !shift) {
    switch (vk) {
      case 'A':
        selectAll();
        return KeyResult::Handled;
      case 'C':
      case 'X': {
        if (!hasSelection()) return KeyResult::Handled;
        size_t b = std::min(caret_, anchor_), e = std::max(caret_, anchor_);
        setClipboardText(owner, text_.substr(b, e - b));
        if (vk == 'X') {
          eraseSelection();
          return KeyResult::Changed;
        }
        return KeyResult::Handled;
      }
      case 'V':
        insert(getClipboardText(owner));
        return KeyResult::Changed;
      default:
        break;
    }
  }
  if (shift && !ctrl && vk == VK_INSERT) {
    insert(getClipboardText(owner));
    return KeyResult::Changed;
  }
  return KeyResult::NotHandled;
}

void LineEdit::remember() {
  if (text_.empty()) return;
  history_.erase(std::remove(history_.begin(), history_.end(), text_), history_.end());
  history_.push_back(text_);
  if (history_.size() > 50) history_.erase(history_.begin());
  historyPos_ = -1;
}

bool LineEdit::historyStep(int dir) {
  if (history_.empty()) return false;
  int n = static_cast<int>(history_.size());
  if (historyPos_ < 0) historyPos_ = dir < 0 ? n - 1 : n;
  else historyPos_ += dir;
  if (historyPos_ < 0) historyPos_ = 0;
  if (historyPos_ >= n) {
    historyPos_ = n;
    text_.clear();
  } else {
    text_ = history_[static_cast<size_t>(historyPos_)];
  }
  caret_ = text_.size();
  anchor_ = 0;
  return true;
}

}  // namespace verso
