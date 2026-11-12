// Linux implementation of the Win32 calls Verso's shared code makes (see Win32Compat.h).
#include "app/linux/Win32Compat.h"

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <gio/gio.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#include "app/linux/Window.h"
#include "core/TextUtil.h"

namespace verso::platform {

namespace {
Window* gWindow = nullptr;
HWND gHwnd = nullptr;
void (*gOnMessage)(void*, UINT, WPARAM, LPARAM) = nullptr;
void (*gOnTimer)(void*, UINT_PTR) = nullptr;
void* gCtx = nullptr;
bool gButtons[8] = {};
LONG gMessageTime = 0;
HWND gCapture = nullptr;
}  // namespace

void bindWin32(Window* window, HWND hwnd, void (*onMessage)(void*, UINT, WPARAM, LPARAM),
               void (*onTimer)(void*, UINT_PTR), void* ctx) {
  gWindow = window;
  gHwnd = hwnd;
  gOnMessage = onMessage;
  gOnTimer = onTimer;
  gCtx = ctx;
}

HWND appHwnd() { return gHwnd; }

void setMouseButton(int vk, bool down) {
  if (vk >= 0 && vk < 8) gButtons[vk] = down;
}

void setMessageTime(LONG ms) { gMessageTime = ms; }

// Runs a program detached from Verso (its own session, no zombie left behind).
void spawnDetached(const std::vector<std::string>& argv) {
  if (argv.empty()) return;
  pid_t pid = fork();
  if (pid < 0) return;
  if (pid == 0) {
    setsid();
    if (fork() == 0) {
      std::vector<char*> a;
      for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
      a.push_back(nullptr);
      execvp(a[0], a.data());
      _exit(127);
    }
    _exit(0);
  }
  waitpid(pid, nullptr, 0);
}

}  // namespace verso::platform

using namespace verso::platform;

DWORD GetFileAttributesW(LPCWSTR path) {
  if (!path || !*path) return INVALID_FILE_ATTRIBUTES;
  std::string p = verso::wideToUtf8(path);
  struct stat st;
  if (stat(p.c_str(), &st) != 0) {
    if (lstat(p.c_str(), &st) == 0) return FILE_ATTRIBUTE_REPARSE_POINT;  // dangling link
    return INVALID_FILE_ATTRIBUTES;
  }
  DWORD a = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
  struct stat lst;
  if (lstat(p.c_str(), &lst) == 0 && S_ISLNK(lst.st_mode)) a |= FILE_ATTRIBUTE_REPARSE_POINT;
  if (access(p.c_str(), W_OK) != 0) a |= FILE_ATTRIBUTE_READONLY;
  return a;
}

ULONGLONG GetTickCount64() {
  using namespace std::chrono;
  return static_cast<ULONGLONG>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

DWORD GetTickCount() { return static_cast<DWORD>(GetTickCount64()); }

SHORT GetKeyState(int vk) {
  bool down = false;
  uint32_t m = gWindow ? gWindow->mods() : 0;
  switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: down = m & ModShift; break;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: down = m & ModCtrl; break;
    case VK_MENU: case VK_LMENU: case VK_RMENU: down = m & ModAlt; break;
    case VK_LWIN: case VK_RWIN: down = m & ModSuper; break;
    case VK_LBUTTON: case VK_RBUTTON: case VK_MBUTTON: down = gButtons[vk]; break;
    default: break;
  }
  return down ? static_cast<SHORT>(0x8000) : 0;
}

UINT GetDoubleClickTime() { return 400; }
LONG GetMessageTime() { return gMessageTime ? gMessageTime : static_cast<LONG>(GetTickCount64()); }

HWND SetCapture(HWND hwnd) {
  HWND prev = gCapture;
  gCapture = hwnd;
  return prev;
}
BOOL ReleaseCapture() {
  gCapture = nullptr;
  return TRUE;
}
HWND GetCapture() { return gCapture; }

UINT_PTR SetTimer(HWND, UINT_PTR id, UINT ms, void*) {
  if (!gWindow) return 0;
  gWindow->setTimer(id, ms, true, [id] {
    if (gOnTimer) gOnTimer(gCtx, id);
  });
  return id;
}

BOOL KillTimer(HWND, UINT_PTR id) {
  if (gWindow) gWindow->killTimer(id);
  return TRUE;
}

BOOL InvalidateRect(HWND, const RECT*, BOOL) {
  if (gWindow) gWindow->invalidate();
  return TRUE;
}

