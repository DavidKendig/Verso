#include "core/Editor.h"

#include <algorithm>
#include <cwctype>
#include <map>

namespace verso {

namespace {

constexpr size_t npos = static_cast<size_t>(-1);

bool isOpener(wchar_t c) { return c == L'(' || c == L'[' || c == L'{'; }
bool isCloser(wchar_t c) { return c == L')' || c == L']' || c == L'}'; }
bool isQuote(wchar_t c) { return c == L'"' || c == L'\'' || c == L'`'; }

wchar_t closerFor(wchar_t c) {
  switch (c) {
    case L'(': return L')';
    case L'[': return L']';
    case L'{': return L'}';
    case L'"': return L'"';
    case L'\'': return L'\'';
    case L'`': return L'`';
    default: return 0;
  }
}

wchar_t openerFor(wchar_t c) {
  switch (c) {
    case L')': return L'(';
    case L']': return L'[';
    case L'}': return L'{';
    default: return 0;
  }
}

long long sdiff(size_t a, size_t b) { return static_cast<long long>(a) - static_cast<long long>(b); }
size_t shifted(size_t p, long long delta) { return static_cast<size_t>(static_cast<long long>(p) + delta); }

}  // namespace

Editor::Editor() = default;

// ---------------------------------------------------------------------------
// Content and history

void Editor::setSelections(std::vector<Selection> sels, size_t primary) {
  sels_.set(std::move(sels), primary);
  sels_.clamp(buf_.size());
  sels_.normalize();
  groupBroken_ = true;
  touchSelection();
}

void Editor::setText(std::wstring text) {
  ++revision_;
  buf_.assign(std::move(text));
  sels_.set({Selection{}}, 0);
  undo_.clear();
  redo_.clear();
  savedId_ = 0;
  firstChanged_ = 0;
  groupBroken_ = true;
  touchSelection();
}

void Editor::replaceAllText(std::wstring text) {
  commit({{0, buf_.size(), std::move(text)}}, sels_.all(), sels_.primaryIndex(), EditKind::Other);
}

std::wstring Editor::textOf(const Selection& s) const { return std::wstring(buf_.view(s.begin(), s.length())); }

size_t Editor::takeFirstChangedLine() {
  size_t r = firstChanged_;
  firstChanged_ = npos;
  return r;
}

void Editor::applyRaw(size_t pos, size_t eraseLen, std::wstring_view insert) {
  ++revision_;
  firstChanged_ = std::min(firstChanged_, buf_.lineOf(pos));
  buf_.erase(pos, eraseLen);
  buf_.insert(pos, insert);
}

void Editor::commit(std::vector<Change> changes, std::vector<Selection> after, size_t afterPrimary, EditKind kind) {
  std::stable_sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return a.pos < b.pos; });
  // Changes apply back to front on the original offsets, so they must not overlap.
  // Callers expanding several carets can produce the same change twice (two carets in
  // one word); drop exact duplicates and any change that overlaps an earlier one.
  {
    std::vector<Change> kept;
    kept.reserve(changes.size());
    size_t end = 0;
    for (auto& c : changes) {
      if (!kept.empty()) {
        const Change& p = kept.back();
        if (c.pos == p.pos && c.len == p.len && c.text == p.text) continue;
        if (c.pos < end) continue;
      }
      end = c.pos + c.len;
      kept.push_back(std::move(c));
    }
    changes = std::move(kept);
  }
  Transaction t;
  t.before = sels_.all();
  t.beforePrimary = sels_.primaryIndex();
  t.kind = kind;
  for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
    size_t pos = std::min(it->pos, buf_.size());
    size_t len = std::min(it->len, buf_.size() - pos);
    if (len == 0 && it->text.empty()) continue;
    std::wstring removed(buf_.view(pos, len));
    if (removed == it->text) continue;
    applyRaw(pos, len, it->text);
    t.edits.push_back({pos, std::move(removed), it->text});
  }
  sels_.set(std::move(after), afterPrimary);
  sels_.clamp(buf_.size());
  sels_.normalize();
  touchSelection();
  if (t.edits.empty()) return;
  t.after = sels_.all();
  t.afterPrimary = sels_.primaryIndex();
  pushTransaction(std::move(t));
}

void Editor::commitMapped(std::vector<Change> changes, EditKind kind) {
  std::stable_sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return a.pos < b.pos; });
  std::vector<Selection> after;
  after.reserve(sels_.size());
  for (const auto& s : sels_.all()) after.push_back(mapSelection(s, changes));
  commit(std::move(changes), std::move(after), sels_.primaryIndex(), kind);
}

void Editor::pushTransaction(Transaction t) {
  redo_.clear();
  if (!groupBroken_ && t.kind != EditKind::Other && !undo_.empty()) {
    Transaction& p = undo_.back();
    bool contiguous = p.kind == t.kind && p.id != savedId_ && p.after == t.before;
    bool wordBreak = false;
    if (t.kind == EditKind::Typing && !t.edits.empty() && !p.edits.empty()) {
      const auto& ins = t.edits.back().inserted;
      const auto& prev = p.edits.back().inserted;
      wordBreak = !ins.empty() && isSpace(ins[0]) && !prev.empty() && !isSpace(prev.back());
    }
    if (contiguous && !wordBreak) {
      for (auto& e : t.edits) p.edits.push_back(std::move(e));
      p.after = std::move(t.after);
      p.afterPrimary = t.afterPrimary;
      p.id = ++nextId_;
      return;
    }
  }
  groupBroken_ = false;
  t.id = ++nextId_;
  undo_.push_back(std::move(t));
  if (undo_.size() > 5000) undo_.erase(undo_.begin());
}

void Editor::undo() {
  if (undo_.empty()) return;
  Transaction t = std::move(undo_.back());
  undo_.pop_back();
  for (auto it = t.edits.rbegin(); it != t.edits.rend(); ++it) applyRaw(it->pos, it->inserted.size(), it->removed);
  sels_.set(t.before, t.beforePrimary);
  sels_.clamp(buf_.size());
  sels_.normalize();
  redo_.push_back(std::move(t));
  groupBroken_ = true;
  touchSelection();
}

void Editor::redo() {
  if (redo_.empty()) return;
  Transaction t = std::move(redo_.back());
  redo_.pop_back();
  for (const auto& e : t.edits) applyRaw(e.pos, e.removed.size(), e.inserted);
  sels_.set(t.after, t.afterPrimary);
  sels_.clamp(buf_.size());
  sels_.normalize();
  undo_.push_back(std::move(t));
  groupBroken_ = true;
  touchSelection();
}

size_t Editor::mapPos(size_t pos, const std::vector<Change>& changes, bool right) {
  long long delta = 0;
  for (const auto& c : changes) {
    size_t cend = c.pos + c.len;
    long long d = static_cast<long long>(c.text.size()) - static_cast<long long>(c.len);
    if (cend < pos) {
      delta += d;
      continue;
    }
    if (cend == pos) {
      if (c.len > 0 || right) {
        delta += d;
        continue;
      }
      break;
    }
    if (c.pos < pos) return shifted(c.pos, delta) + (right ? c.text.size() : 0);
    break;
  }
  return shifted(pos, delta);
}

