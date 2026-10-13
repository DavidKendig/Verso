#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <optional>

#include "app/App.h"
#include "app/MarkdownLinks.h"
#include "core/Encoding.h"
#include "core/TextUtil.h"

namespace verso {

// ---------------------------------------------------------------------------
// Word wrap

bool App::wrapActive() const { return (mdMode_ && !gitMode_) || settings_.wordWrap; }

size_t App::wrapColumns() const {
  const float cw = gfx_.charWidth();
  float usable = lay_.text.w() - 6.0f - cw * 1.5f;
  return static_cast<size_t>(std::max(20.0f, usable / cw));
}

void App::ensureWrap(Document& d) {
  bool on = wrapActive() && !lay_.text.empty();
  size_t cols = on ? wrapColumns() : 0;
  int tab = d.ed.config.tabSize;
  if (d.wrapBuilt && d.wrapRevision == d.ed.revision() && d.wrapCols == cols && d.wrapOn == on && d.wrapTab == tab)
    return;
  d.wrap.build(d.ed.buffer(), tab, cols, on);
  d.wrapRevision = d.ed.revision();
  d.wrapCols = cols;
  d.wrapOn = on;
  d.wrapTab = tab;
  d.wrapBuilt = true;
}

double App::lineAtEditorY(Document& d, float y) {
  ensureWrap(d);
  const WrapMap& wm = d.wrap;
  const float lh = gfx_.lineHeight();
  size_t rows = wm.rowCount();
  if (rows == 0) return 0;
  double rowPos = std::max(0.0, static_cast<double>(y) / lh);
  size_t r = std::min(rows - 1, static_cast<size_t>(rowPos));
  VisualRow row = wm.row(r);
  size_t firstRow = wm.firstRowOfLine(row.line);
  size_t nextRow = row.line + 1 < d.ed.buffer().lineCount() ? wm.firstRowOfLine(row.line + 1) : rows;
  double frac = (rowPos - static_cast<double>(firstRow)) / static_cast<double>(std::max<size_t>(1, nextRow - firstRow));
  return static_cast<double>(row.line) + std::clamp(frac, 0.0, 1.0);
}

void App::scrollEditorToLine(Document& d, double line) {
  ensureWrap(d);
  const WrapMap& wm = d.wrap;
  size_t lines = d.ed.buffer().lineCount();
  size_t l = std::min(static_cast<size_t>(std::max(0.0, line)), lines - 1);
  size_t firstRow = wm.firstRowOfLine(l);
  size_t nextRow = l + 1 < lines ? wm.firstRowOfLine(l + 1) : wm.rowCount();
  double frac = std::clamp(line - static_cast<double>(l), 0.0, 1.0);
  d.scrollY = static_cast<float>((static_cast<double>(firstRow) + frac * static_cast<double>(nextRow - firstRow)) *
                                 gfx_.lineHeight());
  clampScroll(d);
}

void App::moveVisual(long long rows, bool extend) {
  Document* d = doc();
  if (!d) return;
  ensureWrap(*d);
  const WrapMap& wm = d->wrap;
  Editor& ed = d->ed;
  const TextBuffer& b = ed.buffer();
  std::vector<Selection> out;
  for (const auto& s : ed.selections().all()) {
    size_t r = wm.rowOf(s.caret);
    VisualRow row = wm.row(r);
    double vx = s.xcol >= 0 ? s.xcol
                            : static_cast<double>(ed.columnAt(s.caret)) - static_cast<double>(row.startCol) +
                                  static_cast<double>(row.indent);
    long long target = static_cast<long long>(r) + rows;
    size_t pos;
    bool keep = true;
    if (target < 0) {
      pos = 0;
      keep = false;
    } else if (target >= static_cast<long long>(wm.rowCount())) {
      pos = b.size();
      keep = false;
    } else {
      VisualRow tr = wm.row(static_cast<size_t>(target));
      double col = std::max(vx - static_cast<double>(tr.indent), 0.0) + static_cast<double>(tr.startCol);
      pos = wm.caretOnRow(static_cast<size_t>(target), ed.posAtColumn(tr.line, col, true));
    }
    Selection n = extend ? Selection(s.anchor, pos) : Selection::at(pos);
    if (keep) n.xcol = vx;
    out.push_back(n);
  }
  size_t primary = ed.selections().primaryIndex();
  ed.setSelections(std::move(out), primary);
  afterEdit(*d, true);
}

// ---------------------------------------------------------------------------
// Text / Markdown modes

void App::setEditorMode(bool markdown) {
  if (gitMode_) setGitMode(false);
  terminalMode_ = false;
  if (mdMode_ != markdown) {
    mdMode_ = markdown;
    if (markdown && !doc()) {
      Document& nd = newDocument();
      if (const Language* mdl = languageByName(L"Markdown")) setLanguage(nd, *mdl, true);
    }
    sessionDirty_ = true;
  }
  computeLayout();
  if (doc()) revealCaret();
  updateTitle();
  resetBlink();
  invalidate();
}

void App::setMdView(MdView view) {
  if (!mdMode_ || gitMode_) setEditorMode(true);
  if (Document* d = doc(); d && mdView_ == MdView::Split && view == MdView::Read)
    d->previewScroll = preview_.scrollForLine(lineAtEditorY(*d, d->scrollY));
  mdView_ = view;
  focus_ = Focus::Editor;
  computeLayout();
  if (doc() && view != MdView::Read) revealCaret();
  sessionDirty_ = true;
  invalidate();
}

bool App::markdownReadOnly() const { return mdMode_ && !gitMode_ && mdView_ == MdView::Read; }

float App::previewZoom() const {
  float base = settings_.fontSize > 0 ? settings_.fontSize : 10.5f;
  return std::clamp((fontSizeOverride_ > 0 ? fontSizeOverride_ : base) / base, 0.6f, 3.0f);
}

void App::paintPreview() {
  Document* d = doc();
  const RectF& area = lay_.preview;
  if (area.empty()) return;
  if (!d) {
    gfx_.fill(area, theme_.bg);
    return;
  }
  // The preview lays out the whole document on every change, so binary data and very
  // large documents (where that takes seconds) are not rendered.
  constexpr size_t kMaxPreviewChars = 1000000;
  if (d->binary || d->ed.buffer().size() > kMaxPreviewChars) {
    gfx_.fill(area, theme_.bg);
    std::wstring why = d->binary ? L"This is a binary file, so there is no Markdown preview."
                                 : L"This document is too large to preview (" +
                                       std::to_wstring(d->ed.buffer().size() / 1000000 + 1) +
                                       L" million characters; the preview handles up to 1 million).";
    gfx_.textIn(why, {area.l + 36, area.t + 24, area.r - 16, area.t + 54}, theme_.overlayDetail, gfx_.uiFormat());
    if (mdView_ == MdView::Split) gfx_.line(area.l + 0.5f, area.t, area.l + 0.5f, area.b, theme_.tabBorder, 1);
    return;
  }
  std::wstring base = d->path.empty() ? std::wstring() : parentOf(d->path);
  preview_.update(gfx_, theme_, d->ed.buffer().text(), (d->id << 40) ^ d->ed.revision(), area.w(), base,
                  noteRootFor(*d), previewZoom(), mdView_ == MdView::Read);
  if (pendingAnchorDoc_ == d->id) {  // a followed link named a heading in this document
    pendingAnchorDoc_ = 0;
    float y = preview_.yForAnchor(pendingAnchor_);
    if (y >= 0) {
      d->previewScroll = preview_.scrollForLine(preview_.lineAtY(y));
      if (mdView_ == MdView::Split) {
        scrollEditorToLine(*d, preview_.lineForScroll(d->previewScroll));
        d->previewSyncY = d->scrollY;
        d->previewSyncRev = d->ed.revision();
      }
    } else {
      setStatus(L"No heading matches #" + pendingAnchor_);
    }
  }
  if (mdView_ == MdView::Split && (d->scrollY != d->previewSyncY || d->ed.revision() != d->previewSyncRev)) {
    d->previewScroll = preview_.scrollForLine(lineAtEditorY(*d, d->scrollY));
    d->previewSyncY = d->scrollY;
    d->previewSyncRev = d->ed.revision();
  }
  d->previewScroll = std::clamp(d->previewScroll, 0.0f, preview_.maxScroll(area));
  preview_.paint(gfx_, theme_, area, d->previewScroll, mouseX_, mouseY_);
  if (mdView_ == MdView::Split) gfx_.line(area.l + 0.5f, area.t, area.l + 0.5f, area.b, theme_.tabBorder, 1);
}

void App::scrollPreview(float dy) {
  Document* d = doc();
  if (!d || lay_.preview.empty()) return;
  float target = std::clamp(d->previewScroll + dy, 0.0f, preview_.maxScroll(lay_.preview));
  if (mdView_ == MdView::Split) {
    // Keep the editor in step; the preview keeps its own position when the editor
    // cannot scroll that far (a tall image or table at the end).
    scrollEditorToLine(*d, preview_.lineForScroll(target));
    d->previewSyncY = d->scrollY;
    d->previewSyncRev = d->ed.revision();
  }
  d->previewScroll = target;
  invalidate();
}

bool App::previewKey(UINT vk, bool shift, bool alt) {
  if (!markdownReadOnly() || overlay_ != Overlay::None || focus_ != Focus::Editor) return false;
  const float line = 40.0f, page = std::max(60.0f, lay_.preview.h() * 0.9f);
  if (alt) {
    if (vk != VK_LEFT && vk != VK_RIGHT) return false;
    navigateMarkdown(vk == VK_LEFT);
    return true;
  }
  switch (vk) {
    case VK_BACK: navigateMarkdown(!shift); return true;
    case VK_UP: scrollPreview(-line); return true;
    case VK_DOWN: scrollPreview(line); return true;
    case VK_PRIOR: scrollPreview(-page); return true;
    case VK_NEXT: scrollPreview(page); return true;
    case VK_SPACE: scrollPreview(shift ? -page : page); return true;
    case VK_HOME:
      scrollPreview(-1e9f);
      return true;
    case VK_END:
      scrollPreview(1e9f);
      return true;
    default:
      return false;
  }
}

void App::openPreviewLink(const MarkdownPreview::Link& link, bool newTab) {
  Document* d = doc();
  std::wstring url(trim(link.url));
  if (url.empty() || !d) return;
  const std::wstring base = d->path.empty() ? std::wstring() : parentOf(d->path);
  if (link.wiki) {
    WikiTarget target = parseWikiTarget(url);
    std::wstring anchor = target.heading.empty() ? std::wstring() : md::slug(target.heading);
    if (target.page.empty()) {  // [[#Heading]]
      followHeading(anchor);
      return;
    }
    invalidateNoteTrees();  // a click acts on the files as they are now
    WikiResolution r = resolveWikiLink(target.page, base, noteRootFor(*d));
    if (!r.error.empty()) {
      setStatus(r.error, 3000);
      return;
    }
    // A link to a note that does not exist yet creates it, as note apps do.
    if (!r.exists && !createNote(r.path)) return;
    openLinkedFile(r.path, anchor, newTab, !r.exists);
    return;
  }
  size_t hash = url.find(L'#');
  std::wstring fragment = hash == std::wstring::npos ? std::wstring() : decodeUrlComponent(url.substr(hash + 1));
  if (hash == 0) {  // a heading in this document
    followHeading(fragment);
    return;
  }
  std::wstring lowerUrl = url;
  for (auto& c : lowerUrl) c = static_cast<wchar_t>(std::towlower(c));
  if (lowerUrl.rfind(L"http://", 0) == 0 || lowerUrl.rfind(L"https://", 0) == 0 ||
      lowerUrl.rfind(L"mailto:", 0) == 0) {
    ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return;
  }
  // A local file, optionally with a #heading; note links resolve as in note apps.
  invalidateNoteTrees();  // a click acts on the files as they are now
  WikiResolution r = resolveNoteLink(url.substr(0, hash), base, noteRootFor(*d));
  if (!r.error.empty()) {
    setStatus(r.error, 3000);
    return;
  }
  std::wstring path = r.path;
  bool created = false;
  if (!r.exists) {
    // A link to a note that does not exist yet creates it; other missing files are reported.
    if (!endsWithNoCase(path, L".md")) {
      setStatus(L"Link target not found: " + path, 3000);
      return;
    }
    if (!createNote(path)) return;
    created = true;
  }
  DWORD attrs = GetFileAttributesW(path.c_str());
  if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
    // A folder link shows the folder's README, as GitHub does; otherwise the folder opens in the side bar.
    for (const wchar_t* name : {L"README.md", L"readme.md", L"Readme.md", L"index.md"}) {
      std::wstring candidate = path + L"\\" + name;
      DWORD ca = GetFileAttributesW(candidate.c_str());
      if (ca != INVALID_FILE_ATTRIBUTES && !(ca & FILE_ATTRIBUTE_DIRECTORY)) {
        path = candidate;
        attrs = ca;
        break;
      }
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
      openFile(path);
      return;
    }
  }
  openLinkedFile(path, fragment, newTab, created);
}