BOOL UpdateWindow(HWND) { return TRUE; }

HWND GetFocus() { return gWindow && gWindow->active() ? gHwnd : nullptr; }
HWND SetFocus(HWND hwnd) { return hwnd; }

BOOL PostMessageW(HWND, UINT msg, WPARAM wp, LPARAM lp) {
  // Background threads (git status, file index, images, PDF pages, terminal, AI tools)
  // post here; the message is handled on the UI thread like WM_APP+n on Windows.
  if (!gWindow) return FALSE;
  gWindow->post([msg, wp, lp] {
    if (gOnMessage) gOnMessage(gCtx, msg, wp, lp);
  });
  return TRUE;
}

HINSTANCE ShellExecuteW(HWND, LPCWSTR op, LPCWSTR file, LPCWSTR params, LPCWSTR dir, INT) {
  std::wstring f = file ? file : L"";
  std::wstring p = params ? params : L"";
  std::wstring o = op ? op : L"open";
  if (f.empty()) return reinterpret_cast<HINSTANCE>(2);
  // "explorer /select,<path>": show the item in the file manager (its folder).
  if (f == L"explorer.exe" || f == L"explorer") {
    std::wstring target = p;
    if (target.rfind(L"/select,", 0) == 0) target = target.substr(8);
    if (!target.empty() && target.front() == L'"') target = target.substr(1, target.size() - 2);
    DWORD a = GetFileAttributesW(target.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
      size_t slash = target.find_last_of(L'/');
      if (slash != std::wstring::npos) target = target.substr(0, slash ? slash : 1);
    }
    spawnDetached({"xdg-open", verso::wideToUtf8(target)});
    return reinterpret_cast<HINSTANCE>(33);
  }
  if (o == L"open" || o == L"explore") {
    if (!p.empty()) {  // run a program with arguments
      std::vector<std::string> argv = {verso::wideToUtf8(f)};
      std::string args = verso::wideToUtf8(p), cur;
      bool quoted = false;
      for (char c : args) {
        if (c == '"') quoted = !quoted;
        else if (c == ' ' && !quoted) {
          if (!cur.empty()) argv.push_back(cur);
          cur.clear();
        } else cur += c;
      }
      if (!cur.empty()) argv.push_back(cur);
      if (dir && *dir) {
        std::string d = verso::wideToUtf8(dir);
        pid_t pid = fork();
        if (pid == 0) {
          setsid();
          if (chdir(d.c_str()) != 0) _exit(127);
          std::vector<char*> a;
          for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
          a.push_back(nullptr);
          execvp(a[0], a.data());
          _exit(127);
        }
        return reinterpret_cast<HINSTANCE>(33);
      }
      spawnDetached(argv);
      return reinterpret_cast<HINSTANCE>(33);
    }
    spawnDetached({"xdg-open", verso::wideToUtf8(f)});  // a URL, a file or a folder
    return reinterpret_cast<HINSTANCE>(33);
  }
  return reinterpret_cast<HINSTANCE>(2);
}

BOOL SetWindowTextW(HWND, LPCWSTR text) {
  if (gWindow && text) gWindow->setTitle(verso::wideToUtf8(text));
  return TRUE;
}

BOOL GetClientRect(HWND, RECT* rect) {
  if (!rect) return FALSE;
  *rect = RECT{};
  if (!gWindow) return FALSE;
  rect->right = gWindow->width();
  rect->bottom = gWindow->height() - static_cast<LONG>(menuBarHeight());
  if (rect->bottom < 0) rect->bottom = 0;
  return TRUE;
}

// "Screen" coordinates are the window surface's: client coordinates plus the menu bar.
BOOL ClientToScreen(HWND, POINT* pt) {
  if (pt) pt->y += static_cast<LONG>(menuBarHeight());
  return TRUE;
}
BOOL ScreenToClient(HWND, POINT* pt) {
  if (pt) pt->y -= static_cast<LONG>(menuBarHeight());
  return TRUE;
}

namespace verso::platform {
float gPointerX = 0, gPointerY = 0;  // surface coordinates (AppLinux.cpp keeps them current)
}

BOOL GetCursorPos(POINT* pt) {
  if (!pt) return FALSE;
  pt->x = static_cast<LONG>(verso::platform::gPointerX);
  pt->y = static_cast<LONG>(verso::platform::gPointerY);
  return TRUE;
}

BOOL MessageBeep(UINT) { return TRUE; }

