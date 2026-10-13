#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "app/App.h"

namespace verso {

namespace {


bool isModifierKey(UINT vk) {
  return vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN ||
         vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LMENU ||
         vk == VK_RMENU || vk == VK_CAPITAL;
}

}  // namespace

// ---------------------------------------------------------------------------
// Keyboard

void App::onKeyDown(UINT vk, bool& handled) {
  suppressChar_ = false;
  if (isModifierKey(vk)) return;
  const bool ctrl = GetKeyState(VK_CONTROL) < 0;
  const bool shift = GetKeyState(VK_SHIFT) < 0;
  const bool alt = GetKeyState(VK_MENU) < 0;
  resetBlink();
  if (gitMode_) return;  // the git view handles its own keys and shortcuts
  if (terminalMode_ && overlay_ == Overlay::None && focus_ == Focus::Editor && !pendingChord_) {
    switch (terminalKeyDown(vk, ctrl, shift, alt)) {
      case TermKey::Consumed:
        handled = true;
        return;
      case TermKey::Char:
        return;  // the shell gets the character; Verso's key bindings do not apply
      case TermKey::App:
        break;
    }
  }
  if (!pendingChord_ && (previewKey(vk, shift, alt) || (!ctrl && !alt && pdfKey(vk, shift)))) {
    handled = true;
    suppressChar_ = true;
    return;
  }
  if (mdMode_ && !pendingChord_ && overlay_ == Overlay::None && focus_ == Focus::Editor && ctrl && !alt && !shift &&
      (vk == 'B' || vk == 'I')) {
    runCommand(vk == 'B' ? "md_bold" : "md_italic");
    handled = true;
    suppressChar_ = true;
    return;
  }

  if (!pendingChord_ && widgetKey(vk, ctrl, shift, alt)) {
    handled = true;
    suppressChar_ = true;
    return;
  }

  KeyCombo combo{vk, ctrl, shift, alt};
  std::string cmd;
  Keymap::Result r = keymap_.lookup(pendingChord_, combo, cmd);
  if (pendingChord_) {
    pendingChord_.reset();
    status_.clear();
    if (r == Keymap::Result::Command) runCommand(cmd);
    handled = true;
    suppressChar_ = true;
    invalidate();
    return;
  }
  if (r == Keymap::Result::Prefix) {
    pendingChord_ = combo;
    setStatus(Keymap::comboName(combo) + L" was pressed. Waiting for the second key...", 5000);
    handled = true;
    suppressChar_ = true;
    return;
  }
  if (r == Keymap::Result::Command) {
    runCommand(cmd);
    handled = true;
    suppressChar_ = true;
    return;
  }
  handled = false;
}

bool App::widgetKey(UINT vk, bool ctrl, bool shift, bool alt) {
  if (overlay_ != Overlay::None) {
    int n = static_cast<int>(overlayItems_.size());
    switch (vk) {
      case VK_ESCAPE:
        closeOverlay(false);
        return true;
      case VK_RETURN:
        acceptOverlay(overlaySel_);
        return true;
      case VK_UP:
      case VK_DOWN:
      case VK_PRIOR:
      case VK_NEXT: {
        if (n == 0) return true;
        int step = (vk == VK_UP || vk == VK_DOWN) ? 1 : overlayVisibleCount();
        int dir = (vk == VK_UP || vk == VK_PRIOR) ? -1 : 1;
        if (step == 1)
          overlaySel_ = (overlaySel_ + dir + n) % n;
        else
          overlaySel_ = std::clamp(overlaySel_ + dir * step, 0, n - 1);
        int vis = overlayVisibleCount();
        if (overlaySel_ < overlayTop_) overlayTop_ = overlaySel_;
        if (overlaySel_ >= overlayTop_ + vis) overlayTop_ = overlaySel_ - vis + 1;
        previewOverlay();
        invalidate();
        return true;
      }
      case VK_TAB:
        return true;
      default:
        break;
    }
    auto res = overlayEdit_.onKey(vk, ctrl, shift, hwnd_);
    if (res == LineEdit::KeyResult::Changed) refreshOverlay();
    if (res != LineEdit::KeyResult::NotHandled) {
      invalidate();
      return true;
    }
    return false;
  }

  if (focus_ == Focus::Find || focus_ == Focus::Replace) {
    LineEdit& e = focus_ == Focus::Find ? findEdit_ : replaceEdit_;
    switch (vk) {
      case VK_ESCAPE:
        hideFind();
        return true;
      case VK_RETURN:
        if (findIncremental_) {
          findEdit_.remember();
          hideFind();
        } else if (focus_ == Focus::Replace) {
          if (ctrl && alt) replaceAll();
          else replaceNext();
        } else if (alt) {
          findAll();
        } else {
          findNext(!shift);
        }
        return true;
      case VK_TAB:
        if (findMode_ == FindMode::Replace) focus_ = focus_ == Focus::Find ? Focus::Replace : Focus::Find;
        invalidate();
        return true;
      case VK_UP:
      case VK_DOWN:
        if (e.historyStep(vk == VK_UP ? -1 : 1) && focus_ == Focus::Find) onFindTextChanged();
        invalidate();
        return true;
      default:
        break;
    }
    if (alt && !ctrl) {
      switch (vk) {
        case 'R': findOpts_.regex = !findOpts_.regex; onFindTextChanged(); return true;
        case 'C': findOpts_.caseSensitive = !findOpts_.caseSensitive; onFindTextChanged(); return true;
        case 'W': findOpts_.wholeWord = !findOpts_.wholeWord; onFindTextChanged(); return true;
        case 'A': findWrap_ = !findWrap_; invalidate(); return true;
        default: break;
      }
    }
    auto res = e.onKey(vk, ctrl, shift, hwnd_);
    if (res == LineEdit::KeyResult::Changed && focus_ == Focus::Find) onFindTextChanged();
    if (res != LineEdit::KeyResult::NotHandled) {
      invalidate();
      return true;
    }
    return false;
  }
  return false;
}

void App::onChar(wchar_t c) {
  if (gitMode_) return;
  if (terminalMode_ && overlay_ == Overlay::None && focus_ == Focus::Editor) {
    if (suppressChar_) {
      suppressChar_ = false;
      return;
    }
    terminalChar(c, false);
    return;
  }
  if (editBlocked() && overlay_ == Overlay::None && focus_ == Focus::Editor) return;
  if (suppressChar_) {
    suppressChar_ = false;
    return;
  }
  if (isHighSurrogate(c)) {
    highSurrogate_ = c;
    return;
  }
  std::wstring s;
  if (isLowSurrogate(c)) {
    if (!highSurrogate_) return;
    s = {highSurrogate_, c};
    highSurrogate_ = 0;
  } else {
    if (c < 0x20 || c == 0x7F) return;
    s.assign(1, c);
  }
  if (overlay_ != Overlay::None) {
    overlayEdit_.insert(s);
    refreshOverlay();
    invalidate();
    return;
  }
  if (focus_ == Focus::Find || focus_ == Focus::Replace) {
    LineEdit& e = focus_ == Focus::Find ? findEdit_ : replaceEdit_;
    e.insert(s);
    if (focus_ == Focus::Find) onFindTextChanged();
    invalidate();
    return;
  }
  typeText(s);
}

void App::typeText(const std::wstring& s) {
  if (!doc()) return;
  if (recording_ && !playingMacro_) {
    if (!recordingMacro_.empty() && recordingMacro_.back().command.empty())
      recordingMacro_.back().text += s;
    else
      recordingMacro_.push_back({"", s});
  }
  edit([&](Editor& e) { e.insertText(s, true); });
}

// ---------------------------------------------------------------------------
// Mouse

POINT App::toScreen(float x, float y) const {
  float scale = gfx_.dpi() / 96.0f;
  POINT pt{static_cast<LONG>(x * scale), static_cast<LONG>(y * scale)};
  ClientToScreen(hwnd_, &pt);
  return pt;
}

void App::pressField(LineEdit& e, const RectF& r, float x, float y) {
  const DWORD now = GetMessageTime();
  const size_t pos = lineEditHit(e, r, x);
  const bool doubleClick = fieldClicked_ == &e && now - fieldClickTime_ <= GetDoubleClickTime() &&
                           std::fabs(x - fieldClickX_) < 5 && std::fabs(y - fieldClickY_) < 5;
  fieldClicked_ = doubleClick ? nullptr : &e;  // a third click starts over
  fieldClickTime_ = now;
  fieldClickX_ = x;
  fieldClickY_ = y;
  if (doubleClick) {
    e.selectWordAt(pos);
  } else {
    e.setCaret(pos, GetKeyState(VK_SHIFT) < 0);
    drag_ = Drag::Field;
    dragField_ = &e;
    dragFieldRect_ = r;
    SetCapture(hwnd_);
  }
  invalidate();
}

void App::onMouseDown(int button, float x, float y) {
  mouseX_ = x;
  mouseY_ = y;
  resetBlink();
  if (gitMode_ && !lay_.status.contains(x, y)) return;

  if (overlay_ != Overlay::None) {
    RectF r = overlayRect();
    if (!r.contains(x, y)) {
      closeOverlay(false);
      return;
    }
    RectF in = overlayInputRect();
    if (in.contains(x, y)) {
      pressField(overlayEdit_, in, x, y);
      return;
    }
    float itemTop = in.b + 6;
    float ih = overlayItemHeight();
    if (y >= itemTop) {
      int idx = overlayTop_ + static_cast<int>((y - itemTop) / ih);
      if (idx >= 0 && idx < static_cast<int>(overlayItems_.size())) {
        overlaySel_ = idx;
        acceptOverlay(idx);
      }
    }
    return;
  }

  if (findMode_ != FindMode::Hidden && lay_.find.contains(x, y)) {
    FindLayout fl = findLayout();
    bool* toggles[3] = {&findOpts_.regex, &findOpts_.caseSensitive, &findOpts_.wholeWord};
    for (int i = 0; i < 4; ++i) {
      if (fl.toggles[i].contains(x, y)) {
        if (i < 3) {
          *toggles[i] = !*toggles[i];
          onFindTextChanged();
        } else {
          findWrap_ = !findWrap_;
        }
        invalidate();
        return;
      }
    }
    auto field = [&](LineEdit& e, const RectF& r, Focus f) {
      focus_ = f;
      pressField(e, r, x, y);
    };
    if (fl.findBox.contains(x, y)) return field(findEdit_, fl.findBox, Focus::Find);
    if (findMode_ == FindMode::Replace && fl.replaceBox.contains(x, y))
      return field(replaceEdit_, fl.replaceBox, Focus::Replace);
    if (fl.find.contains(x, y)) return findNext(true);
    if (fl.prev.contains(x, y)) return findNext(false);
    if (fl.all.contains(x, y)) return findAll();
    if (findMode_ == FindMode::Replace && fl.replace.contains(x, y)) return replaceNext();
    if (findMode_ == FindMode::Replace && fl.replaceAll.contains(x, y)) return replaceAll();
    return;
  }

  if (lay_.status.contains(x, y)) {
    if (terminalToggleRect().contains(x, y) && button == 0) {
      setTerminalMode(!terminalMode_);
      return;
    }
    if (RectF ms = modeSwitchRect(); ms.contains(x, y) && button == 0) {
      int seg = std::min(2, static_cast<int>((x - ms.l) / (ms.w() / 3)));
      if (terminalMode_) setTerminalMode(false);
      if (seg == 2)
        setGitMode(true);
      else
        setEditorMode(seg == 1);
      return;
    }
    if (RectF mv = mdViewRect(); mdMode_ && !gitMode_ && !(doc() && doc()->pdf) && mv.contains(x, y) && button == 0) {
      setMdView(static_cast<MdView>(std::min(2, static_cast<int>((x - mv.l) / (mv.w() / 3)))));
      return;
    }
    if (!gitMode_ && sidebarToggleRect().contains(x, y) && button == 0) {
      runCommand("toggle_sidebar");
      return;
    }
    for (const auto& it : statusItems_) {
      if (it.r.contains(x, y)) {
        if (it.kind == 5) return;  // PDF label
        if (it.kind == 4) {
          gitOpenRepository();
          return;
        }
        showStatusMenu(it.kind, toScreen(it.r.l, it.r.t));
        return;
      }
    }
    return;
  }

  if (!lay_.terminal.empty() && lay_.terminal.contains(x, y)) {
    focus_ = Focus::Editor;
    terminalMouseDown(button, x, y);
    return;
  }

  if (!lay_.preview.empty() && lay_.preview.contains(x, y)) {
    if (Document* pd = doc(); pd && (button == 0 || button == 2)) {
      // Shift+click or middle-click opens the destination in a new tab.
      MarkdownPreview::Link link = preview_.linkAt(x, y, lay_.preview, pd->previewScroll);
      if (!link.url.empty()) openPreviewLink(link, button == 2 || GetKeyState(VK_SHIFT) < 0);
    }
    return;
  }

  if (!lay_.splitter.empty() && lay_.splitter.contains(x, y) && button == 0) {
    drag_ = Drag::Splitter;
    SetCapture(hwnd_);
    return;
  }

  if (lay_.sidebar.contains(x, y)) {
    auto rows = sidebarRows();
    float rel = y - lay_.sidebar.t - 4 + sidebarScroll_;
    if (rel < 0) return;
    size_t idx = static_cast<size_t>(rel / kSidebarRow);
    if (idx >= rows.size()) return;
    const SidebarRow& row = rows[idx];
    if (row.kind == SidebarRow::Kind::Header) return;
    if (row.kind == SidebarRow::Kind::OpenFile) {
      bool onClose = x >= lay_.sidebar.r - 24 && x < lay_.sidebar.r - 8;
      if (button == 2 || (button == 0 && onClose)) {
        closeDocument(row.docIndex);
      } else if (button == 0) {
        activate(row.docIndex);
        focus_ = Focus::Editor;
        drag_ = Drag::OpenFile;  // dragging reorders the open files (and tabs)
        SetCapture(hwnd_);
      }
      invalidate();
      return;
    }
    TreeNode* node = row.node;
    if (button == 1) {
      sidebarContextMenu(node, toScreen(x, y));
      return;
    }
    if (button == 0) {
      if (node->dir)
        tree_.toggle(node);
      else
      {
        if (terminalMode_) setTerminalMode(false);  // show the file that was clicked
        openFile(node->path);
      }
      focus_ = Focus::Editor;
    } else if (button == 2 && !node->dir) {
      openFile(node->path);
    }
    invalidate();
    return;
  }

  if (lay_.tabs.contains(x, y)) {
    for (const auto& t : tabRects()) {
      if (!t.r.contains(x, y)) continue;
      if (button == 2 || (button == 0 && t.close.contains(x, y))) {
        closeDocument(t.index);
      } else if (button == 0) {
        activate(t.index);
        drag_ = Drag::Tab;
        dragOffset_ = x;
        SetCapture(hwnd_);
      } else if (button == 1) {
        activate(t.index);
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, L"Close");
        AppendMenuW(m, MF_STRING, 2, L"Close Other Tabs");
        AppendMenuW(m, MF_STRING, 3, L"Close All");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 4, L"Copy File Path");
        AppendMenuW(m, MF_STRING, 5, L"Open Containing Folder");
        AppendMenuW(m, MF_STRING, 6, L"Reveal in Side Bar");
        UINT r = trackMenu(m, toScreen(x, y));
        DestroyMenu(m);
        const char* cmds[] = {"",         "close_file",           "close_others",
                              "close_all", "copy_file_path",      "open_containing_folder",
                              "reveal_in_side_bar"};
        if (r >= 1 && r <= 6) runCommand(cmds[r]);
      }
      return;
    }
    // Double-click on empty tab strip opens a new file.
    DWORD now = GetMessageTime();
    if (button == 0 && now - lastClickTime_ <= GetDoubleClickTime()) newDocument();
    lastClickTime_ = now;
    return;
  }

  Document* d = doc();
  if (!d) return;
  focus_ = Focus::Editor;

  if (lay_.vscroll.contains(x, y)) {
    RectF thumb = scrollThumb(*d);
    if (thumb.contains(x, y)) {
      drag_ = Drag::Scrollbar;
      dragOffset_ = y - thumb.t;
      SetCapture(hwnd_);
    } else {
      scrollBy(y < thumb.t ? -lay_.text.h() : lay_.text.h(), 0);
    }
    return;
  }
  if (!lay_.minimap.empty() && lay_.minimap.contains(x, y)) {
    drag_ = Drag::Minimap;
    SetCapture(hwnd_);
    updateDrag(x, y);
    return;
  }
  if (lay_.gutter.contains(x, y) && button == 0) {
    size_t line;
    hitTestText(lay_.text.l, y, nullptr, &line);
    dragLine_ = line;
    const TextBuffer& b = d->ed.buffer();
    size_t e = line + 1 < b.lineCount() ? b.lineStart(line + 1) : b.size();
    d->ed.setSelections({Selection(b.lineStart(line), e)}, 0);
    drag_ = Drag::Gutter;
    SetCapture(hwnd_);
    invalidate();
    return;
  }
  if (!lay_.text.contains(x, y) && !lay_.editor.contains(x, y)) return;

  const bool ctrl = GetKeyState(VK_CONTROL) < 0;
  const bool shift = GetKeyState(VK_SHIFT) < 0;
  const bool alt = GetKeyState(VK_MENU) < 0;

  if (button == 2 || (button == 1 && shift) || (button == 0 && alt && shift)) {
    double col;
    size_t line;
    hitTestText(x, y, &col, &line);
    dragLine_ = line;
    dragCol_ = col;
    d->ed.setColumnSelection(line, col, line, col);
    drag_ = Drag::Column;
    SetCapture(hwnd_);
    invalidate();
    return;
  }
  if (button == 1) {
    size_t pos = hitTestText(x, y);
    bool inside = false;
    for (const auto& s : d->ed.selections().all())
      if (!s.empty() && pos >= s.begin() && pos <= s.end()) inside = true;
    if (!inside) d->ed.setCaret(pos, false, false);
    invalidate();
    showContextMenu(toScreen(x, y));
    return;
  }

  DWORD now = GetMessageTime();
  if (now - lastClickTime_ <= GetDoubleClickTime() && std::fabs(x - lastClickX_) < 5 && std::fabs(y - lastClickY_) < 5)
    clickCount_ = clickCount_ % 3 + 1;
  else
    clickCount_ = 1;
  lastClickTime_ = now;
  lastClickX_ = x;
  lastClickY_ = y;

  size_t pos = hitTestText(x, y);
  if (clickCount_ == 2 && d->findResults && !ctrl && !shift && openResultAt(*d, d->ed.buffer().lineOf(pos))) {
    clickCount_ = 0;
    return;
  }
  // dragBaseSels_ holds the selections kept unchanged while the new one is dragged.
  dragBaseSels_.clear();
  const TextBuffer& b = d->ed.buffer();
  if (clickCount_ == 1) {
    if (ctrl) dragBaseSels_ = d->ed.selections().all();
    if (shift && !ctrl) {
      dragAnchor_ = Selection::at(d->ed.selections().primary().anchor);
      d->ed.setCaret(pos, true, false);
    } else {
      dragAnchor_ = Selection::at(pos);
      d->ed.setCaret(pos, false, ctrl);
    }
    dragGranularity_ = 0;
  } else {
    if (ctrl) {
      // Keep everything except the caret the previous click of this sequence added.
      const auto& all = d->ed.selections().all();
      size_t prim = d->ed.selections().primaryIndex();
      for (size_t i = 0; i < all.size(); ++i)
        if (i != prim) dragBaseSels_.push_back(all[i]);
    }
    size_t s, e;
    if (clickCount_ == 2) {
      auto w = d->ed.wordAt(pos);
      s = w.first;
      e = w.second;
      dragGranularity_ = 1;
    } else {
      size_t line = b.lineOf(pos);
      s = b.lineStart(line);
      e = line + 1 < b.lineCount() ? b.lineStart(line + 1) : b.size();
      dragGranularity_ = 2;
    }
    dragAnchor_ = Selection(s, e);
    std::vector<Selection> v = dragBaseSels_;
    v.push_back(Selection(s, e));
    size_t primary = v.size() - 1;
    d->ed.setSelections(std::move(v), primary);
  }
  drag_ = Drag::Text;
  SetCapture(hwnd_);
  invalidate();
}