void App::followHeading(const std::wstring& anchor) {
  Document* d = doc();
  if (!d) return;
  float y = preview_.yForAnchor(anchor);
  if (y < 0) {
    setStatus(L"No heading matches #" + anchor);
    return;
  }
  pushMarkdownNav(navPointOf(*d));
  scrollPreview(preview_.scrollForLine(preview_.lineAtY(y)) - d->previewScroll);
}

std::wstring App::noteRootFor(const Document& d) {
  if (d.path.empty()) return {};
  std::wstring base = parentOf(d.path);
  std::wstring root = markdownRoot(base);
  if (!root.empty()) return root;
  for (const auto& folder : tree_.folders())
    if (base.size() >= folder.size() && _wcsnicmp(base.c_str(), folder.c_str(), folder.size()) == 0 &&
        (base.size() == folder.size() || base[folder.size()] == L'\\'))
      return folder;
  return base;
}

bool App::createNote(const std::wstring& path) {
  std::wstring name = fileNameOf(path);
  if (endsWithNoCase(name, L".md")) name.resize(name.size() - 3);
  SHCreateDirectoryExW(nullptr, parentOf(path).c_str(), nullptr);
  std::wstring error;
  if (!writeFileAtomic(path, wideToUtf8(L"# " + name + L"\n"), &error)) {
    setStatus(L"Could not create " + path + L": " + error, 4000);
    return false;
  }
  invalidateNoteTrees();
  return true;
}

