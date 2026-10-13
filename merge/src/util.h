#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

std::wstring Widen(std::string_view s);
std::string Narrow(std::wstring_view s);

std::vector<std::string> Split(std::string_view s, char sep);
std::string Trim(std::string_view s);
std::wstring TrimW(std::wstring_view s);
bool StartsWith(std::string_view s, std::string_view prefix);

// Converts LF line endings to CRLF for Win32 edit controls.
std::wstring ToCrlf(std::wstring_view s);
// Converts CRLF to LF (used for commit messages sent to git).
std::string ToLf(std::string_view s);

std::wstring RelativeTime(int64_t unixTime);

// Case-insensitive match of every whitespace-separated token in `query`.
bool MatchesAllTokens(std::wstring_view haystack, std::wstring_view query);

// Converts a git (forward slash) relative path into a Windows absolute path.
std::wstring JoinRepoPath(const std::wstring& root, std::string_view relPath);

bool CopyToClipboard(void* hwnd, const std::wstring& text);