void App::onMouseMove(float x, float y) {
  bool hoverChanged = (lay_.tabs.contains(x, y) || lay_.tabs.contains(mouseX_, mouseY_) || lay_.preview.contains(x, y) ||
                       lay_.sidebar.contains(x, y) || lay_.sidebar.contains(mouseX_, mouseY_) ||
                       lay_.minimap.contains(x, y) != lay_.minimap.contains(mouseX_, mouseY_) ||
                       lay_.find.contains(x, y) || overlay_ != Overlay::None);
  mouseX_ = x;
  mouseY_ = y;
  if (drag_ != Drag::None) {
    updateDrag(x, y);
    if ((drag_ == Drag::Text || drag_ == Drag::Column || drag_ == Drag::Gutter) &&
        (y < lay_.text.t || y > lay_.text.b || x < lay_.text.l || x > lay_.text.r))
      SetTimer(hwnd_, kTimerAutoscroll, 30, nullptr);
    return;
  }
  if (hoverChanged) invalidate();
}

void App::onMouseUp(int /*button*/, float x, float y) {
  if (drag_ != Drag::None) {
    updateDrag(x, y);
    drag_ = Drag::None;
    dragField_ = nullptr;
    KillTimer(hwnd_, kTimerAutoscroll);
    ReleaseCapture();
    if (Document* d = doc()) afterEdit(*d, false);
  }
}

