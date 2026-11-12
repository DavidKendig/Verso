#pragma once
// Linux: the small part of the Win32 API that Verso's shared front-end code calls.
//
// Verso's app code (src/app) was written against Win32. On Linux these declarations stand
// in for <windows.h> (src/app/linux/win32/*.h include this file), and Win32Compat.cpp,
// Menus.cpp and Dialogs.cpp implement them on top of the Wayland window
// (src/app/linux/Window.h): messages and timers become posts and timers on the UI thread,
// menus are drawn by Verso, message boxes are drawn dialogs, files use POSIX calls.
// Only what Verso uses is here, with Win32's names, values and semantics.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>

// --- basic types --------------------------------------------------------------------------
using BYTE = uint8_t;
using WORD = uint16_t;
using DWORD = uint32_t;
using UINT = unsigned int;
using INT = int;
using LONG = long;  // as wide as std::max(0L, ...) expects; values stay 32-bit
using ULONG = unsigned long;
using SHORT = int16_t;
using USHORT = uint16_t;
using BOOL = int;
using UINT_PTR = uintptr_t;
using INT_PTR = intptr_t;
using LONG_PTR = intptr_t;
using ULONG_PTR = uintptr_t;
using DWORD_PTR = uintptr_t;
using WPARAM = uintptr_t;
using LPARAM = intptr_t;
using LRESULT = intptr_t;
using ULONGLONG = uint64_t;
using LONGLONG = int64_t;
using HRESULT = int32_t;
using WCHAR = wchar_t;
using LPCWSTR = const wchar_t*;
using LPWSTR = wchar_t*;
using PWSTR = wchar_t*;
using LPCSTR = const char*;
using SIZE_T = size_t;
using errno_t = int;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define WINAPI
#define CALLBACK

struct HWND__;
using HWND = HWND__*;    // the app window
struct HMENU__;
using HMENU = HMENU__*;  // a menu built with CreateMenu/CreatePopupMenu (Menus.cpp)
using HINSTANCE = void*;
using HANDLE = void*;
using HICON = void*;
using HCURSOR = void*;
using HMODULE = void*;

struct POINT {
  LONG x = 0, y = 0;
};
struct RECT {
  LONG left = 0, top = 0, right = 0, bottom = 0;
};

#define LOWORD(l) (static_cast<WORD>(static_cast<DWORD_PTR>(l) & 0xffff))
#define HIWORD(l) (static_cast<WORD>((static_cast<DWORD_PTR>(l) >> 16) & 0xffff))
#define MAKEINTRESOURCEW(i) (reinterpret_cast<LPWSTR>(static_cast<ULONG_PTR>(static_cast<WORD>(i))))
#define SUCCEEDED(hr) (static_cast<HRESULT>(hr) >= 0)
#define FAILED(hr) (static_cast<HRESULT>(hr) < 0)
#define S_OK 0
#define E_FAIL static_cast<HRESULT>(0x80004005)

// --- messages -----------------------------------------------------------------------------
#define WM_COMMAND 0x0111
#define WM_TIMER 0x0113
#define WM_APP 0x8000

// --- virtual keys (Windows values; the Linux window maps key symbols to these) -----------
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_CLEAR 0x0C
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_LWIN 0x5B
#define VK_RWIN 0x5C
#define VK_APPS 0x5D
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SEPARATOR 0x6C
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_F13 0x7C
#define VK_F14 0x7D
#define VK_F15 0x7E
#define VK_F16 0x7F
#define VK_F17 0x80
#define VK_F18 0x81
#define VK_F19 0x82
#define VK_F20 0x83
#define VK_F21 0x84
#define VK_F22 0x85
#define VK_F23 0x86
#define VK_F24 0x87
#define VK_NUMLOCK 0x90
#define VK_SCROLL 0x91
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
#define VK_OEM_102 0xE2

// --- message boxes --------------------------------------------------------------------------
#define MB_OK 0x00000000u
#define MB_OKCANCEL 0x00000001u
#define MB_ABORTRETRYIGNORE 0x00000002u
#define MB_YESNOCANCEL 0x00000003u
#define MB_YESNO 0x00000004u
#define MB_RETRYCANCEL 0x00000005u
#define MB_ICONHAND 0x00000010u
#define MB_ICONQUESTION 0x00000020u
#define MB_ICONEXCLAMATION 0x00000030u
#define MB_ICONASTERISK 0x00000040u
#define MB_ICONERROR MB_ICONHAND
#define MB_ICONWARNING MB_ICONEXCLAMATION
#define MB_ICONINFORMATION MB_ICONASTERISK
#define MB_DEFBUTTON1 0x00000000u
#define MB_DEFBUTTON2 0x00000100u
#define MB_DEFBUTTON3 0x00000200u
#define MB_TOPMOST 0x00040000u
#define MB_SETFOREGROUND 0x00010000u
#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7