void App::openLinkedFile(const std::wstring& path, const std::wstring& anchor, bool newTab, bool created) {
  Document* d = doc();
  if (!d) return;
  if (samePath(path, d->path)) {
    if (!anchor.empty()) followHeading(anchor);
    return;
  }
  // Say which file the link reached, so an existing empty note is not mistaken for a new one.
  std::wstring shown = path;
  if (std::wstring root = noteRootFor(*d); !root.empty() && path.size() > root.size() + 1 &&
                                           _wcsnicmp(path.c_str(), root.c_str(), root.size()) == 0)
    shown = path.substr(root.size() + 1);
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  bool empty = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) && fad.nFileSizeLow == 0 &&
               fad.nFileSizeHigh == 0;
  const NavPoint here = navPointOf(*d);
  Document* target = newTab ? openFile(path, true) : openFileInPlace(path);
  if (!target) return;
  pushMarkdownNav(here);
  std::wstring folder = shown.size() > fileNameOf(path).size() ? shown.substr(0, shown.size() - fileNameOf(path).size() - 1)
                                                                : std::wstring();
  setStatus((created ? L"Created new note " : L"Opened ") + fileNameOf(path) + (empty ? L" (empty note)" : L"") +
                (folder.empty() ? L"" : L"  —  " + folder),
            5000);
  if (!anchor.empty()) {
    pendingAnchorDoc_ = target->id;
    pendingAnchor_ = anchor;
  }
  invalidate();
}