int _wcsicmp(const wchar_t* a, const wchar_t* b) { return wcscasecmp(a, b); }
int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n) { return wcsncasecmp(a, b, n); }

errno_t wcsncpy_s(wchar_t* dst, size_t dstSize, const wchar_t* src, size_t count) {
  if (!dst || !dstSize) return 22;
  size_t n = wcslen(src);
  if (count != _TRUNCATE && count < n) n = count;
  if (n >= dstSize) n = dstSize - 1;
  wmemcpy(dst, src, n);
  dst[n] = 0;
  return 0;
}

// --- errors --------------------------------------------------------------------------------

namespace {
thread_local int gLastError = 0;
template <typename T>
T fail(T value) {
  gLastError = errno;
  return value;
}
}  // namespace

DWORD GetLastError() { return static_cast<DWORD>(gLastError); }

// --- windows -------------------------------------------------------------------------------

BOOL MoveWindow(HWND, int, int, int, int, BOOL) { return FALSE; }
BOOL ShowWindow(HWND, int) { return FALSE; }
BOOL IsChild(HWND, HWND) { return FALSE; }
HCURSOR LoadCursorW(HINSTANCE, LPCWSTR) { return nullptr; }
HCURSOR SetCursor(HCURSOR) { return nullptr; }

int SHCreateDirectoryExW(HWND, LPCWSTR path, void*) {
  if (!path || !*path) return EINVAL;
  std::string p = verso::wideToUtf8(path);
  for (size_t i = 1; i <= p.size(); ++i) {
    if (i < p.size() && p[i] != '/') continue;
    std::string part = p.substr(0, i);
    if (mkdir(part.c_str(), 0777) != 0 && errno != EEXIST) return errno;
  }
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode) ? 0 : ENOTDIR;
}

// --- keyboard ------------------------------------------------------------------------------

SHORT VkKeyScanW(wchar_t ch) {
  // US layout, unshifted and shifted (shift adds 0x100, as on Windows).
  static const char plain[] = ";=,-./`[\\]'";
  static const char shifted[] = ":+<_>?~{|}\"";
  static const BYTE vks[] = {VK_OEM_1, VK_OEM_PLUS, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PERIOD, VK_OEM_2,
                             VK_OEM_3, VK_OEM_4,    VK_OEM_5,     VK_OEM_6,     VK_OEM_7};
  if (ch >= L'a' && ch <= L'z') return static_cast<SHORT>('A' + (ch - L'a'));
  if (ch >= L'A' && ch <= L'Z') return static_cast<SHORT>(0x100 | ch);
  if (ch >= L'0' && ch <= L'9') return static_cast<SHORT>(ch);
  if (ch == L' ') return VK_SPACE;
  for (size_t i = 0; i < sizeof vks; ++i) {
    if (ch == static_cast<wchar_t>(plain[i])) return vks[i];
    if (ch == static_cast<wchar_t>(shifted[i])) return static_cast<SHORT>(0x100 | vks[i]);
  }
  static const char digitsShifted[] = ")!@#$%^&*(";
  for (int i = 0; i < 10; ++i)
    if (ch == static_cast<wchar_t>(digitsShifted[i])) return static_cast<SHORT>(0x100 | ('0' + i));
  return -1;
}

UINT MapVirtualKeyW(UINT code, UINT mapType) {
  if (mapType != MAPVK_VK_TO_CHAR) return 0;
  if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9') || code == VK_SPACE) return code;
  switch (code) {
    case VK_OEM_1: return ';';
    case VK_OEM_PLUS: return '=';
    case VK_OEM_COMMA: return ',';
    case VK_OEM_MINUS: return '-';
    case VK_OEM_PERIOD: return '.';
    case VK_OEM_2: return '/';
    case VK_OEM_3: return '`';
    case VK_OEM_4: return '[';
    case VK_OEM_5: return '\\';
    case VK_OEM_6: return ']';
    case VK_OEM_7: return '\'';
    case VK_MULTIPLY: return '*';
    case VK_ADD: return '+';
    case VK_SUBTRACT: return '-';
    case VK_DECIMAL: return '.';
    case VK_DIVIDE: return '/';
  }
  if (code >= VK_NUMPAD0 && code <= VK_NUMPAD0 + 9) return '0' + (code - VK_NUMPAD0);
  return 0;
}

// --- files ---------------------------------------------------------------------------------

