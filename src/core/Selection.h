#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace verso {

struct Selection {
  size_t anchor = 0;
  size_t caret = 0;
  double xcol = -1.0;  // remembered visual column for vertical movement

  Selection() = default;
  Selection(size_t a, size_t c) : anchor(a), caret(c) {}
  static Selection at(size_t p) { return Selection(p, p); }

  size_t begin() const { return std::min(anchor, caret); }
  size_t end() const { return std::max(anchor, caret); }
  size_t length() const { return end() - begin(); }
  bool empty() const { return anchor == caret; }
  bool reversed() const { return caret < anchor; }
  bool operator==(const Selection& o) const { return anchor == o.anchor && caret == o.caret; }
};

// Sorted, non-overlapping selections with one primary region.
class SelectionSet {
 public:
  const std::vector<Selection>& all() const { return sels_; }
  size_t size() const { return sels_.size(); }
  const Selection& operator[](size_t i) const { return sels_[i]; }
  size_t primaryIndex() const { return primary_; }
  const Selection& primary() const { return sels_[primary_]; }

  void set(std::vector<Selection> sels, size_t primary);
  void clamp(size_t size);
  void normalize();

 private:
  std::vector<Selection> sels_{Selection{}};
  size_t primary_ = 0;
};

}  // namespace verso
