#pragma once

#include <optional>
#include <string_view>
#include <vector>

namespace verso {

struct FuzzyMatch {
  int score = 0;
  std::vector<size_t> positions;  // matched indices in the candidate
};

// Case-insensitive subsequence match favoring word starts and consecutive
// runs. Spaces in the query are ignored. Returns nullopt when not all query
// characters can be matched in order.
std::optional<FuzzyMatch> fuzzyMatch(std::wstring_view query, std::wstring_view candidate);

// Matches a path, preferring hits inside the file name.
std::optional<FuzzyMatch> fuzzyMatchPath(std::wstring_view query, std::wstring_view path);

}  // namespace verso
