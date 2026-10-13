#include "core/TextUtil.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cwctype>

namespace verso {

CharClass classify(wchar_t c, std::wstring_view separators) {
  if (c == L'\n') return CharClass::Newline;
  if (isSpace(c)) return CharClass::Space;
  if (separators.find(c) != std::wstring_view::npos) return CharClass::Punct;
  return CharClass::Word;
}

int unitColumns(wchar_t c) {
  if (isLowSurrogate(c)) return 0;
  if (isHighSurrogate(c)) return 2;
  if ((c >= 0x0300 && c <= 0x036F) || (c >= 0x200B && c <= 0x200F) || (c >= 0xFE00 && c <= 0xFE0F)) return 0;
  if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) ||
      (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
      (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
      (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6))
    return 2;
  return 1;
}

#ifdef _WIN32
// Invariant-culture case mapping (handles letters such as "Æ" the same everywhere).
static std::wstring mapCase(std::wstring_view s, DWORD flags) {
  if (s.empty()) return {};
  int n = LCMapStringEx(LOCALE_NAME_INVARIANT, flags, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr,
                        nullptr, 0);
  if (n <= 0) return std::wstring(s);
  std::wstring out(static_cast<size_t>(n), L'\0');
  LCMapStringEx(LOCALE_NAME_INVARIANT, flags, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr,
                0);
  return out;
}

std::wstring toUpper(std::wstring_view s) { return mapCase(s, LCMAP_UPPERCASE); }
std::wstring toLower(std::wstring_view s) { return mapCase(s, LCMAP_LOWERCASE); }
#else
std::wstring toUpper(std::wstring_view s) {
  std::wstring out(s);
  for (auto& c : out) c = static_cast<wchar_t>(std::towupper(c));
  return out;
}

std::wstring toLower(std::wstring_view s) {
  std::wstring out(s);
  for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
  return out;
}
#endif

std::wstring toTitle(std::wstring_view s) {
  std::wstring out = toLower(s);
  bool start = true;
  for (auto& c : out) {
    if (std::iswalnum(c)) {
      if (start) c = static_cast<wchar_t>(std::towupper(c));
      start = false;
    } else {
      start = c != L'\'';
    }
  }
  return out;
}

std::wstring swapCase(std::wstring_view s) {
  std::wstring out(s);
  for (auto& c : out) {
    if (std::iswupper(c))
      c = static_cast<wchar_t>(std::towlower(c));
    else if (std::iswlower(c))
      c = static_cast<wchar_t>(std::towupper(c));
  }
  return out;
}

bool startsWithNoCase(std::wstring_view s, std::wstring_view prefix) {
  return s.size() >= prefix.size() && toLower(s.substr(0, prefix.size())) == toLower(prefix);
}

bool endsWithNoCase(std::wstring_view s, std::wstring_view suffix) {
  return s.size() >= suffix.size() && toLower(s.substr(s.size() - suffix.size())) == toLower(suffix);
}

std::wstring_view trim(std::wstring_view s) {
  size_t b = 0, e = s.size();
  while (b < e && isSpace(s[b])) ++b;
  while (e > b && isSpace(s[e - 1])) --e;
  return s.substr(b, e - b);
}

size_t leadingBlanks(std::wstring_view line) {
  size_t i = 0;
  while (i < line.size() && isBlank(line[i])) ++i;
  return i;
}

bool startsWith(std::wstring_view s, std::wstring_view prefix) {
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool endsWith(std::wstring_view s, std::wstring_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::vector<std::wstring> splitLines(std::wstring_view s) {
  std::vector<std::wstring> out;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == L'\n') {
      out.emplace_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

std::wstring normalizeNewlines(std::wstring_view s) {
  std::wstring out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == L'\r') {
      out.push_back(L'\n');
      if (i + 1 < s.size() && s[i + 1] == L'\n') ++i;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

namespace {

constexpr char32_t kReplacement = 0xFFFD;

// Decodes one UTF-8 sequence at s[i]; returns its length (at least 1), or 0 when the
// sequence is malformed (then `cp` is U+FFFD and one byte should be skipped).
size_t decodeUtf8(std::string_view s, size_t i, char32_t& cp) {
  const auto b = static_cast<unsigned char>(s[i]);
  size_t len;
  char32_t min;
  if (b < 0x80) {
    cp = b;
    return 1;
  } else if ((b & 0xE0) == 0xC0) {
    cp = b & 0x1F;
    len = 2;
    min = 0x80;
  } else if ((b & 0xF0) == 0xE0) {
    cp = b & 0x0F;
    len = 3;
    min = 0x800;
  } else if ((b & 0xF8) == 0xF0) {
    cp = b & 0x07;
    len = 4;
    min = 0x10000;
  } else {
    cp = kReplacement;
    return 0;
  }
  if (i + len > s.size()) {
    cp = kReplacement;
    return 0;
  }
  for (size_t k = 1; k < len; ++k) {
    const auto c = static_cast<unsigned char>(s[i + k]);
    if ((c & 0xC0) != 0x80) {
      cp = kReplacement;
      return 0;
    }
    cp = (cp << 6) | (c & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    cp = kReplacement;
    return 0;
  }
  return len;
}

void appendWide(std::wstring& out, char32_t cp) {
  if constexpr (sizeof(wchar_t) == 2) {
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out += static_cast<wchar_t>(0xD800 + (cp >> 10));
      out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
      return;
    }
  }
  out += static_cast<wchar_t>(cp);
}

void appendUtf8(std::string& out, char32_t c) {
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else if (c < 0x800) {
    out += static_cast<char>(0xC0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += static_cast<char>(0xE0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (c >> 18));
    out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
}

// Windows-1252 bytes 0x80-0x9F. Unassigned positions keep their C1 code points.
constexpr char16_t kCp1252High[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

}  // namespace

std::wstring utf8ToWide(std::string_view s) {
  std::wstring out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    char32_t cp;
    size_t len = decodeUtf8(s, i, cp);
    appendWide(out, cp);
    i += len ? len : 1;
  }
  return out;
}

bool isValidUtf8(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    char32_t cp;
    size_t len = decodeUtf8(s, i, cp);
    if (!len) return false;
    i += len;
  }
  return true;
}

std::string wideToUtf8(std::wstring_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    char32_t cp = static_cast<char32_t>(s[i]);
    if constexpr (sizeof(wchar_t) == 2) {
      if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<char32_t>(s[i + 1]) - 0xDC00);
        ++i;
      } else if (cp >= 0xD800 && cp <= 0xDFFF) {
        cp = kReplacement;  // a lone surrogate
      }
    }
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = kReplacement;
    appendUtf8(out, cp);
  }
  return out;
}

std::wstring utf16ToWide(std::u16string_view s) {
  std::wstring out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    char32_t c = s[i];
    if constexpr (sizeof(wchar_t) != 2) {
      if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
        c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<char32_t>(s[i + 1]) - 0xDC00);
        ++i;
      }
    }
    out += static_cast<wchar_t>(c);
  }
  return out;
}

