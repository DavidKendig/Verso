// verso-mcp: connects an MCP client (Claude Code, Codex, Gemini CLI, ...) to Verso.
//
// MCP clients start this program and talk JSON-RPC over its stdin/stdout. It relays
// those bytes to the running Verso's MCP pipe, which only the same Windows user can
// open. The pipe is found through VERSO_MCP_PIPE (set in Verso's terminal), else the
// first running Verso; when none runs, Verso.exe next to this program is started.
#include <windows.h>

#include <string>
#include <thread>

namespace {

constexpr DWORD kChunk = 64 * 1024;

std::wstring envVar(const wchar_t* name) {
  wchar_t buf[512];
  DWORD n = GetEnvironmentVariableW(name, buf, 512);
  return n > 0 && n < 512 ? std::wstring(buf, n) : std::wstring();
}

// The first \\.\pipe\verso-mcp-* pipe, if a Verso is running.
std::wstring findVersoPipe() {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(L"\\\\.\\pipe\\*", &fd);
  if (h == INVALID_HANDLE_VALUE) return {};
  std::wstring found;
  do {
    if (wcsncmp(fd.cFileName, L"verso-mcp-", 10) == 0) {
      found = L"\\\\.\\pipe\\" + std::wstring(fd.cFileName);
      break;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return found;
}

bool launchVerso() {
  wchar_t self[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
  if (n == 0 || n >= std::size(self)) return false;
  std::wstring dir(self, n);
  dir.resize(dir.find_last_of(L'\\') + 1);
  std::wstring exe = dir + L"Verso.exe";
  std::wstring cmd = L"\"" + exe + L"\"";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, dir.c_str(), &si,
                      &pi))
    return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

// One read or write on the overlapped pipe; returns the bytes moved (0 on failure).
DWORD pipeIo(HANDLE pipe, char* data, DWORD size, bool write) {
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ov.hEvent) return 0;
  BOOL ok = write ? WriteFile(pipe, data, size, nullptr, &ov) : ReadFile(pipe, data, size, nullptr, &ov);
  DWORD done = 0;
  if (ok || GetLastError() == ERROR_IO_PENDING) {
    if (!GetOverlappedResult(pipe, &ov, &done, TRUE)) done = 0;
  }
  CloseHandle(ov.hEvent);
  return done;
}

void fail(const char* message) {
  HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  DWORD wrote = 0;
  std::string text = std::string("verso-mcp: ") + message + "\n";
  if (err && err != INVALID_HANDLE_VALUE) WriteFile(err, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
}

}  // namespace

int wmain() {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!in || in == INVALID_HANDLE_VALUE || !out || out == INVALID_HANDLE_VALUE) {
    fail("run by an MCP client, which provides stdin and stdout");
    return 2;
  }
  std::wstring name = envVar(L"VERSO_MCP_PIPE");
  if (name.empty()) name = findVersoPipe();
  if (name.empty()) {
    if (!launchVerso()) {
      fail("Verso is not running and Verso.exe could not be started");
      return 1;
    }
    for (int i = 0; i < 100 && name.empty(); ++i) {  // up to 20 seconds
      Sleep(200);
      name = findVersoPipe();
    }
  }
  HANDLE pipe = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 50 && pipe == INVALID_HANDLE_VALUE && !name.empty(); ++attempt) {
    // Overlapped, so the two relay directions can read and write the pipe at the same time.
    pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                       nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      if (GetLastError() != ERROR_PIPE_BUSY) Sleep(200);
      WaitNamedPipeW(name.c_str(), 2000);
    }
  }
  if (pipe == INVALID_HANDLE_VALUE) {
    fail("could not connect to Verso's MCP server (is \"mcp_server\" turned off in Preferences?)");
    return 1;
  }
  // Client -> Verso. When the client closes stdin, the session is over.
  std::thread([in, pipe] {
    char buf[kChunk];
    DWORD got = 0;
    while (ReadFile(in, buf, kChunk, &got, nullptr) && got > 0)
      if (!pipeIo(pipe, buf, got, true)) break;
    ExitProcess(0);
  }).detach();
  // Verso -> client. When Verso closes, the session is over.
  static char buf[kChunk];
  DWORD wrote = 0;
  for (;;) {
    const DWORD got = pipeIo(pipe, buf, kChunk, false);
    if (got == 0 || !WriteFile(out, buf, got, &wrote, nullptr)) break;
    FlushFileBuffers(out);
  }
  return 0;
}
