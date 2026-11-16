// Pty on Linux: the shell runs in a pseudo terminal (forkpty). As with ConPTY on Windows,
// a background thread reads its output and posts `message` to the window (wParam 0: output
// is waiting, 1: the program exited); the UI thread collects it with takeOutput().
#include "app/Pty.h"

#include <errno.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "core/TextUtil.h"

namespace verso {

struct Pty::Shared {
  std::mutex mu;
  std::string output;
  std::atomic<bool> exited{false};
  std::atomic<int> exitCode{0};
  std::thread reader;
};

Pty::~Pty() { stop(); }

bool Pty::start(const std::wstring& commandLine, const std::wstring& cwd,
                const std::vector<std::pair<std::wstring, std::wstring>>& env, int cols, int rows, HWND notify,
                UINT message, std::wstring* error) {
  stop();
  winsize ws{};
  ws.ws_col = static_cast<unsigned short>(cols);
  ws.ws_row = static_cast<unsigned short>(rows);
  std::string cmd = wideToUtf8(commandLine), dir = wideToUtf8(cwd);
  std::vector<std::pair<std::string, std::string>> vars;
  for (const auto& [k, v] : env) vars.emplace_back(wideToUtf8(k), wideToUtf8(v));
  int master = -1;
  pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
  if (pid < 0) {
    if (error) *error = L"Couldn't create a pseudo terminal: " + utf8ToWide(strerror(errno));
    return false;
  }
  if (pid == 0) {
    // The child: the shell, in its folder, with Verso's variables.
    for (const auto& [k, v] : vars) {
      if (v.empty()) unsetenv(k.c_str());
      else setenv(k.c_str(), v.c_str(), 1);
    }
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    if (!dir.empty() && chdir(dir.c_str()) != 0) (void)!chdir(getenv("HOME") ? getenv("HOME") : "/");
    signal(SIGPIPE, SIG_DFL);
    execl("/bin/sh", "sh", "-c", ("exec " + cmd).c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  master_ = master;
  pid_ = pid;
  cols_ = cols;
  rows_ = rows;
  auto shared = std::make_shared<Shared>();
  shared_ = shared;
  shared->reader = std::thread([shared, master, pid, notify, message] {
    char buf[16384];
    for (;;) {
      ssize_t n = read(master, buf, sizeof buf);
      if (n > 0) {
        bool first;
        {
          std::lock_guard<std::mutex> lock(shared->mu);
          first = shared->output.empty();
          shared->output.append(buf, static_cast<size_t>(n));
        }
        if (first) PostMessageW(notify, message, 0, 0);
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      break;  // EIO: the shell (and everything on the terminal) is gone
    }
    int st = 0;
    if (waitpid(pid, &st, 0) == pid) shared->exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    shared->exited = true;
    PostMessageW(notify, message, 1, 0);
  });
  return true;
}

void Pty::stop() {
  if (pid_ > 0 && shared_ && !shared_->exited) kill(pid_, SIGHUP);
  if (shared_ && shared_->reader.joinable()) {
    if (!shared_->exited) {
      // The reader stops when the terminal closes; give the shell a moment, then insist.
      for (int i = 0; i < 50 && !shared_->exited; ++i) usleep(10000);
      if (!shared_->exited && pid_ > 0) kill(pid_, SIGKILL);
    }
    shared_->reader.join();
  }
  if (master_ >= 0) close(master_);
  master_ = -1;
  pid_ = -1;
}

void Pty::write(std::string_view bytes) {
  size_t off = 0;
  while (master_ >= 0 && off < bytes.size()) {
    ssize_t n = ::write(master_, bytes.data() + off, bytes.size() - off);
    if (n > 0) off += static_cast<size_t>(n);
    else if (n < 0 && errno == EINTR) continue;
    else break;
  }
}

void Pty::resize(int cols, int rows) {
  if (master_ < 0 || (cols == cols_ && rows == rows_)) return;
  cols_ = cols;
  rows_ = rows;
  winsize ws{};
  ws.ws_col = static_cast<unsigned short>(cols);
  ws.ws_row = static_cast<unsigned short>(rows);
  ioctl(master_, TIOCSWINSZ, &ws);
}

std::string Pty::takeOutput() {
  if (!shared_) return {};
  std::lock_guard<std::mutex> lock(shared_->mu);
  std::string out;
  out.swap(shared_->output);
  return out;
}

bool Pty::running() const { return shared_ && pid_ > 0 && !shared_->exited; }

DWORD Pty::exitCode() const { return shared_ ? static_cast<DWORD>(shared_->exitCode.load()) : 0; }

}  // namespace verso
