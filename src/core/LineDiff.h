#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace verso {

// One changed region between an old and a new list of lines (0-based).
// oldCount == 0 is a pure insertion; newCount == 0 is a pure deletion, which
// sits before new line `newStart`.
struct LineHunk {
  size_t oldStart = 0, oldCount = 0;
  size_t newStart = 0, newCount = 0;
  bool operator==(const LineHunk& o) const {
    return oldStart == o.oldStart && oldCount == o.oldCount && newStart == o.newStart && newCount == o.newCount;
  }
};

// Myers line diff. When the edit distance exceeds `maxEdits`, the differing
// middle section is reported as one replacement hunk instead.
std::vector<LineHunk> diffLines(const std::vector<std::wstring_view>& oldLines,
                                const std::vector<std::wstring_view>& newLines, size_t maxEdits = 4000);

// Splits text on '\n'. A trailing newline does not produce an extra empty line.
std::vector<std::wstring_view> splitLineViews(std::wstring_view text);

}  // namespace verso