Selection Editor::mapSelection(const Selection& s, const std::vector<Change>& changes) const {
  if (s.empty()) return Selection::at(mapPos(s.caret, changes, true));
  size_t b = mapPos(s.begin(), changes, false);
  size_t e = mapPos(s.end(), changes, true);
  return s.reversed() ? Selection(e, b) : Selection(b, e);
}

// ---------------------------------------------------------------------------
// Geometry helpers

std::wstring Editor::indentUnit() const {
  return config.translateTabs ? std::wstring(static_cast<size_t>(config.tabSize), L' ') : std::wstring(L"\t");
}

size_t Editor::columnAt(size_t pos) const {
  pos = std::min(pos, buf_.size());
  size_t ls = buf_.lineStart(buf_.lineOf(pos));
  size_t col = 0;
  const size_t tab = static_cast<size_t>(std::max(1, config.tabSize));
  const auto& t = buf_.text();
  for (size_t i = ls; i < pos; ++i) {
    wchar_t c = t[i];
    col += c == L'\t' ? tab - col % tab : static_cast<size_t>(unitColumns(c));
  }
  return col;
}

size_t Editor::posAtColumn(size_t line, double col, bool round) const {
  size_t ls = buf_.lineStart(line), le = buf_.lineEnd(line);
  const size_t tab = static_cast<size_t>(std::max(1, config.tabSize));
  const auto& t = buf_.text();
  size_t cur = 0, i = ls;
  while (i < le) {
    wchar_t c = t[i];
    size_t clen = (isHighSurrogate(c) && i + 1 < le && isLowSurrogate(t[i + 1])) ? 2 : 1;
    size_t w = c == L'\t' ? tab - cur % tab : static_cast<size_t>(unitColumns(c));
    if (w == 0) {
      i += clen;
      continue;
    }
    double threshold = round ? static_cast<double>(cur) + static_cast<double>(w) / 2.0 : static_cast<double>(cur + w);
    if (col < threshold) return i;
    cur += w;
    i += clen;
  }
  return le;
}

std::pair<size_t, size_t> Editor::wordAt(size_t pos) const {
  auto w = [&](size_t i) { return i < buf_.size() && isWordChar(buf_.at(i), config.wordSeparators); };
  if (!w(pos) && !(pos > 0 && w(pos - 1))) return {pos, pos};
  size_t b = pos, e = pos;
  while (b > 0 && w(b - 1)) --b;
  while (w(e)) ++e;
  return {b, e};
}

size_t Editor::matchBracket(size_t pos) const {
  wchar_t c = buf_.at(pos);
  const auto& t = buf_.text();
  constexpr size_t kLimit = 4'000'000;
  int depth = 0;
  if (isOpener(c)) {
    wchar_t cl = closerFor(c);
    for (size_t i = pos; i < t.size() && i - pos < kLimit; ++i) {
      if (t[i] == c)
        ++depth;
      else if (t[i] == cl && --depth == 0)
        return i;
    }
  } else if (isCloser(c)) {
    wchar_t op = openerFor(c);
    for (size_t i = pos + 1; i-- > 0 && pos - i < kLimit;) {
      if (t[i] == c)
        ++depth;
      else if (t[i] == op && --depth == 0)
        return i;
    }
  }
  return npos;
}

size_t Editor::nextCharPos(size_t p) const {
  if (p >= buf_.size()) return buf_.size();
  if (isHighSurrogate(buf_.at(p)) && isLowSurrogate(buf_.at(p + 1))) return p + 2;
  return p + 1;
}

size_t Editor::prevCharPos(size_t p) const {
  if (p == 0) return 0;
  if (p >= 2 && isLowSurrogate(buf_.at(p - 1)) && isHighSurrogate(buf_.at(p - 2))) return p - 2;
  return p - 1;
}

size_t Editor::wordBoundary(size_t p, bool forward) const {
  auto cls = [&](wchar_t c) { return classify(c, config.wordSeparators); };
  const size_t n = buf_.size();
  if (forward) {
    if (p >= n) return n;
    if (buf_.at(p) == L'\n') return p + 1;
    while (p < n && cls(buf_.at(p)) == CharClass::Space) ++p;
    if (p < n && buf_.at(p) != L'\n') {
      CharClass c = cls(buf_.at(p));
      while (p < n && cls(buf_.at(p)) == c) ++p;
    }
    return p;
  }
  if (p == 0) return 0;
  if (buf_.at(p - 1) == L'\n') return p - 1;
  while (p > 0 && cls(buf_.at(p - 1)) == CharClass::Space) --p;
  if (p > 0 && buf_.at(p - 1) != L'\n') {
    CharClass c = cls(buf_.at(p - 1));
    while (p > 0 && cls(buf_.at(p - 1)) == c) --p;
  }
  return p;
}

size_t Editor::subwordBoundary(size_t p, bool forward) const {
  auto cls = [&](wchar_t c) { return classify(c, config.wordSeparators); };
  auto upper = [](wchar_t c) { return std::iswupper(c) != 0; };
  const size_t n = buf_.size();
  if (forward) {
    if (p >= n) return n;
    if (buf_.at(p) == L'\n') return p + 1;
    while (p < n && cls(buf_.at(p)) == CharClass::Space) ++p;
    if (p >= n || buf_.at(p) == L'\n') return p;
    wchar_t c = buf_.at(p);
    if (cls(c) != CharClass::Word) {
      CharClass k = cls(c);
      while (p < n && cls(buf_.at(p)) == k) ++p;
      return p;
    }
    if (c == L'_') {
      while (p < n && buf_.at(p) == L'_') ++p;
      return p;
    }
    if (upper(c)) {
      ++p;
      if (p < n && upper(buf_.at(p))) {
        while (p < n && upper(buf_.at(p)) && !(p + 1 < n && std::iswlower(buf_.at(p + 1)))) ++p;
        return p;
      }
    }
    while (p < n && cls(buf_.at(p)) == CharClass::Word && !upper(buf_.at(p)) && buf_.at(p) != L'_') ++p;
    return p;
  }
  if (p == 0) return 0;
  if (buf_.at(p - 1) == L'\n') return p - 1;
  while (p > 0 && cls(buf_.at(p - 1)) == CharClass::Space) --p;
  if (p == 0 || buf_.at(p - 1) == L'\n') return p;
  wchar_t c = buf_.at(p - 1);
  if (cls(c) != CharClass::Word) {
    CharClass k = cls(c);
    while (p > 0 && cls(buf_.at(p - 1)) == k) --p;
    return p;
  }
  if (c == L'_') {
    while (p > 0 && buf_.at(p - 1) == L'_') --p;
    return p;
  }
  if (upper(c)) {
    while (p > 0 && upper(buf_.at(p - 1))) --p;
    return p;
  }
  while (p > 0 && cls(buf_.at(p - 1)) == CharClass::Word && !upper(buf_.at(p - 1)) && buf_.at(p - 1) != L'_') --p;
  if (p > 0 && upper(buf_.at(p - 1))) --p;
  return p;
}

