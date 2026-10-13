#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/TextBuffer.h"
#include "syntax/Language.h"

namespace verso {

struct Token {
  uint32_t start = 0;
  uint32_t len = 0;
  TokenKind kind = TokenKind::Text;
};

// Tokenizes one line. `state` carries multi-line constructs (block comments,
// long strings, fenced code) from the end of one line to the next.
void tokenizeLine(const Language& lang, std::wstring_view line, uint32_t& state, std::vector<Token>& out);

// Lazily computed per-line tokens for one buffer. Invalidate from the first
// edited line; lines are re-tokenized on demand.
class SyntaxCache {
 public:
  void reset(const Language* lang);
  const Language& language() const { return *lang_; }
  void invalidateFrom(size_t line);
  void ensure(const TextBuffer& buf, size_t line);
  const std::vector<Token>& tokens(const TextBuffer& buf, size_t line);

 private:
  const Language* lang_ = &plainTextLanguage();
  std::vector<uint32_t> state_{0};
  std::vector<std::vector<Token>> tokens_;
  size_t valid_ = 0;
};

struct Symbol {
  std::wstring name;
  size_t line = 0;
};

std::vector<Symbol> extractSymbols(const TextBuffer& buf, SyntaxCache& cache);

}  // namespace verso
