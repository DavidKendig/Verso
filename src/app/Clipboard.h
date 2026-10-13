#pragma once

#include <windows.h>

#include <string>

namespace verso {

// Clipboard text uses CRLF on the system clipboard and '\n' internally.
std::wstring getClipboardText(HWND owner);
void setClipboardText(HWND owner, const std::wstring& text);

}  // namespace verso