size_t Editor::moveTarget(size_t pos, MoveBy by, bool forward) const {
  switch (by) {
    case MoveBy::Chars:
      return forward ? nextCharPos(pos) : prevCharPos(pos);
    case MoveBy::Words:
    case MoveBy::WordEnds:
      return wordBoundary(pos, forward);
    case MoveBy::Subwords:
    case MoveBy::SubwordEnds:
      return subwordBoundary(pos, forward);
    case MoveBy::LineBegin: {
      size_t line = buf_.lineOf(pos);
      size_t ls = buf_.lineStart(line);
      size_t first = ls + leadingBlanks(buf_.lineText(line));
      return pos == first ? ls : first;
    }
    case MoveBy::LineEnd:
      return buf_.lineEnd(buf_.lineOf(pos));
    case MoveBy::DocBegin:
      return 0;
    case MoveBy::DocEnd:
      return buf_.size();
    case MoveBy::Brackets: {
      size_t m;
      if (isOpener(buf_.at(pos)) && (m = matchBracket(pos)) != npos) return m + 1;
      if (pos > 0 && isCloser(buf_.at(pos - 1)) && (m = matchBracket(pos - 1)) != npos) return m;
      if (isCloser(buf_.at(pos)) && (m = matchBracket(pos)) != npos) return m;
      if (pos > 0 && isOpener(buf_.at(pos - 1)) && (m = matchBracket(pos - 1)) != npos) return m;
      int depth = 0;
      const auto& t = buf_.text();
      for (size_t i = pos; i < t.size(); ++i) {
        if (isOpener(t[i])) {
          ++depth;
        } else if (isCloser(t[i])) {
          if (depth == 0) return i;
          --depth;
        }
      }
      return pos;
    }
    default:
      return pos;
  }
}

std::vector<std::pair<size_t, size_t>> Editor::selectedLineBlocks() const {
  std::vector<std::pair<size_t, size_t>> blocks;
  for (const auto& s : sels_.all()) {
    size_t l0 = buf_.lineOf(s.begin()), l1 = buf_.lineOf(s.end());
    if (!s.empty() && l1 > l0 && s.end() == buf_.lineStart(l1)) --l1;
    if (!blocks.empty() && l0 <= blocks.back().second + 1)
      blocks.back().second = std::max(blocks.back().second, l1);
    else
      blocks.emplace_back(l0, l1);
  }
  return blocks;
}

// ---------------------------------------------------------------------------
// Typing and deletion

void Editor::insertText(std::wstring_view s, bool typed) {
  std::wstring text = normalizeNewlines(s);
  if (text.empty()) return;
  const auto& all = sels_.all();
  std::vector<Change> ch;
  std::vector<Selection> after;
  long long delta = 0;
  for (const auto& sel : all) {
    size_t b = sel.begin(), e = sel.end();
    std::wstring ins = text;
    size_t caretOff = ins.size();
    bool wrapSel = false, overtype = false;
    if (typed && config.autoMatch && text.size() == 1) {
      wchar_t c = text[0];
      wchar_t next = buf_.at(e);
      wchar_t prev = b > 0 ? buf_.at(b - 1) : L'\0';
      if (sel.empty() && (isCloser(c) || isQuote(c)) && next == c) {
        overtype = true;
      } else if (isOpener(c) || isQuote(c)) {
        if (!sel.empty()) {
          ins = std::wstring(1, c) + textOf(sel) + closerFor(c);
          wrapSel = true;
        } else {
          bool nextOk = next == L'\0' || next == L'\n' || isSpace(next) || isCloser(next) ||
                        next == L';' || next == L',';
          bool prevOk = !isQuote(c) || prev == L'\0' || prev == L'\n' ||
                        (!isWordChar(prev, config.wordSeparators) && prev != c && prev != L'\\');
          if (nextOk && prevOk) {
            ins = {c, closerFor(c)};
            caretOff = 1;
          }
        }
      }
    }
    size_t base = shifted(b, delta);
    if (overtype) {
      after.push_back(Selection::at(base + 1));
      continue;
    }
    ch.push_back({b, e - b, ins});
    if (wrapSel) {
      Selection inner(base + 1, base + ins.size() - 1);
      after.push_back(sel.reversed() ? Selection(inner.caret, inner.anchor) : inner);
    } else {
      after.push_back(Selection::at(base + caretOff));
    }
    delta += sdiff(ins.size(), e - b);
  }
  commit(std::move(ch), std::move(after), sels_.primaryIndex(), typed ? EditKind::Typing : EditKind::Other);
}

void Editor::newline() {
  const std::wstring unit = indentUnit();
  std::vector<Change> ch;
  std::vector<Selection> after;
  long long delta = 0;
  size_t lastEnd = 0;
  for (const auto& sel : sels_.all()) {
    size_t b = sel.begin(), e = sel.end();
    size_t line = buf_.lineOf(b);
    size_t ls = buf_.lineStart(line);
    std::wstring_view lineText = buf_.lineText(line);
    size_t ws = leadingBlanks(lineText);
    std::wstring indent(lineText.substr(0, std::min(ws, b - ls)));
    size_t pb = b;
    while (pb > ls && isBlank(buf_.at(pb - 1))) --pb;
    wchar_t prev = pb > ls ? buf_.at(pb - 1) : L'\0';
    wchar_t next = buf_.at(e);
    size_t replB = b;
    if (config.trimAutoWhitespace) replB = std::max(pb, lastEnd);
    std::wstring ins = L"\n";
    size_t caret;
    if (config.autoIndent) {
      ins += indent;
      if (prev && isOpener(prev) && next == closerFor(prev)) {
        ins += unit;
        caret = ins.size();
        ins += L"\n" + indent;
      } else if (isOpener(prev) || (comments.indentAfterColon && prev == L':')) {
        ins += unit;
        caret = ins.size();
      } else {
        caret = ins.size();
      }
    } else {
      caret = ins.size();
    }
    ch.push_back({replB, e - replB, ins});
    after.push_back(Selection::at(shifted(replB, delta) + caret));
    delta += sdiff(ins.size(), e - replB);
    lastEnd = e;
  }
  commit(std::move(ch), std::move(after), sels_.primaryIndex(), EditKind::Other);
}

void Editor::insertLineAfter() {
  std::vector<Change> ch;
  std::vector<Selection> after;
  long long delta = 0;
  size_t lastLine = npos;
  for (const auto& sel : sels_.all()) {
    size_t line = buf_.lineOf(sel.end());
    std::wstring_view lt = buf_.lineText(line);
    std::wstring t = L"\n" + std::wstring(lt.substr(0, leadingBlanks(lt)));
    size_t pos = buf_.lineEnd(line);
    if (line == lastLine) {
      after.push_back(after.back());
      continue;
    }
    lastLine = line;
    ch.push_back({pos, 0, t});
    after.push_back(Selection::at(shifted(pos, delta) + t.size()));
    delta += static_cast<long long>(t.size());
  }
  commit(std::move(ch), std::move(after), sels_.primaryIndex(), EditKind::Other);
}

