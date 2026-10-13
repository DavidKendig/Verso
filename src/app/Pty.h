#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace verso {

// A program running in a Windows pseudo console (ConPTY). Output is read on a
// background thread; the owner is told by a posted message (wParam 0: output is
// waiting, 1: the program exited) and collects it with takeOutput().
class Pty {
 public:
  Pty() = default;
  ~Pty();
  Pty(const Pty&) = delete;
  Pty& operator=(const Pty&) = delete;

  // Starts `commandLine` in `cwd`. `env` replaces or adds variables, and an entry
  // with an empty value removes one. Returns false with `error` set on failure.
  bool start(const std::wstring& commandLine, const std::wstring& cwd,
             const std::vector<std::pair<std::wstring, std::wstring>>& env, int cols, int rows, HWND notify,
             UINT message, std::wstring* error);
  void stop();

  void write(std::string_view bytes);
  void resize(int cols, int rows);
  std::string takeOutput();

  bool running() const;
  DWORD exitCode() const;

 private:
  struct Shared;
  std::shared_ptr<Shared> shared_;
  HPCON console_ = nullptr;
  HANDLE input_ = nullptr;  // our end of the program's input pipe
  HANDLE process_ = nullptr;
  int cols_ = 0, rows_ = 0;
};

}  // namespace verso
