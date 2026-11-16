#include "util.h"

#include <windows.h>  // Linux: src/app/linux/win32/windows.h

#include <ctime>
#include <cwctype>

#ifdef _WIN32
std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
#else
#include "core/TextUtil.h"
std::wstring Widen(std::string_view s) { return verso::utf8ToWide(s); }
std::string Narrow(std::wstring_view w) { return verso::wideToUtf8(w); }
#define localtime_s(tmp, tp) localtime_r((tp), (tmp))
#endif

std::vector<std::string> Split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::string Trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return std::string(s.substr(a, b - a));
}

std::wstring TrimW(std::wstring_view s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return std::wstring(s.substr(a, b - a));
}

bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::wstring ToCrlf(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size() + s.size() / 16);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\n' && (i == 0 || s[i - 1] != L'\r')) out.push_back(L'\r');
        out.push_back(s[i]);
    }
    return out;
}

std::string ToLf(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != '\r') out.push_back(c);
    return out;
}

static std::wstring FormatTime(int64_t unixTime) {
    time_t t = (time_t)unixTime;
    tm local{};
    localtime_s(&local, &t);
    wchar_t buf[64];
    wcsftime(buf, 64, L"%Y-%m-%d %H:%M", &local);
    return buf;
}

std::wstring RelativeTime(int64_t unixTime) {
    int64_t now = (int64_t)time(nullptr);
    int64_t d = now - unixTime;
    if (d < 0) return FormatTime(unixTime);
    if (d < 60) return L"just now";
    if (d < 3600) return std::to_wstring(d / 60) + L" min ago";
    if (d < 86400) {
        int64_t h = d / 3600;
        return std::to_wstring(h) + (h == 1 ? L" hour ago" : L" hours ago");
    }
    if (d < 86400 * 30) {
        int64_t days = d / 86400;
        return std::to_wstring(days) + (days == 1 ? L" day ago" : L" days ago");
    }
    time_t t = (time_t)unixTime;
    tm local{};
    localtime_s(&local, &t);
    wchar_t buf[64];
    wcsftime(buf, 64, L"%d %b %Y", &local);
    return buf;
}

static std::wstring Lower(std::wstring_view s) {
    std::wstring r(s);
#ifdef _WIN32
    if (!r.empty()) CharLowerBuffW(r.data(), (DWORD)r.size());
#else
    for (auto& c : r) c = (wchar_t)towlower(c);
#endif
    return r;
}

bool MatchesAllTokens(std::wstring_view haystack, std::wstring_view query) {
    std::wstring h = Lower(haystack);
    std::wstring q = Lower(query);
    size_t i = 0;
    while (i < q.size()) {
        while (i < q.size() && iswspace(q[i])) ++i;
        size_t j = i;
        while (j < q.size() && !iswspace(q[j])) ++j;
        if (j > i && h.find(q.substr(i, j - i)) == std::wstring::npos) return false;
        i = j;
    }
    return true;
}

std::wstring JoinRepoPath(const std::wstring& root, std::string_view relPath) {
    std::wstring p = root;
#ifdef _WIN32
    if (!p.empty() && p.back() != L'\\') p.push_back(L'\\');
    for (wchar_t c : Widen(relPath)) p.push_back(c == L'/' ? L'\\' : c);
#else
    if (!p.empty() && p.back() != L'/') p.push_back(L'/');
    p += Widen(relPath);
#endif
    return p;
}

#ifndef _WIN32
namespace verso {
void setClipboardText(HWND owner, const std::wstring& text);  // src/app/linux/Clipboard.cpp
}
bool CopyToClipboard(void* hwnd, const std::wstring& text) {
    verso::setClipboardText((HWND)hwnd, text);
    return true;
}
#else
bool CopyToClipboard(void* hwnd, const std::wstring& text) {
    if (!OpenClipboard((HWND)hwnd)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
    return mem != nullptr;
}
#endif