void App::updateDrag(float x, float y) {
  Document* d = doc();
  switch (drag_) {
    case Drag::Terminal:
      terminalDrag(x, y);
      return;
    case Drag::Field:
      if (dragField_) dragField_->setCaret(lineEditHit(*dragField_, dragFieldRect_, x), true);
      break;
    case Drag::Splitter:
      sidebarWidth_ = std::clamp(x, 120.0f, std::max(120.0f, width_ - 300.0f));
      computeLayout();
      sessionDirty_ = true;
      break;
    case Drag::Tab: {
      for (const auto& t : tabRects()) {
        if (t.index != active_ && t.r.contains(x, lay_.tabs.t + 10)) {
          moveDocument(active_, t.index);
          break;
        }
      }
      break;
    }
    case Drag::OpenFile: {
      auto rows = sidebarRows();
      float rel = y - lay_.sidebar.t - 4 + sidebarScroll_;
      size_t idx = rel < 0 ? 0 : static_cast<size_t>(rel / kSidebarRow);
      if (idx < rows.size() && rows[idx].kind == SidebarRow::Kind::OpenFile && rows[idx].docIndex != active_)
        moveDocument(active_, rows[idx].docIndex);
      break;
    }
    default:
      break;
  }
  if (!d) {
    invalidate();
    return;
  }
  const TextBuffer& b = d->ed.buffer();
  switch (drag_) {
    case Drag::Text: {
      size_t pos = hitTestText(x, y);
      Selection n;
      if (dragGranularity_ == 0) {
        n = Selection(dragAnchor_.anchor, pos);
      } else {
        size_t s, e;
        if (dragGranularity_ == 1) {
          auto w = d->ed.wordAt(pos);
          s = w.first;
          e = w.second;
        } else {
          size_t line = b.lineOf(pos);
          s = b.lineStart(line);
          e = line + 1 < b.lineCount() ? b.lineStart(line + 1) : b.size();
        }
        if (s < dragAnchor_.begin())
          n = Selection(dragAnchor_.end(), s);
        else
          n = Selection(dragAnchor_.begin(), std::max(e, dragAnchor_.end()));
      }
      std::vector<Selection> v = dragBaseSels_;
      v.push_back(n);
      size_t primary = v.size() - 1;
      d->ed.setSelections(std::move(v), primary);
      break;
    }
    case Drag::Column: {
      double col;
      size_t line;
      hitTestText(x, y, &col, &line);
      d->ed.setColumnSelection(dragLine_, dragCol_, line, col);
      break;
    }
    case Drag::Gutter: {
      size_t line;
      hitTestText(lay_.text.l, y, nullptr, &line);
      size_t l0 = std::min(line, dragLine_), l1 = std::max(line, dragLine_);
      size_t s = b.lineStart(l0);
      size_t e = l1 + 1 < b.lineCount() ? b.lineStart(l1 + 1) : b.size();
      d->ed.setSelections({line < dragLine_ ? Selection(e, s) : Selection(s, e)}, 0);
      break;
    }
    case Drag::Minimap: {
      const float miniLine = 2.0f;
      float docMini = static_cast<float>(b.lineCount()) * miniLine;
      float viewH = lay_.minimap.h();
      float maxY = maxScrollY(*d);
      float ratio = maxY > 0 ? d->scrollY / maxY : 0;
      float offset = docMini > viewH ? ratio * (docMini - viewH) : 0;
      float line = (y - lay_.minimap.t + offset) / miniLine;
      scrollEditorToLine(*d, line);
      d->scrollY -= lay_.text.h() / 2;
      clampScroll(*d);
      break;
    }
    case Drag::Scrollbar: {
      RectF thumb = scrollThumb(*d);
      float track = lay_.vscroll.h() - thumb.h();
      float top = y - dragOffset_ - lay_.vscroll.t;
      if (track > 0) d->scrollY = std::clamp(top / track, 0.0f, 1.0f) * maxScrollY(*d);
      clampScroll(*d);
      break;
    }
    default:
      break;
  }
  invalidate();
}

