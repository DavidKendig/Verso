#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace verso {

enum class Encoding { Utf8, Utf8Bom, Utf16LE, Utf16BE, Windows1252 };
enum class LineEnding { CRLF, LF, CR };

struct DecodedText {
  std::wstring text;  // '\n' separated
  Encoding encoding = Encoding::Utf8;
  LineEnding lineEnding = LineEnding::CRLF;
  bool sawLineEnding = false;
  // NUL bytes in the first 8000 bytes of an 8-bit encoding (the test git uses), or
  // UTF-16 with an odd byte count: the file is not text that can be saved back.
  bool binary = false;
};

DecodedText decodeText(std::string_view bytes, LineEnding fallbackEnding);
std::string encodeText(std::wstring_view text, Encoding enc, LineEnding eol);
// False when saving `text` in `enc` would lose characters (Windows-1252 has no 日本).
bool canEncode(std::wstring_view text, Encoding enc);

const wchar_t* encodingName(Encoding e);
const wchar_t* lineEndingName(LineEnding e);
std::string encodingId(Encoding e);
Encoding encodingFromId(std::string_view id);
std::string lineEndingId(LineEnding e);
LineEnding lineEndingFromId(std::string_view id);

// File I/O. Errors are returned as human-readable messages.
bool readFileBytes(const std::wstring& path, std::string& out, std::wstring* error);
// Last-write time (FILETIME ticks) and size of a file; false for folders and missing files.
bool fileStat(const std::wstring& path, uint64_t* writeTime, uint64_t* size);
bool writeFileAtomic(const std::wstring& path, std::string_view bytes, std::wstring* error);
std::wstring lastErrorMessage(unsigned long code);

}  // namespace verso
