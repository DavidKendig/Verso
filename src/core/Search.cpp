#include "core/Search.h"

#include <algorithm>
#include <cwctype>

namespace verso {

namespace {

wchar_t fold(wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); }

std::wstring translateFormat(std::wstring_view fmt) {
  // Accept PCRE-style "\1" group references and "\n"/"\t" escapes in
  // addition to ECMAScript "$1".
  std::wstring out;
  for (size_t i = 0; i < fmt.size(); ++i) {
    wchar_t c = fmt[i];
    if (c == L'\\' && i + 1 < fmt.size()) {
      wchar_t n = fmt[i + 1];
      if (n >= L'0' && n <= L'9') {
        out += L'$';
        out += n;
        ++i;
        continue;
      }
      if (n == L'n') { out += L'\n'; ++i; continue; }
      if (n == L't') { out += L'\t'; ++i; continue; }
      if (n == L'\\') { out += L'\\'; ++i; continue; }
    }
    out += c;
  }
  return out;
}

}  // namespace

bool Searcher::compile(std::wstring_view pattern, const SearchOptions& opts, std::wstring* error,
                       std::wstring_view wordSeparators) {
  pattern_ = pattern;
  opts_ = opts;
  separators_ = wordSeparators;
  re_.reset();
  folded_.clear();
  valid_ = false;
  if (pattern_.empty()) return true;
  if (opts.regex) {
    try {
      auto flags = std::regex_constants::ECMAScript | std::regex_constants::multiline;
      if (!opts.caseSensitive) flags |= std::regex_constants::icase;
      re_.emplace(pattern_, flags);
    } catch (const std::regex_error& e) {
      if (error) *error = utf8ToWide(e.what());
      return false;
    }
  } else {
    folded_ = pattern_;
    for (auto& c : folded_) c = fold(c);
  }
  valid_ = true;
  return true;
}

bool Searcher::wordBoundaryOk(const std::wstring& text, size_t pos, size_t len) const {
  if (!opts_.wholeWord || len == 0) return true;
  if (isWordChar(text[pos], separators_) && pos > 0 && isWordChar(text[pos - 1], separators_)) return false;
  size_t end = pos + len;
  if (isWordChar(text[end - 1], separators_) && end < text.size() && isWordChar(text[end], separators_)) return false;
  return true;
}

std::vector<Match> Searcher::all(const std::wstring& text, size_t rangeBegin, size_t rangeEnd, size_t limit,
                                 bool includeEmpty) const {
  std::vector<Match> out;
  if (!valid_ || pattern_.empty()) return out;
  rangeEnd = std::min(rangeEnd, text.size());
  if (rangeBegin >= rangeEnd) return out;

  if (!opts_.regex) {
    const std::wstring& needle = opts_.caseSensitive ? pattern_ : folded_;
    auto first = text.begin() + static_cast<ptrdiff_t>(rangeBegin);
    auto last = text.begin() + static_cast<ptrdiff_t>(rangeEnd);
    while (first < last && out.size() < limit) {
      auto it = opts_.caseSensitive
                    ? std::search(first, last, needle.begin(), needle.end())
                    : std::search(first, last, needle.begin(), needle.end(),
                                  [](wchar_t a, wchar_t b) { return fold(a) == b; });
      if (it == last) break;
      size_t pos = static_cast<size_t>(it - text.begin());
      if (wordBoundaryOk(text, pos, needle.size())) {
        out.push_back({pos, needle.size()});
        first = it + static_cast<ptrdiff_t>(needle.size());
      } else {
        first = it + 1;
      }
    }
    return out;
  }

  try {
    auto first = text.begin() + static_cast<ptrdiff_t>(rangeBegin);
    std::wsmatch m;
    while (out.size() < limit) {
      auto flags = std::regex_constants::match_default;
      if (first != text.begin()) flags |= std::regex_constants::match_prev_avail;
      if (!std::regex_search(first, text.end(), m, *re_, flags)) break;
      size_t pos = static_cast<size_t>(m[0].first - text.begin());
      size_t len = static_cast<size_t>(m.length(0));
      if (pos > rangeEnd || (len > 0 && pos == rangeEnd)) break;
      if (pos + len > rangeEnd) {  // runs past the range: a later match may still fit
        if (m[0].first == text.end()) break;
        first = m[0].first + 1;
        continue;
      }
      if (len == 0) {
        if (includeEmpty && (out.empty() || out.back().pos + out.back().len != pos || out.back().len != 0))
          out.push_back({pos, 0});
        if (m[0].first == text.end()) break;
        first = m[0].first + 1;
        continue;
      }
      if (wordBoundaryOk(text, pos, len)) out.push_back({pos, len});
      first = m[0].second;
    }
  } catch (const std::regex_error&) {
    // Complexity/stack failures: return what was found so far.
  }
  return out;
}