void App::onMouseWheel(float notches, bool horizontal, float x, float y) {
  if (gitMode_) return;
  if (terminalMode_ && overlay_ == Overlay::None && lay_.terminal.contains(x, y)) {
    if (!horizontal) terminalWheel(notches);
    return;
  }
  if (Document* pd = doc(); pd && pd->pdf && overlay_ == Overlay::None && lay_.pdf.contains(x, y)) {
    if (GetKeyState(VK_CONTROL) < 0)
      zoomPdf(notches > 0 ? 1.1f : 1 / 1.1f, y - lay_.pdf.t);
    else if (horizontal)
      pd->pdf->scrollBy(lay_.pdf, -notches * 120, 0);
    else
      pd->pdf->scrollBy(lay_.pdf, 0, -notches * 120);
    invalidate();
    return;
  }
  if (overlay_ == Overlay::None && !lay_.preview.empty() && lay_.preview.contains(x, y)) {
    scrollPreview(-notches * 3 * 40.0f);
    return;
  }
  if (overlay_ != Overlay::None) {
    int n = static_cast<int>(overlayItems_.size());
    int vis = overlayVisibleCount();
    overlayTop_ = std::clamp(overlayTop_ - static_cast<int>(notches * 3), 0, std::max(0, n - vis));
    invalidate();
    return;
  }
  if (lay_.sidebar.contains(x, y)) {
    float total = static_cast<float>(sidebarRows().size()) * kSidebarRow + 4;
    sidebarScroll_ = std::clamp(sidebarScroll_ - notches * 3 * kSidebarRow, 0.0f,
                                std::max(0.0f, total - lay_.sidebar.h() + kSidebarRow));
    invalidate();
    return;
  }
  if (lay_.tabs.contains(x, y)) {
    tabScroll_ = std::max(0.0f, tabScroll_ - notches * 60);
    invalidate();
    return;
  }
  float step = gfx_.lineHeight() * 3 * notches;
  if (horizontal)
    scrollBy(0, -step);
  else
    scrollBy(-step, 0);
}

