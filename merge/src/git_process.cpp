#include "git_process.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "core/TextUtil.h"
#endif

#include <algorithm>
#include <thread>

std::wstring QuoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring r = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            r.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            r.append(backslashes * 2 + 1, L'\\');
            r.push_back(L'"');
        } else {
            r.append(backslashes, L'\\');
            r.push_back(*it);
        }
    }
    r.push_back(L'"');
    return r;
}

#ifdef _WIN32
static void ReadAll(HANDLE h, std::string& out) {
    char buf[16384];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
}

static void WriteAll(HANDLE h, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        DWORD n = 0;
        DWORD chunk = (DWORD)std::min<size_t>(data.size() - off, 1 << 20);
        if (!WriteFile(h, data.data() + off, chunk, &n, nullptr) || n == 0) break;
        off += n;
    }
}

ProcResult RunProcess(const std::wstring& exe, const std::vector<std::wstring>& args,
                      const std::wstring& cwd, const std::string* input) {
    ProcResult res;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr, inR = nullptr, inW = nullptr;
    if (!CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0) ||
        !CreatePipe(&inR, &inW, &sa, 0)) {
        for (HANDLE h : {outR, outW, errR, errW, inR, inW})
            if (h) CloseHandle(h);
        res.err = "CreatePipe failed";
        return res;
    }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);

    HANDLE inherit[3] = {inR, outW, errW};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuf(attrSize);
    auto attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit),
                              nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inR;
    si.StartupInfo.hStdOutput = outW;
    si.StartupInfo.hStdError = errW;
    si.lpAttributeList = attrs;

    std::wstring cmd = QuoteArg(exe);
    for (const auto& a : args) {
        cmd.push_back(L' ');
        cmd += QuoteArg(a);
    }

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    DWORD lastError = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(errW);
    if (!ok) {
        CloseHandle(outR);
        CloseHandle(errR);
        CloseHandle(inW);
        res.err = "Failed to start process (Win32 error " + std::to_string(lastError) + ")";
        return res;
    }

    std::thread errThread([&] { ReadAll(errR, res.err); });
    std::thread inThread([&] {
        if (input && !input->empty()) WriteAll(inW, *input);
        CloseHandle(inW);
    });
    ReadAll(outR, res.out);
    errThread.join();
    inThread.join();

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    res.exitCode = (int)code;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(outR);
    CloseHandle(errR);
    return res;
}
#else
// POSIX: fork/exec with three pipes; stdout is read here, stderr and stdin on threads,
// as on Windows. Pipes are close-on-exec, so concurrent calls never leak them to children.
static void ReadAllFd(int fd, std::string& out) {
    char buf[16384];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) out.append(buf, (size_t)n);
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
}

static void WriteAllFd(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = write(fd, data.data() + off, data.size() - off);
        if (n > 0) off += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
}

ProcResult RunProcess(const std::wstring& exe, const std::vector<std::wstring>& args,
                      const std::wstring& cwd, const std::string* input) {
    ProcResult res;
    int out[2] = {-1, -1}, err[2] = {-1, -1}, in[2] = {-1, -1};
    if (pipe2(out, O_CLOEXEC) < 0 || pipe2(err, O_CLOEXEC) < 0 || pipe2(in, O_CLOEXEC) < 0) {
        for (int fd : {out[0], out[1], err[0], err[1], in[0], in[1]})
            if (fd >= 0) close(fd);
        res.err = "pipe failed";
        return res;
    }
    std::string file = verso::wideToUtf8(exe), dir = verso::wideToUtf8(cwd);
    std::vector<std::string> argv8 = {file};
    for (const auto& a : args) argv8.push_back(verso::wideToUtf8(a));
    std::vector<char*> argv;
    for (auto& a : argv8) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(err[1], 2);
        if (!dir.empty() && chdir(dir.c_str()) != 0) _exit(127);
        signal(SIGPIPE, SIG_DFL);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int startErrno = errno;
    close(in[0]);
    close(out[1]);
    close(err[1]);
    if (pid < 0) {
        close(out[0]);
        close(err[0]);
        close(in[1]);
        res.err = std::string("Failed to start process: ") + strerror(startErrno);
        return res;
    }
    std::thread errThread([&] { ReadAllFd(err[0], res.err); });
    std::thread inThread([&] {
        if (input && !input->empty()) WriteAllFd(in[1], *input);
        close(in[1]);
    });
    ReadAllFd(out[0], res.out);
    errThread.join();
    inThread.join();
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    res.exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 127 && res.out.empty() && res.err.empty())
        res.err = "Failed to start " + file;
    close(out[0]);
    close(err[0]);
    return res;
}
#endif