namespace {

FILETIME fileTime(const timespec& t) {
  // 100 ns ticks since 1601-01-01 (11644473600 s before the Unix epoch).
  uint64_t ticks = (static_cast<uint64_t>(t.tv_sec) + 11644473600ull) * 10000000ull + t.tv_nsec / 100;
  return {static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
}

// Fills the attribute fields shared by WIN32_FIND_DATAW and WIN32_FILE_ATTRIBUTE_DATA.
template <typename T>
bool statInto(const std::string& path, T& d) {
  struct stat st, lst;
  bool link = lstat(path.c_str(), &lst) == 0 && S_ISLNK(lst.st_mode);
  if (stat(path.c_str(), &st) != 0) {
    if (!link) return false;
    st = lst;  // a dangling link: describe the link itself
  }
  d.dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
  if (link) d.dwFileAttributes |= FILE_ATTRIBUTE_REPARSE_POINT;
  if (!(st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH))) d.dwFileAttributes |= FILE_ATTRIBUTE_READONLY;
  d.ftCreationTime = fileTime(st.st_ctim);
  d.ftLastAccessTime = fileTime(st.st_atim);
  d.ftLastWriteTime = fileTime(st.st_mtim);
  const uint64_t size = S_ISDIR(st.st_mode) ? 0 : static_cast<uint64_t>(st.st_size);
  d.nFileSizeHigh = static_cast<DWORD>(size >> 32);
  d.nFileSizeLow = static_cast<DWORD>(size);
  return true;
}

bool matchSpec(const std::string& name, const std::string& spec) {
  size_t start = 0;
  for (;;) {
    size_t end = spec.find(';', start);
    std::string one = spec.substr(start, end == std::string::npos ? std::string::npos : end - start);
    while (!one.empty() && one.front() == ' ') one.erase(0, 1);
    while (!one.empty() && one.back() == ' ') one.pop_back();
    if (!one.empty() && (one == "*" || one == "*.*" || fnmatch(one.c_str(), name.c_str(), FNM_CASEFOLD) == 0))
      return true;
    if (end == std::string::npos) return false;
    start = end + 1;
  }
}

struct Find {
  DIR* dir = nullptr;
  std::string folder, pattern;
};

bool nextMatch(Find& f, WIN32_FIND_DATAW* out) {
  while (dirent* e = readdir(f.dir)) {
    std::string name = e->d_name;
    if (!matchSpec(name, f.pattern)) continue;
    WIN32_FIND_DATAW d{};
    if (!statInto(f.folder + "/" + name, d)) continue;  // vanished meanwhile
    std::wstring w = verso::utf8ToWide(name);
    if (w.size() >= std::size(d.cFileName)) continue;
    wmemcpy(d.cFileName, w.c_str(), w.size() + 1);
    *out = d;
    return true;
  }
  return false;
}

}  // namespace

HANDLE FindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS, void* data, FINDEX_SEARCH_OPS, void*, DWORD) {
  std::string p = verso::wideToUtf8(pattern ? pattern : L"");
  size_t slash = p.find_last_of('/');
  auto* f = new Find;
  f->folder = slash == std::string::npos ? "." : (slash == 0 ? "/" : p.substr(0, slash));
  f->pattern = slash == std::string::npos ? p : p.substr(slash + 1);
  f->dir = opendir(f->folder.c_str());
  if (!f->dir) {
    gLastError = errno;
    delete f;
    return INVALID_HANDLE_VALUE;
  }
  if (!nextMatch(*f, static_cast<WIN32_FIND_DATAW*>(data))) {
    gLastError = ENOENT;
    closedir(f->dir);
    delete f;
    return INVALID_HANDLE_VALUE;
  }
  return f;
}

HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW* data) {
  return FindFirstFileExW(pattern, FindExInfoStandard, data, FindExSearchNameMatch, nullptr, 0);
}

BOOL FindNextFileW(HANDLE find, WIN32_FIND_DATAW* data) {
  if (!find || find == INVALID_HANDLE_VALUE) return FALSE;
  return nextMatch(*static_cast<Find*>(find), data) ? TRUE : FALSE;
}

BOOL FindClose(HANDLE find) {
  if (!find || find == INVALID_HANDLE_VALUE) return FALSE;
  auto* f = static_cast<Find*>(find);
  closedir(f->dir);
  delete f;
  return TRUE;
}

BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS, void* data) {
  if (!path || !*path) return FALSE;
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (!statInto(verso::wideToUtf8(path), d)) return fail(FALSE);
  *static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(data) = d;
  return TRUE;
}

