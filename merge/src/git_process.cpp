#include "git_process.h"

#include <windows.h>

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