void Editor::insertLineBefore() {
  std::vector<Change> ch;
  std::vector<Selection> after;
  long long delta = 0;
  size_t lastLine = npos;
  for (const auto& sel : sels_.all()) {
    size_t line = buf_.lineOf(sel.begin());
    std::wstring_view lt = buf_.lineText(line);
    std::wstring indent(lt.substr(0, leadingBlanks(lt)));
    size_t pos = buf_.lineStart(line);
    if (line == lastLine) {
      after.push_back(after.back());
      continue;
    }
    lastLine = line;
    ch.push_back({pos, 0, indent + L"\n"});
    after.push_back(Selection::at(shifted(pos, delta) + indent.size()));
    delta += static_cast<long long>(indent.size() + 1);
  }
  commit(std::move(ch), std::move(after), sels_.primaryIndex(), EditKind::Other);
}

void Editor::deleteRanges(std::vector<std::pair<size_t, size_t>> ranges, EditKind kind) {
  std::sort(ranges.begin(), ranges.end());
  std::vector<std::pair<size_t, size_t>> merged;
  for (const auto& r : ranges) {
    if (r.second <= r.first) continue;
    if (!merged.empty() && r.first < merged.back().second)
      merged.back().second = std::max(merged.back().second, r.second);
    else
      merged.push_back(r);
  }
  std::vector<Change> ch;
  for (const auto& r : merged) ch.push_back({r.first, r.second - r.first, L""});
  std::vector<Selection> after;
  for (const auto& s : sels_.all()) after.push_back(Selection::at(mapPos(s.begin(), ch, false)));
  commit(std::move(ch), std::move(after), sels_.primaryIndex(), kind);
}

void Editor::backspace() {
  std::vector<std::pair<size_t, size_t>> ranges;
  const size_t tab = static_cast<size_t>(std::max(1, config.tabSize));
  for (const auto& s : sels_.all()) {
    if (!s.empty()) {
      ranges.emplace_back(s.begin(), s.end());
      continue;
    }
    size_t p = s.caret;
    if (p == 0) continue;
    wchar_t prev = buf_.at(p - 1), next = buf_.at(p);
    if (config.autoMatch && (isOpener(prev) || isQuote(prev)) && next != L'\0' && closerFor(prev) == next) {
      ranges.emplace_back(p - 1, p + 1);
      continue;
    }
    if (config.translateTabs && prev == L' ') {
      size_t ls = buf_.lineStart(buf_.lineOf(p));
      bool allSpaces = true;
      for (size_t i = ls; i < p; ++i)
        if (buf_.at(i) != L' ') allSpaces = false;
      if (allSpaces) {
        size_t col = p - ls;
        size_t n = col % tab == 0 ? tab : col % tab;
        ranges.emplace_back(p - std::min(n, col), p);
        continue;
      }
    }
    ranges.emplace_back(prevCharPos(p), p);
  }
  deleteRanges(std::move(ranges), EditKind::DeleteBack);
}

void Editor::deleteForward() {
  std::vector<std::pair<size_t, size_t>> ranges;
  for (const auto& s : sels_.all()) {
    if (!s.empty())
      ranges.emplace_back(s.begin(), s.end());
    else if (s.caret < buf_.size())
      ranges.emplace_back(s.caret, nextCharPos(s.caret));
  }
  deleteRanges(std::move(ranges), EditKind::DeleteForward);
}

void Editor::deleteWord(bool forward) {
  std::vector<std::pair<size_t, size_t>> ranges;
  for (const auto& s : sels_.all()) {
    if (!s.empty()) {
      ranges.emplace_back(s.begin(), s.end());
      continue;
    }
    size_t t = wordBoundary(s.caret, forward);
    ranges.emplace_back(std::min(t, s.caret), std::max(t, s.caret));
  }
  deleteRanges(std::move(ranges), forward ? EditKind::DeleteForward : EditKind::DeleteBack);
}

void Editor::deleteToLineBoundary(bool forward) {
  std::vector<std::pair<size_t, size_t>> ranges;
  for (const auto& s : sels_.all()) {
    if (!s.empty()) {
      ranges.emplace_back(s.begin(), s.end());
      continue;
    }
    size_t line = buf_.lineOf(s.caret);
    if (forward) {
      size_t le = buf_.lineEnd(line);
      ranges.emplace_back(s.caret, le == s.caret ? nextCharPos(le) : le);
    } else {
      size_t ls = buf_.lineStart(line);
      ranges.emplace_back(ls == s.caret ? prevCharPos(ls) : ls, s.caret);
    }
  }
  deleteRanges(std::move(ranges), EditKind::Other);
}

void Editor::tabKey() {
  for (const auto& s : sels_.all()) {
    if (buf_.lineOf(s.begin()) != buf_.lineOf(s.end())) {
      indent();
      return;
    }
  }
  const size_t tab = static_cast<size_t>(std::max(1, config.tabSize));
  std::vector<Change> ch;
  std::vector<Selection> after;
  long long delta = 0;
  for (const auto& s : sels_.all()) {
    std::wstring t = L"\t";
    if (config.translateTabs) t.assign(tab - columnAt(s.begin()) % tab, L' ');
    ch.push_back({s.begin(), s.length(), t});
    after.push_back(Selection::at(shifted(s.begin(), delta) + t.size()));
    delta += sdiff(t.size(), s.length());
  }
  commit(std::move(ch), std::move(after), sels_.primaryIndex(), EditKind::Typing);
}


void Editor::indent() {
  const std::wstring unit = indentUnit();
  std::vector<Change> ch;
  for (auto [l0, l1] : selectedLineBlocks()) {
    for (size_t l = l0; l <= l1; ++l) {
      if (buf_.lineLength(l) == 0 && l0 != l1) continue;
      ch.push_back({buf_.lineStart(l), 0, unit});
    }
  }
  commitMapped(std::move(ch), EditKind::Other);
}

void Editor::unindent() {
  const size_t tab = static_cast<size_t>(std::max(1, config.tabSize));
  std::vector<Change> ch;
  for (auto [l0, l1] : selectedLineBlocks()) {
    for (size_t l = l0; l <= l1; ++l) {
      std::wstring_view t = buf_.lineText(l);
      size_t n = 0;
      if (!t.empty() && t[0] == L'\t')
        n = 1;
      else
        while (n < tab && n < t.size() && t[n] == L' ') ++n;
      if (n) ch.push_back({buf_.lineStart(l), n, L""});
    }
  }
  commitMapped(std::move(ch), EditKind::Other);
}

// ---------------------------------------------------------------------------
// Movement and selection

