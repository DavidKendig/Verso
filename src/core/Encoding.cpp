#include "core/Encoding.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#endif

#include <algorithm>
#include <vector>

#include "core/TextUtil.h"

namespace verso {

namespace {

// UTF-16 code units from bytes in either byte order (a trailing odd byte is dropped).
std::u16string readUtf16(std::string_view b, bool bigEndian) {
  std::u16string out(b.size() / 2, u'\0');
  for (size_t i = 0; i < out.size(); ++i) {
    const auto first = static_cast<unsigned char>(b[i * 2]), second = static_cast<unsigned char>(b[i * 2 + 1]);
    out[i] = static_cast<char16_t>(bigEndian ? (first << 8) | second : (second << 8) | first);
  }
  return out;
}

}  // namespace

DecodedText decodeText(std::string_view bytes, LineEnding fallbackEnding) {
  DecodedText out;
  std::wstring w;
  auto starts = [&](std::initializer_list<unsigned char> sig) {
    if (bytes.size() < sig.size()) return false;
    size_t i = 0;
    for (unsigned char c : sig)
      if (static_cast<unsigned char>(bytes[i++]) != c) return false;
    return true;
  };
  if (starts({0xEF, 0xBB, 0xBF})) {
    out.encoding = Encoding::Utf8Bom;
    w = utf8ToWide(bytes.substr(3));
  } else if (starts({0xFF, 0xFE})) {
    out.encoding = Encoding::Utf16LE;
    out.binary = bytes.size() % 2 != 0;  // a dangling byte cannot round-trip through UTF-16
    w = utf16ToWide(readUtf16(bytes.substr(2), false));
  } else if (starts({0xFE, 0xFF})) {
    out.encoding = Encoding::Utf16BE;
    out.binary = bytes.size() % 2 != 0;
    w = utf16ToWide(readUtf16(bytes.substr(2), true));
  } else {
    out.binary = bytes.substr(0, 8000).find('\0') != std::string_view::npos;
    if (isValidUtf8(bytes)) {
      out.encoding = Encoding::Utf8;
      w = utf8ToWide(bytes);
    } else {
      out.encoding = Encoding::Windows1252;
      w = cp1252ToWide(bytes);
    }
  }

  size_t crlf = 0, lf = 0, cr = 0;
  for (size_t i = 0; i < w.size(); ++i) {
    if (w[i] == L'\r') {
      if (i + 1 < w.size() && w[i + 1] == L'\n') {
        ++crlf;
        ++i;
      } else {
        ++cr;
      }
    } else if (w[i] == L'\n') {
      ++lf;
    }
  }
  out.sawLineEnding = crlf + lf + cr > 0;
  if (!out.sawLineEnding)
    out.lineEnding = fallbackEnding;
  else if (crlf >= lf && crlf >= cr)
    out.lineEnding = LineEnding::CRLF;
  else if (lf >= cr)
    out.lineEnding = LineEnding::LF;
  else
    out.lineEnding = LineEnding::CR;
  out.text = (crlf || cr) ? normalizeNewlines(w) : std::move(w);
  return out;
}

bool canEncode(std::wstring_view text, Encoding enc) {
  if (enc != Encoding::Windows1252) return true;  // the Unicode encodings store everything
  bool lost = false;
  wideToCp1252(text, &lost);
  return !lost;
}

std::string encodeText(std::wstring_view text, Encoding enc, LineEnding eol) {
  std::wstring w;
  if (eol == LineEnding::LF) {
    w.assign(text);
  } else {
    w.reserve(text.size() + text.size() / 16);
    for (wchar_t c : text) {
      if (c == L'\n') {
        if (eol == LineEnding::CRLF) w += L"\r\n";
        else w += L'\r';
      } else {
        w += c;
      }
    }
  }
  switch (enc) {
    case Encoding::Utf8:
      return wideToUtf8(w);
    case Encoding::Utf8Bom:
      return "\xEF\xBB\xBF" + wideToUtf8(w);
    case Encoding::Windows1252:
      return wideToCp1252(w);
    case Encoding::Utf16LE:
    case Encoding::Utf16BE: {
      const bool big = enc == Encoding::Utf16BE;
      std::string out = big ? std::string("\xFE\xFF", 2) : std::string("\xFF\xFE", 2);
      const std::u16string units = wideToUtf16(w);
      out.reserve(2 + units.size() * 2);
      for (char16_t c : units) {
        const char hi = static_cast<char>((c >> 8) & 0xFF), lo = static_cast<char>(c & 0xFF);
        out += big ? hi : lo;
        out += big ? lo : hi;
      }
      return out;
    }
  }
  return {};
}

const wchar_t* encodingName(Encoding e) {
  switch (e) {
    case Encoding::Utf8: return L"UTF-8";
    case Encoding::Utf8Bom: return L"UTF-8 with BOM";
    case Encoding::Utf16LE: return L"UTF-16 LE with BOM";
    case Encoding::Utf16BE: return L"UTF-16 BE with BOM";
    case Encoding::Windows1252: return L"Western (Windows 1252)";
  }
  return L"UTF-8";
}

const wchar_t* lineEndingName(LineEnding e) {
  switch (e) {
    case LineEnding::CRLF: return L"Windows";
    case LineEnding::LF: return L"Unix";
    case LineEnding::CR: return L"Mac OS 9";
  }
  return L"Windows";
}

std::string encodingId(Encoding e) {
  switch (e) {
    case Encoding::Utf8: return "utf-8";
    case Encoding::Utf8Bom: return "utf-8-bom";
    case Encoding::Utf16LE: return "utf-16le";
    case Encoding::Utf16BE: return "utf-16be";
    case Encoding::Windows1252: return "windows-1252";
  }
  return "utf-8";
}

Encoding encodingFromId(std::string_view id) {
  if (id == "utf-8-bom") return Encoding::Utf8Bom;
  if (id == "utf-16le") return Encoding::Utf16LE;
  if (id == "utf-16be") return Encoding::Utf16BE;
  if (id == "windows-1252") return Encoding::Windows1252;
  return Encoding::Utf8;
}

std::string lineEndingId(LineEnding e) {
  switch (e) {
    case LineEnding::CRLF: return "windows";
    case LineEnding::LF: return "unix";
    case LineEnding::CR: return "cr";
  }
  return "windows";
}

LineEnding lineEndingFromId(std::string_view id) {
  if (id == "unix") return LineEnding::LF;
  if (id == "cr") return LineEnding::CR;
  return LineEnding::CRLF;
}

#ifdef _WIN32
std::wstring lastErrorMessage(unsigned long code) {
  wchar_t* buf = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                 code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
  std::wstring msg = buf ? std::wstring(trim(buf)) : L"Unknown error " + std::to_wstring(code);
  if (buf) LocalFree(buf);
  return msg;
}

bool fileStat(const std::wstring& path, uint64_t* writeTime, uint64_t* size) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) || (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    return false;
  if (writeTime)
    *writeTime = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) | fad.ftLastWriteTime.dwLowDateTime;
  if (size) *size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
  return true;
}