App::NavPoint App::navPointOf(const Document& d) const { return {d.id, d.path, d.previewScroll, d.scrollY}; }

void App::pushMarkdownNav(const NavPoint& p) {
  navBack_.push_back(p);
  if (navBack_.size() > 100) navBack_.erase(navBack_.begin());
  navForward_.clear();
}

bool App::navigateMarkdown(bool back) {
  auto& from = back ? navBack_ : navForward_;
  auto& to = back ? navForward_ : navBack_;
  while (!from.empty()) {
    NavPoint p = from.back();
    from.pop_back();
    size_t index = docs_.size();
    for (size_t i = 0; i < docs_.size(); ++i)
      if (docs_[i]->id == p.docId || (!p.path.empty() && samePath(docs_[i]->path, p.path))) index = i;
    bool reopen = index == docs_.size();
    if (reopen && (p.path.empty() || GetFileAttributesW(p.path.c_str()) == INVALID_FILE_ATTRIBUTES)) continue;
    std::optional<NavPoint> here;
    if (Document* cur = doc()) here = navPointOf(*cur);
    Document* target = nullptr;
    if (reopen) {
      target = openFileInPlace(p.path);  // the tab was replaced by a followed link
    } else {
      activate(index);
      target = docs_[index].get();
    }
    if (!target) continue;
    if (here) to.push_back(*here);  // only a completed step can be retraced
    target->scrollY = p.scrollY;
    target->previewScroll = p.previewScroll;
    pendingAnchorDoc_ = 0;
    invalidate();
    return true;
  }
  setStatus(back ? L"Nothing to go back to" : L"Nothing to go forward to", 1500);
  return false;
}

