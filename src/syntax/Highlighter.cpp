#include "syntax/Highlighter.h"

#include <algorithm>
#include <cwctype>

#include "core/TextUtil.h"

namespace verso {

namespace {

enum State : uint32_t {
  kNormal = 0,
  kBlockComment = 1,
  kTripleDouble = 2,
  kTripleSingle = 3,
  kBacktick = 4,
  kMarkupComment = 5,
  kMarkupTag = 6,
  kFencedCode = 7,
};

constexpr size_t npos = std::wstring_view::npos;

class Emitter {
 public:
  explicit Emitter(std::vector<Token>& out) : out_(out) {}
  void operator()(size_t b, size_t e, TokenKind k) {
    if (e <= b) return;
    if (!out_.empty() && out_.back().kind == k && out_.back().start + out_.back().len == b) {
      out_.back().len += static_cast<uint32_t>(e - b);
      return;
    }
    out_.push_back({static_cast<uint32_t>(b), static_cast<uint32_t>(e - b), k});
  }

 private:
  std::vector<Token>& out_;
};

bool matchAt(std::wstring_view s, size_t i, std::wstring_view tok, bool ci) {
  if (tok.empty() || i + tok.size() > s.size()) return false;
  for (size_t k = 0; k < tok.size(); ++k) {
    wchar_t a = s[i + k], b = tok[k];
    if (ci ? std::towlower(a) != std::towlower(b) : a != b) return false;
  }
  return true;
}

bool isIdentStart(wchar_t c, const Language& L) {
  return std::iswalpha(c) || c == L'_' || c > 0x7F || (c != L'-' && L.identExtra.find(c) != npos);
}

bool isIdentChar(wchar_t c, const Language& L) {
  return std::iswalnum(c) || c == L'_' || c > 0x7F || L.identExtra.find(c) != npos;
}

std::wstring lower(std::wstring_view s) {
  std::wstring out(s);
  for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
  return out;
}

// Scans a quoted string starting after the opening quote; returns the index
// just past the closing quote or the line end.
size_t scanQuoted(std::wstring_view s, size_t i, wchar_t q, bool escapes = true) {
  while (i < s.size()) {
    if (escapes && s[i] == L'\\') {
      i += 2;
      continue;
    }
    if (s[i] == q) return i + 1;
    ++i;
  }
  return s.size();
}

void lexCode(const Language& L, std::wstring_view s, uint32_t& state, Emitter& emit) {
  const size_t n = s.size();
  size_t i = 0;
  bool expectFunction = false, expectType = false;
  const size_t firstNonBlank = leadingBlanks(s);
  const bool ci = L.caseInsensitive;

  while (i < n) {
    if (state == kBlockComment) {
      size_t e = s.find(L.blockEnd, i);
      if (e == npos) {
        emit(i, n, TokenKind::Comment);
        return;
      }
      emit(i, e + L.blockEnd.size(), TokenKind::Comment);
      i = e + L.blockEnd.size();
      state = kNormal;
      continue;
    }
    if (state == kTripleDouble || state == kTripleSingle) {
      std::wstring_view q = state == kTripleDouble ? L"\"\"\"" : L"'''";
      size_t j = i;
      while (j < n) {
        if (s[j] == L'\\') {
          j += 2;
          continue;
        }
        if (matchAt(s, j, q, false)) break;
        ++j;
      }
      if (j >= n) {
        emit(i, n, TokenKind::String);
        return;
      }
      emit(i, j + 3, TokenKind::String);
      i = j + 3;
      state = kNormal;
      continue;
    }
    if (state == kBacktick) {
      size_t j = i;
      bool closed = false;
      while (j < n) {
        if (s[j] == L'\\') {
          j += 2;
          continue;
        }
        if (s[j++] == L'`') {
          closed = true;
          break;
        }
      }
      j = std::min(j, n);
      emit(i, j, TokenKind::String);
      i = j;
      if (closed) state = kNormal;
      if (i >= n) return;
      continue;
    }

    const wchar_t c = s[i];
    if (isBlank(c)) {
      size_t b = i;
      while (i < n && isBlank(s[i])) ++i;
      emit(b, i, TokenKind::Text);
      continue;
    }
    if (L.hashPreprocessor && c == L'#' && i == firstNonBlank) {
      size_t b = i++;
      while (i < n && isBlank(s[i])) ++i;
      while (i < n && isIdentChar(s[i], L)) ++i;
      emit(b, i, TokenKind::Preprocessor);
      size_t j = i;
      while (j < n && isBlank(s[j])) ++j;
      if (j < n && s[j] == L'<') {
        size_t e = s.find(L'>', j);
        if (e != npos) {
          emit(i, j, TokenKind::Text);
          emit(j, e + 1, TokenKind::String);
          i = e + 1;
        }
      }
      continue;
    }
    if (!L.blockStart.empty() && matchAt(s, i, L.blockStart, ci)) {
      emit(i, i + L.blockStart.size(), TokenKind::Comment);
      i += L.blockStart.size();
      state = kBlockComment;
      continue;
    }
    if (matchAt(s, i, L.lineComment, ci) || matchAt(s, i, L.lineComment2, ci)) {
      bool wordComment = L.lineComment2.size() && std::iswalpha(L.lineComment2[0]) && matchAt(s, i, L.lineComment2, ci);
      if (!wordComment || i == firstNonBlank) {
        emit(i, n, TokenKind::Comment);
        return;
      }
    }
    if (L.tripleQuotes && (matchAt(s, i, L"\"\"\"", false) || matchAt(s, i, L"'''", false))) {
      state = s[i] == L'"' ? kTripleDouble : kTripleSingle;
      emit(i, i + 3, TokenKind::String);
      i += 3;
      continue;
    }
    if (L.backtickStrings && c == L'`') {
      state = kBacktick;
      emit(i, i + 1, TokenKind::String);
      ++i;
      continue;
    }
    if (L.rustChars && c == L'\'') {
      bool charLit = (i + 2 < n && s[i + 2] == L'\'') || (i + 1 < n && s[i + 1] == L'\\');
      size_t b = i++;
      if (charLit) {
        i = scanQuoted(s, i, L'\'');
        emit(b, i, TokenKind::String);
      } else {
        while (i < n && isIdentChar(s[i], L)) ++i;
        emit(b, i, TokenKind::Storage);
      }
      continue;
    }
    if (L.stringQuotes.find(c) != npos) {
      size_t b = i;
      i = scanQuoted(s, i + 1, c);
      TokenKind k = TokenKind::String;
      if (L.jsonKeys) {
        size_t j = i;
        while (j < n && isBlank(s[j])) ++j;
        if (j < n && s[j] == L':') k = TokenKind::Attribute;
      }
      emit(b, i, k);
      continue;
    }
    if (std::iswdigit(c) || (c == L'.' && i + 1 < n && std::iswdigit(s[i + 1]))) {
      size_t b = i++;
      bool hex = c == L'0' && i < n && (s[i] == L'x' || s[i] == L'X');
      while (i < n) {
        wchar_t d = s[i];
        if (std::iswalnum(d) || d == L'.' || d == L'_' || d == L'\'') {
          if (d == L'\'' && !(i + 1 < n && std::iswalnum(s[i + 1]))) break;
          ++i;
        } else if (!hex && (d == L'+' || d == L'-') && (s[i - 1] == L'e' || s[i - 1] == L'E')) {
          ++i;
        } else {
          break;
        }
      }
      emit(b, i, TokenKind::Number);
      continue;
    }
    if (L.dollarVariables && c == L'$' && i + 1 < n &&
        (isIdentStart(s[i + 1], L) || s[i + 1] == L'{' || s[i + 1] == L'(')) {
      size_t b = i++;
      if (s[i] == L'{' || s[i] == L'(') {
        wchar_t close = s[i] == L'{' ? L'}' : L')';
        size_t e = s.find(close, i);
        i = e == npos ? n : e + 1;
      } else {
        while (i < n && isIdentChar(s[i], L)) ++i;
      }
      // "$true", "$null": constants written like variables (PowerShell).
      std::wstring name(s.substr(b, i - b));
      emit(b, i, L.constants.count(ci ? lower(name) : name) ? TokenKind::Constant : TokenKind::Variable);
      continue;
    }
    // "-eq", "-like": word operators that start with '-' (PowerShell).
    if (c == L'-' && i + 1 < n && std::iswalpha(s[i + 1]) && L.identExtra.find(L'-') != npos &&
        (i == 0 || !isIdentChar(s[i - 1], L))) {
      size_t e = i + 1;
      while (e < n && isIdentChar(s[e], L)) ++e;
      std::wstring word(s.substr(i, e - i));
      if (L.keywords.count(ci ? lower(word) : word)) {
        emit(i, e, TokenKind::Keyword);
        i = e;
        continue;
      }
    }
    // Single-character constants such as YAML's "~".
    if (!isIdentStart(c, L) && L.constants.count(std::wstring(1, c)) &&
        (i == 0 || !isIdentChar(s[i - 1], L)) && (i + 1 >= n || !isIdentChar(s[i + 1], L))) {
      emit(i, i + 1, TokenKind::Constant);
      ++i;
      continue;
    }
    if (L.percentVariables && c == L'%' && i + 1 < n) {
      size_t b = i++;
      if (std::iswdigit(s[i]) || s[i] == L'~' || s[i] == L'*') {
        while (i < n && (std::iswalnum(s[i]) || s[i] == L'~')) ++i;
        emit(b, i, TokenKind::Variable);
        continue;
      }
      if (s[i] == L'%') ++i;
      size_t e = s.find(L'%', i);
      if (e != npos && e > i) {
        emit(b, e + 1, TokenKind::Variable);
        i = e + 1;
      } else {
        emit(b, i, TokenKind::Operator);
      }
      continue;
    }
    if (L.atAnnotations && c == L'@' && i + 1 < n && isIdentStart(s[i + 1], L)) {
      size_t b = i++;
      while (i < n && (isIdentChar(s[i], L) || s[i] == L'.')) ++i;
      emit(b, i, L.colonKeys ? TokenKind::Keyword : TokenKind::Attribute);
      continue;
    }
    if (isIdentStart(c, L)) {
      size_t b = i++;
      while (i < n && isIdentChar(s[i], L)) ++i;
      std::wstring_view w = s.substr(b, i - b);
      std::wstring key = ci ? lower(w) : std::wstring(w);
      TokenKind k = TokenKind::Text;
      size_t j = i;
      while (j < n && isBlank(s[j])) ++j;
      const wchar_t after = j < n ? s[j] : L'\0';
      if (expectFunction) {
        k = TokenKind::Function;
        expectFunction = false;
      } else if (expectType) {
        k = TokenKind::Type;
        expectType = false;
      } else if (L.keywords.count(key) || L.storage.count(key)) {
        k = L.keywords.count(key) ? TokenKind::Keyword : TokenKind::Storage;
        if (L.declFunction.count(key)) expectFunction = true;
        if (L.declType.count(key)) expectType = true;
      } else if (L.constants.count(key)) {
        k = TokenKind::Constant;
      } else if (L.types.count(key)) {
        k = TokenKind::Type;
      } else if (L.colonKeys && after == L':' && (j + 1 >= n || s[j + 1] != L':')) {
        k = TokenKind::Attribute;
      } else if (L.builtins.count(key)) {
        k = TokenKind::Function;
      } else if (after == L'(' || (L.rustChars && after == L'!')) {
        k = TokenKind::Function;
      } else if (L.capitalizedTypes && std::iswupper(w[0]) &&
                 std::any_of(w.begin(), w.end(), [](wchar_t ch) { return std::iswlower(ch) != 0; })) {
        k = TokenKind::Type;
      }
      emit(b, i, k);
      continue;
    }
    if (std::wcschr(L"+-*/%=<>!&|^~?:", c) && c != 0) {
      emit(i, i + 1, TokenKind::Operator);
      ++i;
      continue;
    }
    emit(i, i + 1, TokenKind::Punctuation);
    ++i;
  }
}

void lexMarkup(std::wstring_view s, uint32_t& state, Emitter& emit) {
  const size_t n = s.size();
  size_t i = 0;
  auto nameChar = [](wchar_t c) { return std::iswalnum(c) || c == L'-' || c == L'_' || c == L':' || c == L'.'; };
  while (i < n) {
    if (state == kMarkupComment) {
      size_t e = s.find(L"-->", i);
      if (e == npos) {
        emit(i, n, TokenKind::Comment);
        return;
      }
      emit(i, e + 3, TokenKind::Comment);
      i = e + 3;
      state = kNormal;
      continue;
    }
    if (state == kMarkupTag) {
      wchar_t c = s[i];
      if (isBlank(c)) {
        size_t b = i;
        while (i < n && isBlank(s[i])) ++i;
        emit(b, i, TokenKind::Text);
      } else if (c == L'>' || (c == L'/' && i + 1 < n && s[i + 1] == L'>') ||
                 (c == L'?' && i + 1 < n && s[i + 1] == L'>')) {
        size_t len = c == L'>' ? 1 : 2;
        emit(i, i + len, TokenKind::Punctuation);
        i += len;
        state = kNormal;
      } else if (c == L'"' || c == L'\'') {
        size_t b = i;
        i = scanQuoted(s, i + 1, c, false);
        emit(b, i, TokenKind::String);
      } else if (c == L'=') {
        emit(i, i + 1, TokenKind::Operator);
        ++i;
      } else if (nameChar(c)) {
        size_t b = i;
        while (i < n && nameChar(s[i])) ++i;
        emit(b, i, TokenKind::Attribute);
      } else {
        emit(i, i + 1, TokenKind::Punctuation);
        ++i;
      }
      continue;
    }
    if (matchAt(s, i, L"<!--", false)) {
      emit(i, i + 4, TokenKind::Comment);
      i += 4;
      state = kMarkupComment;
      continue;
    }
    if (s[i] == L'<' && i + 1 < n && (std::iswalpha(s[i + 1]) || s[i + 1] == L'/' || s[i + 1] == L'!' ||
                                      s[i + 1] == L'?')) {
      size_t b = i++;
      if (s[i] == L'/' || s[i] == L'!' || s[i] == L'?') ++i;
      emit(b, i, TokenKind::Punctuation);
      size_t t = i;
      while (i < n && nameChar(s[i])) ++i;
      emit(t, i, TokenKind::Tag);
      state = kMarkupTag;
      continue;
    }
    if (s[i] == L'&') {
      size_t e = s.find(L';', i);
      if (e != npos && e - i <= 10) {
        emit(i, e + 1, TokenKind::Constant);
        i = e + 1;
        continue;
      }
    }
    size_t b = i++;
    while (i < n && s[i] != L'<' && s[i] != L'&') ++i;
    emit(b, i, TokenKind::Text);
  }
}

void lexMarkdownInline(std::wstring_view s, size_t i, Emitter& emit) {
  const size_t n = s.size();
  while (i < n) {
    wchar_t c = s[i];
    if (c == L'`') {
      size_t e = s.find(L'`', i + 1);
      if (e != npos) {
        emit(i, e + 1, TokenKind::String);
        i = e + 1;
        continue;
      }
    }
    if ((c == L'*' || c == L'_') && i + 1 < n) {
      bool strong = s[i + 1] == c;
      std::wstring_view delim = strong ? s.substr(i, 2) : s.substr(i, 1);
      size_t e = s.find(delim, i + delim.size());
      if (e != npos && e > i + delim.size() && !isBlank(s[i + delim.size()])) {
        emit(i, e + delim.size(), TokenKind::Emphasis);
        i = e + delim.size();
        continue;
      }
    }
    if (c == L'[' || (c == L'!' && i + 1 < n && s[i + 1] == L'[')) {
      size_t open = c == L'!' ? i + 1 : i;
      size_t close = s.find(L']', open);
      if (close != npos && close + 1 < n && s[close + 1] == L'(') {
        size_t paren = s.find(L')', close);
        if (paren != npos) {
          emit(i, close + 1, TokenKind::Link);
          emit(close + 1, paren + 1, TokenKind::String);
          i = paren + 1;
          continue;
        }
      }
    }
    if (c == L'<' && (matchAt(s, i, L"<http", false) || matchAt(s, i, L"<mailto:", false))) {
      size_t e = s.find(L'>', i);
      if (e != npos) {
        emit(i, e + 1, TokenKind::Link);
        i = e + 1;
        continue;
      }
    }
    size_t b = i++;
    while (i < n && !std::wcschr(L"`*_[!<", s[i])) ++i;
    emit(b, i, TokenKind::Text);
  }
}

void lexMarkdown(std::wstring_view s, uint32_t& state, Emitter& emit) {
  const size_t n = s.size();
  size_t lead = leadingBlanks(s);
  std::wstring_view t = s.substr(lead);
  if (state == kFencedCode) {
    emit(0, n, TokenKind::String);
    if (startsWith(t, L"```") || startsWith(t, L"~~~")) state = kNormal;
    return;
  }
  if (startsWith(t, L"```") || startsWith(t, L"~~~")) {
    emit(0, n, TokenKind::String);
    state = kFencedCode;
    return;
  }
  if (!t.empty() && t[0] == L'#') {
    size_t h = 0;
    while (h < t.size() && t[h] == L'#') ++h;
    if (h <= 6 && (h == t.size() || t[h] == L' ')) {
      emit(0, n, TokenKind::Heading);
      return;
    }
  }
  if (!t.empty() && t[0] == L'>') {
    emit(0, n, TokenKind::Comment);
    return;
  }
  std::wstring_view tt = trim(t);
  if (tt.size() >= 3 && (tt.find_first_not_of(L"-") == npos || tt.find_first_not_of(L"*") == npos ||
                         tt.find_first_not_of(L"=") == npos)) {
    emit(0, n, TokenKind::Punctuation);
    return;
  }
  if (matchAt(s, lead, L"<!--", false)) {
    size_t e = s.find(L"-->", lead);
    emit(0, e == npos ? n : e + 3, TokenKind::Comment);
    if (e == npos) return;
    lexMarkdownInline(s, e + 3, emit);
    return;
  }
  size_t i = lead;
  emit(0, lead, TokenKind::Text);
  if (t.size() >= 2 && (t[0] == L'-' || t[0] == L'*' || t[0] == L'+') && t[1] == L' ') {
    emit(i, i + 1, TokenKind::Keyword);
    ++i;
  } else {
    size_t d = 0;
    while (d < t.size() && std::iswdigit(t[d])) ++d;
    if (d > 0 && d + 1 < t.size() && (t[d] == L'.' || t[d] == L')') && t[d + 1] == L' ') {
      emit(i, i + d + 1, TokenKind::Keyword);
      i += d + 1;
    }
  }
  lexMarkdownInline(s, i, emit);
}

void lexDiff(std::wstring_view s, Emitter& emit) {
  const size_t n = s.size();
  if (startsWith(s, L"+++") || startsWith(s, L"---") || startsWith(s, L"diff ") || startsWith(s, L"index "))
    emit(0, n, TokenKind::Heading);
  else if (startsWith(s, L"@@"))
    emit(0, n, TokenKind::Keyword);
  else if (startsWith(s, L"+") || startsWith(s, L">"))
    emit(0, n, TokenKind::Inserted);
  else if (startsWith(s, L"-") || startsWith(s, L"<"))
    emit(0, n, TokenKind::Deleted);
  else
    emit(0, n, TokenKind::Text);
}

void lexIni(const Language& L, std::wstring_view s, Emitter& emit) {
  const size_t n = s.size();
  size_t lead = leadingBlanks(s);
  if (lead == n) {
    emit(0, n, TokenKind::Text);
    return;
  }
  if (matchAt(s, lead, L.lineComment, false) || matchAt(s, lead, L.lineComment2, false)) {
    emit(0, n, TokenKind::Comment);
    return;
  }
  if (s[lead] == L'[') {
    emit(0, n, TokenKind::Heading);
    return;
  }
  size_t eq = s.find_first_of(L"=:", lead);
  if (eq == npos) {
    emit(0, n, L.name == L"Git Ignore" ? TokenKind::Text : TokenKind::Attribute);
    return;
  }
  emit(0, eq, TokenKind::Attribute);
  emit(eq, eq + 1, TokenKind::Operator);
  std::wstring_view v = trim(s.substr(eq + 1));
  TokenKind k = TokenKind::String;
  if (!v.empty() && (std::iswdigit(v[0]) || v[0] == L'-')) k = TokenKind::Number;
  if (v == L"true" || v == L"false" || v == L"yes" || v == L"no" || v == L"on" || v == L"off") k = TokenKind::Constant;
  emit(eq + 1, n, k);
}

}  // namespace

void tokenizeLine(const Language& lang, std::wstring_view line, uint32_t& state, std::vector<Token>& out) {
  Emitter emit(out);
  switch (lang.mode) {
    case LexMode::Plain: emit(0, line.size(), TokenKind::Text); break;
    case LexMode::Code: lexCode(lang, line, state, emit); break;
    case LexMode::Markup: lexMarkup(line, state, emit); break;
    case LexMode::Markdown: lexMarkdown(line, state, emit); break;
    case LexMode::Diff: lexDiff(line, emit); break;
    case LexMode::Ini: lexIni(lang, line, emit); break;
  }
}

void SyntaxCache::reset(const Language* lang) {
  lang_ = lang ? lang : &plainTextLanguage();
  state_.assign(1, 0);
  tokens_.clear();
  valid_ = 0;
}

void SyntaxCache::invalidateFrom(size_t line) { valid_ = std::min(valid_, line); }

void SyntaxCache::ensure(const TextBuffer& buf, size_t line) {
  const size_t count = buf.lineCount();
  if (state_.size() != count + 1) state_.resize(count + 1, 0);
  if (tokens_.size() != count) tokens_.resize(count);
  line = std::min(line, count - 1);
  for (size_t l = valid_; l <= line; ++l) {
    uint32_t st = state_[l];
    tokens_[l].clear();
    tokenizeLine(*lang_, buf.lineText(l), st, tokens_[l]);
    state_[l + 1] = st;
  }
  valid_ = std::max(valid_, line + 1);
}

const std::vector<Token>& SyntaxCache::tokens(const TextBuffer& buf, size_t line) {
  ensure(buf, line);
  return tokens_[std::min(line, tokens_.size() - 1)];
}

std::vector<Symbol> extractSymbols(const TextBuffer& buf, SyntaxCache& cache) {
  std::vector<Symbol> out;
  const Language& L = cache.language();
  const size_t count = buf.lineCount();
  if (count == 0) return out;
  cache.ensure(buf, count - 1);
  for (size_t l = 0; l < count; ++l) {
    std::wstring_view text = buf.lineText(l);
    const auto& toks = cache.tokens(buf, l);
    if (L.mode == LexMode::Markdown) {
      if (!toks.empty() && toks[0].kind == TokenKind::Heading) {
        std::wstring_view t = trim(text);
        size_t h = 0;
        while (h < t.size() && t[h] == L'#') ++h;
        std::wstring name(static_cast<size_t>(h > 0 ? (h - 1) * 2 : 0), L' ');
        name += trim(t.substr(h));
        out.push_back({name, l});
      }
      continue;
    }
    if (L.mode == LexMode::Ini) {
      if (!toks.empty() && toks[0].kind == TokenKind::Heading) out.push_back({std::wstring(trim(text)), l});
      continue;
    }
    if (L.mode != LexMode::Code) continue;
    bool found = false;
    for (size_t k = 0; k < toks.size() && !found; ++k) {
      const Token& t = toks[k];
      if (t.kind != TokenKind::Keyword && t.kind != TokenKind::Storage) continue;
      std::wstring word(text.substr(t.start, t.len));
      if (L.caseInsensitive) word = lower(word);
      if (!L.declFunction.count(word) && !L.declType.count(word)) continue;
      for (size_t m = k + 1; m < toks.size(); ++m) {
        if (toks[m].kind == TokenKind::Text && trim(text.substr(toks[m].start, toks[m].len)).empty()) continue;
        if (toks[m].kind == TokenKind::Function || toks[m].kind == TokenKind::Type) {
          out.push_back({std::wstring(text.substr(toks[m].start, toks[m].len)), l});
          found = true;
        }
        break;
      }
    }
    if (found || !L.cFunctions || text.empty() || isBlank(text[0])) continue;
    std::wstring_view tt = trim(text);
    if (tt.empty() || tt.back() == L';' || tt[0] == L'#' || tt[0] == L'/' || tt[0] == L'*') continue;
    for (size_t k = 0; k < toks.size(); ++k) {
      if (toks[k].kind != TokenKind::Function) continue;
      size_t b = toks[k].start, e = b + toks[k].len;
      while (b >= 2 && text[b - 1] == L':' && text[b - 2] == L':') {
        size_t s2 = b - 2;
        while (s2 > 0 && (std::iswalnum(text[s2 - 1]) || text[s2 - 1] == L'_' || text[s2 - 1] == L'~')) --s2;
        if (s2 == b - 2) break;
        b = s2;
      }
      if (b > 0 && text[b - 1] == L'~') --b;
      out.push_back({std::wstring(text.substr(b, e - b)), l});
      break;
    }
  }
  return out;
}

}  // namespace verso
