#include "app/Pty.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <map>
#include <mutex>
#include <thread>

namespace verso {

namespace {

// ConPTY is resolved at run time so Verso still starts on systems without it.
using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
using ResizePseudoConsoleFn = HRESULT(WINAPI*)(HPCON, COORD);
using ClosePseudoConsoleFn = void(WINAPI*)(HPCON);

struct ConPty {
  CreatePseudoConsoleFn create = nullptr;
  ResizePseudoConsoleFn resize = nullptr;
  ClosePseudoConsoleFn close = nullptr;
};

const ConPty& conPty() {
  static const ConPty api = [] {
    ConPty a;
    if (HMODULE k = GetModuleHandleW(L"kernel32.dll")) {
      a.create = reinterpret_cast<CreatePseudoConsoleFn>(GetProcAddress(k, "CreatePseudoConsole"));
      a.resize = reinterpret_cast<ResizePseudoConsoleFn>(GetProcAddress(k, "ResizePseudoConsole"));
      a.close = reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(k, "ClosePseudoConsole"));
    }
    return a;
  }();
  return api;
}

std::wstring upper(std::wstring s) {
  for (auto& c : s) c = static_cast<wchar_t>(std::towupper(c));
  return s;
}

// This process's environment with `changes` applied, as a CreateProcess block.
std::wstring environmentBlock(const std::vector<std::pair<std::wstring, std::wstring>>& changes) {
  std::map<std::wstring, std::pair<std::wstring, std::wstring>> vars;  // UPPER name -> (name, value)
  if (wchar_t* env = GetEnvironmentStringsW()) {
    for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
      std::wstring entry(p);
      size_t eq = entry.find(L'=', 1);  // "=C:" style entries start with '='
      if (eq == std::wstring::npos) continue;
      std::wstring name = entry.substr(0, eq);
      vars[upper(name)] = {name, entry.substr(eq + 1)};
    }
    FreeEnvironmentStringsW(env);
  }
  for (const auto& [name, value] : changes) {
    if (value.empty())
      vars.erase(upper(name));
    else
      vars[upper(name)] = {name, value};
  }
  std::wstring block;
  for (const auto& [key, nv] : vars) block += nv.first + L"=" + nv.second + L'\0';
  block += L'\0';
  return block;
}

}  // namespace

struct Pty::Shared {
  std::mutex m;
  std::string output;
  std::atomic<bool> notified{false};  // a message is already queued
  std::atomic<bool> exited{false};
  std::atomic<DWORD> exitCode{0};
  std::atomic<HWND> notify{nullptr};  // cleared by stop(); threads read it
  UINT message = 0;
};

Pty::~Pty() { stop(); }

bool Pty::start(const std::wstring& commandLine, const std::wstring& cwd,
                const std::vector<std::pair<std::wstring, std::wstring>>& env, int cols, int rows, HWND notify,
                UINT message, std::wstring* error) {
  stop();
  const ConPty& api = conPty();
  if (!api.create) {
    if (error) *error = L"This version of Windows has no pseudo console (Windows 10 1809 or later is required).";
    return false;
  }
  HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
  if (!CreatePipe(&inRead, &inWrite, nullptr, 0) || !CreatePipe(&outRead, &outWrite, nullptr, 0)) {
    if (error) *error = L"Could not create the terminal pipes.";
    return false;
  }
  cols_ = std::max(1, cols);
  rows_ = std::max(1, rows);
  HRESULT hr = api.create({static_cast<SHORT>(cols_), static_cast<SHORT>(rows_)}, inRead, outWrite, 0, &console_);
  CloseHandle(inRead);  // the pseudo console holds its own references
  CloseHandle(outWrite);
  if (FAILED(hr)) {
    CloseHandle(inWrite);
    CloseHandle(outRead);
    console_ = nullptr;
    if (error) *error = L"Could not create the pseudo console.";
    return false;
  }
  input_ = inWrite;

  SIZE_T attrSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
  std::vector<char> attrBuf(attrSize);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring cmd = commandLine;  // CreateProcessW may modify its command line
  std::wstring block = environmentBlock(env);
  BOOL ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
            UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console_, sizeof(console_),
                                      nullptr, nullptr);
  if (ok) {
    si.lpAttributeList = attrs;
    ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, block.data(),
                        cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
  }
  DeleteProcThreadAttributeList(attrs);
  if (!ok) {
    DWORD code = GetLastError();
    CloseHandle(outRead);
    stop();
    if (error) *error = L"Could not start " + commandLine + L" (error " + std::to_wstring(code) + L").";
    return false;
  }
  CloseHandle(pi.hThread);
  process_ = pi.hProcess;

  shared_ = std::make_shared<Shared>();
  shared_->notify = notify;
  shared_->message = message;
  auto post = [](Shared& s, WPARAM kind) {
    HWND h = s.notify.load();
    if (h && (kind == 1 || !s.notified.exchange(true))) PostMessageW(h, s.message, kind, 0);
  };
  // Reader: runs until the pseudo console closes its end of the output pipe.
  std::thread([shared = shared_, outRead, post] {
    char buf[16384];
    DWORD got = 0;
    while (ReadFile(outRead, buf, sizeof buf, &got, nullptr) && got > 0) {
      {
        std::lock_guard<std::mutex> l(shared->m);
        shared->output.append(buf, got);
      }
      post(*shared, 0);
    }
    CloseHandle(outRead);
  }).detach();
  // Waiter: reports when the program exits.
  HANDLE waitHandle = nullptr;
  DuplicateHandle(GetCurrentProcess(), process_, GetCurrentProcess(), &waitHandle, SYNCHRONIZE | PROCESS_QUERY_INFORMATION,
                  FALSE, 0);
  std::thread([shared = shared_, waitHandle, post] {
    if (!waitHandle) return;
    WaitForSingleObject(waitHandle, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(waitHandle, &code);
    CloseHandle(waitHandle);
    shared->exitCode = code;
    shared->exited = true;
    post(*shared, 1);
  }).detach();
  return true;
}

void Pty::stop() {
  if (console_) {
    // Closing the console ends the programs attached to it and lets the reader finish.
    conPty().close(console_);
    console_ = nullptr;
  }
  if (input_) {
    CloseHandle(input_);
    input_ = nullptr;
  }
  if (process_) {
    CloseHandle(process_);
    process_ = nullptr;
  }
  if (shared_) shared_->notify = nullptr;  // late notifications go nowhere
  shared_.reset();
}

void Pty::write(std::string_view bytes) {
  if (!input_ || bytes.empty()) return;
  DWORD written = 0;
  WriteFile(input_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

void Pty::resize(int cols, int rows) {
  cols = std::max(1, cols);
  rows = std::max(1, rows);
  if (!console_ || (cols == cols_ && rows == rows_)) return;
  cols_ = cols;
  rows_ = rows;
  conPty().resize(console_, {static_cast<SHORT>(cols), static_cast<SHORT>(rows)});
}

std::string Pty::takeOutput() {
  if (!shared_) return {};
  std::lock_guard<std::mutex> l(shared_->m);
  shared_->notified = false;
  std::string out;
  out.swap(shared_->output);
  return out;
}

bool Pty::running() const { return shared_ && !shared_->exited; }

DWORD Pty::exitCode() const { return shared_ ? shared_->exitCode.load() : 0; }

}  // namespace verso