// ---------------------------------------------------------------------------
// Markdown writing helpers

void App::mdToggleWrap(const std::wstring& marker) {
  Document* d = doc();
  if (!d || editBlocked()) return;
  Editor& ed = d->ed;
  const TextBuffer& b = ed.buffer();
  const size_t m = marker.size();
  std::vector<Editor::Change> ch;
  std::vector<Selection> after;
  long long delta = 0;
  size_t covered = 0;  // end of the last expanded selection
  bool any = false;
  for (const auto& s0 : ed.selections().all()) {
    Selection s = s0;
    if (s.empty()) {
      auto w = ed.wordAt(s.caret);
      if (w.first < w.second) s = Selection(w.first, w.second);
    }
    size_t sb = s.begin(), se = s.end();
    if (any && sb < covered) continue;  // another caret in a word already handled
    any = true;
    covered = se;
    bool wrapped = sb >= m && se + m <= b.size() && b.view(sb - m, m) == marker && b.view(se, m) == marker;
    if (wrapped) {
      ch.push_back({sb - m, m, L""});
      ch.push_back({se, m, L""});
      size_t nb = static_cast<size_t>(static_cast<long long>(sb) - static_cast<long long>(m) + delta);
      after.emplace_back(nb, nb + (se - sb));
      delta -= static_cast<long long>(2 * m);
    } else {
      ch.push_back({sb, 0, marker});
      ch.push_back({se, 0, marker});
      size_t nb = static_cast<size_t>(static_cast<long long>(sb + m) + delta);
      if (s.empty())
        after.push_back(Selection::at(nb));
      else
        after.emplace_back(nb, nb + (se - sb));
      delta += static_cast<long long>(2 * m);
    }
  }
  ed.commit(std::move(ch), std::move(after), ed.selections().primaryIndex(), EditKind::Other);
  afterEdit(*d, true);
}