void Editor::move(MoveBy by, bool forward, bool extend, int pageLines) {
  std::vector<Selection> out;
  out.reserve(sels_.size());
  for (const auto& s : sels_.all()) {
    Selection n;
    if (by == MoveBy::Lines || by == MoveBy::Pages) {
      size_t line = buf_.lineOf(s.caret);
      double x = s.xcol >= 0 ? s.xcol : static_cast<double>(columnAt(s.caret));
      long long step = (by == MoveBy::Lines ? 1 : std::max(1, pageLines)) * (forward ? 1 : -1);
      long long target = static_cast<long long>(line) + step;
      size_t p;
      bool keepX = true;
      if (target < 0) {
        p = 0;
        keepX = false;
      } else if (target >= static_cast<long long>(buf_.lineCount())) {
        p = buf_.size();
        keepX = false;
      } else {
        p = posAtColumn(static_cast<size_t>(target), x, true);
      }
      n = extend ? Selection(s.anchor, p) : Selection::at(p);
      if (keepX) n.xcol = x;
    } else if (by == MoveBy::Chars && !extend && !s.empty()) {
      n = Selection::at(forward ? s.end() : s.begin());
    } else {
      size_t p = moveTarget(s.caret, by, forward);
      n = extend ? Selection(s.anchor, p) : Selection::at(p);
    }
    out.push_back(n);
  }
  setSelections(std::move(out), sels_.primaryIndex());
}

void Editor::selectAll() { setSelections({Selection(0, buf_.size())}, 0); }

void Editor::singleSelection() {
  if (sels_.size() <= 1) return;
  setSelections({sels_.all().front()}, 0);
}

void Editor::splitSelectionIntoLines() {
  std::vector<Selection> out;
  for (const auto& s : sels_.all()) {
    if (s.empty()) {
      out.push_back(s);
      continue;
    }
    size_t l0 = buf_.lineOf(s.begin()), l1 = buf_.lineOf(s.end());
    for (size_t l = l0; l <= l1; ++l) {
      size_t lb = std::max(s.begin(), buf_.lineStart(l));
      size_t le = std::min(s.end(), buf_.lineEnd(l));
      if (l == l1 && l > l0 && s.end() == buf_.lineStart(l)) continue;
      out.emplace_back(lb, le);
    }
  }
  size_t primary = out.empty() ? 0 : out.size() - 1;
  setSelections(std::move(out), primary);
}

void Editor::addCursorLine(bool down) {
  std::vector<Selection> out = sels_.all();
  size_t primary = sels_.primaryIndex();
  const size_t count = sels_.size();
  for (size_t i = 0; i < count; ++i) {
    const Selection s = sels_[i];
    size_t line = buf_.lineOf(s.caret);
    if (down ? line + 1 >= buf_.lineCount() : line == 0) continue;
    size_t target = down ? line + 1 : line - 1;
    double x = s.xcol >= 0 ? s.xcol : static_cast<double>(columnAt(s.caret));
    double ax = static_cast<double>(columnAt(s.anchor));
    Selection n(s.empty() ? posAtColumn(target, x) : posAtColumn(target, ax), posAtColumn(target, x));
    n.xcol = x;
    out.push_back(n);
    if (down ? i == count - 1 : i == 0) primary = out.size() - 1;
  }
  setSelections(std::move(out), primary);
}

void Editor::expandToLine() {
  std::vector<Selection> out;
  for (const auto& s : sels_.all()) {
    size_t b = buf_.lineStart(buf_.lineOf(s.begin()));
    size_t el = buf_.lineOf(s.end());
    // A selection already ending at a line start grows to include that line.
    size_t e = std::min(buf_.lineEnd(el) + 1, buf_.size());
    out.emplace_back(b, e);
  }
  setSelections(std::move(out), sels_.primaryIndex());
}

void Editor::expandToWord() {
  std::vector<Selection> out;
  for (const auto& s : sels_.all()) {
    if (s.empty()) {
      auto [b, e] = wordAt(s.caret);
      out.push_back(b < e ? Selection(b, e) : s);
    } else {
      out.push_back(s);
    }
  }
  setSelections(std::move(out), sels_.primaryIndex());
}

void Editor::expandToBrackets() {
  const auto& t = buf_.text();
  std::vector<Selection> out;
  for (const auto& s : sels_.all()) {
    size_t o = npos;
    int depth = 0;
    for (size_t i = s.begin(); i-- > 0;) {
      if (isCloser(t[i])) {
        ++depth;
      } else if (isOpener(t[i])) {
        if (depth == 0) {
          o = i;
          break;
        }
        --depth;
      }
    }
    size_t c = o == npos ? npos : matchBracket(o);
    if (o == npos || c == npos || c < s.end()) {
      out.push_back(s);
      continue;
    }
    if (s.begin() == o + 1 && s.end() == c)
      out.emplace_back(o, c + 1);
    else
      out.emplace_back(o + 1, c);
  }
  setSelections(std::move(out), sels_.primaryIndex());
}

void Editor::setCaret(size_t pos, bool extend, bool add) {
  pos = std::min(pos, buf_.size());
  if (add) {
    auto v = sels_.all();
    v.push_back(Selection::at(pos));
    size_t primary = v.size() - 1;
    setSelections(std::move(v), primary);
  } else if (extend) {
    auto v = sels_.all();
    size_t pi = sels_.primaryIndex();
    v[pi] = Selection(v[pi].anchor, pos);
    setSelections(std::move(v), pi);
  } else {
    setSelections({Selection::at(pos)}, 0);
  }
}

void Editor::setColumnSelection(size_t anchorLine, double anchorCol, size_t line, double col) {
  size_t l0 = std::min(anchorLine, line), l1 = std::max(anchorLine, line);
  l1 = std::min(l1, buf_.lineCount() - 1);
  std::vector<Selection> out;
  size_t primary = 0;
  for (size_t l = l0; l <= l1; ++l) {
    Selection s(posAtColumn(l, anchorCol), posAtColumn(l, col));
    s.xcol = col;
    if (l == line) primary = out.size();
    out.push_back(s);
  }
  setSelections(std::move(out), primary);
}

// ---------------------------------------------------------------------------
// Find under cursor

std::vector<Match> Editor::occurrences(const std::wstring& needle, bool wholeWord) const {
  Searcher s;
  s.compile(needle, {false, true, wholeWord}, nullptr, config.wordSeparators);
  return s.all(buf_.text());
}

void Editor::findUnderExpand() {
  const auto& all = sels_.all();
  bool anyEmpty = std::any_of(all.begin(), all.end(), [](const Selection& s) { return s.empty(); });
  if (anyEmpty) {
    expandToWord();
    wholeWordVersion_ = selVersion_;
    return;
  }
  bool whole = wholeWordVersion_ == selVersion_;
  const Selection p = sels_.primary();
  auto ms = occurrences(textOf(p), whole);
  auto selected = [&](const Match& m) {
    return std::any_of(all.begin(), all.end(),
                       [&](const Selection& s) { return s.begin() == m.pos && s.end() == m.pos + m.len; });
  };
  const Match* found = nullptr;
  for (const auto& m : ms)
    if (m.pos >= p.end() && !selected(m)) {
      found = &m;
      break;
    }
  if (!found)
    for (const auto& m : ms)
      if (!selected(m)) {
        found = &m;
        break;
      }
  if (!found) return;
  auto v = all;
  v.emplace_back(found->pos, found->pos + found->len);
  size_t primary = v.size() - 1;
  setSelections(std::move(v), primary);
  wholeWordVersion_ = whole ? selVersion_ : UINT64_MAX;
}