DWORD GetFullPathNameW(LPCWSTR path, DWORD size, LPWSTR buffer, LPWSTR* filePart) {
  if (!path || !*path) return 0;
  std::wstring in = path;
  if (in[0] != L'/') {
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) return fail(0u);
    in = verso::utf8ToWide(cwd) + L"/" + in;
  }
  // Resolve "." and ".." textually, keeping symlinks as written.
  std::vector<std::wstring> parts;
  size_t i = 0;
  while (i <= in.size()) {
    size_t j = in.find(L'/', i);
    if (j == std::wstring::npos) j = in.size();
    std::wstring seg = in.substr(i, j - i);
    if (seg == L"..") {
      if (!parts.empty()) parts.pop_back();
    } else if (!seg.empty() && seg != L".") {
      parts.push_back(seg);
    }
    i = j + 1;
  }
  std::wstring out;
  for (const auto& s : parts) out += L"/" + s;
  if (out.empty()) out = L"/";
  if (in.size() > 1 && in.back() == L'/' && out != L"/") out += L'/';
  if (out.size() + 1 > size || !buffer) return static_cast<DWORD>(out.size() + 1);
  wmemcpy(buffer, out.c_str(), out.size() + 1);
  if (filePart) {
    size_t s = out.find_last_of(L'/');
    *filePart = s == std::wstring::npos || s + 1 >= out.size() ? nullptr : buffer + s + 1;
  }
  return static_cast<DWORD>(out.size());
}

BOOL PathMatchSpecW(LPCWSTR name, LPCWSTR spec) {
  if (!name || !spec) return FALSE;
  return matchSpec(verso::wideToUtf8(name), verso::wideToUtf8(spec)) ? TRUE : FALSE;
}

BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
  std::string a = verso::wideToUtf8(from), b = verso::wideToUtf8(to);
  if (!(flags & MOVEFILE_REPLACE_EXISTING)) {
    // Without MOVEFILE_REPLACE_EXISTING, Windows refuses to replace the target.
    struct stat st;
    if (lstat(b.c_str(), &st) == 0 && a != b) {
      errno = EEXIST;
      return fail(FALSE);
    }
  }
  if (rename(a.c_str(), b.c_str()) != 0) return fail(FALSE);
  if (flags & MOVEFILE_WRITE_THROUGH) sync();
  return TRUE;
}

BOOL CreateDirectoryW(LPCWSTR path, void*) {
  return mkdir(verso::wideToUtf8(path).c_str(), 0777) == 0 ? TRUE : fail(FALSE);
}

BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists) {
  std::string a = verso::wideToUtf8(from), b = verso::wideToUtf8(to);
  int in = open(a.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) return fail(FALSE);
  struct stat st;
  fstat(in, &st);
  int out = open(b.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (failIfExists ? O_EXCL : O_TRUNC), st.st_mode & 0777);
  if (out < 0) {
    gLastError = errno;
    close(in);
    return FALSE;
  }
  char buf[65536];
  ssize_t n;
  bool ok = true;
  while (ok && (n = read(in, buf, sizeof buf)) != 0) {
    if (n < 0) {
      if (errno == EINTR) continue;
      ok = false;
      break;
    }
    for (ssize_t off = 0; off < n;) {
      ssize_t w = write(out, buf + off, static_cast<size_t>(n - off));
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) {
        ok = false;
        break;
      }
      off += w;
    }
  }
  if (!ok) gLastError = errno;
  close(in);
  if (close(out) != 0 && ok) ok = fail(false);
  return ok ? TRUE : FALSE;
}

BOOL DeleteFileW(LPCWSTR path) { return unlink(verso::wideToUtf8(path).c_str()) == 0 ? TRUE : fail(FALSE); }

int SHFileOperationW(SHFILEOPSTRUCTW* op) {
  if (!op || op->wFunc != FO_DELETE || !op->pFrom) return 1;
  int rc = 0;
  for (const wchar_t* p = op->pFrom; *p; p += wcslen(p) + 1) {
    GFile* file = g_file_new_for_path(verso::wideToUtf8(p).c_str());
    GError* err = nullptr;
    if (!(op->fFlags & FOF_ALLOWUNDO) || !g_file_trash(file, nullptr, &err)) {
      if (err) g_error_free(err);
      rc = 1;  // no Trash here (another file system): leave the file rather than erase it
    }
    g_object_unref(file);
  }
  return rc;
}
