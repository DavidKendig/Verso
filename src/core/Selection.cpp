#include "core/Selection.h"

namespace verso {

void SelectionSet::set(std::vector<Selection> sels, size_t primary) {
  sels_ = std::move(sels);
  primary_ = primary;
  normalize();
}

void SelectionSet::clamp(size_t size) {
  for (auto& s : sels_) {
    s.anchor = std::min(s.anchor, size);
    s.caret = std::min(s.caret, size);
  }
}

void SelectionSet::normalize() {
  if (sels_.empty()) {
    sels_.push_back(Selection{});
    primary_ = 0;
    return;
  }
  primary_ = std::min(primary_, sels_.size() - 1);
  const Selection prim = sels_[primary_];

  std::stable_sort(sels_.begin(), sels_.end(), [](const Selection& a, const Selection& b) {
    return a.begin() < b.begin() || (a.begin() == b.begin() && a.end() < b.end());
  });

  std::vector<Selection> out;
  out.reserve(sels_.size());
  for (const auto& s : sels_) {
    if (!out.empty()) {
      Selection& last = out.back();
      bool merge = s.begin() < last.end() || s.begin() == last.begin() ||
                   (s.begin() == last.end() && (s.empty() || last.empty()));
      if (merge) {
        size_t b = last.begin(), e = std::max(last.end(), s.end());
        bool rev = last.empty() ? s.reversed() : last.reversed();
        double xcol = last.xcol;
        last = rev ? Selection(e, b) : Selection(b, e);
        last.xcol = xcol;
        continue;
      }
    }
    out.push_back(s);
  }

  size_t pi = out.size() - 1;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i].begin() <= prim.begin() && prim.end() <= out[i].end()) {
      pi = i;
      break;
    }
  }
  sels_ = std::move(out);
  primary_ = pi;
}

}  // namespace verso