// ---------------------------------------------------------------------------
// Geometry and scrolling

float App::textLeft(const Document& d) const {
  (void)d;
  return lay_.text.l + 6.0f;
}

size_t App::hitTestText(float x, float y, double* col, size_t* line) {
  Document* d = doc();
  if (!d) return 0;
  ensureWrap(*d);
  const WrapMap& wm = d->wrap;
  const TextBuffer& b = d->ed.buffer();
  const float lh = gfx_.lineHeight(), cw = gfx_.charWidth();
  float relY = y - lay_.text.t + d->scrollY;
  long long ri = static_cast<long long>(std::floor(relY / lh));
  bool past = ri >= static_cast<long long>(wm.rowCount());
  ri = std::clamp<long long>(ri, 0, static_cast<long long>(wm.rowCount()) - 1);
  VisualRow row = wm.row(static_cast<size_t>(ri));
  double c = (x - textLeft(*d) + d->scrollX) / cw - static_cast<double>(row.indent);
  if (c < 0) c = 0;
  c += static_cast<double>(row.startCol);
  if (col) *col = c;
  if (line) *line = row.line;
  if (past && !col) return b.size();
  return wm.caretOnRow(static_cast<size_t>(ri), d->ed.posAtColumn(row.line, c, true));
}

float App::maxScrollY(const Document& d) const {
  const float lh = gfx_.lineHeight();
  size_t rowCount = d.wrapBuilt ? d.wrap.rowCount() : d.ed.buffer().lineCount();
  float lines = static_cast<float>(rowCount);
  if (settings_.scrollPastEnd) return std::max(0.0f, (lines - 1) * lh);
  return std::max(0.0f, lines * lh - lay_.text.h());
}

