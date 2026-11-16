// Clipboard on Linux: the Wayland clipboard as UTF-8 text with '\n' line ends.
#include "app/Clipboard.h"

#include "app/linux/Window.h"
#include "core/TextUtil.h"

namespace verso {

std::wstring getClipboardText(HWND) {
  platform::Window* w = platform::mainWindow();
  if (!w) return {};
  std::wstring text = utf8ToWide(w->clipboardText());
  std::wstring out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == L'\r') {
      if (i + 1 < text.size() && text[i + 1] == L'\n') continue;
      out += L'\n';
    } else {
      out += text[i];
    }
  }
  return out;
}

void setClipboardText(HWND, const std::wstring& text) {
  if (platform::Window* w = platform::mainWindow()) w->setClipboard(wideToUtf8(text));
}

}  // namespace verso