// --- menus ----------------------------------------------------------------------------------
#define MF_STRING 0x00000000u
#define MF_ENABLED 0x00000000u
#define MF_UNCHECKED 0x00000000u
#define MF_BYCOMMAND 0x00000000u
#define MF_GRAYED 0x00000001u
#define MF_DISABLED 0x00000002u
#define MF_CHECKED 0x00000008u
#define MF_POPUP 0x00000010u
#define MF_MENUBARBREAK 0x00000020u
#define MF_MENUBREAK 0x00000040u
#define MF_OWNERDRAW 0x00000100u
#define MF_BYPOSITION 0x00000400u
#define MF_SEPARATOR 0x00000800u
#define MF_DEFAULT 0x00001000u
#define MFS_GRAYED 0x00000003u
#define MFS_DISABLED MFS_GRAYED
#define MFS_CHECKED MF_CHECKED
#define MFS_ENABLED MF_ENABLED
#define TPM_LEFTALIGN 0x0000u
#define TPM_RIGHTBUTTON 0x0002u
#define TPM_CENTERALIGN 0x0004u
#define TPM_RIGHTALIGN 0x0008u
#define TPM_TOPALIGN 0x0000u
#define TPM_VCENTERALIGN 0x0010u
#define TPM_BOTTOMALIGN 0x0020u
#define TPM_NONOTIFY 0x0080u
#define TPM_RETURNCMD 0x0100u

// --- files ----------------------------------------------------------------------------------
#define FILE_ATTRIBUTE_READONLY 0x00000001u
#define FILE_ATTRIBUTE_HIDDEN 0x00000002u
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#define FILE_ATTRIBUTE_NORMAL 0x00000080u
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400u
#define INVALID_FILE_ATTRIBUTES (static_cast<DWORD>(-1))
#define MAX_PATH 4096
#define SW_SHOWNORMAL 1
#define SW_SHOW 5

// --- functions ------------------------------------------------------------------------------
DWORD GetFileAttributesW(LPCWSTR path);
ULONGLONG GetTickCount64();
DWORD GetTickCount();
SHORT GetKeyState(int vk);
UINT GetDoubleClickTime();
LONG GetMessageTime();
HWND SetCapture(HWND hwnd);
BOOL ReleaseCapture();
HWND GetCapture();
UINT_PTR SetTimer(HWND hwnd, UINT_PTR id, UINT ms, void* fn);
BOOL KillTimer(HWND hwnd, UINT_PTR id);
BOOL InvalidateRect(HWND hwnd, const RECT* rect, BOOL erase);
BOOL UpdateWindow(HWND hwnd);
HWND GetFocus();
HWND SetFocus(HWND hwnd);
BOOL PostMessageW(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
int MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type);
HINSTANCE ShellExecuteW(HWND hwnd, LPCWSTR op, LPCWSTR file, LPCWSTR params, LPCWSTR dir, INT show);
BOOL SetWindowTextW(HWND hwnd, LPCWSTR text);
BOOL GetClientRect(HWND hwnd, RECT* rect);
BOOL ClientToScreen(HWND hwnd, POINT* pt);
BOOL ScreenToClient(HWND hwnd, POINT* pt);
BOOL GetCursorPos(POINT* pt);
BOOL MessageBeep(UINT type);
int _wcsicmp(const wchar_t* a, const wchar_t* b);
int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n);
#define _TRUNCATE (static_cast<size_t>(-1))
errno_t wcsncpy_s(wchar_t* dst, size_t dstSize, const wchar_t* src, size_t count);
template <size_t N>
errno_t wcsncpy_s(wchar_t (&dst)[N], const wchar_t* src, size_t count) {
  return wcsncpy_s(dst, N, src, count);
}

DWORD GetLastError();  // errno of the last failed compat call

// Child windows: the Linux git view is drawn inside the main window (linux/GitView.cpp)
// rather than being a child window, so these only keep the shared code compiling.
#define WM_CLOSE 0x0010
#define SW_HIDE 0
BOOL MoveWindow(HWND hwnd, int x, int y, int w, int h, BOOL repaint);
BOOL ShowWindow(HWND hwnd, int cmd);
BOOL IsChild(HWND parent, HWND child);
// The wait cursor: the compositor shows busy state itself.
#define IDC_ARROW (reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(32512)))
#define IDC_WAIT (reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(32514)))
HCURSOR LoadCursorW(HINSTANCE instance, LPCWSTR name);
HCURSOR SetCursor(HCURSOR cursor);
// Creates a folder and any missing parents (0 on success, as on Windows).
int SHCreateDirectoryExW(HWND hwnd, LPCWSTR path, void* security);

// Keyboard: US-layout virtual keys, as linux/Window.cpp produces them.
#define VK_CANCEL 0x03
#define MAPVK_VK_TO_CHAR 2
SHORT VkKeyScanW(wchar_t ch);
UINT MapVirtualKeyW(UINT code, UINT mapType);