bool readFileBytes(const std::wstring& path, std::string& out, std::wstring* error) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (error) *error = lastErrorMessage(GetLastError());
    return false;
  }
  LARGE_INTEGER size{};
  GetFileSizeEx(h, &size);
  if (size.QuadPart > (1LL << 31)) {
    CloseHandle(h);
    if (error) *error = L"File is larger than 2 GB.";
    return false;
  }
  out.resize(static_cast<size_t>(size.QuadPart));
  size_t done = 0;
  while (done < out.size()) {
    DWORD got = 0;
    DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 30));
    if (!ReadFile(h, out.data() + done, want, &got, nullptr)) {
      if (error) *error = lastErrorMessage(GetLastError());
      CloseHandle(h);
      return false;
    }
    if (got == 0) break;
    done += got;
  }
  out.resize(done);
  CloseHandle(h);
  return true;
}

bool writeFileAtomic(const std::wstring& path, std::string_view bytes, std::wstring* error) {
  std::wstring tmp = path + L".verso-save-tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  bool direct = false;
  if (h == INVALID_HANDLE_VALUE) {
    // Directory may not allow creating siblings; fall back to writing in place.
    h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                    nullptr);
    direct = true;
    if (h == INVALID_HANDLE_VALUE) {
      if (error) *error = lastErrorMessage(GetLastError());
      return false;
    }
  }
  size_t done = 0;
  while (done < bytes.size()) {
    DWORD wrote = 0;
    DWORD want = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, 1u << 30));
    if (!WriteFile(h, bytes.data() + done, want, &wrote, nullptr)) {
      DWORD code = GetLastError();
      CloseHandle(h);
      if (!direct) DeleteFileW(tmp.c_str());
      if (error) *error = lastErrorMessage(code);
      return false;
    }
    done += wrote;
  }
  FlushFileBuffers(h);
  CloseHandle(h);
  if (direct) return true;

  BOOL ok;
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
    ok = ReplaceFileW(path.c_str(), tmp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr);
  else
    ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!ok) {
    DWORD code = GetLastError();
    DeleteFileW(tmp.c_str());
    if (error) *error = lastErrorMessage(code);
    return false;
  }
  return true;
}

