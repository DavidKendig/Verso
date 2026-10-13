#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace verso {

enum class TokenKind : uint8_t {
  Text,
  Comment,
  String,
  Number,
  Keyword,
  Storage,
  Type,
  Function,
  Constant,
  Operator,
  Punctuation,
  Preprocessor,
  Tag,
  Attribute,
  Heading,
  Emphasis,
  Link,
  Variable,
  Inserted,
  Deleted,
  Count
};

enum class LexMode { Plain, Code, Markup, Markdown, Diff, Ini };

using WordSet = std::unordered_set<std::wstring>;

struct Language {
  std::wstring name;
  LexMode mode = LexMode::Code;
  std::vector<std::wstring> extensions;  // lowercase, without the dot
  std::vector<std::wstring> filenames;   // lowercase file names
  std::vector<std::wstring> shebangs;    // interpreter names in "#!" lines

  std::wstring lineComment;
  std::wstring lineComment2;
  std::wstring blockStart, blockEnd;
  std::wstring stringQuotes = L"\"'";
  std::wstring identExtra;  // extra identifier characters, e.g. '$' or '-'

  bool tripleQuotes = false;      // """ and ''' strings spanning lines
  bool backtickStrings = false;   // `...` strings spanning lines
  bool hashPreprocessor = false;  // C-style #directives
  bool caseInsensitive = false;
  bool indentAfterColon = false;
  bool jsonKeys = false;          // "string" followed by ':' is a key
  bool colonKeys = false;         // identifier followed by ':' is a key (YAML, CSS)
  bool dollarVariables = false;   // $name, ${name}, $(name)
  bool percentVariables = false;  // %name%
  bool atAnnotations = false;     // @decorator
  bool capitalizedTypes = false;  // CamelCase identifiers are types
  bool rustChars = false;         // 'a' char literals vs 'lifetime, macro!
  bool cFunctions = false;        // top-level "type name(" definitions are symbols

  WordSet keywords, storage, types, constants, builtins;
  WordSet declFunction, declType;
};

const std::vector<Language>& allLanguages();
const Language& plainTextLanguage();
const Language* languageByName(std::wstring_view name);
const Language& detectLanguage(std::wstring_view path, std::wstring_view firstLine);

}  // namespace verso
