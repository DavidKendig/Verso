#include <algorithm>
#include <cmath>

#include "app/App.h"
#include "app/Icons.h"

namespace verso {

namespace {

constexpr size_t npos = static_cast<size_t>(-1);

// Draws a git status letter right-aligned at `right`; returns the width used.
float drawGitBadge(Gfx& g, const Theme& t, GitBadge b, float right, float top, float h) {
  const wchar_t* letter = nullptr;
  Color c = t.gitModified;
  switch (b) {
    case GitBadge::Modified: letter = L"M"; break;
    case GitBadge::Staged: letter = L"M"; c = t.gitAdded; break;
    case GitBadge::Added: letter = L"A"; c = t.gitAdded; break;
    case GitBadge::Renamed: letter = L"R"; c = t.gitAdded; break;
    case GitBadge::Untracked: letter = L"U"; c = t.gitAdded; break;
    case GitBadge::Deleted: letter = L"D"; c = t.gitDeleted; break;
    case GitBadge::Conflict: letter = L"!"; c = t.gitDeleted; break;
    default: return 0;
  }
  g.textIn(letter, {right - 14, top, right, top + h}, c, g.uiBoldFormat(), true);
  return 16;
}

void drawBranchIcon(Gfx& g, float x, float y, Color c) {
  g.fillEllipse(x + 3, y + 3, 1.8f, 1.8f, c);
  g.fillEllipse(x + 3, y + 12, 1.8f, 1.8f, c);
  g.fillEllipse(x + 10, y + 4.5f, 1.8f, 1.8f, c);
  g.line(x + 3, y + 4.5f, x + 3, y + 10.5f, c, 1.3f);
  D2D1_POINT_2F arc[] = {D2D1::Point2F(x + 10, y + 6.3f), D2D1::Point2F(x + 10, y + 7.5f), D2D1::Point2F(x + 4.4f, y + 10.4f)};
  g.path(arc, 3, c, 1.3f, false, false);
}

bool isBracketChar(wchar_t c) {
  return c == L'(' || c == L')' || c == L'[' || c == L']' || c == L'{' || c == L'}';
}

}  // namespace

float App::gutterWidth(const Document& d) const {
  const float cw = gfx_.charWidth();
  if (!settings_.lineNumbers) return cw * 2;
  size_t lines = d.ed.buffer().lineCount();
  int digits = 1;
  while (lines >= 10) {
    lines /= 10;
    ++digits;
  }
  digits = std::max(digits, 3);
  return static_cast<float>(digits + 3) * cw;
}

RectF App::scrollThumb(Document& d) const {
  const RectF& t = lay_.vscroll;
  float viewH = lay_.text.h();
  float maxY = maxScrollY(d);
  float content = maxY + viewH;
  float h = content > 0 ? std::max(28.0f, t.h() * viewH / content) : t.h();
  h = std::min(h, t.h());
  float y = t.t + (maxY > 0 ? (t.h() - h) * (d.scrollY / maxY) : 0);
  return {t.l + 2, y, t.r - 2, y + h};
}

std::vector<App::TabRect> App::tabRects() {
  std::vector<TabRect> out;
  if (lay_.tabs.h() <= 0 || docs_.empty()) return out;
  float avail = lay_.tabs.w() - 8;
  float n = static_cast<float>(docs_.size());
  float w = std::clamp(avail / n, 110.0f, 220.0f);
  float total = w * n;
  float activeL = w * static_cast<float>(active_);
  if (total <= avail) {
    tabScroll_ = 0;
  } else {
    tabScroll_ = std::clamp(tabScroll_, 0.0f, total - avail);
    if (activeL < tabScroll_) tabScroll_ = activeL;
    if (activeL + w > tabScroll_ + avail) tabScroll_ = activeL + w - avail;
  }
  float x = lay_.tabs.l + 6 - tabScroll_;
  for (size_t i = 0; i < docs_.size(); ++i) {
    TabRect t;
    t.r = {x, lay_.tabs.t + 6, x + w - 2, lay_.tabs.b};
    t.close = {t.r.r - 26, t.r.t + 7, t.r.r - 8, t.r.t + 25};
    t.index = i;
    out.push_back(t);
    x += w;
  }
  return out;
}

void App::paint() {
  if (!gfx_.beginDraw()) return;
  computeLayout();
  gfx_.clear(theme_.bg);
  if (gitMode_) {  // the git view covers everything above the status bar
    paintStatus();
    gfx_.endDraw();
    return;
  }
  if (terminalMode_) {
    paintTerminal();
    if (lay_.sidebar.w() > 0) paintSidebar();
    if (lay_.status.h() > 0) paintStatus();
    if (overlay_ != Overlay::None) paintOverlay();
    gfx_.endDraw();
    return;
  }
  if (!docs_.empty()) {
    paintEditor();
    if (!lay_.preview.empty()) paintPreview();
    if (!lay_.pdf.empty()) paintPdf();
  } else {
    paintWelcome();
  }
  if (lay_.tabs.h() > 0) paintTabs();
  if (lay_.sidebar.w() > 0) paintSidebar();
  if (findMode_ != FindMode::Hidden) paintFind();
  if (lay_.status.h() > 0) paintStatus();
  if (overlay_ != Overlay::None) paintOverlay();
  gfx_.endDraw();
}

void App::paintWelcome() {
  gfx_.fill(lay_.editor, theme_.bg);
  RectF r = lay_.editor;
  float cy = r.t + r.h() / 2 - 70;
  gfx_.textIn(L"Verso", {r.l, cy, r.r, cy + 30}, theme_.tabTextActive, gfx_.uiLargeFormat(), true);
  const wchar_t* lines[] = {L"New File\tCtrl+N", L"Open File\tCtrl+O", L"Goto Anything\tCtrl+P",
                            L"Command Palette\tCtrl+Shift+P"};
  float y = cy + 44;
  for (const wchar_t* l : lines) {
    std::wstring s = l;
    size_t tab = s.find(L'\t');
    float mid = r.l + r.w() / 2;
    gfx_.textIn(s.substr(0, tab), {mid - 170, y, mid - 10, y + 22}, theme_.statusText, gfx_.uiFormat());
    gfx_.textIn(s.substr(tab + 1), {mid + 10, y, mid + 200, y + 22}, theme_.accent, gfx_.uiFormat());
    y += 26;
  }
}

void App::paintTabs() {
  gfx_.fill(lay_.tabs, theme_.chrome);
  gfx_.pushClip(lay_.tabs);
  for (const auto& t : tabRects()) {
    if (t.r.r < lay_.tabs.l || t.r.l > lay_.tabs.r) continue;
    const Document& d = *docs_[t.index];
    bool active = t.index == active_;
    bool hot = t.r.contains(mouseX_, mouseY_);
    if (active) {
      gfx_.fillRounded({t.r.l, t.r.t, t.r.r, t.r.b + 6}, 6, theme_.tabActive);
    } else if (hot) {
      gfx_.fillRounded({t.r.l, t.r.t, t.r.r, t.r.b + 6}, 6, theme_.tabHover);
    } else if (t.index + 1 != active_ && t.index + 1 < docs_.size()) {
      gfx_.line(t.r.r + 1, t.r.t + 8, t.r.r + 1, t.r.b - 8, theme_.tabBorder, 1);
    }
    RectF label{t.r.l + 12, t.r.t, t.close.l - 4, t.r.b};
    gfx_.textIn(d.title(), label, active ? theme_.tabTextActive : theme_.tabText, gfx_.uiFormat());
    bool closeHot = t.close.contains(mouseX_, mouseY_);
    if (d.ed.isDirty() && !closeHot) {
      float cx = (t.close.l + t.close.r) / 2, cy = (t.close.t + t.close.b) / 2;
      gfx_.fillRounded({cx - 4, cy - 4, cx + 4, cy + 4}, 4, active ? theme_.tabTextActive : theme_.tabText);
    } else if (active || hot) {
      if (closeHot) gfx_.fillRounded(t.close, 4, theme_.buttonHover);
      float cx = (t.close.l + t.close.r) / 2, cy = (t.close.t + t.close.b) / 2;
      Color c = closeHot ? theme_.tabTextActive : theme_.tabText;
      gfx_.line(cx - 4, cy - 4, cx + 4, cy + 4, c, 1.3f);
      gfx_.line(cx + 4, cy - 4, cx - 4, cy + 4, c, 1.3f);
    }
  }
  gfx_.popClip();
}

std::vector<App::SidebarRow> App::sidebarRows() {
  std::vector<SidebarRow> rows;
  if (settings_.sidebarShowOpenFiles && !docs_.empty()) {
    rows.push_back({SidebarRow::Kind::Header, L"OPEN FILES"});
    for (size_t i = 0; i < docs_.size(); ++i) rows.push_back({SidebarRow::Kind::OpenFile, docs_[i]->title(), i});
  }
  if (!tree_.empty()) {
    rows.push_back({SidebarRow::Kind::Header, L"FOLDERS"});
    for (const auto& t : tree_.rows()) rows.push_back({SidebarRow::Kind::Tree, t.node->name, 0, t.node, t.depth});
  }
  return rows;
}

void App::paintSidebar() {
  const RectF& r = lay_.sidebar;
  gfx_.fill(r, theme_.sidebarBg);
  gfx_.pushClip(r);
  const auto rows = sidebarRows();
  Document* d = doc();
  const bool hoverable = overlay_ == Overlay::None && drag_ == Drag::None;
  const Color folderTint = theme_.sidebarText.withAlpha(0.45f);
  const Color fileTint = theme_.sidebarText.withAlpha(0.6f);
  float y = r.t + 4 - sidebarScroll_;
  for (size_t i = 0; i < rows.size(); ++i, y += kSidebarRow) {
    if (y + kSidebarRow < r.t) continue;
    if (y > r.b) break;
    const SidebarRow& row = rows[i];
    RectF rr{r.l, y, r.r, y + kSidebarRow};
    bool hot = hoverable && rr.contains(mouseX_, mouseY_);
    if (row.kind == SidebarRow::Kind::Header) {
      gfx_.textIn(row.label, {r.l + 14, y + 4, r.r - 6, y + kSidebarRow}, theme_.sidebarTextDim, gfx_.uiSmallFormat());
      continue;
    }
    if (row.kind == SidebarRow::Kind::OpenFile) {
      const Document& od = *docs_[row.docIndex];
      bool selected = row.docIndex == active_;
      if (selected) gfx_.fill(rr, theme_.sidebarSelected);
      else if (hot) gfx_.fill(rr, theme_.sidebarHover);
      Icon icon = od.findResults || od.path.empty() ? Icon::Text : iconForFile(od.path);
      drawIcon(gfx_, icon, r.l + 14, y + 4, fileTint);
      RectF close{r.r - 24, y + 4, r.r - 8, y + kSidebarRow - 4};
      float badgeW = 0;
      if (settings_.showGitStatus && !od.path.empty())
        badgeW = drawGitBadge(gfx_, theme_, git_.badgeFor(od.path, false), close.l - 2, y, kSidebarRow);
      gfx_.textIn(row.label, {r.l + 38, y, close.l - 4 - badgeW, y + kSidebarRow},
                  selected ? theme_.tabTextActive : theme_.sidebarText, gfx_.uiFormat());
      float cx = (close.l + close.r) / 2, cy = (close.t + close.b) / 2;
      if (hot) {
        bool closeHot = close.contains(mouseX_, mouseY_);
        if (closeHot) gfx_.fillRounded(close, 3, theme_.buttonHover);
        Color c = closeHot ? theme_.tabTextActive : theme_.sidebarTextDim;
        gfx_.line(cx - 3.5f, cy - 3.5f, cx + 3.5f, cy + 3.5f, c, 1.2f);
        gfx_.line(cx + 3.5f, cy - 3.5f, cx - 3.5f, cy + 3.5f, c, 1.2f);
      } else if (od.ed.isDirty()) {
        gfx_.fillEllipse(cx, cy, 3.5f, 3.5f, theme_.sidebarTextDim);
      }
      continue;
    }
    TreeNode* n = row.node;
    bool selected = d && !n->dir && _wcsicmp(d->path.c_str(), n->path.c_str()) == 0;
    if (selected) gfx_.fill(rr, theme_.sidebarSelected);
    else if (hot) gfx_.fill(rr, theme_.sidebarHover);
    float x = r.l + 10 + static_cast<float>(row.depth) * 14;
    float cy = y + kSidebarRow / 2;
    if (n->dir) {
      Color c = theme_.sidebarTextDim;
      float ax = x + 3;
      if (n->expanded) {
        D2D1_POINT_2F pts[] = {D2D1::Point2F(ax - 3, cy - 1.5f), D2D1::Point2F(ax, cy + 1.5f),
                               D2D1::Point2F(ax + 3, cy - 1.5f)};
        gfx_.path(pts, 3, c, 1.3f, false, false);
      } else {
        D2D1_POINT_2F pts[] = {D2D1::Point2F(ax - 1.5f, cy - 3), D2D1::Point2F(ax + 1.5f, cy),
                               D2D1::Point2F(ax - 1.5f, cy + 3)};
        gfx_.path(pts, 3, c, 1.3f, false, false);
      }
      drawIcon(gfx_, n->expanded ? Icon::FolderOpen : Icon::FolderClosed, x + 12, y + 4, folderTint);
    } else {
      drawIcon(gfx_, iconForFile(n->path), x + 12, y + 4, fileTint);
    }
    bool open = false;
    if (!n->dir)
      for (const auto& od : docs_)
        if (_wcsicmp(od->path.c_str(), n->path.c_str()) == 0) open = true;
    Color tc = n->dir || open || selected ? theme_.sidebarText : theme_.sidebarTextDim;
    if (selected) tc = theme_.tabTextActive;
    float badgeW = 0;
    if (settings_.showGitStatus) {
      GitBadge gb = git_.badgeFor(n->path, n->dir);
      if (gb == GitBadge::Ignored) {
        tc = tc.withAlpha(0.45f);
      } else if (n->dir && gb == GitBadge::Modified) {
        gfx_.fillEllipse(r.r - 13, cy, 3, 3, theme_.gitModified.withAlpha(0.85f));
        badgeW = 16;
      } else {
        badgeW = drawGitBadge(gfx_, theme_, gb, r.r - 6, y, kSidebarRow);
      }
    }
    gfx_.textIn(n->name, {x + 34, y, r.r - 6 - badgeW, y + kSidebarRow}, tc,
                row.depth == 0 ? gfx_.uiBoldFormat() : gfx_.uiFormat());
  }
  gfx_.popClip();
  gfx_.line(r.r - 0.5f, r.t, r.r - 0.5f, r.b, theme_.tabBorder, 1);
}

RectF App::sidebarToggleRect() const {
  return {lay_.status.l + 8, lay_.status.t + 2, lay_.status.l + 34, lay_.status.b - 2};
}

RectF App::modeSwitchRect() const {
  float l = terminalToggleRect().r + 6;
  return {l, lay_.status.t + 3, l + 198, lay_.status.b - 3};
}

void App::paintEditor() {
  Document* dp = doc();
  if (!dp || lay_.text.empty()) return;  // the Markdown Read view hides the editor
  Document& d = *dp;
  Editor& ed = d.ed;
  const TextBuffer& buf = ed.buffer();
  size_t changed = ed.takeFirstChangedLine();
  if (changed != npos) d.syntax.invalidateFrom(changed);
  ensureWrap(d);
  clampScroll(d);
  const WrapMap& wm = d.wrap;

  const float lh = gfx_.lineHeight(), cw = gfx_.charWidth(), ty = gfx_.textOffsetY();
  const RectF& text = lay_.text;
  const size_t lineCount = buf.lineCount();
  const size_t rowCount = wm.rowCount();
  const size_t firstRow = std::min(rowCount - 1, static_cast<size_t>(std::max(0.0f, std::floor(d.scrollY / lh))));
  const size_t lastRow = std::min(rowCount - 1, static_cast<size_t>((d.scrollY + text.h()) / lh) + 1);
  const size_t first = wm.row(firstRow).line;
  const size_t last = wm.row(lastRow).line;
  const float x0 = textLeft(d) - d.scrollX;
  const size_t tab = static_cast<size_t>(std::max(1, ed.config.tabSize));
  auto rowY = [&](size_t r) { return text.t + static_cast<float>(r) * lh - d.scrollY; };
  // x of a visual column on a row (continuation rows hang at their indent).
  auto colX = [&](const VisualRow& row, size_t col) {
    return x0 + (static_cast<float>(col) - static_cast<float>(row.startCol) + static_cast<float>(row.indent)) * cw;
  };
  auto posX = [&](const VisualRow& row, size_t pos) { return colX(row, ed.columnAt(pos)); };
  auto lineTopY = [&](size_t line) { return line >= lineCount ? rowY(rowCount) : rowY(wm.firstRowOfLine(line)); };
  const bool editorFocused = focus_ == Focus::Editor && overlay_ == Overlay::None;
  const auto& sels = ed.selections().all();

  gfx_.fill(lay_.editor, theme_.bg);
  if (lineCount > 0) d.syntax.ensure(buf, last);

  // Gutter -------------------------------------------------------------------
  if (lay_.gutter.w() > 0 && settings_.lineNumbers) {
    gfx_.pushClip(lay_.gutter);
    std::vector<bool> caretLines(last - first + 1, false);
    for (const auto& s : sels) {
      size_t l0 = buf.lineOf(s.begin()), l1 = buf.lineOf(s.end());
      for (size_t l = std::max(l0, first); l <= std::min(l1, last); ++l) caretLines[l - first] = true;
    }
    for (size_t r = firstRow; r <= lastRow; ++r) {
      VisualRow row = wm.row(r);
      if (row.start != buf.lineStart(row.line)) continue;  // continuation rows have no number
      std::wstring num = std::to_wstring(row.line + 1);
      float x = lay_.gutter.r - cw * 2 - static_cast<float>(num.size()) * cw;
      gfx_.text(num, x, rowY(r) + ty, caretLines[row.line - first] ? theme_.gutterFgActive : theme_.gutterFg,
                gfx_.editorFormat(FontStyle::Regular));
    }
    gfx_.popClip();
  }

  // Git change markers (lines that differ from the index) ---------------------
  if (settings_.miniDiff && d.hasGitBaseline && lay_.gutter.w() > 0) {
    updateGitHunks(d);
    gfx_.pushClip(lay_.gutter);
    const float gx = lay_.gutter.l + 3;
    for (const auto& h : d.gitHunks) {
      if (h.newCount == 0) {
        float y = lineTopY(h.newStart);
        if (y < text.t - lh || y > text.b + lh) continue;
        D2D1_POINT_2F tri[] = {D2D1::Point2F(gx, y - 4), D2D1::Point2F(gx + 5, y), D2D1::Point2F(gx, y + 4)};
        gfx_.path(tri, 3, theme_.gitDeleted, 1, true, true);
        continue;
      }
      float y0 = lineTopY(h.newStart), y1 = lineTopY(h.newStart + h.newCount);
      if (y1 < text.t || y0 > text.b) continue;
      gfx_.fill({gx, y0 + 1, gx + 3, y1 - 1}, h.oldCount == 0 ? theme_.gitAdded : theme_.gitModified);
    }
    gfx_.popClip();
  }

  gfx_.pushClip(text);

  // Current line highlight ---------------------------------------------------
  if (settings_.highlightLine && editorFocused) {
    size_t lastHighlighted = npos;
    for (const auto& s : sels) {
      if (!s.empty()) continue;
      size_t r = wm.rowOf(s.caret);
      if (r == lastHighlighted || r < firstRow || r > lastRow) continue;
      lastHighlighted = r;
      gfx_.fill({text.l, rowY(r), text.r, rowY(r) + lh}, theme_.lineHighlight);
    }
  }

  // Rulers -------------------------------------------------------------------
  for (int col : settings_.rulers) {
    float x = std::floor(x0 + static_cast<float>(std::max(0, col)) * cw) + 0.5f;
    gfx_.line(x, text.t, x, text.b, theme_.ruler, 1);
  }

  // Selections ---------------------------------------------------------------
  for (const auto& s : sels) {
    if (s.empty()) continue;
    size_t r0 = std::max(wm.rowOf(s.begin()), firstRow), r1 = std::min(wm.rowOf(s.end()), lastRow);
    for (size_t r = r0; r <= r1 && r0 <= lastRow; ++r) {
      VisualRow row = wm.row(r);
      size_t sb = std::max(s.begin(), row.start), se = std::min(s.end(), row.end);
      bool lineEndRow = row.end == buf.lineEnd(row.line);
      bool includesNewline = lineEndRow && s.end() > row.end;
      if (sb > se || (sb == se && !includesNewline)) continue;
      float xa = posX(row, sb);
      float xb = posX(row, se) + (includesNewline ? cw * 0.6f : 0);
      float y = rowY(r);
      gfx_.fillRounded({xa, y, xb, y + lh}, 3, theme_.selection);
      if (settings_.drawWhiteSpaceInSelection) {
        size_t col = ed.columnAt(sb);
        for (size_t p = sb; p < se; ++p) {
          wchar_t c = buf.at(p);
          if (c == L' ') {
            float cx = colX(row, col) + cw / 2, cy = y + lh / 2;
            gfx_.fill({cx - 0.8f, cy - 0.8f, cx + 0.8f, cy + 0.8f}, theme_.whitespace);
            ++col;
          } else if (c == L'\t') {
            size_t w = tab - col % tab;
            float xs = colX(row, col) + 2, xe = colX(row, col + w) - 2, cy = y + lh / 2;
            gfx_.line(xs, cy, xe, cy, theme_.whitespace, 1);
            col += w;
          } else {
            col += static_cast<size_t>(unitColumns(c));
          }
        }
      }
    }
  }

  // Visible text range for match highlighting.
  const size_t visBegin = wm.row(firstRow).start;
  const size_t visEnd = wm.row(lastRow).end;
  auto boxMatch = [&](size_t pos, size_t len, Color fillC, Color border) {
    size_t r = wm.rowOf(pos);
    if (r < firstRow || r > lastRow) return;
    VisualRow row = wm.row(r);
    size_t endPos = std::min(pos + len, row.end);
    float xa = posX(row, pos), xb = posX(row, endPos);
    if (xb - xa < 2) xb = xa + 2;
    float y = rowY(r);
    RectF rr{xa - 0.5f, y + 1, xb + 0.5f, y + lh - 1};
    if (fillC.a > 0) gfx_.fillRounded(rr, 3, fillC);
    gfx_.strokeRounded(rr, 3, border, 1);
  };

  // Find highlights ----------------------------------------------------------
  if (findMode_ != FindMode::Hidden && !findEdit_.text().empty()) {
    Searcher s;
    std::wstring err;
    if (s.compile(findEdit_.text(), findOpts_, &err, settings_.wordSeparators)) {
      for (const auto& m : s.all(buf.text(), visBegin, visEnd, 4000))
        boxMatch(m.pos, m.len, theme_.findMatch, theme_.findMatchBorder);
    }
  } else if (settings_.matchSelection && sels.size() == 1 && !sels[0].empty()) {
    // Outline other occurrences of a selected word.
    const Selection& p = sels[0];
    if (buf.lineOf(p.begin()) == buf.lineOf(p.end()) && p.length() <= 200) {
      std::wstring needle = ed.textOf(p);
      auto w = ed.wordAt(p.begin());
      bool isWord = w.first == p.begin() && w.second == p.end();
      if (!trim(needle).empty()) {
        Searcher s;
        s.compile(needle, {false, true, isWord}, nullptr, settings_.wordSeparators);
        for (const auto& m : s.all(buf.text(), visBegin, visEnd, 2000))
          if (m.pos != p.begin()) boxMatch(m.pos, m.len, Color{0, 0, 0, 0}, theme_.wordMatchBorder);
      }
    }
  }

  // Indent guides ------------------------------------------------------------
  if (settings_.drawIndentGuides) {
    auto indentCols = [&](size_t l) -> long long {
      std::wstring_view t = buf.lineText(l);
      size_t n = leadingBlanks(t);
      if (n == t.size()) return -1;
      size_t col = 0;
      for (size_t i = 0; i < n; ++i) col += t[i] == L'\t' ? tab - col % tab : 1;
      return static_cast<long long>(col);
    };
    long long prev = 0;
    for (size_t l = first; l <= last; ++l) {
      long long ind = indentCols(l);
      if (ind < 0) {
        long long next = 0;
        for (size_t k = l + 1; k < lineCount && k < l + 200; ++k) {
          next = indentCols(k);
          if (next >= 0) break;
        }
        ind = std::min(prev, std::max<long long>(next, 0));
      } else {
        prev = ind;
      }
      // One guide at the start of each indentation level, through all of the line's rows.
      float y0 = lineTopY(l), y1 = lineTopY(l + 1);
      for (long long c = 0; c < ind; c += static_cast<long long>(tab)) {
        float x = std::floor(x0 + static_cast<float>(c) * cw) + 0.5f;
        gfx_.line(x, y0, x, y1, theme_.guide, 1);
      }
    }
  }

  // Text ---------------------------------------------------------------------
  IDWriteTextFormat* regular = gfx_.editorFormat(FontStyle::Regular);
  IDWriteTextFormat* italic = gfx_.editorFormat(FontStyle::Italic);
  IDWriteTextFormat* bold = gfx_.editorFormat(FontStyle::Bold);
  for (size_t r = firstRow; r <= lastRow; ++r) {
    const VisualRow row = wm.row(r);
    std::wstring_view line = buf.lineText(row.line);
    const size_t rs = row.start - buf.lineStart(row.line), re = row.end - buf.lineStart(row.line);
    const auto& toks = d.syntax.tokens(buf, row.line);
    const float y = rowY(r) + ty;
    size_t col = row.startCol;
    for (const Token& tk : toks) {
      size_t i = std::max<size_t>(tk.start, rs), end = std::min<size_t>(tk.start + tk.len, re);
      if (i >= end) continue;
      Color color = theme_.token(tk.kind);
      IDWriteTextFormat* fmt = regular;
      if (tk.kind == TokenKind::Comment && theme_.italicComments) fmt = italic;
      if (tk.kind == TokenKind::Heading) fmt = bold;
      size_t runStart = npos, runCol = 0;
      auto flush = [&](size_t upTo) {
        if (runStart == npos) return;
        float xs = colX(row, runCol);
        float xe = xs + static_cast<float>(upTo - runStart) * cw;
        if (xe >= text.l && xs <= text.r) {
          // Skip the invisible prefix of very long runs.
          size_t skip = 0;
          if (xs < text.l - cw * 2) skip = static_cast<size_t>((text.l - xs) / cw) - 1;
          skip = std::min(skip, upTo - runStart);
          size_t len = std::min<size_t>(upTo - runStart - skip, static_cast<size_t>(text.w() / cw) + 4);
          gfx_.text(line.substr(runStart + skip, len), xs + static_cast<float>(skip) * cw, y, color, fmt);
        }
        runStart = npos;
      };
      while (i < end) {
        wchar_t c = line[i];
        if (c == L'\t') {
          flush(i);
          col += tab - col % tab;
          ++i;
          continue;
        }
        if (c < 0x20 || c == 0x7F) {
          flush(i);
          float cx = colX(row, col);
          gfx_.stroke({cx + 1, y + 2, cx + cw - 1, y + lh - ty * 2 - 2}, theme_.gutterFg, 1);
          ++col;
          ++i;
          continue;
        }
        if (c < 0x7F) {
          if (runStart == npos) {
            runStart = i;
            runCol = col;
          }
          ++col;
          ++i;
          continue;
        }
        flush(i);
        size_t clen = (isHighSurrogate(c) && i + 1 < end && isLowSurrogate(line[i + 1])) ? 2 : 1;
        int w = clen == 2 ? 2 : unitColumns(c);
        float cx = colX(row, w == 0 && col > 0 ? col - 1 : col);
        if (cx >= text.l - cw * 2 && cx <= text.r) gfx_.text(line.substr(i, clen), cx, y, color, fmt);
        col += static_cast<size_t>(w);
        i += clen;
      }
      flush(end);
      if (colX(row, col) > text.r) break;
    }
  }

  // Bracket matching ---------------------------------------------------------
  if (settings_.matchBrackets && sels.size() == 1 && sels[0].empty()) {
    if (bracketDoc_ != d.id || bracketState_ != ed.stateId() || bracketSel_ != ed.selectionVersion()) {
      bracketDoc_ = d.id;
      bracketState_ = ed.stateId();
      bracketSel_ = ed.selectionVersion();
      bracketA_ = bracketB_ = npos;
      size_t p = sels[0].caret;
      size_t at = npos;
      if (isBracketChar(buf.at(p))) at = p;
      else if (p > 0 && isBracketChar(buf.at(p - 1))) at = p - 1;
      if (at != npos) {
        size_t m = ed.matchBracket(at);
        if (m != npos) {
          bracketA_ = at;
          bracketB_ = m;
        }
      }
    }
    for (size_t pos : {bracketA_, bracketB_}) {
      if (pos == npos || pos >= buf.size()) continue;
      size_t r = wm.rowOf(pos);
      if (r < firstRow || r > lastRow) continue;
      float x = posX(wm.row(r), pos);
      float y = rowY(r) + lh - 2;
      gfx_.fill({x, y, x + cw, y + 1.5f}, theme_.bracket);
    }
  }

  // Carets -------------------------------------------------------------------
  bool drawCarets = editorFocused ? (caretVisible_ || !settings_.caretBlink) : true;
  if (drawCarets) {
    Color cc = editorFocused ? theme_.caret : theme_.caret.withAlpha(0.45f);
    for (const auto& s : sels) {
      size_t r = wm.rowOf(s.caret);
      if (r < firstRow || r > lastRow) continue;
      float x = std::round(posX(wm.row(r), s.caret));
      float y = rowY(r);
      gfx_.fill({x - 1, y + 1, x - 1 + settings_.caretWidth, y + lh - 1}, cc);
    }
  }
  gfx_.popClip();

  if (lay_.minimap.w() > 0) paintMinimap(d);
  paintScrollbar(d);

  // Shadow under the tab strip when scrolled.
  if (d.scrollY > 0 && lay_.tabs.h() > 0) {
    for (int i = 0; i < 4; ++i)
      gfx_.fill({lay_.editor.l, lay_.editor.t + static_cast<float>(i), lay_.editor.r,
                 lay_.editor.t + static_cast<float>(i) + 1},
                theme_.shadow.withAlpha(theme_.shadow.a * (1.0f - static_cast<float>(i) / 4.0f) * 0.6f));
  }
}

void App::paintMinimap(Document& d) {
  const RectF& r = lay_.minimap;
  const TextBuffer& buf = d.ed.buffer();
  const float miniLine = 2.0f, miniChar = 1.0f;
  const size_t lineCount = buf.lineCount();
  float docMini = static_cast<float>(lineCount) * miniLine;
  float maxY = maxScrollY(d);
  float ratio = maxY > 0 ? d.scrollY / maxY : 0;
  float offset = docMini > r.h() ? ratio * (docMini - r.h()) : 0;
  size_t first = static_cast<size_t>(offset / miniLine);
  size_t last = std::min(lineCount - 1, static_cast<size_t>((offset + r.h()) / miniLine) + 1);
  if (lineCount == 0) return;
  d.syntax.ensure(buf, last);
  const size_t tab = static_cast<size_t>(std::max(1, d.ed.config.tabSize));
  gfx_.pushClip(r);
  for (size_t l = first; l <= last; ++l) {
    float y = r.t + static_cast<float>(l) * miniLine - offset;
    std::wstring_view line = buf.lineText(l);
    const auto& toks = d.syntax.tokens(buf, l);
    size_t col = 0;
    for (const Token& tk : toks) {
      size_t i = tk.start, end = std::min<size_t>(tk.start + tk.len, line.size());
      size_t runCol = npos;
      Color c = theme_.token(tk.kind).withAlpha(0.55f);
      auto flush = [&](size_t endCol) {
        if (runCol == npos) return;
        float xs = r.l + 6 + static_cast<float>(runCol) * miniChar;
        float xe = std::min(r.r - 2, r.l + 6 + static_cast<float>(endCol) * miniChar);
        if (xe > xs) gfx_.fill({xs, y, xe, y + miniLine * 0.8f}, c);
        runCol = npos;
      };
      for (; i < end; ++i) {
        wchar_t ch = line[i];
        if (ch == L' ' || ch == L'\t') {
          flush(col);
          col += ch == L'\t' ? tab - col % tab : 1;
        } else {
          if (runCol == npos) runCol = col;
          col += static_cast<size_t>(std::max(1, unitColumns(ch)));
        }
        if (static_cast<float>(col) * miniChar > r.w()) break;
      }
      flush(col);
      if (static_cast<float>(col) * miniChar > r.w()) break;
    }
  }
  // Visible region.
  double topLine = lineAtEditorY(d, d.scrollY), botLine = lineAtEditorY(d, d.scrollY + lay_.text.h());
  float vy = r.t + static_cast<float>(topLine) * miniLine - offset;
  float vh = std::max(miniLine, static_cast<float>(botLine - topLine) * miniLine);
  bool hot = r.contains(mouseX_, mouseY_) || drag_ == Drag::Minimap;
  gfx_.fill({r.l, vy, r.r, vy + vh}, hot ? theme_.minimapView.withAlpha(theme_.minimapView.a * 2) : theme_.minimapView);
  gfx_.popClip();
}

void App::paintScrollbar(Document& d) {
  if (maxScrollY(d) <= 0) return;
  RectF thumb = scrollThumb(d);
  bool hot = lay_.vscroll.contains(mouseX_, mouseY_) || drag_ == Drag::Scrollbar;
  gfx_.fillRounded({thumb.l + 2, thumb.t + 2, thumb.r - 1, thumb.b - 2}, 3,
                   hot ? theme_.scrollThumbHot : theme_.scrollThumb);
}

void App::paintButton(const RectF& r, std::wstring_view label, bool on) {
  bool hot = r.contains(mouseX_, mouseY_);
  gfx_.fillRounded(r, 4, on ? theme_.buttonOn : (hot ? theme_.buttonHover : theme_.buttonBg));
  gfx_.textIn(label, r, theme_.buttonText, gfx_.uiFormat(), true);
}

void App::paintLineEdit(LineEdit& e, const RectF& r, bool focused, const std::wstring& placeholder, bool error) {
  gfx_.fillRounded(r, 4, theme_.inputBg);
  gfx_.strokeRounded(r, 4, error ? theme_.error : (focused ? theme_.inputBorderFocus : theme_.inputBorder), 1);
  RectF inner = r.inset(8, 0);
  IDWriteTextFormat* fmt = gfx_.uiFormat();
  const std::wstring& t = e.text();
  float caretX = gfx_.textWidth(std::wstring_view(t).substr(0, e.caret()), fmt);
  if (caretX - e.scrollX > inner.w() - 4) e.scrollX = caretX - inner.w() + 4;
  if (caretX - e.scrollX < 0) e.scrollX = std::max(0.0f, caretX - 20);
  float th = gfx_.formatHeight(fmt);
  float ty = r.t + (r.h() - th) / 2;
  gfx_.pushClip(inner);
  if (t.empty()) {
    gfx_.text(placeholder, inner.l, ty, theme_.inputPlaceholder, fmt);
  } else {
    if (e.hasSelection()) {
      size_t b = std::min(e.caret(), e.anchor()), en = std::max(e.caret(), e.anchor());
      float xa = gfx_.textWidth(std::wstring_view(t).substr(0, b), fmt);
      float xb = gfx_.textWidth(std::wstring_view(t).substr(0, en), fmt);
      gfx_.fill({inner.l + xa - e.scrollX, ty, inner.l + xb - e.scrollX, ty + th}, theme_.inputSelection);
    }
    gfx_.text(t, inner.l - e.scrollX, ty, theme_.inputText, fmt);
  }
  if (focused && (caretVisible_ || !settings_.caretBlink)) {
    float x = std::round(inner.l + caretX - e.scrollX);
    gfx_.fill({x, ty, x + 1.5f, ty + th}, theme_.inputText);
  }
  gfx_.popClip();
}

size_t App::lineEditHit(LineEdit& e, const RectF& r, float x) {
  RectF inner = r.inset(8, 0);
  float target = x - inner.l + e.scrollX;
  if (target <= 0) return 0;
  auto layout = gfx_.layout(e.text(), gfx_.uiFormat(), 100000, 100);
  if (!layout) return e.text().size();
  BOOL trailing = FALSE, inside = FALSE;
  DWRITE_HIT_TEST_METRICS m{};
  layout->HitTestPoint(target, 5, &trailing, &inside, &m);
  return std::min<size_t>(m.textPosition + (trailing ? m.length : 0), e.text().size());
}

void App::paintFind() {
  const RectF& r = lay_.find;
  gfx_.fill(r, theme_.panelBg);
  gfx_.line(r.l, r.t + 0.5f, r.r, r.t + 0.5f, theme_.tabBorder, 1);
  FindLayout fl = findLayout();
  const wchar_t* labels[4] = {L".*", L"Aa", L"“”", L"↻"};
  bool states[4] = {findOpts_.regex, findOpts_.caseSensitive, findOpts_.wholeWord, findWrap_};
  for (int i = 0; i < 4; ++i) paintButton(fl.toggles[i], labels[i], states[i]);
  std::wstring placeholder = findIncremental_ ? L"Incremental find" : L"Find";
  if (findOpts_.regex) placeholder += L" (regex)";
  paintLineEdit(findEdit_, fl.findBox, focus_ == Focus::Find, placeholder, !findError_.empty());
  std::wstring info;
  bool bad = false;
  if (!findError_.empty()) {
    info = findError_;
    bad = true;
  } else if (Document* d = doc(); d && !findEdit_.text().empty()) {
    std::wstring key = std::to_wstring(d->id) + L"|" + std::to_wstring(d->ed.stateId()) + L"|" +
                       std::to_wstring(findOpts_.regex) + std::to_wstring(findOpts_.caseSensitive) +
                       std::to_wstring(findOpts_.wholeWord) + L"|" + findEdit_.text();
    if (key != findCountKey_) {
      findCountKey_ = key;
      Searcher s;
      findCount_ = s.compile(findEdit_.text(), findOpts_, nullptr, settings_.wordSeparators)
                       ? s.all(d->ed.buffer().text(), 0, std::wstring::npos, 100000).size()
                       : 0;
    }
    info = findCount_ == 0 ? L"No results"
                           : std::to_wstring(findCount_) + (findCount_ == 1 ? L" match" : L" matches");
    bad = findCount_ == 0;
  }
  if (!info.empty()) {
    float w = std::min(gfx_.textWidth(info, gfx_.uiSmallFormat()) + 10, fl.findBox.w() / 2);
    gfx_.fill({fl.findBox.r - w - 2, fl.findBox.t + 2, fl.findBox.r - 2, fl.findBox.b - 2}, theme_.inputBg);
    gfx_.textIn(info, {fl.findBox.r - w, fl.findBox.t, fl.findBox.r - 4, fl.findBox.b},
                bad ? theme_.error : theme_.inputPlaceholder, gfx_.uiSmallFormat());
  }
  paintButton(fl.find, L"Find", false);
  paintButton(fl.prev, L"Prev", false);
  paintButton(fl.all, L"Find All", false);
  if (findMode_ == FindMode::Replace) {
    paintLineEdit(replaceEdit_, fl.replaceBox, focus_ == Focus::Replace, L"Replace", false);
    paintButton(fl.replace, L"Replace", false);
    paintButton(fl.replaceAll, L"Replace All", false);
  }
}

void App::paintStatus() {
  const RectF& r = lay_.status;
  gfx_.fill(r, theme_.statusBg);
  statusItems_.clear();
  Document* d = doc();
  IDWriteTextFormat* fmt = gfx_.uiSmallFormat();
  std::wstring left;
  if (!status_.empty()) {
    left = status_;
  } else if (terminalMode_) {
    left = terminalStatusText();
  } else if (d && d->pdf) {
    left = pdfStatusText(*d);
  } else if (d) {
    const Editor& ed = d->ed;
    const auto& sels = ed.selections().all();
    const Selection& p = ed.selections().primary();
    size_t line = ed.buffer().lineOf(p.caret);
    size_t col = ed.columnAt(p.caret);
    if (sels.size() > 1) {
      left = std::to_wstring(sels.size()) + L" selection regions";
    } else if (!p.empty()) {
      size_t lines = ed.buffer().lineOf(p.end()) - ed.buffer().lineOf(p.begin()) + 1;
      left = lines > 1 ? std::to_wstring(lines) + L" lines, " + std::to_wstring(p.length()) + L" characters selected"
                       : std::to_wstring(p.length()) + L" characters selected";
    } else {
      left = L"Line " + std::to_wstring(line + 1) + L", Column " + std::to_wstring(col + 1);
    }
    if (recording_) left = L"● Recording macro   " + left;
  }
  if (!gitMode_) {
    RectF toggle = sidebarToggleRect();
    bool toggleHot = toggle.contains(mouseX_, mouseY_);
    if (toggleHot) gfx_.fillRounded(toggle, 3, theme_.buttonHover);
    drawSidebarToggle(gfx_, toggle.l + 5, toggle.t + (toggle.h() - 14) / 2,
                      theme_.statusText.withAlpha(toggleHot ? 1.0f : 0.75f), lay_.sidebar.w() > 0);
  } else if (status_.empty()) {
    left = L"Git mode \u2014 Ctrl+Shift+G returns to the editor";
  }
  // Text | MD | Git mode switch, each segment with an icon.
  RectF ms = modeSwitchRect();
  const float segW = ms.w() / 3;
  gfx_.fillRounded(ms, 4, theme_.buttonBg);
  const int activeSeg = gitMode_ ? 2 : terminalMode_ ? -1 : (mdMode_ ? 1 : 0);
  {
    // Terminal mode: a ">_" button between the side bar toggle and the mode switch.
    RectF t = terminalToggleRect();
    if (terminalMode_)
      gfx_.fillRounded(t, 4, theme_.buttonOn);
    else if (t.contains(mouseX_, mouseY_))
      gfx_.fillRounded(t, 4, theme_.buttonHover);
    const Color ic = terminalMode_ ? theme_.buttonText : theme_.statusText;
    const float cx = t.l + 7, cy = std::floor((t.t + t.b) / 2) + 0.5f;
    D2D1_POINT_2F chevron[] = {D2D1::Point2F(cx, cy - 4), D2D1::Point2F(cx + 4.5f, cy), D2D1::Point2F(cx, cy + 4)};
    gfx_.path(chevron, 3, ic, 1.5f, false, false);
    gfx_.line(cx + 7, cy + 4, cx + 14, cy + 4, ic, 1.5f);
  }
  const wchar_t* labels[3] = {L"Text", L"MD", L"Git"};
  for (int i = 0; i < 3; ++i) {
    RectF seg{ms.l + segW * static_cast<float>(i), ms.t, ms.l + segW * static_cast<float>(i + 1), ms.b};
    if (i == activeSeg)
      gfx_.fillRounded(seg, 4, theme_.buttonOn);
    else if (seg.contains(mouseX_, mouseY_))
      gfx_.fillRounded(seg, 4, theme_.buttonHover);
    const float iconW = 15, gap = 5;
    float tw = gfx_.textWidth(labels[i], fmt);
    float gx = std::floor(seg.l + (seg.w() - iconW - gap - tw) / 2);
    float cy = std::floor((seg.t + seg.b) / 2) + 0.5f;
    const Color ic = theme_.buttonText;
    if (i == 0) {  // lines of text
      gfx_.line(gx + 1, cy - 4, gx + 13, cy - 4, ic, 1.4f);
      gfx_.line(gx + 1, cy, gx + 13, cy, ic, 1.4f);
      gfx_.line(gx + 1, cy + 4, gx + 9, cy + 4, ic, 1.4f);
    } else if (i == 1) {  // Markdown mark: M with a down arrow in a frame
      gfx_.strokeRounded({gx - 0.5f, cy - 5.5f, gx + 15.5f, cy + 5.5f}, 2, ic, 1.1f);
      D2D1_POINT_2F m[] = {D2D1::Point2F(gx + 2.5f, cy + 2.8f), D2D1::Point2F(gx + 2.5f, cy - 2.8f),
                           D2D1::Point2F(gx + 5, cy - 0.2f), D2D1::Point2F(gx + 7.5f, cy - 2.8f),
                           D2D1::Point2F(gx + 7.5f, cy + 2.8f)};
      gfx_.path(m, 5, ic, 1.3f, false, false);
      gfx_.line(gx + 11.5f, cy - 3, gx + 11.5f, cy + 2.6f, ic, 1.3f);
      D2D1_POINT_2F arrow[] = {D2D1::Point2F(gx + 9.5f, cy + 0.6f), D2D1::Point2F(gx + 11.5f, cy + 2.8f),
                               D2D1::Point2F(gx + 13.5f, cy + 0.6f)};
      gfx_.path(arrow, 3, ic, 1.3f, false, false);
    } else {
      drawBranchIcon(gfx_, gx + 1, cy - 7.5f, ic);
    }
    gfx_.textIn(labels[i], {gx + iconW + gap, seg.t, seg.r, seg.b}, theme_.buttonText, fmt);
  }
  float textStart = ms.r + 10;
  if (mdMode_ && !gitMode_ && !terminalMode_ && !(d && d->pdf)) {
    // Write | Split | Read for Markdown mode.
    RectF mv = mdViewRect();
    const wchar_t* views[3] = {L"Write", L"Split", L"Read"};
    const float w3 = mv.w() / 3;
    gfx_.fillRounded(mv, 4, theme_.buttonBg);
    for (int i = 0; i < 3; ++i) {
      RectF seg{mv.l + w3 * static_cast<float>(i), mv.t, mv.l + w3 * static_cast<float>(i + 1), mv.b};
      if (i == static_cast<int>(mdView_))
        gfx_.fillRounded(seg, 4, theme_.buttonOn);
      else if (seg.contains(mouseX_, mouseY_))
        gfx_.fillRounded(seg, 4, theme_.buttonHover);
      gfx_.textIn(views[i], seg, theme_.buttonText, fmt, true);
    }
    textStart = mv.r + 10;
  }
  gfx_.textIn(left, {textStart, r.t, r.r - 400, r.b}, theme_.statusText, fmt);
  if (gitMode_ || terminalMode_ || !d) return;
  std::vector<std::pair<int, std::wstring>> items;
  if (d->pdf) {
    items.push_back({5, L"PDF (read-only)"});
  } else {
    if (d->binary) items.push_back({5, L"Binary (read-only)"});
    items.push_back({0, (d->ed.config.translateTabs ? L"Spaces: " : L"Tab Size: ") +
                            std::to_wstring(d->ed.config.tabSize)});
    if (settings_.showLineEndings) items.push_back({1, lineEndingName(d->eol)});
    if (settings_.showEncoding) items.push_back({2, encodingName(d->encoding)});
    items.push_back({3, d->lang->name});
  }
  if (settings_.showGitStatus) {
    if (const GitRepoState* repo = gitRepoForActive()) {
      std::wstring label = repo->branch;
      int pending = repo->changed + repo->staged + repo->untracked;
      if (!repo->operation.empty()) label += L" (" + repo->operation + L")";
      if (pending > 0) label += L"  \u2022 " + std::to_wstring(pending);
      items.insert(items.begin(), {4, label});
    }
  }
  float x = r.r - 14;
  for (auto it = items.rbegin(); it != items.rend(); ++it) {
    float iconW = it->first == 4 ? 16.0f : 0.0f;
    float w = gfx_.textWidth(it->second, fmt) + 20 + iconW;
    RectF ir{x - w, r.t, x, r.b};
    if (ir.contains(mouseX_, mouseY_)) gfx_.fill(ir, theme_.buttonHover);
    if (iconW > 0) drawBranchIcon(gfx_, ir.l + 9, ir.t + (ir.h() - 15) / 2, theme_.statusText);
    gfx_.textIn(it->second, {ir.l + iconW, ir.t, ir.r, ir.b}, theme_.statusText, fmt, true);
    statusItems_.push_back({ir, it->first});
    x -= w + 4;
  }
}

void App::paintOverlay() {
  RectF r = overlayRect();
  for (int i = 1; i <= 6; ++i) {
    float s = static_cast<float>(i) * 2;
    gfx_.fillRounded({r.l - s + 2, r.t - s + 6, r.r + s - 2, r.b + s + 2}, 8 + s,
                     theme_.shadow.withAlpha(theme_.shadow.a / 7.0f));
  }
  gfx_.fillRounded(r, 8, theme_.overlayBg);
  gfx_.strokeRounded(r, 8, theme_.overlayBorder, 1);
  RectF in = overlayInputRect();
  if (overlay_ == Overlay::Prompt)
    gfx_.textIn(promptLabel_, {in.l, r.t + 4, in.r, in.t - 2}, theme_.overlayDetail, gfx_.uiSmallFormat());
  std::wstring placeholder;
  switch (overlay_) {
    case Overlay::Commands: placeholder = L"Type a command"; break;
    case Overlay::Files: placeholder = L"Search files by name (: line, @ symbol, # term)"; break;
    case Overlay::Line: placeholder = L"Line number"; break;
    case Overlay::Symbols: placeholder = L"Search symbols"; break;
    default: break;
  }
  paintLineEdit(overlayEdit_, in, true, placeholder);

  int n = overlayVisibleCount();
  float ih = overlayItemHeight();
  float y = in.b + 6;
  IDWriteTextFormat* fmt = gfx_.uiFormat();
  IDWriteTextFormat* smallFmt = gfx_.uiSmallFormat();
  gfx_.pushClip(r);
  for (int i = 0; i < n; ++i) {
    int idx = overlayTop_ + i;
    if (idx >= static_cast<int>(overlayItems_.size())) break;
    const OverlayItem& it = overlayItems_[static_cast<size_t>(idx)];
    RectF row{r.l + 6, y, r.r - 6, y + ih};
    if (idx == overlaySel_) gfx_.fillRounded(row, 5, theme_.overlaySelected);
    else if (row.contains(mouseX_, mouseY_)) gfx_.fillRounded(row, 5, theme_.overlaySelected.withAlpha(0.5f));
    float hintW = 0;
    if (!it.hint.empty()) {
      hintW = gfx_.textWidth(it.hint, smallFmt) + 16;
      gfx_.textIn(it.hint, {row.r - hintW, row.t, row.r - 8, row.t + 30}, theme_.overlayDetail, smallFmt);
    }
    float th = gfx_.formatHeight(fmt);
    gfx_.pushClip({row.l, row.t, row.r - hintW, row.b});
    gfx_.textMarked(it.title, it.titleMarks, row.l + 10, row.t + (30 - th) / 2, theme_.overlayText,
                    theme_.overlayMatch, fmt, row.w() - hintW);
    if (!it.detail.empty() && ih > 30) {
      gfx_.textMarked(it.detail, it.detailMarks, row.l + 10, row.t + 23, theme_.overlayDetail, theme_.overlayMatch,
                      smallFmt, row.w() - hintW);
    }
    gfx_.popClip();
    y += ih;
  }
  gfx_.popClip();
  // Scroll indicator for long lists.
  int total = static_cast<int>(overlayItems_.size());
  if (total > n && n > 0) {
    float listT = in.b + 6, listH = static_cast<float>(n) * ih;
    float h = std::max(16.0f, listH * static_cast<float>(n) / static_cast<float>(total));
    float t = listT + (listH - h) * static_cast<float>(overlayTop_) / static_cast<float>(total - n);
    gfx_.fillRounded({r.r - 5, t, r.r - 2, t + h}, 1.5f, theme_.scrollThumb);
  }
}

}  // namespace verso