#else
std::wstring lastErrorMessage(unsigned long code) {
  return utf8ToWide(std::strerror(static_cast<int>(code)));
}

namespace {

std::filesystem::path fsPath(const std::wstring& p) { return std::filesystem::path(wideToUtf8(p)); }

}  // namespace

bool fileStat(const std::wstring& path, uint64_t* writeTime, uint64_t* size) {
  std::error_code ec;
  const auto p = fsPath(path);
  if (!std::filesystem::is_regular_file(p, ec)) return false;
  if (writeTime) {
    auto t = std::filesystem::last_write_time(p, ec);
    *writeTime = ec ? 0 : static_cast<uint64_t>(t.time_since_epoch().count());
  }
  if (size) *size = static_cast<uint64_t>(std::filesystem::file_size(p, ec));
  return true;
}

bool readFileBytes(const std::wstring& path, std::string& out, std::wstring* error) {
  std::ifstream in(fsPath(path), std::ios::binary);
  if (!in) {
    if (error) *error = lastErrorMessage(static_cast<unsigned long>(errno));
    return false;
  }
  in.seekg(0, std::ios::end);
  const auto size = static_cast<long long>(in.tellg());
  if (size > (1LL << 31)) {
    if (error) *error = L"File is larger than 2 GB.";
    return false;
  }
  in.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(std::max(0LL, size)));
  in.read(out.data(), static_cast<std::streamsize>(out.size()));
  out.resize(static_cast<size_t>(in.gcount()));
  return true;
}

bool writeFileAtomic(const std::wstring& path, std::string_view bytes, std::wstring* error) {
  // Write a sibling, flush it to disk, then rename it over the original.
  const std::string target = wideToUtf8(path);
  const std::string tmp = target + ".verso-save-tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  bool direct = false;
  if (fd < 0) {  // the folder may not allow new files; write in place instead
    fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    direct = true;
    if (fd < 0) {
      if (error) *error = lastErrorMessage(static_cast<unsigned long>(errno));
      return false;
    }
  }
  size_t done = 0;
  while (done < bytes.size()) {
    ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      const int code = errno;
      ::close(fd);
      if (!direct) ::unlink(tmp.c_str());
      if (error) *error = lastErrorMessage(static_cast<unsigned long>(code));
      return false;
    }
    done += static_cast<size_t>(n);
  }
  ::fsync(fd);
  ::close(fd);
  if (!direct) {
    struct stat st {};
    if (::stat(target.c_str(), &st) == 0) ::chmod(tmp.c_str(), st.st_mode & 07777);  // keep permissions
    if (::rename(tmp.c_str(), target.c_str()) != 0) {
      const int code = errno;
      ::unlink(tmp.c_str());
      if (error) *error = lastErrorMessage(static_cast<unsigned long>(code));
      return false;
    }
  }
  return true;
}

#endif

}  // namespace verso
