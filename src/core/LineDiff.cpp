#include "core/LineDiff.h"

#include <algorithm>
#include <functional>

namespace verso {

std::vector<std::wstring_view> splitLineViews(std::wstring_view text) {
  std::vector<std::wstring_view> out;
  size_t start = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == L'\n') {
      out.push_back(text.substr(start, i - start));
      start = i + 1;
    }
  }
  if (start < text.size()) out.push_back(text.substr(start));
  return out;
}

std::vector<LineHunk> diffLines(const std::vector<std::wstring_view>& a, const std::vector<std::wstring_view>& b,
                                size_t maxEdits) {
  std::vector<LineHunk> hunks;
  // Trim the common prefix and suffix; most edits touch a small region.
  size_t pre = 0;
  while (pre < a.size() && pre < b.size() && a[pre] == b[pre]) ++pre;
  size_t suf = 0;
  while (suf < a.size() - pre && suf < b.size() - pre && a[a.size() - 1 - suf] == b[b.size() - 1 - suf]) ++suf;
  const long long N = static_cast<long long>(a.size() - pre - suf);
  const long long M = static_cast<long long>(b.size() - pre - suf);
  if (N == 0 && M == 0) return hunks;
  if (N == 0 || M == 0) {
    hunks.push_back({pre, static_cast<size_t>(N), pre, static_cast<size_t>(M)});
    return hunks;
  }

  std::hash<std::wstring_view> h;
  std::vector<size_t> ha(static_cast<size_t>(N)), hb(static_cast<size_t>(M));
  for (long long i = 0; i < N; ++i) ha[static_cast<size_t>(i)] = h(a[pre + static_cast<size_t>(i)]);
  for (long long j = 0; j < M; ++j) hb[static_cast<size_t>(j)] = h(b[pre + static_cast<size_t>(j)]);
  auto eq = [&](long long x, long long y) {
    return ha[static_cast<size_t>(x)] == hb[static_cast<size_t>(y)] &&
           a[pre + static_cast<size_t>(x)] == b[pre + static_cast<size_t>(y)];
  };

  const long long MAX = N + M;
  const long long limit = std::min<long long>(MAX, static_cast<long long>(maxEdits));
  std::vector<long long> V(static_cast<size_t>(2 * MAX + 3), 0);
  auto at = [&](long long k) -> long long& { return V[static_cast<size_t>(k + MAX + 1)]; };
  std::vector<std::vector<long long>> trace;  // trace[d][k + d] = V[k] before step d
  long long found = -1;
  for (long long d = 0; d <= limit && found < 0; ++d) {
    std::vector<long long> snap(static_cast<size_t>(2 * d + 1));
    for (long long k = -d; k <= d; ++k) snap[static_cast<size_t>(k + d)] = at(k);
    trace.push_back(std::move(snap));
    for (long long k = -d; k <= d; k += 2) {
      long long x = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? at(k + 1) : at(k - 1) + 1;
      long long y = x - k;
      while (x < N && y < M && eq(x, y)) {
        ++x;
        ++y;
      }
      at(k) = x;
      if (x >= N && y >= M) {
        found = d;
        break;
      }
    }
  }
  if (found < 0) {
    hunks.push_back({pre, static_cast<size_t>(N), pre, static_cast<size_t>(M)});
    return hunks;
  }

  // Backtrack, collecting matched (equal) line pairs.
  std::vector<std::pair<long long, long long>> matches;
  long long x = N, y = M;
  for (long long d = found; d > 0; --d) {
    const auto& prev = trace[static_cast<size_t>(d)];  // V before step d, indexed by k + d
    auto pv = [&](long long k) { return k < -(d - 1) || k > d - 1 ? -1 : prev[static_cast<size_t>(k + d)]; };
    long long k = x - y;
    long long prevK = (k == -d || (k != d && pv(k - 1) < pv(k + 1))) ? k + 1 : k - 1;
    long long prevX = pv(prevK);
    long long prevY = prevX - prevK;
    long long startX = prevK == k + 1 ? prevX : prevX + 1;  // x after the non-diagonal step
    while (x > startX && y > startX - k) {
      --x;
      --y;
      matches.emplace_back(x, y);
    }
    x = prevX;
    y = prevY;
  }
  while (x > 0 && y > 0) {
    --x;
    --y;
    matches.emplace_back(x, y);
  }
  std::reverse(matches.begin(), matches.end());

  long long i = 0, j = 0;
  auto flush = [&](long long ai, long long bj) {
    if (ai > i || bj > j)
      hunks.push_back({pre + static_cast<size_t>(i), static_cast<size_t>(ai - i), pre + static_cast<size_t>(j),
                       static_cast<size_t>(bj - j)});
  };
  for (const auto& [ai, bj] : matches) {
    flush(ai, bj);
    i = ai + 1;
    j = bj + 1;
  }
  flush(N, M);
  return hunks;
}

}  // namespace verso