// Files and folders (Win32Compat.cpp, over POSIX). Paths are UTF-32 wchar_t strings.
struct FILETIME {
  DWORD dwLowDateTime, dwHighDateTime;  // 100 ns ticks since 1601, as on Windows
};
struct WIN32_FIND_DATAW {
  DWORD dwFileAttributes;
  FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
  DWORD nFileSizeHigh, nFileSizeLow;
  wchar_t cFileName[1024];
};
struct WIN32_FILE_ATTRIBUTE_DATA {
  DWORD dwFileAttributes;
  FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
  DWORD nFileSizeHigh, nFileSizeLow;
};
enum FINDEX_INFO_LEVELS { FindExInfoStandard, FindExInfoBasic };
enum FINDEX_SEARCH_OPS { FindExSearchNameMatch };
enum GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard };
#define FIND_FIRST_EX_LARGE_FETCH 2u
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)))
#define MOVEFILE_REPLACE_EXISTING 0x1u
#define MOVEFILE_COPY_ALLOWED 0x2u
#define MOVEFILE_WRITE_THROUGH 0x8u
// FindFirstFile*W take "folder/pattern"; the pattern part matches like PathMatchSpecW.
HANDLE FindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS level, void* data, FINDEX_SEARCH_OPS op, void* filter,
                        DWORD flags);
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW* data);
BOOL FindNextFileW(HANDLE find, WIN32_FIND_DATAW* data);
BOOL FindClose(HANDLE find);
BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, void* data);
// An absolute, normalized path (no symlinks resolved), Windows buffer conventions.
DWORD GetFullPathNameW(LPCWSTR path, DWORD size, LPWSTR buffer, LPWSTR* filePart);
// Case-insensitive wildcard match ('*', '?'); several specs may be joined by ';'.
BOOL PathMatchSpecW(LPCWSTR name, LPCWSTR spec);
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
BOOL CreateDirectoryW(LPCWSTR path, void* security);
BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists);
BOOL DeleteFileW(LPCWSTR path);

// SHFileOperationW: FO_DELETE with FOF_ALLOWUNDO moves to the desktop's Trash (GIO).
#define FO_DELETE 0x3u
#define FOF_SILENT 0x4u
#define FOF_NOCONFIRMATION 0x10u
#define FOF_ALLOWUNDO 0x40u
struct SHFILEOPSTRUCTW {
  HWND hwnd;
  UINT wFunc;
  LPCWSTR pFrom;  // double-NUL-terminated list
  LPCWSTR pTo;
  WORD fFlags;
  BOOL fAnyOperationsAborted;
  void* hNameMappings;
  LPCWSTR lpszProgressTitle;
};
int SHFileOperationW(SHFILEOPSTRUCTW* op);

// Menus (Menus.cpp)
HMENU CreateMenu();
HMENU CreatePopupMenu();
BOOL DestroyMenu(HMENU menu);
BOOL AppendMenuW(HMENU menu, UINT flags, UINT_PTR idOrSub, LPCWSTR text);
BOOL InsertMenuW(HMENU menu, UINT position, UINT flags, UINT_PTR idOrSub, LPCWSTR text);
BOOL DeleteMenu(HMENU menu, UINT position, UINT flags);
BOOL RemoveMenu(HMENU menu, UINT position, UINT flags);
int GetMenuItemCount(HMENU menu);
HMENU GetSubMenu(HMENU menu, int pos);
UINT GetMenuItemID(HMENU menu, int pos);
DWORD CheckMenuItem(HMENU menu, UINT item, UINT check);
BOOL EnableMenuItem(HMENU menu, UINT item, UINT enable);
BOOL ModifyMenuW(HMENU menu, UINT position, UINT flags, UINT_PTR idOrSub, LPCWSTR text);
BOOL SetMenu(HWND hwnd, HMENU menu);
HMENU GetMenu(HWND hwnd);
BOOL DrawMenuBar(HWND hwnd);
BOOL TrackPopupMenu(HMENU menu, UINT flags, int x, int y, int reserved, HWND hwnd, const RECT* rect);

namespace verso::platform {
class Window;
// Connects the compat layer to the app's window: posted WM_APP+n messages and WM_COMMAND
// go to `onMessage`, timers to `onTimer`; `window` backs HWND.
void bindWin32(Window* window, HWND hwnd, void (*onMessage)(void*, UINT, WPARAM, LPARAM),
               void (*onTimer)(void*, UINT_PTR), void* ctx);
HWND appHwnd();
// Mouse button state for GetKeyState(VK_LBUTTON...) and the time of the event being handled.
void setMouseButton(int vk, bool down);
void setMessageTime(LONG ms);
// Height of the drawn menu bar (0 when there is none); GetClientRect excludes it.
float menuBarHeight();
}  // namespace verso::platform