void App::mdInsertLink() {
  Document* d = doc();
  if (!d || editBlocked()) return;
  Editor& ed = d->ed;
  std::vector<Editor::Change> ch;
  std::vector<Selection> after;
  long long delta = 0;
  for (const auto& s : ed.selections().all()) {
    std::wstring label = ed.textOf(s);
    std::wstring text = L"[" + label + L"]()";
    ch.push_back({s.begin(), s.length(), text});
    size_t base = static_cast<size_t>(static_cast<long long>(s.begin()) + delta);
    // Caret inside "()" when there is a label, otherwise inside "[]".
    after.push_back(Selection::at(label.empty() ? base + 1 : base + text.size() - 1));
    delta += static_cast<long long>(text.size()) - static_cast<long long>(s.length());
  }
  ed.commit(std::move(ch), std::move(after), ed.selections().primaryIndex(), EditKind::Other);
  afterEdit(*d, true);
}

bool App::mdContinueList() {
  Document* d = doc();
  if (!d) return false;
  Editor& ed = d->ed;
  if (ed.selections().size() != 1 || !ed.selections().primary().empty()) return false;
  const TextBuffer& b = ed.buffer();
  size_t caret = ed.selections().primary().caret;
  size_t line = b.lineOf(caret);
  size_t ls = b.lineStart(line), le = b.lineEnd(line);
  std::wstring_view t = b.lineText(line);
  // Prefix: indentation, quote markers, a list marker and an optional task box.
  size_t i = leadingBlanks(t);
  std::wstring prefix(t.substr(0, i));
  bool any = false;
  while (i < t.size() && t[i] == L'>') {
    ++i;
    if (i < t.size() && t[i] == L' ') ++i;
    any = true;
  }
  std::wstring next(t.substr(0, i));
  size_t markerEnd = i;
  if (i + 1 < t.size() && (t[i] == L'-' || t[i] == L'*' || t[i] == L'+') && t[i + 1] == L' ') {
    next += t.substr(i, 2);
    markerEnd = i + 2;
    any = true;
  } else {
    size_t dgt = i;
    while (dgt < t.size() && std::iswdigit(t[dgt])) ++dgt;
    // At most 9 digits, as in CommonMark; that also keeps n + 1 in range.
    if (dgt > i && dgt - i <= 9 && dgt + 1 < t.size() && (t[dgt] == L'.' || t[dgt] == L')') && t[dgt + 1] == L' ') {
      long n = std::wcstol(std::wstring(t.substr(i, dgt - i)).c_str(), nullptr, 10);
      next += std::to_wstring(n + 1) + t[dgt] + L' ';
      markerEnd = dgt + 2;
      any = true;
    }
  }
  if (!any) return false;
  if (markerEnd + 3 <= t.size() && t[markerEnd] == L'[' &&
      (t[markerEnd + 1] == L' ' || t[markerEnd + 1] == L'x' || t[markerEnd + 1] == L'X') && t[markerEnd + 2] == L']') {
    markerEnd += 3;
    if (markerEnd < t.size() && t[markerEnd] == L' ') ++markerEnd;
    next += L"[ ] ";
  }
  if (caret < ls + markerEnd) return false;
  if (trim(t.substr(markerEnd)).empty()) {
    // Enter on an empty item ends the list.
    ed.commit({{ls, le - ls, L""}}, {Selection::at(ls)}, 0, EditKind::Other);
  } else {
    std::wstring ins = L"\n" + next;
    ed.commit({{caret, 0, ins}}, {Selection::at(caret + ins.size())}, 0, EditKind::Other);
  }
  afterEdit(*d, true);
  return true;
}

// ---------------------------------------------------------------------------
// Status bar geometry

RectF App::mdViewRect() const {
  RectF ms = modeSwitchRect();
  return {ms.r + 8, ms.t, ms.r + 8 + 168, ms.b};
}

}  // namespace verso