std::optional<Match> Searcher::next(const std::wstring& text, size_t from, bool wrap) const {
  auto ms = all(text, from, text.size(), 1);
  if (!ms.empty()) return ms[0];
  if (wrap) {
    ms = all(text, 0, text.size(), 1);
    if (!ms.empty()) return ms[0];
  }
  return std::nullopt;
}

std::optional<Match> Searcher::prev(const std::wstring& text, size_t before, bool wrap) const {
  if (!valid_ || pattern_.empty()) return std::nullopt;
  before = std::min(before, text.size());
  if (!opts_.regex) {
    const std::wstring& needle = opts_.caseSensitive ? pattern_ : folded_;
    auto search = [&](size_t end) -> std::optional<Match> {
      while (end >= needle.size()) {
        auto first = text.begin();
        auto last = text.begin() + static_cast<ptrdiff_t>(end);
        auto it = opts_.caseSensitive ? std::find_end(first, last, needle.begin(), needle.end())
                                      : std::find_end(first, last, needle.begin(), needle.end(),
                                                      [](wchar_t a, wchar_t b) { return fold(a) == b; });
        if (it == last) return std::nullopt;
        size_t pos = static_cast<size_t>(it - text.begin());
        if (wordBoundaryOk(text, pos, needle.size())) return Match{pos, needle.size()};
        end = pos + needle.size() - 1;
      }
      return std::nullopt;
    };
    if (auto m = search(before)) return m;
    if (wrap) return search(text.size());
    return std::nullopt;
  }
  auto ms = all(text, 0, before);
  if (!ms.empty()) return ms.back();
  if (wrap) {
    ms = all(text, 0, text.size());
    if (!ms.empty()) return ms.back();
  }
  return std::nullopt;
}

bool Searcher::matchesExactly(const std::wstring& text, size_t pos, size_t len) const {
  if (len == 0 || !valid_ || pos + len > text.size()) return false;
  if (!opts_.regex) {
    auto ms = all(text, pos, pos + len, 1);
    return !ms.empty() && ms[0].pos == pos && ms[0].len == len;
  }
  // Anchored at `pos` with the rest of the text as context (for lookaheads and $).
  try {
    std::wsmatch m;
    auto flags = std::regex_constants::match_continuous;
    if (pos > 0) flags |= std::regex_constants::match_prev_avail;
    return std::regex_search(text.begin() + static_cast<ptrdiff_t>(pos), text.end(), m, *re_, flags) &&
           static_cast<size_t>(m.length(0)) == len && wordBoundaryOk(text, pos, len);
  } catch (const std::regex_error&) {
    return false;
  }
}

std::wstring Searcher::replacementFor(const std::wstring& text, const Match& m, std::wstring_view format) const {
  if (!opts_.regex || !re_) return std::wstring(format);
  try {
    std::wsmatch res;
    auto first = text.begin() + static_cast<ptrdiff_t>(m.pos);
    auto flags = std::regex_constants::match_continuous;
    if (m.pos > 0) flags |= std::regex_constants::match_prev_avail;
    if (std::regex_search(first, text.end(), res, *re_, flags) && static_cast<size_t>(res.length(0)) == m.len)
      return res.format(translateFormat(format));
  } catch (const std::regex_error&) {
  }
  return std::wstring(format);
}

}  // namespace verso
