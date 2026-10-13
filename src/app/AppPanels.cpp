#include <algorithm>
#include <cstring>
#include <cwctype>

#include "app/App.h"
#include "util/Fuzzy.h"

namespace verso {

namespace {

constexpr size_t kMaxOverlayItems = 300;

// Parses "N" or "N:C" (1-based). Returns false when no number is present.
bool parseLineSpec(std::wstring_view s, size_t& line, size_t& col) {
  line = 0;
  col = 0;
  size_t i = 0;
  while (i < s.size() && std::iswdigit(s[i])) line = line * 10 + static_cast<size_t>(s[i++] - L'0');
  if (i == 0) return false;
  if (i < s.size() && s[i] == L':') {
    ++i;
    while (i < s.size() && std::iswdigit(s[i])) col = col * 10 + static_cast<size_t>(s[i++] - L'0');
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Find panel

void App::showFind(bool replace, bool incremental) {
  if (overlay_ != Overlay::None) closeOverlay(false);
  findMode_ = replace ? FindMode::Replace : FindMode::Find;
  findIncremental_ = incremental && !replace;
  focus_ = Focus::Find;
  if (Document* d = doc()) {
    const Selection& p = d->ed.selections().primary();
    const TextBuffer& b = d->ed.buffer();
    if (!p.empty() && b.lineOf(p.begin()) == b.lineOf(p.end()) && p.length() < 500)
      findEdit_.setText(d->ed.textOf(p), true);
    else
      findEdit_.selectAll();
    findOrigin_ = d->ed.selections().all();
  }
  findError_.clear();
  computeLayout();
  revealCaret();
  invalidate();
}

void App::hideFind() {
  findEdit_.remember();
  replaceEdit_.remember();
  findMode_ = FindMode::Hidden;
  focus_ = Focus::Editor;
  computeLayout();
  revealCaret();
  invalidate();
}

bool App::compileFind(Searcher& s) {
  std::wstring err;
  bool ok = s.compile(findEdit_.text(), findOpts_, &err, settings_.wordSeparators);
  findError_ = ok ? L"" : err;
  return ok && !findEdit_.text().empty();
}

void App::onFindTextChanged() {
  Document* d = doc();
  Searcher s;
  if (!d || !compileFind(s)) {
    invalidate();
    return;
  }
  size_t from = findOrigin_.empty() ? 0 : findOrigin_.front().begin();
  if (auto m = s.next(d->ed.buffer().text(), from, findWrap_)) {
    d->ed.setSelections({Selection(m->pos, m->pos + m->len)}, 0);
    revealCaret();
  }
  invalidate();
}

void App::findNext(bool forward) {
  Document* d = doc();
  if (!d) return;
  if (findEdit_.text().empty()) {
    showFind(false);
    return;
  }
  Searcher s;
  if (!compileFind(s)) {
    setStatus(findError_.empty() ? L"Nothing to find" : findError_);
    return;
  }
  findEdit_.remember();
  if (!d->ed.findNext(s, forward, findWrap_)) {
    setStatus(L"Unable to find " + findEdit_.text());
    return;
  }
  findOrigin_ = d->ed.selections().all();
  afterEdit(*d, true);
}

void App::findAll() {
  Document* d = doc();
  Searcher s;
  if (!d || !compileFind(s)) return;
  findEdit_.remember();
  size_t n = d->ed.selectAllMatches(s);
  if (n == 0) {
    setStatus(L"Unable to find " + findEdit_.text());
    return;
  }
  hideFind();
  setStatus(std::to_wstring(n) + L" matches selected");
  afterEdit(*d, true);
}

void App::replaceNext() {
  Document* d = doc();
  Searcher s;
  if (!d || editBlocked() || !compileFind(s)) return;
  findEdit_.remember();
  replaceEdit_.remember();
  if (!d->ed.replaceNext(s, replaceEdit_.text(), findWrap_)) setStatus(L"Unable to find " + findEdit_.text());
  findOrigin_ = d->ed.selections().all();
  afterEdit(*d, true);
}

void App::replaceAll() {
  Document* d = doc();
  Searcher s;
  if (!d || editBlocked() || !compileFind(s)) return;
  findEdit_.remember();
  replaceEdit_.remember();
  size_t n = d->ed.replaceAll(s, replaceEdit_.text());
  setStatus(n ? L"Replaced " + std::to_wstring(n) + L" occurrences" : L"Unable to find " + findEdit_.text());
  afterEdit(*d, true);
}

void App::useSelectionForFind() {
  Document* d = doc();
  if (!d) return;
  Selection p = d->ed.selections().primary();
  if (p.empty()) {
    auto w = d->ed.wordAt(p.caret);
    p = Selection(w.first, w.second);
  }
  if (p.empty()) return;
  findEdit_.setText(d->ed.textOf(p), true);
  findEdit_.remember();
  setStatus(L"Find: " + findEdit_.text(), 2000);
}

void App::findUnder(bool forward) {
  Document* d = doc();
  if (!d) return;
  edit([&](Editor& e) { e.findUnder(forward); });
  const Selection& p = d->ed.selections().primary();
  if (!p.empty()) findEdit_.setText(d->ed.textOf(p), true);
}

void App::findInFiles(const std::wstring& query) {
  if (query.empty()) return;
  if (tree_.empty()) {
    setStatus(L"Open a folder (File > Open Folder) to search in files.");
    return;
  }
  Searcher s;
  std::wstring err;
  if (!s.compile(query, findOpts_, &err, settings_.wordSeparators)) {
    setStatus(L"Invalid pattern: " + err);
    return;
  }
  if (!indexStarted_) rebuildIndex();
  if (!index_.ready()) {
    setStatus(L"Still indexing the folder; try again in a moment.");
    return;
  }
  indexSnapshot_ = index_.snapshot();
  HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
  std::wstring out = L"Searching " + std::to_wstring(indexSnapshot_.size()) + L" files for \"" + query + L"\"";
  if (findOpts_.regex || findOpts_.caseSensitive || findOpts_.wholeWord) {
    out += L" (";
    if (findOpts_.regex) out += L"regex ";
    if (findOpts_.caseSensitive) out += L"case-sensitive ";
    if (findOpts_.wholeWord) out += L"whole-word ";
    out.back() = L')';
  }
  out += L"\n\n";
  size_t files = 0, matches = 0;
  bool truncated = false;
  for (const auto& e : indexSnapshot_) {
    std::string bytes;
    if (!readFileBytes(e.path, bytes, nullptr) || bytes.size() > (8u << 20)) continue;
    bool utf16 = bytes.size() >= 2 && ((static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) ||
                                       (static_cast<unsigned char>(bytes[0]) == 0xFE && static_cast<unsigned char>(bytes[1]) == 0xFF));
    if (!utf16 && std::memchr(bytes.data(), 0, std::min<size_t>(bytes.size(), 8000))) continue;  // binary
    DecodedText dt = decodeText(bytes, LineEnding::LF);
    auto ms = s.all(dt.text, 0, std::wstring::npos, 2000);
    if (ms.empty()) continue;
    TextBuffer tb(std::move(dt.text));
    ++files;
    out += e.path + L":\n";
    size_t lastLine = static_cast<size_t>(-1);
    for (const auto& m : ms) {
      size_t l = tb.lineOf(m.pos);
      if (l == lastLine) continue;
      lastLine = l;
      std::wstring num = std::to_wstring(l + 1);
      // Result lines always start with a space, which tells them from file headers.
      num.insert(0, num.size() < 6 ? 6 - num.size() : 1, L' ');
      std::wstring text(tb.lineText(l).substr(0, 400));
      for (auto& c : text)
        if (c == L'\r') c = L' ';
      out += num + L": " + text + L"\n";
      ++matches;
    }
    out += L"\n";
    if (matches >= 50000) {
      truncated = true;
      break;
    }
  }
  out += std::to_wstring(matches) + (matches == 1 ? L" matching line in " : L" matching lines in ") +
         std::to_wstring(files) + (files == 1 ? L" file" : L" files") + (truncated ? L" (stopped early)" : L"") +
         L"\n";
  SetCursor(old);
  Document* existing = nullptr;
  for (size_t i = 0; i < docs_.size(); ++i)
    if (docs_[i]->findResults) {
      existing = docs_[i].get();
      activate(i);
    }
  Document& d = existing ? *existing : newDocument(true);
  d.findResults = true;
  d.ed.setText(out);
  d.scrollX = d.scrollY = 0;
  setLanguage(d, plainTextLanguage(), true);
  afterEdit(d, false);
  setStatus(L"Double-click a result (or press Enter on it) to open the file.", 6000);
}

bool App::openResultAt(Document& results, size_t line) {
  const TextBuffer& b = results.ed.buffer();
  std::wstring_view t = b.lineText(line);
  size_t i = 0;
  while (i < t.size() && t[i] == L' ') ++i;
  size_t num = 0, digits = 0;
  while (i < t.size() && std::iswdigit(t[i])) {
    num = num * 10 + static_cast<size_t>(t[i++] - L'0');
    ++digits;
  }
  if (digits == 0 || i >= t.size() || t[i] != L':' || t[0] != L' ') return false;
  for (size_t l = line; l-- > 0;) {
    std::wstring_view h = b.lineText(l);
    if (h.empty() || h[0] == L' ') continue;
    if (h.back() != L':') return false;
    std::wstring path(h.substr(0, h.size() - 1));
    Document* d = openFile(path);
    if (!d) return true;
    const TextBuffer& fb = d->ed.buffer();
    size_t target = std::min(num == 0 ? 0 : num - 1, fb.lineCount() - 1);
    size_t pos = fb.lineStart(target) + leadingBlanks(fb.lineText(target));
    d->ed.setSelections({Selection::at(pos)}, 0);
    revealCaret(true);
    return true;
  }
  return false;
}

App::FindLayout App::findLayout() const {
  FindLayout fl;
  const RectF& r = lay_.find;
  float y = r.t + 7, h = 26;
  float x = 8;
  for (int i = 0; i < 4; ++i) {
    fl.toggles[i] = {x, y, x + 30, y + h};
    x += 34;
  }
  float right = r.r - 8;
  float bw[3] = {64, 64, 72};
  fl.all = {right - bw[2], y, right, y + h};
  fl.prev = {fl.all.l - 6 - bw[1], y, fl.all.l - 6, y + h};
  fl.find = {fl.prev.l - 6 - bw[0], y, fl.prev.l - 6, y + h};
  fl.findBox = {x + 6, y, fl.find.l - 10, y + h};
  if (findMode_ == FindMode::Replace) {
    float y2 = y + h + 8;
    fl.replaceAll = {right - (bw[2] + bw[1] + 6), y2, right, y2 + h};
    fl.replace = {fl.replaceAll.l - 6 - (bw[0]), y2, fl.replaceAll.l - 6, y2 + h};
    fl.replaceBox = {fl.findBox.l, y2, fl.findBox.r, y2 + h};
  }
  return fl;
}

// ---------------------------------------------------------------------------
// Overlays

void App::showOverlay(Overlay mode, std::wstring text) {
  Document* d = doc();
  if ((mode == Overlay::Line || mode == Overlay::Symbols) && !d) return;
  if (overlay_ == Overlay::None) {
    overlayPreviewed_ = false;
    if (d) {
      overlayDocId_ = d->id;
      overlayRestoreSels_ = d->ed.selections().all();
      overlayRestoreX_ = d->scrollX;
      overlayRestoreY_ = d->scrollY;
    }
  }
  if (findMode_ != FindMode::Hidden) focus_ = Focus::Editor;
  overlay_ = mode;
  focus_ = Focus::Overlay;
  overlayEdit_.setText(std::move(text), false);
  overlaySel_ = 0;
  overlayTop_ = 0;
  if (mode == Overlay::Files) {
    if (!indexStarted_ && !tree_.empty()) rebuildIndex();
    indexSnapshot_ = index_.snapshot();
  }
  overlaySymbols_.clear();
  if (d && (mode == Overlay::Symbols || mode == Overlay::Files)) overlaySymbols_ = extractSymbols(d->ed.buffer(), d->syntax);
  refreshOverlay();
  invalidate();
}

void App::prompt(std::wstring label, std::wstring initial, std::function<void(const std::wstring&)> done) {
  promptLabel_ = std::move(label);
  promptDone_ = std::move(done);
  showOverlay(Overlay::Prompt, std::move(initial));
  overlayEdit_.selectAll();
}

void App::closeOverlay(bool accepted) {
  Document* d = doc();
  if (!accepted && overlayPreviewed_ && d && d->id == overlayDocId_) {
    d->ed.setSelections(overlayRestoreSels_, overlayRestoreSels_.empty() ? 0 : overlayRestoreSels_.size() - 1);
    d->scrollX = overlayRestoreX_;
    d->scrollY = overlayRestoreY_;
  }
  overlay_ = Overlay::None;
  overlayItems_.clear();
  overlayPreviewed_ = false;
  promptDone_ = nullptr;
  focus_ = Focus::Editor;
  invalidate();
}

void App::refreshOverlay() {
  overlayItems_.clear();
  const std::wstring q = overlayEdit_.text();
  Document* d = doc();
  auto sortItems = [&] {
    std::stable_sort(overlayItems_.begin(), overlayItems_.end(),
                     [](const OverlayItem& a, const OverlayItem& b) { return a.score > b.score; });
    if (overlayItems_.size() > kMaxOverlayItems) overlayItems_.resize(kMaxOverlayItems);
  };
  auto symbolItems = [&](std::wstring_view query) {
    for (const auto& s : overlaySymbols_) {
      auto m = fuzzyMatch(query, s.name);
      if (!m) continue;
      OverlayItem it;
      it.title = s.name;
      it.titleMarks = m->positions;
      it.hint = L"line " + std::to_wstring(s.line + 1);
      it.score = query.empty() ? 0 : m->score;
      it.line = s.line;
      it.symbol = std::wstring(trim(s.name));
      overlayItems_.push_back(std::move(it));
    }
    if (!query.empty()) sortItems();
  };
  auto lineItem = [&](std::wstring_view spec) {
    OverlayItem it;
    size_t line, col;
    if (d && parseLineSpec(spec, line, col)) {
      it.title = L"Go to line " + std::to_wstring(line);
      if (col) it.title += L", column " + std::to_wstring(col);
      it.line = line;
    } else {
      it.title = d ? L"Type a line number between 1 and " + std::to_wstring(d->ed.buffer().lineCount())
                   : L"No file open";
    }
    overlayItems_.push_back(std::move(it));
  };

  switch (overlay_) {
    case Overlay::Commands: {
      for (const auto& c : commands_) {
        if (c.caption.empty()) continue;
        auto m = fuzzyMatch(q, c.caption);
        if (!m) continue;
        OverlayItem it;
        it.title = c.caption;
        it.titleMarks = m->positions;
        it.hint = keymap_.shortcutFor(c.id);
        it.command = c.id;
        if (q.empty()) {
          auto r = std::find(recentCommands_.begin(), recentCommands_.end(), c.id);
          it.score = r == recentCommands_.end() ? 0 : 1000 - static_cast<int>(r - recentCommands_.begin());
        } else {
          it.score = m->score;
        }
        overlayItems_.push_back(std::move(it));
      }
      if (q.empty())
        std::stable_sort(overlayItems_.begin(), overlayItems_.end(), [](const OverlayItem& a, const OverlayItem& b) {
          if (a.score != b.score) return a.score > b.score;
          return a.title < b.title;
        });
      else
        sortItems();
      break;
    }
    case Overlay::Line:
      lineItem(q);
      previewOverlay();
      break;
    case Overlay::Symbols:
      symbolItems(q);
      break;
    case Overlay::Prompt:
      break;
    case Overlay::Files: {
      if (!q.empty() && q[0] == L':') {
        lineItem(std::wstring_view(q).substr(1));
        previewOverlay();
        break;
      }
      if (!q.empty() && q[0] == L'@') {
        symbolItems(std::wstring_view(q).substr(1));
        break;
      }
      if (!q.empty() && q[0] == L'#') {
        OverlayItem it;
        it.title = L"Find \"" + q.substr(1) + L"\" in this file";
        overlayItems_.push_back(std::move(it));
        break;
      }
      size_t sep = q.find_first_of(L":@#");
      std::wstring fq = sep == std::wstring::npos ? q : q.substr(0, sep);
      std::vector<std::wstring> openPaths;
      for (size_t i = 0; i < docs_.size(); ++i) {
        const Document& od = *docs_[i];
        std::wstring hay = od.path.empty() ? od.title() : od.path;
        auto m = fuzzyMatchPath(fq, hay);
        if (!m) continue;
        OverlayItem it;
        it.title = od.title();
        it.detail = od.path.empty() ? L"open buffer" : od.path;
        size_t base = od.path.empty() ? 0 : od.path.size() - it.title.size();
        for (size_t p : m->positions) {
          if (od.path.empty() || p >= base) it.titleMarks.push_back(p - base);
          if (!od.path.empty()) it.detailMarks.push_back(p);
        }
        it.score = (fq.empty() ? 0 : m->score) + 5 + (i == active_ ? -1 : 0);
        it.docIndex = static_cast<int>(i);
        it.path = od.path;
        overlayItems_.push_back(std::move(it));
        if (!od.path.empty()) openPaths.push_back(od.path);
      }
      if (!fq.empty() || overlayItems_.size() < 50) {
        for (const auto& e : indexSnapshot_) {
          if (std::any_of(openPaths.begin(), openPaths.end(),
                          [&](const std::wstring& p) { return _wcsicmp(p.c_str(), e.path.c_str()) == 0; }))
            continue;
          auto m = fuzzyMatchPath(fq, e.display);
          if (!m) continue;
          OverlayItem it;
          size_t slash = e.display.find_last_of(L'/');
          size_t base = slash == std::wstring::npos ? 0 : slash + 1;
          it.title = e.display.substr(base);
          it.detail = e.display;
          for (size_t p : m->positions) {
            if (p >= base) it.titleMarks.push_back(p - base);
            it.detailMarks.push_back(p);
          }
          it.score = fq.empty() ? -1 : m->score;
          it.path = e.path;
          overlayItems_.push_back(std::move(it));
          if (fq.empty() && overlayItems_.size() >= kMaxOverlayItems) break;
        }
      }
      sortItems();
      break;
    }
    case Overlay::None:
      break;
  }
  overlaySel_ = overlayItems_.empty() ? 0 : std::min(overlaySel_, static_cast<int>(overlayItems_.size()) - 1);
  if (overlay_ == Overlay::Commands || overlay_ == Overlay::Files || overlay_ == Overlay::Symbols)
    overlaySel_ = 0;
  overlayTop_ = 0;
  if (overlay_ == Overlay::Symbols || (overlay_ == Overlay::Files && !q.empty() && q[0] == L'@')) previewOverlay();
  invalidate();
}

void App::previewOverlay() {
  Document* d = doc();
  if (!d || d->id != overlayDocId_) return;
  const std::wstring q = overlayEdit_.text();
  bool lineMode = overlay_ == Overlay::Line || (overlay_ == Overlay::Files && !q.empty() && q[0] == L':');
  bool symbolMode = overlay_ == Overlay::Symbols || (overlay_ == Overlay::Files && !q.empty() && q[0] == L'@');
  const TextBuffer& b = d->ed.buffer();
  if (lineMode) {
    size_t line, col;
    std::wstring_view spec = overlay_ == Overlay::Line ? std::wstring_view(q) : std::wstring_view(q).substr(1);
    if (!parseLineSpec(spec, line, col)) return;
    size_t l = std::min(line == 0 ? 0 : line - 1, b.lineCount() - 1);
    size_t pos = col ? std::min(b.lineStart(l) + col - 1, b.lineEnd(l)) : b.lineStart(l);
    d->ed.setSelections({Selection::at(pos)}, 0);
    revealCaret(true);
    overlayPreviewed_ = true;
  } else if (symbolMode) {
    if (overlaySel_ < 0 || overlaySel_ >= static_cast<int>(overlayItems_.size())) return;
    const OverlayItem& it = overlayItems_[static_cast<size_t>(overlaySel_)];
    size_t l = std::min(it.line, b.lineCount() - 1);
    std::wstring_view text = b.lineText(l);
    std::wstring name = it.symbol;
    size_t colon = name.rfind(L"::");
    if (colon != std::wstring::npos) name = name.substr(colon + 2);
    size_t at = name.empty() ? std::wstring::npos : text.find(name);
    if (at != std::wstring::npos)
      d->ed.setSelections({Selection(b.lineStart(l) + at, b.lineStart(l) + at + name.size())}, 0);
    else
      d->ed.setSelections({Selection::at(b.lineStart(l))}, 0);
    revealCaret(true);
    overlayPreviewed_ = true;
  }
}

void App::acceptOverlay(int index) {
  const std::wstring q = overlayEdit_.text();
  OverlayItem item;
  bool has = index >= 0 && index < static_cast<int>(overlayItems_.size());
  if (has) item = overlayItems_[static_cast<size_t>(index)];
  Overlay mode = overlay_;
  switch (mode) {
    case Overlay::Commands: {
      closeOverlay(true);
      if (!has) return;
      recentCommands_.erase(std::remove(recentCommands_.begin(), recentCommands_.end(), item.command),
                            recentCommands_.end());
      recentCommands_.insert(recentCommands_.begin(), item.command);
      if (recentCommands_.size() > 30) recentCommands_.resize(30);
      runCommand(item.command);
      return;
    }
    case Overlay::Line:
    case Overlay::Symbols:
      previewOverlay();
      closeOverlay(true);
      if (Document* d = doc()) afterEdit(*d, false);
      return;
    case Overlay::Prompt: {
      auto done = promptDone_;
      closeOverlay(true);
      if (done) done(q);
      return;
    }
    case Overlay::Files: {
      if (!q.empty() && (q[0] == L':' || q[0] == L'@')) {
        previewOverlay();
        closeOverlay(true);
        if (Document* d = doc()) afterEdit(*d, false);
        return;
      }
      if (!q.empty() && q[0] == L'#') {
        closeOverlay(true);
        findEdit_.setText(q.substr(1), true);
        findOpts_ = SearchOptions{};
        findNext(true);
        return;
      }
      closeOverlay(true);
      if (!has) return;
      if (item.docIndex >= 0 && static_cast<size_t>(item.docIndex) < docs_.size())
        activate(static_cast<size_t>(item.docIndex));
      else if (!item.path.empty())
        openFile(item.path);
      size_t sep = q.find_first_of(L":@#");
      if (sep != std::wstring::npos) {
        std::wstring rest = q.substr(sep);
        if (rest[0] == L':') {
          showOverlay(Overlay::Line, rest.substr(1));
          acceptOverlay(0);
        } else if (rest[0] == L'@') {
          showOverlay(Overlay::Symbols, rest.substr(1));
        } else if (rest.size() > 1) {
          findEdit_.setText(rest.substr(1), true);
          findNext(true);
        }
      }
      return;
    }
    case Overlay::None:
      return;
  }
}

RectF App::overlayInputRect() const {
  float w = std::min(640.0f, width_ - 40.0f);
  float x = (width_ - w) / 2;
  float y = (distractionFree_ ? 0.0f : lay_.tabs.b) + 8.0f;
  float top = y + (overlay_ == Overlay::Prompt ? 26.0f : 8.0f);
  return {x + 10, top, x + w - 10, top + 32};
}

float App::overlayItemHeight() const {
  return overlay_ == Overlay::Files && !overlayItems_.empty() && !overlayItems_.front().detail.empty() ? 44.0f
                                                                                                        : 30.0f;
}

int App::overlayVisibleCount() const {
  float avail = height_ - overlayInputRect().b - 60.0f;
  int maxRows = std::max(3, static_cast<int>(avail / overlayItemHeight()));
  return std::min({12, maxRows, static_cast<int>(overlayItems_.size())});
}

RectF App::overlayRect() const {
  RectF in = overlayInputRect();
  float y = in.t - (overlay_ == Overlay::Prompt ? 26.0f : 8.0f);
  int n = overlayVisibleCount();
  float h = (in.b - y) + 8 + (n > 0 ? static_cast<float>(n) * overlayItemHeight() + 6 : 0);
  return {in.l - 10, y, in.r + 10, y + h};
}

}  // namespace verso