void Editor::findUnderExpandSkip() {
  const auto& all = sels_.all();
  if (std::any_of(all.begin(), all.end(), [](const Selection& s) { return s.empty(); })) {
    findUnderExpand();
    return;
  }
  const Selection old = sels_.primary();
  bool whole = wholeWordVersion_ == selVersion_;
  size_t before = sels_.size();
  findUnderExpand();
  if (sels_.size() == before) return;
  const Selection added = sels_.primary();
  std::vector<Selection> v;
  size_t primary = 0;
  for (const auto& s : sels_.all()) {
    if (s == old) continue;
    if (s == added) primary = v.size();
    v.push_back(s);
  }
  setSelections(std::move(v), primary);
  if (whole) wholeWordVersion_ = selVersion_;
}

void Editor::findAllUnder() {
  bool whole = wholeWordVersion_ == selVersion_;
  if (sels_.primary().empty()) {
    expandToWord();
    whole = true;
  }
  const Selection p = sels_.primary();
  if (p.empty()) return;
  auto ms = occurrences(textOf(p), whole);
  if (ms.empty()) return;
  std::vector<Selection> v;
  size_t primary = 0;
  for (const auto& m : ms) {
    if (m.pos == p.begin()) primary = v.size();
    v.emplace_back(m.pos, m.pos + m.len);
  }
  setSelections(std::move(v), primary);
  if (whole) wholeWordVersion_ = selVersion_;
}

bool Editor::findUnder(bool forward) {
  bool whole = wholeWordVersion_ == selVersion_;
  if (sels_.primary().empty()) {
    auto [b, e] = wordAt(sels_.primary().caret);
    if (b == e) return false;
    setSelections({Selection(b, e)}, 0);
    whole = true;
  }
  const Selection p = sels_.primary();
  Searcher s;
  s.compile(textOf(p), {false, true, whole}, nullptr, config.wordSeparators);
  bool found = findNext(s, forward, true);
  if (whole) wholeWordVersion_ = selVersion_;
  return found;
}

// ---------------------------------------------------------------------------
// Line operations

void Editor::swapLines(bool up) {
  auto blocks = selectedLineBlocks();
  if (blocks.empty()) return;
  const size_t last = buf_.lineCount() - 1;
  if (up ? blocks.front().first == 0 : blocks.back().second >= last) return;
  std::vector<Change> ch;
  std::vector<Selection> after = sels_.all();
  for (auto [l0, l1] : blocks) {
    size_t bs = buf_.lineStart(l0), be = buf_.lineEnd(l1);
    std::wstring block(buf_.view(bs, be - bs));
    long long shift;
    if (up) {
      size_t a = l0 - 1;
      std::wstring above(buf_.lineText(a));
      ch.push_back({buf_.lineStart(a), be - buf_.lineStart(a), block + L"\n" + above});
      shift = -static_cast<long long>(above.size() + 1);
    } else {
      size_t b = l1 + 1;
      std::wstring below(buf_.lineText(b));
      ch.push_back({bs, buf_.lineEnd(b) - bs, below + L"\n" + block});
      shift = static_cast<long long>(below.size() + 1);
    }
    for (auto& s : after) {
      if (s.begin() >= bs && s.begin() <= be) {
        s.anchor = shifted(s.anchor, shift);
        s.caret = shifted(s.caret, shift);
      }
    }
  }
  commit(std::move(ch), std::move(after), sels_.primaryIndex(), EditKind::Other);
}

void Editor::duplicate() {
  std::vector<Change> ch;
  std::vector<Selection> after;
  long long delta = 0;
  size_t lastLine = npos;
  for (const auto& s : sels_.all()) {
    if (s.empty()) {
      size_t line = buf_.lineOf(s.caret);
      if (line != lastLine) {
        lastLine = line;
        std::wstring t = std::wstring(buf_.lineText(line)) + L"\n";
        ch.push_back({buf_.lineStart(line), 0, t});
        delta += static_cast<long long>(t.size());
      }
      after.emplace_back(shifted(s.anchor, delta), shifted(s.caret, delta));
    } else {
      std::wstring t = textOf(s);
      ch.push_back({s.end(), 0, t});
      after.emplace_back(shifted(s.anchor, delta), shifted(s.caret, delta));
      delta += static_cast<long long>(t.size());
    }
  }
  commit(std::move(ch), std::move(after), sels_.primaryIndex(), EditKind::Other);
}

void Editor::deleteLine() {
  std::vector<std::pair<size_t, size_t>> ranges;
  const size_t count = buf_.lineCount();
  for (auto [l0, l1] : selectedLineBlocks()) {
    if (l1 + 1 < count)
      ranges.emplace_back(buf_.lineStart(l0), buf_.lineStart(l1 + 1));
    else if (l0 > 0)
      ranges.emplace_back(buf_.lineEnd(l0 - 1), buf_.size());
    else
      ranges.emplace_back(0, buf_.size());
  }
  deleteRanges(std::move(ranges), EditKind::Other);
}

void Editor::joinLines() {
  std::map<size_t, Change> uniq;
  const size_t count = buf_.lineCount();
  for (const auto& s : sels_.all()) {
    size_t l0 = buf_.lineOf(s.begin()), l1 = buf_.lineOf(s.end());
    if (l1 == l0) l1 = l0 + 1;
    for (size_t l = l0; l < l1 && l + 1 < count; ++l) {
      std::wstring_view cur = buf_.lineText(l);
      size_t trimEnd = cur.size();
      while (trimEnd > 0 && isBlank(cur[trimEnd - 1])) --trimEnd;
      std::wstring_view next = buf_.lineText(l + 1);
      size_t lead = leadingBlanks(next);
      size_t from = buf_.lineStart(l) + trimEnd;
      size_t to = buf_.lineStart(l + 1) + lead;
      uniq[from] = Change{from, to - from, L""};
    }
  }
  std::vector<Change> ch;
  for (auto& [pos, c] : uniq) {
    if (!ch.empty() && c.pos <= ch.back().pos + ch.back().len) {
      Change& prev = ch.back();
      prev.len = std::max(prev.pos + prev.len, c.pos + c.len) - prev.pos;
    } else {
      ch.push_back(std::move(c));
    }
  }
  // One space separates the joined text, unless either side of the join is empty.
  for (auto& c : ch) {
    size_t end = c.pos + c.len;
    bool leftEmpty = c.pos == buf_.lineStart(buf_.lineOf(c.pos));
    bool rightEmpty = end >= buf_.size() || buf_.at(end) == L'\n';
    c.text = leftEmpty || rightEmpty ? L"" : L" ";
  }
  commitMapped(std::move(ch), EditKind::Other);
}