float App::maxScrollX(Document& d) {
  if (d.wrapBuilt && d.wrap.wrapping()) return 0.0f;
  if (d.longestState != d.ed.stateId() || d.longestState == UINT64_MAX) {
    const TextBuffer& b = d.ed.buffer();
    size_t best = 0;
    const size_t tab = static_cast<size_t>(std::max(1, d.ed.config.tabSize));
    if (b.size() < (4u << 20)) {
      for (size_t l = 0; l < b.lineCount(); ++l) {
        std::wstring_view t = b.lineText(l);
        if (t.size() <= best) continue;
        size_t cols = 0;
        for (wchar_t c : t) cols += c == L'\t' ? tab - cols % tab : static_cast<size_t>(unitColumns(c));
        best = std::max(best, cols);
      }
    } else {
      best = b.longestLineLength() * 2;
    }
    d.longestLine = best;
    d.longestState = d.ed.stateId();
  }
  float content = static_cast<float>(d.longestLine + 4) * gfx_.charWidth() + 12.0f;
  return std::max(0.0f, content - lay_.text.w());
}

void App::clampScroll(Document& d) {
  ensureWrap(d);
  d.scrollY = std::clamp(d.scrollY, 0.0f, maxScrollY(d));
  d.scrollX = std::clamp(d.scrollX, 0.0f, maxScrollX(d));
}

