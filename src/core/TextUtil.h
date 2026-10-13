#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace verso {

inline constexpr std::wstring_view kDefaultWordSeparators = L"./\\()\"'-:,.;<>~!@#$%^&*|+=[]{}`~?";

enum class CharClass { Space, Newline, Word, Punct };

inline bool isHighSurrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
inline bool isLowSurrogate(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }
inline bool isSpace(wchar_t c) {
  return c == L' ' || c == L'\t' || c == L'\r' || c == L'\f' || c == L'\v' || c == 0xA0 || c == 0x3000;
}
inline bool isBlank(wchar_t c) { return c == L' ' || c == L'\t'; }

CharClass classify(wchar_t c, std::wstring_view separators);
inline bool isWordChar(wchar_t c, std::wstring_view separators) {
  return classify(c, separators) == CharClass::Word;
}

// Display width of a single UTF-16 code unit in columns. Tabs are handled by
// the caller because their width depends on the current column.
int unitColumns(wchar_t c);

std::wstring toUpper(std::wstring_view s);
std::wstring toLower(std::wstring_view s);
std::wstring toTitle(std::wstring_view s);
std::wstring swapCase(std::wstring_view s);

std::wstring_view trim(std::wstring_view s);
size_t leadingBlanks(std::wstring_view line);
bool startsWith(std::wstring_view s, std::wstring_view prefix);
// Case-insensitive (invariant culture, so "Æ" and "æ" match).
bool startsWithNoCase(std::wstring_view s, std::wstring_view prefix);
bool endsWithNoCase(std::wstring_view s, std::wstring_view suffix);
bool endsWith(std::wstring_view s, std::wstring_view suffix);
std::vector<std::wstring> splitLines(std::wstring_view s);
std::wstring normalizeNewlines(std::wstring_view s);

// UTF-8 <-> wide strings (UTF-16 where wchar_t is 16-bit, as on Windows; UTF-32
// elsewhere). Malformed input becomes U+FFFD. Portable: the same code on every OS.
std::wstring utf8ToWide(std::string_view s);
std::string wideToUtf8(std::wstring_view s);
// UTF-16 code units <-> wide strings (a copy where wchar_t is 16-bit). Unpaired
// surrogates are kept as they are, so a file's text round-trips unchanged.
std::wstring utf16ToWide(std::u16string_view s);
std::u16string wideToUtf16(std::wstring_view s);
// Strict UTF-8 check: rejects overlong forms, surrogates and code points past U+10FFFF.
bool isValidUtf8(std::string_view s);
// Windows-1252 (Western) <-> wide strings. Unassigned bytes map to the C1 controls
// U+0081..U+009D, as Windows does, so every byte round-trips.
std::wstring cp1252ToWide(std::string_view s);
// Characters Windows-1252 cannot store become '?'; `lost` reports whether any did.
std::string wideToCp1252(std::wstring_view s, bool* lost = nullptr);

}  // namespace verso