void Editor::toggleComment(bool block) {
  const bool hasLine = !comments.line.empty();
  const bool hasBlock = !comments.blockStart.empty() && !comments.blockEnd.empty();
  if (!hasLine && !hasBlock) return;
  std::vector<Change> ch;

  if ((block && hasBlock) || !hasLine) {
    const std::wstring& bs = comments.blockStart;
    const std::wstring& be = comments.blockEnd;
    for (const auto& s : sels_.all()) {
      size_t b = s.begin(), e = s.end();
      if (s.empty()) {
        size_t line = buf_.lineOf(s.caret);
        std::wstring_view t = buf_.lineText(line);
        b = buf_.lineStart(line) + leadingBlanks(t);
        e = buf_.lineEnd(line);
        while (e > b && isBlank(buf_.at(e - 1))) --e;
      }
      size_t tb = b, te = e;
      while (tb < te && isSpace(buf_.at(tb))) ++tb;
      while (te > tb && isSpace(buf_.at(te - 1))) --te;
      std::wstring_view inner = buf_.view(tb, te - tb);
      if (inner.size() >= bs.size() + be.size() && startsWith(inner, bs) && endsWith(inner, be)) {
        size_t l1 = bs.size();
        if (buf_.at(tb + l1) == L' ') ++l1;
        size_t l2 = be.size();
        if (te - l2 > tb + l1 && buf_.at(te - l2 - 1) == L' ') ++l2;
        ch.push_back({tb, l1, L""});
        ch.push_back({te - l2, l2, L""});
      } else {
        ch.push_back({b, 0, bs + L" "});
        ch.push_back({e, 0, L" " + be});
      }
    }
    commitMapped(std::move(ch), EditKind::Other);
    return;
  }

  const std::wstring& tok = comments.line;
  for (auto [l0, l1] : selectedLineBlocks()) {
    std::vector<size_t> lines;
    bool allCommented = true;
    size_t minIndent = npos;
    for (size_t l = l0; l <= l1; ++l) {
      std::wstring_view t = buf_.lineText(l);
      size_t lead = leadingBlanks(t);
      if (lead == t.size()) continue;
      lines.push_back(l);
      minIndent = std::min(minIndent, lead);
      if (!startsWith(t.substr(lead), tok)) allCommented = false;
    }
    if (lines.empty()) {
      ch.push_back({buf_.lineEnd(l0), 0, tok + L" "});
      continue;
    }
    for (size_t l : lines) {
      std::wstring_view t = buf_.lineText(l);
      size_t lead = leadingBlanks(t);
      if (allCommented) {
        size_t len = tok.size();
        if (lead + len < t.size() && t[lead + len] == L' ') ++len;
        ch.push_back({buf_.lineStart(l) + lead, len, L""});
      } else {
        ch.push_back({buf_.lineStart(l) + minIndent, 0, tok + L" "});
      }
    }
  }
  commitMapped(std::move(ch), EditKind::Other);
}

void Editor::transformCase(CaseMode mode) {
  std::vector<Change> ch;
  std::vector<Selection> after;
  long long delta = 0;
  for (const auto& s : sels_.all()) {
    Selection r = s;
    if (r.empty()) {
      auto [b, e] = wordAt(s.caret);
      if (b == e) {
        after.push_back(Selection::at(shifted(s.caret, delta)));
        continue;
      }
      r = Selection(b, e);
    }
    std::wstring t = textOf(r), n;
    switch (mode) {
      case CaseMode::Upper: n = toUpper(t); break;
      case CaseMode::Lower: n = toLower(t); break;
      case CaseMode::Title: n = toTitle(t); break;
      case CaseMode::Swap: n = swapCase(t); break;
    }
    ch.push_back({r.begin(), r.length(), n});
    size_t nb = shifted(r.begin(), delta);
    if (s.empty())
      after.push_back(Selection::at(shifted(s.caret, delta)));
    else
      after.push_back(s.reversed() ? Selection(nb + n.size(), nb) : Selection(nb, nb + n.size()));
    delta += sdiff(n.size(), r.length());
  }
  commit(std::move(ch), std::move(after), sels_.primaryIndex(), EditKind::Other);
}

void Editor::permuteLines(LineOrder order) {
  const auto& all = sels_.all();
  bool allEmpty = std::all_of(all.begin(), all.end(), [](const Selection& s) { return s.empty(); });
  std::vector<std::pair<size_t, size_t>> blocks;
  if (allEmpty)
    blocks.emplace_back(0, buf_.lineCount() - 1);
  else
    blocks = selectedLineBlocks();
  std::vector<Change> ch;
  std::vector<Selection> after;
  long long delta = 0;
  for (auto [l0, l1] : blocks) {
    std::vector<std::wstring> lines;
    for (size_t l = l0; l <= l1; ++l) lines.emplace_back(buf_.lineText(l));
    switch (order) {
      case LineOrder::Sort: {
        std::vector<std::pair<std::wstring, std::wstring>> keyed;
        for (auto& l : lines) keyed.emplace_back(toLower(l), l);
        std::stable_sort(keyed.begin(), keyed.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < lines.size(); ++i) lines[i] = keyed[i].second;
        break;
      }
      case LineOrder::SortCaseSensitive: std::stable_sort(lines.begin(), lines.end()); break;
      case LineOrder::Reverse: std::reverse(lines.begin(), lines.end()); break;
      case LineOrder::Unique: {
        std::vector<std::wstring> out;
        for (auto& l : lines)
          if (std::find(out.begin(), out.end(), l) == out.end()) out.push_back(l);
        lines = std::move(out);
        break;
      }
    }
    std::wstring joined;
    for (size_t i = 0; i < lines.size(); ++i) {
      if (i) joined += L'\n';
      joined += lines[i];
    }
    size_t bs = buf_.lineStart(l0), be = buf_.lineEnd(l1);
    ch.push_back({bs, be - bs, joined});
    size_t nb = shifted(bs, delta);
    after.emplace_back(nb, nb + joined.size());
    delta += sdiff(joined.size(), be - bs);
  }
  if (allEmpty) after = {Selection::at(std::min(sels_.primary().caret, buf_.size()))};
  commit(std::move(ch), std::move(after), 0, EditKind::Other);
}

void Editor::transpose() {
  const auto& all = sels_.all();
  bool allEmpty = std::all_of(all.begin(), all.end(), [](const Selection& s) { return s.empty(); });
  std::vector<Change> ch;
  std::vector<Selection> after;
  if (allEmpty) {
    for (const auto& s : all) {
      size_t p = s.caret, line = buf_.lineOf(p);
      size_t ls = buf_.lineStart(line), le = buf_.lineEnd(line);
      size_t a;
      size_t caret = p;
      if (p == le && p >= ls + 2) {
        a = p - 2;
      } else if (p > ls && p < le) {
        a = p - 1;
        caret = p + 1;
      } else {
        after.push_back(s);
        continue;
      }
      std::wstring t{buf_.at(a + 1), buf_.at(a)};
      ch.push_back({a, 2, t});
      after.push_back(Selection::at(caret));
    }
  } else if (all.size() > 1) {
    long long delta = 0;
    for (size_t i = 0; i < all.size(); ++i) {
      const Selection& s = all[i];
      std::wstring t = textOf(all[(i + all.size() - 1) % all.size()]);
      ch.push_back({s.begin(), s.length(), t});
      size_t nb = shifted(s.begin(), delta);
      after.emplace_back(nb, nb + t.size());
      delta += sdiff(t.size(), s.length());
    }
  } else {
    return;
  }
  commit(std::move(ch), std::move(after), sels_.primaryIndex(), EditKind::Other);
}

