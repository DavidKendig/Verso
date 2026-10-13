#pragma once

#include <cstddef>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "core/TextUtil.h"

namespace verso {

struct SearchOptions {
  bool regex = false;
  bool caseSensitive = false;
  bool wholeWord = false;
};

struct Match {
  size_t pos = 0;
  size_t len = 0;
};

class Searcher {
 public:
  // Returns false and fills `error` when the pattern is invalid.
  bool compile(std::wstring_view pattern, const SearchOptions& opts, std::wstring* error = nullptr,
               std::wstring_view wordSeparators = kDefaultWordSeparators);
  bool valid() const { return valid_; }

  // First match starting at or after `from`.
  std::optional<Match> next(const std::wstring& text, size_t from, bool wrap) const;
  // Last match ending at or before `before`.
  std::optional<Match> prev(const std::wstring& text, size_t before, bool wrap) const;
  // Matches fully inside [rangeBegin, rangeEnd). Regexes see the whole text, so
  // lookarounds, \b and $ are not fooled by the range ends. Zero-length regex
  // matches (such as "^") are included only when `includeEmpty`.
  std::vector<Match> all(const std::wstring& text, size_t rangeBegin = 0, size_t rangeEnd = std::wstring::npos,
                         size_t limit = static_cast<size_t>(-1), bool includeEmpty = false) const;
  bool matchesExactly(const std::wstring& text, size_t pos, size_t len) const;
  std::wstring replacementFor(const std::wstring& text, const Match& m, std::wstring_view format) const;

 private:
  bool wordBoundaryOk(const std::wstring& text, size_t pos, size_t len) const;

  bool valid_ = false;
  std::wstring pattern_;
  std::wstring folded_;
  SearchOptions opts_;
  std::wstring separators_;
  std::optional<std::wregex> re_;
};

}  // namespace verso
