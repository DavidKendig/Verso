#pragma once
#include <string>
#include <vector>

struct ProcResult {
    int exitCode = -1;
    std::string out;
    std::string err;
    bool ok() const { return exitCode == 0; }
};

// Quotes one argument following the MSVC CommandLineToArgvW rules.
std::wstring QuoteArg(const std::wstring& arg);

// Runs `exe` with `args` (no console window), feeding `input` to stdin and
// capturing stdout/stderr. Only the child's own pipe handles are inherited, so
// it is safe to call from several threads at once.
ProcResult RunProcess(const std::wstring& exe, const std::vector<std::wstring>& args,
                      const std::wstring& cwd, const std::string* input = nullptr);