void Editor::trimTrailingWhitespace() {
  std::vector<Change> ch;
  for (size_t l = 0; l < buf_.lineCount(); ++l) {
    std::wstring_view t = buf_.lineText(l);
    size_t e = t.size();
    while (e > 0 && isBlank(t[e - 1])) --e;
    if (e < t.size()) ch.push_back({buf_.lineStart(l) + e, t.size() - e, L""});
  }
  if (!ch.empty()) commitMapped(std::move(ch), EditKind::Other);
}

void Editor::ensureNewlineAtEof() {
  if (buf_.size() == 0 || buf_.at(buf_.size() - 1) == L'\n') return;
  commit({{buf_.size(), 0, L"\n"}}, sels_.all(), sels_.primaryIndex(), EditKind::Other);
}

void Editor::convertIndentation(bool toSpaces) {
  const size_t tab = static_cast<size_t>(std::max(1, config.tabSize));
  std::vector<Change> ch;
  for (size_t l = 0; l < buf_.lineCount(); ++l) {
    std::wstring_view t = buf_.lineText(l);
    size_t lead = leadingBlanks(t);
    if (!lead) continue;
    size_t col = 0;
    for (size_t i = 0; i < lead; ++i) col += t[i] == L'\t' ? tab - col % tab : 1;
    std::wstring repl = toSpaces ? std::wstring(col, L' ')
                                 : std::wstring(col / tab, L'\t') + std::wstring(col % tab, L' ');
    if (repl != t.substr(0, lead)) ch.push_back({buf_.lineStart(l), lead, repl});
  }
  config.translateTabs = toSpaces;
  if (!ch.empty()) commitMapped(std::move(ch), EditKind::Other);
}

// ---------------------------------------------------------------------------
// Clipboard

ClipboardText Editor::copy() const {
  ClipboardText out;
  const auto& all = sels_.all();
  bool allEmpty = std::all_of(all.begin(), all.end(), [](const Selection& s) { return s.empty(); });
  if (allEmpty) {
    out.wholeLines = true;
    size_t lastLine = npos;
    for (const auto& s : all) {
      size_t l = buf_.lineOf(s.caret);
      if (l == lastLine) continue;
      lastLine = l;
      out.text += buf_.lineText(l);
      out.text += L'\n';
    }
    return out;
  }
  bool first = true;
  for (const auto& s : all) {
    if (s.empty()) continue;
    if (!first) out.text += L'\n';
    out.text += textOf(s);
    first = false;
  }
  return out;
}

ClipboardText Editor::cut() {
  ClipboardText c = copy();
  std::vector<std::pair<size_t, size_t>> ranges;
  const size_t count = buf_.lineCount();
  for (const auto& s : sels_.all()) {
    if (c.wholeLines) {
      size_t l = buf_.lineOf(s.caret);
      ranges.emplace_back(buf_.lineStart(l), l + 1 < count ? buf_.lineStart(l + 1) : buf_.size());
    } else if (!s.empty()) {
      ranges.emplace_back(s.begin(), s.end());
    }
  }
  deleteRanges(std::move(ranges), EditKind::Other);
  return c;
}

void Editor::paste(std::wstring_view raw, bool wholeLines) {
  std::wstring text = normalizeNewlines(raw);
  if (text.empty()) return;
  const auto& all = sels_.all();
  bool allEmpty = std::all_of(all.begin(), all.end(), [](const Selection& s) { return s.empty(); });
  std::vector<std::wstring> parts = splitLines(text);
  if (parts.size() > 1 && parts.back().empty()) parts.pop_back();

  if (all.size() > 1 && parts.size() == all.size()) {
    std::vector<Change> ch;
    std::vector<Selection> after;
    long long delta = 0;
    for (size_t i = 0; i < all.size(); ++i) {
      const Selection& s = all[i];
      ch.push_back({s.begin(), s.length(), parts[i]});
      after.push_back(Selection::at(shifted(s.begin(), delta) + parts[i].size()));
      delta += sdiff(parts[i].size(), s.length());
    }
    commit(std::move(ch), std::move(after), sels_.primaryIndex(), EditKind::Other);
    return;
  }
  if (wholeLines && allEmpty) {
    std::vector<Change> ch;
    size_t lastLine = npos;
    for (const auto& s : all) {
      size_t l = buf_.lineOf(s.caret);
      if (l == lastLine) continue;
      lastLine = l;
      ch.push_back({buf_.lineStart(l), 0, text});
    }
    commitMapped(std::move(ch), EditKind::Other);
    return;
  }
  insertText(text, false);
}

// ---------------------------------------------------------------------------
// Search

bool Editor::findNext(const Searcher& s, bool forward, bool wrap) {
  if (!s.valid()) return false;
  const Selection p = sels_.primary();
  const auto& t = buf_.text();
  auto m = forward ? s.next(t, p.end(), wrap) : s.prev(t, p.begin(), wrap);
  if (!m) return false;
  setSelections({Selection(m->pos, m->pos + m->len)}, 0);
  return true;
}

size_t Editor::selectAllMatches(const Searcher& s) {
  if (!s.valid()) return 0;
  auto ms = s.all(buf_.text(), 0, std::wstring::npos, static_cast<size_t>(-1), true);
  if (ms.empty()) return 0;
  std::vector<Selection> v;
  for (const auto& m : ms) v.emplace_back(m.pos, m.pos + m.len);
  setSelections(std::move(v), 0);
  return ms.size();
}

bool Editor::replaceNext(const Searcher& s, std::wstring_view format, bool wrap) {
  if (!s.valid()) return false;
  const Selection p = sels_.primary();
  if (!p.empty() && s.matchesExactly(buf_.text(), p.begin(), p.length())) {
    Match m{p.begin(), p.length()};
    std::wstring r = s.replacementFor(buf_.text(), m, format);
    commit({{m.pos, m.len, r}}, {Selection::at(m.pos + r.size())}, 0, EditKind::Other);
  }
  return findNext(s, true, wrap);
}

size_t Editor::replaceAll(const Searcher& s, std::wstring_view format) {
  if (!s.valid()) return 0;
  const auto& t = buf_.text();
  auto ms = s.all(t, 0, std::wstring::npos, static_cast<size_t>(-1), true);  // "^" inserts at each line start
  if (ms.empty()) return 0;
  std::vector<Change> ch;
  ch.reserve(ms.size());
  for (const auto& m : ms) ch.push_back({m.pos, m.len, s.replacementFor(t, m, format)});
  commitMapped(std::move(ch), EditKind::Other);
  return ms.size();
}

}  // namespace verso