void App::scrollBy(float dy, float dx) {
  Document* d = doc();
  if (!d) return;
  d->scrollY += dy;
  d->scrollX += dx;
  clampScroll(*d);
  sessionDirty_ = true;
  invalidate();
}

int App::pageLines() const {
  return std::max(1, static_cast<int>(lay_.text.h() / gfx_.lineHeight()) - 1);
}

void App::revealCaret(bool center) {
  Document* d = doc();
  if (!d) return;
  const float lh = gfx_.lineHeight(), cw = gfx_.charWidth();
  const Selection& s = d->ed.selections().primary();
  ensureWrap(*d);
  size_t caretRow = d->wrap.rowOf(s.caret);
  VisualRow row = d->wrap.row(caretRow);
  float y = static_cast<float>(caretRow) * lh;
  float viewH = lay_.text.h();
  bool farAway = y < d->scrollY - viewH || y > d->scrollY + 2 * viewH;
  if (center || farAway) {
    d->scrollY = y - viewH / 2 + lh / 2;
  } else if (y < d->scrollY) {
    d->scrollY = y;
  } else if (y + lh > d->scrollY + viewH) {
    d->scrollY = y + lh - viewH;
  }
  float x = (static_cast<float>(d->ed.columnAt(s.caret)) - static_cast<float>(row.startCol) +
             static_cast<float>(row.indent)) * cw;
  float viewW = lay_.text.w() - 12.0f;
  if (x < d->scrollX)
    d->scrollX = std::max(0.0f, x - cw * 4);
  else if (x + cw > d->scrollX + viewW)
    d->scrollX = x + cw * 4 - viewW;
  clampScroll(*d);
  invalidate();
}

}  // namespace verso
