#include "util/Fuzzy.h"

#include <algorithm>
#include <climits>
#include <cwctype>
#include <string>

namespace verso {

namespace {

constexpr int kNeg = INT_MIN / 4;

int boundaryBonus(std::wstring_view c, size_t j) {
  if (j == 0) return 10;
  wchar_t p = c[j - 1], x = c[j];
  if (std::wcschr(L" _-./\\:", p)) return 8;
  if (std::iswlower(p) && std::iswupper(x)) return 7;
  if (!std::iswalnum(p) && std::iswalnum(x)) return 5;
  return 0;
}

}  // namespace

std::optional<FuzzyMatch> fuzzyMatch(std::wstring_view query, std::wstring_view candidate) {
  std::wstring q;
  for (wchar_t ch : query)
    if (ch != L' ') q += static_cast<wchar_t>(std::towlower(ch));
  if (q.empty()) return FuzzyMatch{0, {}};
  if (q.size() > 64) q.resize(64);
  std::wstring_view c = candidate.substr(0, std::min<size_t>(candidate.size(), 1024));
  const size_t m = q.size(), n = c.size();
  if (m > n) return std::nullopt;

  std::wstring lc(c);
  for (auto& ch : lc) ch = static_cast<wchar_t>(std::towlower(ch));

  // Quick subsequence rejection.
  {
    size_t k = 0;
    for (size_t j = 0; j < n && k < m; ++j)
      if (lc[j] == q[k]) ++k;
    if (k < m) return std::nullopt;
  }

  std::vector<int> score(m * n, kNeg);
  std::vector<int> from(m * n, -1);
  for (size_t j = 0; j < n; ++j) {
    if (lc[j] != q[0]) continue;
    score[j] = boundaryBonus(c, j) - static_cast<int>(std::min<size_t>(j, 15)) + (c[j] == query[0] ? 1 : 0);
  }
  for (size_t i = 1; i < m; ++i) {
    int bestPrev = kNeg;
    int bestIdx = -1;
    for (size_t j = i; j < n; ++j) {
      // Make score[i-1][j-2] available as a gapped predecessor.
      if (j >= 2) {
        int s = score[(i - 1) * n + (j - 2)];
        if (s > bestPrev) {
          bestPrev = s;
          bestIdx = static_cast<int>(j - 2);
        }
      }
      if (lc[j] != q[i]) continue;
      int consecutive = score[(i - 1) * n + (j - 1)];
      int best = kNeg, pred = -1;
      if (consecutive > kNeg) {
        best = consecutive + 12;
        pred = static_cast<int>(j - 1);
      }
      if (bestPrev > kNeg && bestPrev - 3 > best) {
        best = bestPrev - 3;
        pred = bestIdx;
      }
      if (pred < 0) continue;
      score[i * n + j] = best + boundaryBonus(c, j);
      from[i * n + j] = pred;
    }
  }
  int best = kNeg;
  int bestJ = -1;
  for (size_t j = 0; j < n; ++j) {
    int s = score[(m - 1) * n + j];
    if (s > best) {
      best = s;
      bestJ = static_cast<int>(j);
    }
  }
  if (bestJ < 0) return std::nullopt;
  FuzzyMatch out;
  out.score = best - static_cast<int>(n / 8);
  out.positions.resize(m);
  int j = bestJ;
  for (size_t i = m; i-- > 0;) {
    out.positions[i] = static_cast<size_t>(j);
    j = from[i * n + static_cast<size_t>(j)];
  }
  return out;
}

std::optional<FuzzyMatch> fuzzyMatchPath(std::wstring_view query, std::wstring_view path) {
  auto full = fuzzyMatch(query, path);
  if (!full) return std::nullopt;
  size_t slash = path.find_last_of(L"\\/");
  size_t base = slash == std::wstring_view::npos ? 0 : slash + 1;
  if (base > 0) {
    if (auto name = fuzzyMatch(query, path.substr(base))) {
      name->score += 30;
      if (name->score >= full->score) {
        for (auto& p : name->positions) p += base;
        return name;
      }
    }
  }
  return full;
}

}  // namespace verso