std::u16string wideToUtf16(std::wstring_view s) {
  std::u16string out;
  out.reserve(s.size());
  for (wchar_t w : s) {
    auto c = static_cast<char32_t>(w);
    if (c >= 0x10000 && c <= 0x10FFFF) {
      c -= 0x10000;
      out += static_cast<char16_t>(0xD800 + (c >> 10));
      out += static_cast<char16_t>(0xDC00 + (c & 0x3FF));
    } else {
      out += static_cast<char16_t>(c > 0x10FFFF ? 0xFFFD : c);
    }
  }
  return out;
}

std::wstring cp1252ToWide(std::string_view s) {
  std::wstring out;
  out.reserve(s.size());
  for (char ch : s) {
    const auto b = static_cast<unsigned char>(ch);
    out += static_cast<wchar_t>(b >= 0x80 && b < 0xA0 ? kCp1252High[b - 0x80] : b);
  }
  return out;
}

std::string wideToCp1252(std::wstring_view s, bool* lost) {
  std::string out;
  out.reserve(s.size());
  if (lost) *lost = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<char32_t>(s[i]);
    int byte = -1;
    if (c < 0x80 || (c >= 0xA0 && c <= 0xFF)) {
      byte = static_cast<int>(c);
    } else {
      for (int k = 0; k < 32; ++k)
        if (kCp1252High[k] == c) byte = 0x80 + k;
    }
    if (byte < 0) {
      if (lost) *lost = true;
      if constexpr (sizeof(wchar_t) == 2) {
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size()) ++i;  // one '?' per character
      }
      byte = '?';
    }
    out += static_cast<char>(byte);
  }
  return out;
}

}  // namespace verso
