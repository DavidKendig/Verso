#include "app/Clipboard.h"

#include <cstring>

#include "core/TextUtil.h"

namespace verso {

std::wstring getClipboardText(HWND owner) {
  std::wstring out;
  if (!OpenClipboard(owner)) return out;
  if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
    if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
      out = p;
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return normalizeNewlines(out);
}

void setClipboardText(HWND owner, const std::wstring& text) {
  std::wstring crlf;
  crlf.reserve(text.size() + text.size() / 16);
  for (wchar_t c : text) {
    if (c == L'\n') crlf += L'\r';
    crlf += c;
  }
  if (!OpenClipboard(owner)) return;
  EmptyClipboard();
  size_t bytes = (crlf.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    if (void* p = GlobalLock(h)) {
      std::memcpy(p, crlf.c_str(), bytes);
      GlobalUnlock(h);
      if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    } else {
      GlobalFree(h);
    }
  }
  CloseClipboard();
}

}  // namespace verso
