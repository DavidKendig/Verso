// Linux git mode: drawn controls (see GitWidgets.h). The diff model and patch building
// follow merge/src/diff_view.cpp, the commit list merge/src/commit_list.cpp.
#include "app/linux/GitWidgets.h"

#include <cairo.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <unordered_map>

#include "app/Clipboard.h"
#include "util.h"

namespace verso::gitui {

namespace {

constexpr int kMaxLanes = 24;
constexpr size_t kMaxDiffLines = 60000;
constexpr size_t kMaxConsoleLines = 20000;
constexpr float kBar = 10;  // scroll bar track width

PangoContext* context() {
  // The same text rendering as Gfx (GfxCairo.cpp): grayscale antialiasing, slight hinting.
  static PangoContext* ctx = [] {
    PangoContext* c = pango_font_map_create_context(pango_cairo_font_map_get_default());
    cairo_font_options_t* fo = cairo_font_options_create();
    cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
    pango_cairo_context_set_font_options(c, fo);
    cairo_font_options_destroy(fo);
    pango_cairo_context_set_resolution(c, 96.0);
    return c;
  }();
  return ctx;
}

PangoLayout* scratch() {
  static PangoLayout* l = pango_layout_new(context());
  return l;
}

// UTF-8 for Pango, with the byte offset of every character (wchar_t is UTF-32 here).
std::string toUtf8(std::wstring_view s, std::vector<int>* offsets = nullptr) {
  std::string out;
  out.reserve(s.size());
  if (offsets) offsets->assign(s.size() + 1, 0);
  for (size_t i = 0; i < s.size(); ++i) {
    if (offsets) (*offsets)[i] = static_cast<int>(out.size());
    uint32_t c = static_cast<uint32_t>(s[i]);
    if ((c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) c = 0xFFFD;
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  if (offsets) (*offsets)[s.size()] = static_cast<int>(out.size());
  return out;
}

void setColor(cairo_t* cr, Color c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }

float px(int pangoUnits) { return static_cast<float>(pangoUnits) / PANGO_SCALE; }

bool isWordChar(wchar_t c) { return std::iswalnum(c) || c == L'_'; }

}  // namespace

// ---------------------------------------------------------------------------------------
// Colors and text
// ---------------------------------------------------------------------------------------
Palette makePalette(const Theme& t) {
  Palette p;
  const bool d = t.dark;
  p.bg = t.panelBg;
  p.panel = t.bg;
  p.side = t.sidebarBg;
  p.input = t.inputBg;
  p.border = t.tabBorder;
  p.text = t.overlayText;
  p.dim = t.overlayDetail;
  p.accent = t.accent;
  p.select = t.selection;
  p.selectDim = t.sidebarSelected;
  p.hover = t.sidebarHover;
  p.added = t.gitAdded;
  p.modified = t.gitModified;
  p.deleted = t.gitDeleted;
  p.addBg = t.gitAdded.withAlpha(d ? 0.15f : 0.20f);
  p.addGutter = t.gitAdded.withAlpha(d ? 0.09f : 0.12f);
  p.addText = d ? Color::hex(0x8FD99C) : Color::hex(0x2E7D32);
  p.delBg = t.gitDeleted.withAlpha(d ? 0.15f : 0.16f);
  p.delGutter = t.gitDeleted.withAlpha(d ? 0.09f : 0.10f);
  p.delText = d ? Color::hex(0xF09A9A) : Color::hex(0xC62828);
  p.hunkBg = Color::hex(0x5A9CF8, d ? 0.13f : 0.12f);
  p.hunkText = d ? Color::hex(0x8FB4F0) : Color::hex(0x2B5FB4);
  p.fileBg = t.buttonHover;
  p.button = t.buttonBg;
  p.buttonHover = t.buttonHover;
  p.buttonDown = t.buttonBg;
  p.buttonOn = t.buttonOn;
  p.buttonText = t.buttonText;
  p.warn = t.gitModified;
  p.badgeLocal = Color::hex(0x2F7A4A);
  p.badgeCurrent = Color::hex(0x2F6FC2);
  p.badgeRemote = Color::hex(0x4A4F8F);
  p.badgeTag = Color::hex(0x8A6A22);
  p.badgeHead = Color::hex(0xA84F2A);
  p.scrollThumb = t.scrollThumb;
  p.scrollThumbHot = t.scrollThumbHot;
  p.inputBorder = t.inputBorder;
  p.inputBorderFocus = t.inputBorderFocus;
  p.placeholder = t.inputPlaceholder;
  p.inputSelection = t.inputSelection;
  p.overlayBg = t.overlayBg;
  p.overlayBorder = t.overlayBorder;
  p.shadow = t.shadow;
  return p;
}

Color laneColor(int lane) {
  static const uint32_t colors[] = {0x5A9CF8, 0xE08A3C, 0x6CC66F, 0xD06AC8, 0xE5C04B, 0x4FC9C4, 0xE05F5F, 0x9F8CF2};
  return Color::hex(colors[(lane < 0 ? 0 : lane) % 8]);
}

Color statusColor(wchar_t c, const Palette& p) {
  switch (c) {
    case L'A': return p.added;
    case L'D': return p.deleted;
    case L'M': return p.modified;
    case L'R':
    case L'C': return Color::hex(0x5A9CF8);
    case L'U': return Color::hex(0xE08A3C);
    case L'?': return p.dim;
    default: return p.text;
  }
}

void drawText(Gfx& g, std::wstring_view s, const RectF& r, Color c, IDWriteTextFormat* f, Align a, Elide e) {
  cairo_t* cr = g.cairo();
  if (s.empty() || !f || !cr || r.w() <= 0 || r.h() <= 0) return;
  std::wstring clean(s);
  for (auto& ch : clean)
    if (ch == L'\n' || ch == L'\r' || ch == L'\t') ch = L' ';
  std::string u = toUtf8(clean);
  PangoLayout* l = scratch();
  pango_layout_set_attributes(l, nullptr);
  pango_layout_set_font_description(l, f->desc);
  pango_layout_set_text(l, u.data(), static_cast<int>(u.size()));
  const bool bounded = e != Elide::None || a != Align::Left;
  pango_layout_set_width(l, bounded ? static_cast<int>(r.w() * PANGO_SCALE) : -1);
  pango_layout_set_ellipsize(l, !bounded ? PANGO_ELLIPSIZE_NONE
                                : e == Elide::Middle ? PANGO_ELLIPSIZE_MIDDLE
                                                     : PANGO_ELLIPSIZE_END);
  pango_layout_set_alignment(l, a == Align::Center  ? PANGO_ALIGN_CENTER
                                : a == Align::Right ? PANGO_ALIGN_RIGHT
                                                    : PANGO_ALIGN_LEFT);
  PangoRectangle logical;
  pango_layout_get_extents(l, nullptr, &logical);
  float y = std::round(r.t + (r.h() - px(logical.height)) / 2);
  cairo_save(cr);
  cairo_rectangle(cr, std::floor(r.l), std::floor(r.t), std::ceil(r.w()) + 1, std::ceil(r.h()));
  cairo_clip(cr);
  setColor(cr, c);
  cairo_move_to(cr, r.l, y);
  pango_cairo_show_layout(cr, l);
  cairo_restore(cr);
  pango_layout_set_width(l, -1);
  pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_NONE);
  pango_layout_set_alignment(l, PANGO_ALIGN_LEFT);
}

void drawRaw(Gfx& g, std::wstring_view s, float x, float y, Color c, IDWriteTextFormat* f) {
  cairo_t* cr = g.cairo();
  if (s.empty() || !f || !cr) return;
  std::string u = toUtf8(s);
  PangoLayout* l = scratch();
  pango_layout_set_attributes(l, nullptr);
  pango_layout_set_font_description(l, f->desc);
  pango_layout_set_text(l, u.data(), static_cast<int>(u.size()));
  setColor(cr, c);
  cairo_move_to(cr, x, y);
  pango_cairo_show_layout(cr, l);
}

float textHeight(IDWriteTextFormat* f) {
  static std::unordered_map<IDWriteTextFormat*, float> cache;
  if (!f) return 14;
  auto it = cache.find(f);
  if (it != cache.end()) return it->second;
  PangoLayout* l = scratch();
  pango_layout_set_attributes(l, nullptr);
  pango_layout_set_font_description(l, f->desc);
  pango_layout_set_text(l, "Ag", -1);
  PangoRectangle logical;
  pango_layout_get_extents(l, nullptr, &logical);
  float h = px(logical.height);
  cache[f] = h;
  return h;
}

void strokeCircle(Gfx& g, float cx, float cy, float r, Color c, float width) {
  cairo_t* cr = g.cairo();
  if (!cr || r <= 0) return;
  cairo_new_sub_path(cr);
  cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
  setColor(cr, c);
  cairo_set_line_width(cr, width);
  cairo_stroke(cr);
}

// ---------------------------------------------------------------------------------------
// ScrollBar, ClickCounter
// ---------------------------------------------------------------------------------------
RectF ScrollBar::thumb() const {
  if (!visible()) return {};
  float len = vertical ? track.h() : track.w();
  float size = std::min(len, std::max(24.0f, len * page / total));
  float at = (len - size) * (maxPos() > 0 ? std::clamp(pos / maxPos(), 0.0f, 1.0f) : 0.0f);
  if (vertical) return {track.l + 2, track.t + at, track.r - 2, track.t + at + size};
  return {track.l + at, track.t + 2, track.l + at + size, track.b - 2};
}

void ScrollBar::paint(Gfx& g, const Palette& p, float mx, float my) const {
  if (!visible()) return;
  bool hot = dragging || track.contains(mx, my);
  g.fillRounded(thumb(), 3, hot ? p.scrollThumbHot : p.scrollThumb);
}

bool ScrollBar::press(float x, float y) {
  if (!visible() || !track.contains(x, y)) return false;
  RectF th = thumb();
  float at = vertical ? y : x;
  float lo = vertical ? th.t : th.l, hi = vertical ? th.b : th.r;
  if (at >= lo && at <= hi) {
    dragging = true;
    grab = at - lo;
  } else {
    pos = std::clamp(pos + (at < lo ? -page : page), 0.0f, maxPos());
  }
  return true;
}

void ScrollBar::dragTo(float x, float y) {
  if (!dragging || !visible()) return;
  RectF th = thumb();
  float len = vertical ? track.h() : track.w();
  float size = vertical ? th.h() : th.w();
  float at = (vertical ? y - track.t : x - track.l) - grab;
  float frac = len > size ? at / (len - size) : 0;
  pos = std::clamp(frac * maxPos(), 0.0f, maxPos());
}

int ClickCounter::press(float x, float y, const void* target) {
  uint64_t now = GetTickCount64();
  if (target == target_ && now - time_ <= GetDoubleClickTime() && std::fabs(x - x_) <= 4 && std::fabs(y - y_) <= 4)
    count_ = count_ >= 3 ? 1 : count_ + 1;
  else
    count_ = 1;
  time_ = now;
  x_ = x;
  y_ = y;
  target_ = target;
  return count_;
}

namespace {
// Rows to scroll for a wheel turn; fractions (touchpads) accumulate.
int wheelSteps(float& acc, float notches, float perNotch) {
  acc += notches * perNotch;
  int steps = static_cast<int>(acc);
  acc -= static_cast<float>(steps);
  return steps;
}
float gWheelAcc = 0;
}  // namespace

// ---------------------------------------------------------------------------------------
// TextArea
// ---------------------------------------------------------------------------------------
TextArea::TextArea() = default;

TextArea::~TextArea() {
  if (layout_) g_object_unref(layout_);
}

RectF TextArea::textRect() const { return RectF{rect.l + 7, rect.t + 4, rect.r - 12, rect.b - 4}; }

void TextArea::setText(const std::wstring& t) {
  text_.clear();
  for (wchar_t c : t)
    if (c != L'\r') text_ += c;
  caret_ = anchor_ = 0;
  scrollY_ = 0;
  preferredX_ = -1;
  undo_.clear();
  redo_.clear();
  lastTyping_ = false;
  dirty_ = true;
}

void TextArea::ensureLayout() {
  float w = std::max(20.0f, textRect().w());
  if (!layout_) {
    layout_ = pango_layout_new(context());
    pango_layout_set_wrap(layout_, PANGO_WRAP_WORD_CHAR);
  }
  if (dirty_ || w != layoutW_ || font != layoutFont_) {
    std::string u = toUtf8(text_, &offsets_);
    if (font) pango_layout_set_font_description(layout_, font->desc);
    pango_layout_set_text(layout_, u.data(), static_cast<int>(u.size()));
    pango_layout_set_width(layout_, static_cast<int>(w * PANGO_SCALE));
    layoutW_ = w;
    layoutFont_ = font;
    dirty_ = false;
  }
}

int TextArea::byteOf(size_t pos) const {
  if (offsets_.empty()) return 0;
  return offsets_[std::min(pos, offsets_.size() - 1)];
}

size_t TextArea::posOf(int byte) const {
  return static_cast<size_t>(std::lower_bound(offsets_.begin(), offsets_.end(), byte) - offsets_.begin());
}

float TextArea::contentHeight() {
  ensureLayout();
  PangoRectangle logical;
  pango_layout_get_extents(layout_, nullptr, &logical);
  return px(logical.height);
}

void TextArea::revealCaret() {
  ensureLayout();
  PangoRectangle strong;
  pango_layout_get_cursor_pos(layout_, byteOf(caret_), &strong, nullptr);
  float y = px(strong.y), h = px(strong.height);
  float view = textRect().h();
  if (y < scrollY_) scrollY_ = y;
  if (y + h > scrollY_ + view) scrollY_ = y + h - view;
  scrollY_ = std::clamp(scrollY_, 0.0f, std::max(0.0f, contentHeight() - view));
}

void TextArea::setCaret(size_t pos, bool extend) {
  caret_ = std::min(pos, text_.size());
  if (!extend) anchor_ = caret_;
  revealCaret();
}

size_t TextArea::indexAt(float x, float y) {
  ensureLayout();
  RectF tr = textRect();
  int index = 0, trailing = 0;
  pango_layout_xy_to_index(layout_, static_cast<int>((x - tr.l) * PANGO_SCALE),
                           static_cast<int>((y - tr.t + scrollY_) * PANGO_SCALE), &index, &trailing);
  return std::min(posOf(index) + static_cast<size_t>(trailing), text_.size());
}

size_t TextArea::wordLeft(size_t p) const {
  while (p > 0 && std::iswspace(text_[p - 1])) --p;
  bool w = p > 0 && isWordChar(text_[p - 1]);
  while (p > 0 && !std::iswspace(text_[p - 1]) && isWordChar(text_[p - 1]) == w) --p;
  return p;
}

size_t TextArea::wordRight(size_t p) const {
  const size_t n = text_.size();
  while (p < n && std::iswspace(text_[p])) ++p;
  bool w = p < n && isWordChar(text_[p]);
  while (p < n && !std::iswspace(text_[p]) && isWordChar(text_[p]) == w) ++p;
  return p;
}

void TextArea::pushUndo(bool typing) {
  if (!(typing && lastTyping_)) {
    undo_.push_back({text_, caret_});
    if (undo_.size() > 200) undo_.erase(undo_.begin());
  }
  redo_.clear();
  lastTyping_ = typing;
}

void TextArea::replaceSelection(const std::wstring& s, bool typing) {
  if (readOnly) return;
  pushUndo(typing);
  size_t b = std::min(caret_, anchor_), e = std::max(caret_, anchor_);
  std::wstring clean;
  for (wchar_t c : s)
    if (c != L'\r') clean += c;
  text_.replace(b, e - b, clean);
  caret_ = anchor_ = b + clean.size();
  dirty_ = true;
  preferredX_ = -1;
  revealCaret();
}

void TextArea::selectAll() {
  anchor_ = 0;
  caret_ = text_.size();
}

std::wstring TextArea::selectedText() const {
  size_t b = std::min(caret_, anchor_), e = std::max(caret_, anchor_);
  return text_.substr(b, e - b);
}

void TextArea::paint(Gfx& g, const Palette& p, bool focused, bool caretOn) {
  ensureLayout();
  cairo_t* cr = g.cairo();
  g.fillRounded(rect, 4, p.input);
  g.strokeRounded(rect, 4, focused && !readOnly ? p.inputBorderFocus : p.inputBorder, 1);
  RectF tr = textRect();
  float ch = contentHeight();
  scrollY_ = std::clamp(scrollY_, 0.0f, std::max(0.0f, ch - tr.h()));
  sb_.track = RectF{rect.r - kBar, rect.t + 2, rect.r - 1, rect.b - 2};
  sb_.total = ch;
  sb_.page = tr.h();
  sb_.pos = scrollY_;
  g.pushClip(RectF{rect.l + 1, rect.t + 1, rect.r - 1, rect.b - 1});
  if (text_.empty() && !placeholder.empty()) drawRaw(g, placeholder, tr.l, tr.t, p.placeholder, font);
  if (caret_ != anchor_) {
    int a = byteOf(std::min(caret_, anchor_)), b = byteOf(std::max(caret_, anchor_));
    Color sel = focused ? p.inputSelection : p.inputSelection.withAlpha(0.6f);
    PangoLayoutIter* it = pango_layout_get_iter(layout_);
    do {
      PangoLayoutLine* line = pango_layout_iter_get_line_readonly(it);
      PangoRectangle lr;
      pango_layout_iter_get_line_extents(it, nullptr, &lr);
      float y0 = tr.t - scrollY_ + px(lr.y), y1 = y0 + px(lr.height);
      if (y1 < rect.t || y0 > rect.b) continue;
      int ls = line->start_index, le = ls + line->length;
      int s = std::max(a, ls), e = std::min(b, le);
      bool pastEnd = b > le && a <= le;  // the selection runs on past this line
      if (s < e) {
        int* ranges = nullptr;
        int n = 0;
        pango_layout_line_get_x_ranges(line, s, e, &ranges, &n);
        for (int i = 0; i < n; ++i) {
          float x0 = tr.l + px(ranges[2 * i]), x1 = tr.l + px(ranges[2 * i + 1]);
          if (pastEnd && i == n - 1) x1 += 4;
          g.fill(RectF{x0, y0, x1, y1}, sel);
        }
        g_free(ranges);
      } else if (pastEnd) {
        int xpos = 0;
        pango_layout_line_index_to_x(line, le, FALSE, &xpos);
        g.fill(RectF{tr.l + px(xpos), y0, tr.l + px(xpos) + 5, y1}, sel);
      }
    } while (pango_layout_iter_next_line(it));
    pango_layout_iter_free(it);
  }
  setColor(cr, p.text);
  cairo_move_to(cr, tr.l, tr.t - scrollY_);
  pango_cairo_show_layout(cr, layout_);
  if (focused && caretOn && !readOnly) {
    PangoRectangle strong;
    pango_layout_get_cursor_pos(layout_, byteOf(caret_), &strong, nullptr);
    float x = std::round(tr.l + px(strong.x));
    float y = tr.t - scrollY_ + px(strong.y);
    g.fill(RectF{x, y, x + 1.5f, y + px(strong.height)}, p.text);
  }
  g.popClip();
  sb_.paint(g, p, -1, -1);
}

TextArea::Key TextArea::key(unsigned vk, bool ctrl, bool shift) {
  ensureLayout();
  const bool hasSel = caret_ != anchor_;
  const float lineH = std::max(8.0f, textHeight(font));
  float keepX = -1;
  auto vertical = [&](int lines) {
    int line = 0, xpos = 0;
    pango_layout_index_to_line_x(layout_, byteOf(caret_), FALSE, &line, &xpos);
    float want = preferredX_ >= 0 ? preferredX_ : px(xpos);
    int count = pango_layout_get_line_count(layout_);
    int target = std::clamp(line + lines, 0, std::max(0, count - 1));
    size_t pos;
    if (target == line && lines < 0) {
      pos = 0;
    } else if (target == line && lines > 0) {
      pos = text_.size();
    } else {
      PangoLayoutLine* l = pango_layout_get_line_readonly(layout_, target);
      int idx = 0, trailing = 0;
      pango_layout_line_x_to_index(l, static_cast<int>(want * PANGO_SCALE), &idx, &trailing);
      pos = std::min(posOf(idx) + static_cast<size_t>(trailing), text_.size());
      // Past the end of a wrapped line: stay on it rather than at the next line's start.
      size_t lineEnd = posOf(l->start_index + l->length);
      if (pos == lineEnd && target < count - 1 && pos > posOf(l->start_index) && text_[pos - 1] != L'\n')
        --pos;
    }
    setCaret(pos, shift);
    keepX = want;
  };
  auto lineBounds = [&](size_t& start, size_t& end) {
    int line = 0, xpos = 0;
    pango_layout_index_to_line_x(layout_, byteOf(caret_), FALSE, &line, &xpos);
    PangoLayoutLine* l = pango_layout_get_line_readonly(layout_, line);
    start = posOf(l->start_index);
    end = posOf(l->start_index + l->length);
    if (end > start && end < text_.size() && text_[end] != L'\n' && std::iswspace(text_[end - 1])) --end;
  };
  auto copy = [&] {
    if (hasSel) setClipboardText(nullptr, selectedText());
  };
  Key result = Key::NotHandled;
  switch (vk) {
    case VK_LEFT:
      if (!shift && hasSel && !ctrl) setCaret(std::min(caret_, anchor_), false);
      else setCaret(ctrl ? wordLeft(caret_) : (caret_ > 0 ? caret_ - 1 : 0), shift);
      result = Key::Handled;
      break;
    case VK_RIGHT:
      if (!shift && hasSel && !ctrl) setCaret(std::max(caret_, anchor_), false);
      else setCaret(ctrl ? wordRight(caret_) : std::min(caret_ + 1, text_.size()), shift);
      result = Key::Handled;
      break;
    case VK_UP:
      vertical(-1);
      result = Key::Handled;
      break;
    case VK_DOWN:
      vertical(1);
      result = Key::Handled;
      break;
    case VK_PRIOR:
    case VK_NEXT: {
      int lines = std::max(1, static_cast<int>(textRect().h() / lineH) - 1);
      vertical(vk == VK_PRIOR ? -lines : lines);
      result = Key::Handled;
      break;
    }
    case VK_HOME:
    case VK_END: {
      size_t s = 0, e = text_.size();
      if (!ctrl) lineBounds(s, e);
      setCaret(vk == VK_HOME ? s : e, shift);
      result = Key::Handled;
      break;
    }
    case VK_BACK:
      if (readOnly) return Key::Handled;
      if (!hasSel) {
        if (caret_ == 0) return Key::Handled;
        anchor_ = ctrl ? wordLeft(caret_) : caret_ - 1;
      }
      replaceSelection(L"", false);
      return Key::Changed;
    case VK_DELETE:
      if (readOnly) return Key::Handled;
      if (shift && !ctrl) {  // cut
        if (!hasSel) return Key::Handled;
        copy();
        replaceSelection(L"", false);
        return Key::Changed;
      }
      if (!hasSel) {
        if (caret_ >= text_.size()) return Key::Handled;
        anchor_ = ctrl ? wordRight(caret_) : caret_ + 1;
      }
      replaceSelection(L"", false);
      return Key::Changed;
    case VK_RETURN:
      if (readOnly || ctrl) return Key::NotHandled;
      replaceSelection(L"\n", false);
      return Key::Changed;
    case VK_INSERT:
      if (ctrl && !shift) {
        copy();
        return Key::Handled;
      }
      if (shift && !ctrl && !readOnly) {
        replaceSelection(getClipboardText(nullptr), false);
        return Key::Changed;
      }
      return Key::NotHandled;
    default:
      break;
  }
  if (result == Key::NotHandled && ctrl) {
    switch (vk) {
      case 'A':
        if (shift) break;
        selectAll();
        result = Key::Handled;
        break;
      case 'C':
        if (shift) break;
        copy();
        return Key::Handled;
      case 'X':
        if (shift || readOnly) break;
        if (!hasSel) return Key::Handled;
        copy();
        replaceSelection(L"", false);
        return Key::Changed;
      case 'V':
        if (shift || readOnly) break;
        replaceSelection(getClipboardText(nullptr), false);
        return Key::Changed;
      case 'Z':
      case 'Y': {
        if (readOnly) break;
        bool redo = vk == 'Y' || shift;
        auto& from = redo ? redo_ : undo_;
        auto& to = redo ? undo_ : redo_;
        if (from.empty()) return Key::Handled;
        to.push_back({text_, caret_});
        Snapshot s = from.back();
        from.pop_back();
        text_ = s.text;
        caret_ = anchor_ = std::min(s.caret, text_.size());
        lastTyping_ = false;
        dirty_ = true;
        revealCaret();
        return Key::Changed;
      }
      default:
        break;
    }
  }
  if (result != Key::NotHandled) {
    preferredX_ = keepX;
    lastTyping_ = false;
  }
  return result;
}

bool TextArea::typeChar(wchar_t c) {
  if (readOnly || c < 0x20 || c == 0x7F) return false;
  replaceSelection(std::wstring(1, c), true);
  if (c == L' ') lastTyping_ = false;
  return true;
}

void TextArea::press(float x, float y, bool shift, int clicks) {
  if (sb_.press(x, y)) {
    scrollY_ = sb_.pos;
    return;
  }
  size_t pos = indexAt(x, y);
  preferredX_ = -1;
  lastTyping_ = false;
  if (clicks == 2) {
    size_t b = pos, e = pos;
    while (b > 0 && isWordChar(text_[b - 1])) --b;
    while (e < text_.size() && isWordChar(text_[e])) ++e;
    anchor_ = b;
    caret_ = e;
  } else if (clicks >= 3) {
    size_t b = pos, e = pos;
    while (b > 0 && text_[b - 1] != L'\n') --b;
    while (e < text_.size() && text_[e] != L'\n') ++e;
    anchor_ = b;
    caret_ = e;
  } else {
    setCaret(pos, shift);
  }
  dragging_ = true;
}

void TextArea::dragTo(float x, float y) {
  if (sb_.dragging) {
    sb_.dragTo(x, y);
    scrollY_ = sb_.pos;
    return;
  }
  if (!dragging_) return;
  RectF tr = textRect();
  if (y < tr.t) scrollY_ = std::max(0.0f, scrollY_ - 6);
  if (y > tr.b) scrollY_ += 6;
  scrollY_ = std::clamp(scrollY_, 0.0f, std::max(0.0f, contentHeight() - tr.h()));
  caret_ = indexAt(x, std::clamp(y, tr.t, tr.b - 1));
}

void TextArea::wheel(float notches) {
  float lineH = std::max(8.0f, textHeight(font));
  scrollY_ = std::clamp(scrollY_ - notches * 3 * lineH, 0.0f, std::max(0.0f, contentHeight() - textRect().h()));
}

// ---------------------------------------------------------------------------------------
// FileList
// ---------------------------------------------------------------------------------------
void FileList::setRows(std::vector<Row> rows, const std::set<std::string>& keep) {
  rows_ = std::move(rows);
  sel_.assign(rows_.size(), 0);
  for (size_t i = 0; i < rows_.size(); ++i)
    if (keep.count(rows_[i].key)) sel_[i] = 1;
  int n = static_cast<int>(rows_.size());
  caret_ = n ? std::clamp(caret_, -1, n - 1) : -1;
  anchor_ = n ? std::clamp(anchor_, -1, n - 1) : -1;
  top_ = std::clamp(top_, 0, std::max(0, n - visibleRows()));
}

std::vector<int> FileList::selection() const {
  std::vector<int> out;
  for (size_t i = 0; i < sel_.size(); ++i)
    if (sel_[i]) out.push_back(static_cast<int>(i));
  return out;
}

bool FileList::anySelected() const { return std::find(sel_.begin(), sel_.end(), 1) != sel_.end(); }

void FileList::clearSelection() { std::fill(sel_.begin(), sel_.end(), 0); }

void FileList::selectOnly(int row) {
  clearSelection();
  if (row >= 0 && row < static_cast<int>(sel_.size())) sel_[static_cast<size_t>(row)] = 1;
}

int FileList::visibleRows() const { return std::max(1, static_cast<int>((rect.h() - 4) / kRowH)); }

int FileList::rowAt(float x, float y) const {
  if (!rect.contains(x, y)) return -1;
  int r = top_ + static_cast<int>((y - rect.t - 2) / kRowH);
  return r >= 0 && r < static_cast<int>(rows_.size()) ? r : -1;
}

void FileList::reveal(int row) {
  if (row < 0) return;
  int vis = visibleRows();
  if (row < top_) top_ = row;
  else if (row >= top_ + vis) top_ = row - vis + 1;
  top_ = std::clamp(top_, 0, std::max(0, static_cast<int>(rows_.size()) - vis));
}

void FileList::paint(Gfx& g, const Palette& p, const Fonts& f, bool focused, float mx, float my) {
  g.fillRounded(rect, 4, p.input);
  int vis = visibleRows();
  int n = static_cast<int>(rows_.size());
  top_ = std::clamp(top_, 0, std::max(0, n - vis));
  sb_.track = RectF{rect.r - kBar, rect.t + 2, rect.r - 1, rect.b - 2};
  sb_.total = static_cast<float>(n);
  sb_.page = static_cast<float>(vis);
  sb_.pos = static_cast<float>(top_);
  g.pushClip(rect);
  int hot = sb_.track.contains(mx, my) && sb_.visible() ? -1 : rowAt(mx, my);
  for (int r = top_; r < n && r <= top_ + vis; ++r) {
    float y = rect.t + 2 + static_cast<float>(r - top_) * kRowH;
    RectF row{rect.l + 2, y, rect.r - 2, y + kRowH};
    if (sel_[static_cast<size_t>(r)]) g.fillRounded(row, 3, focused ? p.select : p.selectDim);
    else if (r == hot) g.fillRounded(row, 3, p.hover);
    if (focused && r == caret_ && !sel_[static_cast<size_t>(r)]) g.strokeRounded(row, 3, p.select, 1);
    const Row& it = rows_[static_cast<size_t>(r)];
    drawText(g, std::wstring(1, it.status), RectF{row.l + 4, y, row.l + 26, y + kRowH}, statusColor(it.status, p), f.bold,
             Align::Center);
    drawText(g, it.text, RectF{row.l + 30, y, row.r - 8, y + kRowH}, p.text, f.ui);
  }
  g.popClip();
  sb_.paint(g, p, mx, my);
}

bool FileList::press(float x, float y, bool ctrl, bool shift, bool right) {
  if (!right && sb_.press(x, y)) {
    top_ = static_cast<int>(std::lround(sb_.pos));
    return false;
  }
  std::vector<char> before = sel_;
  int r = rowAt(x, y);
  if (r < 0) {
    if (!right && !ctrl && !shift) clearSelection();
  } else if (right) {
    if (!sel_[static_cast<size_t>(r)]) selectOnly(r);
    caret_ = anchor_ = r;
  } else if (ctrl) {
    sel_[static_cast<size_t>(r)] ^= 1;
    caret_ = anchor_ = r;
  } else if (shift && anchor_ >= 0) {
    clearSelection();
    for (int i = std::min(anchor_, r); i <= std::max(anchor_, r); ++i) sel_[static_cast<size_t>(i)] = 1;
    caret_ = r;
  } else {
    selectOnly(r);
    caret_ = anchor_ = r;
  }
  return sel_ != before;
}

bool FileList::key(unsigned vk, bool ctrl, bool shift, bool* changed) {
  *changed = false;
  int n = static_cast<int>(rows_.size());
  if (n == 0) return false;
  int target = caret_;
  switch (vk) {
    case VK_UP: target = caret_ < 0 ? 0 : caret_ - 1; break;
    case VK_DOWN: target = caret_ < 0 ? 0 : caret_ + 1; break;
    case VK_PRIOR: target = caret_ - visibleRows(); break;
    case VK_NEXT: target = caret_ + visibleRows(); break;
    case VK_HOME: target = 0; break;
    case VK_END: target = n - 1; break;
    case 'A':
      if (!ctrl) return false;
      {
        std::vector<char> before = sel_;
        std::fill(sel_.begin(), sel_.end(), 1);
        *changed = sel_ != before;
      }
      return true;
    default: return false;
  }
  target = std::clamp(target, 0, n - 1);
  std::vector<char> before = sel_;
  caret_ = target;
  if (shift && anchor_ >= 0) {
    clearSelection();
    for (int i = std::min(anchor_, target); i <= std::max(anchor_, target); ++i) sel_[static_cast<size_t>(i)] = 1;
  } else if (!ctrl) {
    selectOnly(target);
    anchor_ = target;
  }
  reveal(target);
  *changed = sel_ != before;
  return true;
}

void FileList::wheel(float notches) {
  int steps = wheelSteps(gWheelAcc, notches, 3);
  top_ = std::clamp(top_ - steps, 0, std::max(0, static_cast<int>(rows_.size()) - visibleRows()));
}

void FileList::dragTo(float x, float y) {
  if (!sb_.dragging) return;
  sb_.dragTo(x, y);
  top_ = static_cast<int>(std::lround(sb_.pos));
}

// ---------------------------------------------------------------------------------------
// SideTree
// ---------------------------------------------------------------------------------------
void SideTree::setNodes(std::vector<Node> nodes, std::map<std::wstring, bool>* collapsed, int selectNode) {
  nodes_ = std::move(nodes);
  collapsed_ = collapsed;
  selected_ = selectNode >= 0 && selectNode < static_cast<int>(nodes_.size()) ? selectNode : -1;
  rebuildVisible();
}

bool SideTree::expanded(int node) const {
  const Node& n = nodes_[static_cast<size_t>(node)];
  if (!n.expandable || !collapsed_) return true;
  auto it = collapsed_->find(n.collapseKey);
  return it == collapsed_->end() || !it->second;
}

void SideTree::rebuildVisible() {
  visible_.clear();
  for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
    bool shown = true;
    for (int p = nodes_[static_cast<size_t>(i)].parent; p >= 0 && shown; p = nodes_[static_cast<size_t>(p)].parent)
      shown = expanded(p);
    if (shown) visible_.push_back(i);
  }
  top_ = std::clamp(top_, 0, std::max(0, static_cast<int>(visible_.size()) - visibleRows()));
}

int SideTree::visibleRows() const { return std::max(1, static_cast<int>((rect.h() - 6) / kRowH)); }

int SideTree::nodeAt(float x, float y) const {
  if (!rect.contains(x, y)) return -1;
  int r = top_ + static_cast<int>((y - rect.t - 4) / kRowH);
  return r >= 0 && r < static_cast<int>(visible_.size()) ? visible_[static_cast<size_t>(r)] : -1;
}

void SideTree::reveal(int node) {
  auto it = std::find(visible_.begin(), visible_.end(), node);
  if (it == visible_.end()) return;
  int r = static_cast<int>(it - visible_.begin());
  int vis = visibleRows();
  if (r < top_) top_ = r;
  else if (r >= top_ + vis) top_ = r - vis + 1;
}

void SideTree::select(int node) {
  selected_ = node;
  reveal(node);
}

void SideTree::toggle(int node) {
  if (node < 0 || !nodes_[static_cast<size_t>(node)].expandable || !collapsed_) return;
  (*collapsed_)[nodes_[static_cast<size_t>(node)].collapseKey] = expanded(node);
  rebuildVisible();
  if (selected_ >= 0 && std::find(visible_.begin(), visible_.end(), selected_) == visible_.end()) selected_ = node;
}

void SideTree::paint(Gfx& g, const Palette& p, const Fonts& f, bool focused, float mx, float my) {
  g.fill(rect, p.side);
  int vis = visibleRows();
  int n = static_cast<int>(visible_.size());
  top_ = std::clamp(top_, 0, std::max(0, n - vis));
  sb_.track = RectF{rect.r - kBar, rect.t + 2, rect.r - 1, rect.b - 2};
  sb_.total = static_cast<float>(n);
  sb_.page = static_cast<float>(vis);
  sb_.pos = static_cast<float>(top_);
  int hot = sb_.visible() && sb_.track.contains(mx, my) ? -1 : nodeAt(mx, my);
  g.pushClip(rect);
  for (int r = top_; r < n && r <= top_ + vis; ++r) {
    int i = visible_[static_cast<size_t>(r)];
    const Node& node = nodes_[static_cast<size_t>(i)];
    float y = rect.t + 4 + static_cast<float>(r - top_) * kRowH;
    RectF row{rect.l + 4, y, rect.r - 4, y + kRowH};
    if (i == selected_) g.fillRounded(row, 4, focused ? p.select : p.selectDim);
    else if (i == hot) g.fillRounded(row, 4, p.hover);
    float x = rect.l + 10 + static_cast<float>(node.depth) * 14;
    if (node.expandable) {
      float cx = x + 4, cy = std::floor(y + kRowH / 2) + 0.5f;
      Color c = p.dim;
      if (expanded(i)) {
        D2D1_POINT_2F v[] = {D2D1::Point2F(cx - 3.5f, cy - 1.5f), D2D1::Point2F(cx, cy + 2),
                             D2D1::Point2F(cx + 3.5f, cy - 1.5f)};
        g.path(v, 3, c, 1.4f, false, false);
      } else {
        D2D1_POINT_2F v[] = {D2D1::Point2F(cx - 1.5f, cy - 3.5f), D2D1::Point2F(cx + 2, cy),
                             D2D1::Point2F(cx - 1.5f, cy + 3.5f)};
        g.path(v, 3, c, 1.4f, false, false);
      }
    }
    x += 14;
    bool header = node.depth == 0;
    drawText(g, node.text, RectF{x, y, row.r - 6, y + kRowH}, header ? p.dim : p.text,
             header ? f.caption : (node.bold ? f.bold : f.ui));
  }
  g.popClip();
  sb_.paint(g, p, mx, my);
}

bool SideTree::press(float x, float y, int clicks, bool* toggled) {
  *toggled = false;
  if (sb_.press(x, y)) {
    top_ = static_cast<int>(std::lround(sb_.pos));
    return false;
  }
  int node = nodeAt(x, y);
  if (node < 0) return false;
  const Node& n = nodes_[static_cast<size_t>(node)];
  float chevronR = rect.l + 10 + static_cast<float>(n.depth) * 14 + 12;
  bool changed = node != selected_;
  // A header row folds with a click anywhere; others with the chevron or a double-click.
  if (n.expandable && (x < chevronR || clicks == 2 || n.depth == 0)) {
    toggle(node);
    *toggled = true;
  }
  if (std::find(visible_.begin(), visible_.end(), node) != visible_.end()) selected_ = node;
  return changed;
}

bool SideTree::key(unsigned vk, bool* changed) {
  *changed = false;
  if (visible_.empty()) return false;
  auto it = std::find(visible_.begin(), visible_.end(), selected_);
  int r = it == visible_.end() ? -1 : static_cast<int>(it - visible_.begin());
  int n = static_cast<int>(visible_.size());
  int target = r;
  switch (vk) {
    case VK_UP: target = r < 0 ? 0 : r - 1; break;
    case VK_DOWN: target = r < 0 ? 0 : r + 1; break;
    case VK_PRIOR: target = r - visibleRows(); break;
    case VK_NEXT: target = r + visibleRows(); break;
    case VK_HOME: target = 0; break;
    case VK_END: target = n - 1; break;
    case VK_LEFT:
      if (selected_ < 0) return true;
      if (nodes_[static_cast<size_t>(selected_)].expandable && expanded(selected_)) {
        toggle(selected_);
        return true;
      }
      if (int parent = nodes_[static_cast<size_t>(selected_)].parent; parent >= 0) {
        select(parent);
        *changed = true;
      }
      return true;
    case VK_RIGHT:
      if (selected_ < 0) return true;
      if (nodes_[static_cast<size_t>(selected_)].expandable) {
        if (!expanded(selected_)) {
          toggle(selected_);
        } else if (selected_ + 1 < static_cast<int>(nodes_.size()) &&
                   nodes_[static_cast<size_t>(selected_ + 1)].parent == selected_) {
          select(selected_ + 1);
          *changed = true;
        }
      }
      return true;
    default: return false;
  }
  target = std::clamp(target, 0, n - 1);
  int node = visible_[static_cast<size_t>(target)];
  *changed = node != selected_;
  select(node);
  return true;
}

void SideTree::wheel(float notches) {
  int steps = wheelSteps(gWheelAcc, notches, 3);
  top_ = std::clamp(top_ - steps, 0, std::max(0, static_cast<int>(visible_.size()) - visibleRows()));
}

void SideTree::dragTo(float x, float y) {
  if (!sb_.dragging) return;
  sb_.dragTo(x, y);
  top_ = static_cast<int>(std::lround(sb_.pos));
}

// ---------------------------------------------------------------------------------------
// CommitListPane
// ---------------------------------------------------------------------------------------
void CommitListPane::setModel(const std::vector<Commit>* commits, const std::vector<GraphRow>* graph,
                              std::vector<int> rows, bool showGraph, bool workdirRow, const std::string& headHash) {
  commits_ = commits;
  graph_ = graph;
  rows_ = std::move(rows);
  showGraph_ = showGraph;
  workdir_ = workdirRow;
  headHash_ = headHash;
  if (selected_ != kNone && rowOfItem(selected_) < 0) selected_ = kNone;
  top_ = std::clamp(top_, 0, std::max(0, rowCount() - visibleRows()));
}

void CommitListPane::setWorkdirText(const std::wstring& text, bool dirty) {
  workdirText_ = text;
  workdirDirty_ = dirty;
}

void CommitListPane::setBanner(const std::wstring& text) { banner_ = text; }

int CommitListPane::itemOfRow(int row) const {
  if (row < 0) return kNone;
  if (workdir_) {
    if (row == 0) return kWorkdir;
    --row;
  }
  return row < static_cast<int>(rows_.size()) ? rows_[static_cast<size_t>(row)] : kNone;
}

int CommitListPane::rowOfItem(int item) const {
  if (item == kWorkdir) return workdir_ ? 0 : -1;
  for (size_t i = 0; i < rows_.size(); ++i)
    if (rows_[i] == item) return static_cast<int>(i) + (workdir_ ? 1 : 0);
  return -1;
}

int CommitListPane::visibleRows() const {
  return std::max(1, static_cast<int>((rect.h() - bannerHeight()) / kRowH));
}

void CommitListPane::scrollTo(int top) { top_ = std::clamp(top, 0, std::max(0, rowCount() - visibleRows())); }

void CommitListPane::ensureVisible(int row) {
  int vis = visibleRows();
  if (row < top_) scrollTo(row);
  else if (row >= top_ + vis) scrollTo(row - vis + 1);
}

void CommitListPane::selectRow(int row, bool notify) {
  if (rowCount() == 0) return;
  row = std::clamp(row, 0, rowCount() - 1);
  int item = itemOfRow(row);
  bool changed = item != selected_;
  selected_ = item;
  ensureVisible(row);
  if (changed && notify && onSelChange) onSelChange();
}

void CommitListPane::select(int item, bool notify) {
  int row = rowOfItem(item);
  if (row < 0) {
    selected_ = kNone;
    if (notify && onSelChange) onSelChange();
    return;
  }
  if (itemOfRow(row) == selected_ && notify) {
    // Re-selecting the same item still refreshes the dependent views.
    ensureVisible(row);
    if (onSelChange) onSelChange();
    return;
  }
  selectRow(row, notify);
}

int CommitListPane::itemAt(float x, float y) const {
  if (!rect.contains(x, y) || y < rect.t + bannerHeight()) return kNone;
  return itemOfRow(top_ + static_cast<int>((y - rect.t - bannerHeight()) / kRowH));
}

bool CommitListPane::press(float x, float y) {
  if (sb_.press(x, y)) {
    top_ = static_cast<int>(std::lround(sb_.pos));
    return true;
  }
  if (!rect.contains(x, y) || y < rect.t + bannerHeight()) return false;
  int row = top_ + static_cast<int>((y - rect.t - bannerHeight()) / kRowH);
  if (row < rowCount()) selectRow(row, true);
  return true;
}

bool CommitListPane::key(unsigned vk) {
  int row = rowOfItem(selected_);
  switch (vk) {
    case VK_UP: selectRow(row < 0 ? 0 : row - 1, true); return true;
    case VK_DOWN: selectRow(row < 0 ? 0 : row + 1, true); return true;
    case VK_PRIOR: selectRow(row - visibleRows(), true); return true;
    case VK_NEXT: selectRow(row + visibleRows(), true); return true;
    case VK_HOME: selectRow(0, true); return true;
    case VK_END: selectRow(rowCount() - 1, true); return true;
    default: return false;
  }
}

void CommitListPane::wheel(float notches) { scrollTo(top_ - wheelSteps(gWheelAcc, notches, 3)); }

void CommitListPane::dragTo(float x, float y) {
  if (!sb_.dragging) return;
  sb_.dragTo(x, y);
  scrollTo(static_cast<int>(std::lround(sb_.pos)));
}

void CommitListPane::paintGraph(Gfx& g, const Palette& p, const RectF& rowRc, int item) {
  if (!graph_ || item < 0 || item >= static_cast<int>(graph_->size())) return;
  const GraphRow& gr = (*graph_)[static_cast<size_t>(item)];
  auto X = [&](int lane) { return rowRc.l + 8 + static_cast<float>(std::min(lane, kMaxLanes)) * kLaneW + kLaneW / 2; };
  float yTop = rowRc.t, yBot = rowRc.b, yMid = (yTop + yBot) / 2;
  const float penW = 1.8f;
  for (const auto& s : gr.top) {
    D2D1_POINT_2F seg[] = {D2D1::Point2F(X(s.from), yTop), D2D1::Point2F(X(s.to), yMid)};
    g.path(seg, 2, laneColor(s.from), penW, false, false);
  }
  for (const auto& s : gr.bottom) {
    D2D1_POINT_2F seg[] = {D2D1::Point2F(X(s.from), yMid), D2D1::Point2F(X(s.to), yBot)};
    g.path(seg, 2, laneColor(s.to), penW, false, false);
  }
  const float r = 5;
  float cx = X(gr.col);
  if (gr.merge) {
    g.fillEllipse(cx, yMid, r, r, p.panel);
    strokeCircle(g, cx, yMid, r - 1, laneColor(gr.col), penW);
  } else {
    g.fillEllipse(cx, yMid, r, r, laneColor(gr.col));
  }
  if ((*commits_)[static_cast<size_t>(item)].hash == headHash_) strokeCircle(g, cx, yMid, r + 2.5f, p.text, 1.5f);
}

void CommitListPane::paint(Gfx& g, const Palette& p, const Fonts& f, bool focused) {
  g.fill(rect, p.panel);
  top_ = std::clamp(top_, 0, std::max(0, rowCount() - visibleRows()));
  g.pushClip(rect);
  float bannerH = bannerHeight();
  if (bannerH > 0) {
    RectF b{rect.l, rect.t, rect.r, rect.t + bannerH};
    g.fill(b, p.hunkBg);
    drawText(g, banner_, RectF{b.l + 10, b.t, b.r - 8, b.b}, p.hunkText, f.bold);
  }
  int laneCount = 1;
  if (showGraph_ && graph_) {
    int last = std::min(rowCount(), top_ + visibleRows() + 1);
    for (int r = top_; r < last; ++r) {
      int item = itemOfRow(r);
      if (item >= 0 && item < static_cast<int>(graph_->size()))
        laneCount = std::max(laneCount, (*graph_)[static_cast<size_t>(item)].width);
    }
    laneCount = std::min(laneCount, kMaxLanes + 1);
  }
  float graphW = showGraph_ ? 8 + static_cast<float>(laneCount) * kLaneW + 8 : 10;
  float w = rect.w();
  bool showAuthor = w > 520, showHash = w > 680;
  float dateW = 96, authorW = showAuthor ? 130.0f : 0.0f, hashW = showHash ? 70.0f : 0.0f;
  float rightCols = dateW + authorW + hashW + 8 + kBar;
  const int rows = rowCount();
  for (int r = top_; r < rows; ++r) {
    float y = rect.t + bannerH + static_cast<float>(r - top_) * kRowH;
    if (y > rect.b) break;
    RectF rowRc{rect.l, y, rect.r, y + kRowH};
    int item = itemOfRow(r);
    if (item == selected_) g.fill(rowRc, focused ? p.select : p.selectDim);
    if (item == kWorkdir) {
      float cx = rect.l + (showGraph_ ? 8 + kLaneW / 2 : 14);
      strokeCircle(g, cx, y + kRowH / 2, 4.5f, workdirDirty_ ? p.warn : p.dim, 2);
      drawText(g, workdirText_, RectF{rect.l + graphW, y, rect.r - 8 - kBar, y + kRowH}, workdirDirty_ ? p.text : p.dim,
               f.bold);
      continue;
    }
    if (item < 0 || !commits_) continue;
    const Commit& c = (*commits_)[static_cast<size_t>(item)];
    if (showGraph_) paintGraph(g, p, rowRc, item);
    float x = rect.l + graphW;
    float textRight = rect.r - rightCols;
    for (const auto& b : c.badges) {
      if (x > textRight - 40) break;
      std::wstring label = b.name;
      Color col = p.badgeLocal;
      switch (b.kind) {
        case RefBadge::Head: col = p.badgeHead; break;
        case RefBadge::Local: col = b.current ? p.badgeCurrent : p.badgeLocal; break;
        case RefBadge::Remote: col = p.badgeRemote; break;
        case RefBadge::Tag:
          col = p.badgeTag;
          label = L"⚑ " + label;
          break;
      }
      float bw = std::min(g.textWidth(label, f.caption) + 12, textRight - x);
      RectF br{x, y + 4, x + bw, y + kRowH - 4};
      g.fillRounded(br, 4, col);
      drawText(g, label, br.inset(3, 0), Color::hex(0xFFFFFF), f.caption, Align::Center);
      x += bw + 4;
    }
    drawText(g, c.subject, RectF{x + 2, y, textRight - 6, y + kRowH}, p.text, f.ui);
    float cx = rect.r - rightCols + 4;
    if (showAuthor) {
      drawText(g, c.author, RectF{cx, y, cx + authorW - 8, y + kRowH}, p.dim, f.ui);
      cx += authorW;
    }
    if (showHash) {
      drawText(g, Widen(c.hash.substr(0, 7)), RectF{cx, y, cx + hashW, y + kRowH}, p.dim, f.mono, Align::Left,
               Elide::None);
      cx += hashW;
    }
    drawText(g, RelativeTime(c.time), RectF{cx, y, rect.r - 8 - kBar, y + kRowH}, p.dim, f.ui, Align::Right);
  }
  if (rows == 0 || (rows == 1 && workdir_ && commits_ && commits_->empty())) {
    RectF t{rect.l, rect.t + bannerH + (workdir_ ? kRowH : 0), rect.r, rect.b};
    drawText(g, rows_.empty() && !workdir_ ? L"No matching commits" : L"No commits yet", t, p.dim, f.ui, Align::Center);
  }
  g.popClip();
  sb_.track = RectF{rect.r - kBar, rect.t + bannerH + 2, rect.r - 1, rect.b - 2};
  sb_.total = static_cast<float>(rowCount());
  sb_.page = static_cast<float>(visibleRows());
  sb_.pos = static_cast<float>(top_);
  sb_.paint(g, p, -1, -1);
}

// ---------------------------------------------------------------------------------------
// DiffPane
// ---------------------------------------------------------------------------------------
namespace {

std::wstring expandTabs(std::string_view s) {
  std::wstring w = Widen(s), out;
  out.reserve(w.size());
  for (wchar_t c : w) {
    if (c == L'\t') out.append(4 - out.size() % 4, L' ');
    else if (c != L'\r') out.push_back(c);
  }
  return out;
}

std::string pathFromDiffGit(const std::string& rest) {
  // "a/<path> b/<path>": when both halves are equal the split is unambiguous.
  if (rest.size() > 4 && (rest.size() % 2) == 1) {
    size_t half = rest.size() / 2;
    if (rest[half] == ' ' && StartsWith(rest, "a/") && rest.compare(half + 1, 2, "b/") == 0 &&
        rest.compare(2, half - 2, rest, half + 3, std::string::npos) == 0)
      return rest.substr(2, half - 2);
  }
  size_t b = rest.rfind(" b/");
  return b == std::string::npos ? rest : rest.substr(b + 3);
}

}  // namespace

void DiffPane::reset() {
  lines_.clear();
  rows_.clear();
  files_.clear();
  placeholder_.clear();
  maxChars_ = 0;
  anchor_ = cursor_ = -1;
}

void DiffPane::setDiff(const std::string& diff, bool keepScroll) {
  int oldTop = top_;
  float oldH = hscroll_;
  reset();
  diffMode_ = true;
  int oldNo = 0, newNo = 0, curFile = -1;
  bool inHunk = false;
  size_t pos = 0;
  while (pos < diff.size() && lines_.size() < kMaxDiffLines) {
    size_t nl = diff.find('\n', pos);
    if (nl == std::string::npos) nl = diff.size();
    Line L;
    L.raw = diff.substr(pos, nl - pos);
    pos = nl + 1;
    const std::string& raw = L.raw;
    if (StartsWith(raw, "diff --git ") || StartsWith(raw, "diff --cc ") || StartsWith(raw, "diff --combined ")) {
      FileInfo f;
      f.header = static_cast<int>(lines_.size());
      f.combined = !StartsWith(raw, "diff --git ");
      f.path = f.combined ? raw.substr(raw.find(' ', 7) + 1) : pathFromDiffGit(raw.substr(11));
      files_.push_back(f);
      curFile = static_cast<int>(files_.size()) - 1;
      L.kind = Kind::FileHeader;
      inHunk = false;
    } else if (StartsWith(raw, "@@")) {
      L.kind = Kind::Hunk;
      inHunk = true;
      // @@ -a,b +c,d @@  (combined diffs have several '-' ranges)
      size_t minus = raw.find(" -");
      size_t plus = raw.find(" +");
      oldNo = minus != std::string::npos ? atoi(raw.c_str() + minus + 2) : 0;
      newNo = plus != std::string::npos ? atoi(raw.c_str() + plus + 2) : 0;
    } else if (inHunk && curFile >= 0 && files_[static_cast<size_t>(curFile)].combined) {
      std::string prefix = raw.substr(0, std::min<size_t>(2, raw.size()));
      if (prefix.find('+') != std::string::npos) {
        L.kind = Kind::Add;
        L.newNo = newNo++;
      } else if (prefix.find('-') != std::string::npos) {
        L.kind = Kind::Del;
      } else {
        L.kind = Kind::Context;
        L.newNo = newNo++;
      }
    } else if (inHunk && (raw.empty() || raw[0] == ' ' || raw[0] == '+' || raw[0] == '-' || raw[0] == '\\')) {
      char c = raw.empty() ? ' ' : raw[0];
      if (c == '+') {
        L.kind = Kind::Add;
        L.newNo = newNo++;
      } else if (c == '-') {
        L.kind = Kind::Del;
        L.oldNo = oldNo++;
      } else if (c == '\\') {
        L.kind = Kind::NoNewline;
      } else {
        L.kind = Kind::Context;
        L.oldNo = oldNo++;
        L.newNo = newNo++;
      }
    } else {
      inHunk = false;
      L.kind = Kind::Meta;
      L.hidden = true;
      if (curFile >= 0) {
        FileInfo& f = files_[static_cast<size_t>(curFile)];
        if (StartsWith(raw, "+++ b/")) f.path = raw.substr(6);
        else if (StartsWith(raw, "rename to ")) f.path = raw.substr(10);
        else if (StartsWith(raw, "rename from ")) f.origPath = raw.substr(12);
        else if (StartsWith(raw, "copy from ")) f.origPath = raw.substr(10);
        else if (StartsWith(raw, "new file mode")) f.added = true;
        else if (StartsWith(raw, "deleted file mode")) f.deleted = true;
        else if (StartsWith(raw, "old mode ")) f.modeChange = Widen(raw.substr(9));
        else if (StartsWith(raw, "new mode ")) f.modeChange += L" → " + Widen(raw.substr(9));
        else if (StartsWith(raw, "Binary files") || StartsWith(raw, "GIT binary patch")) {
          f.binary = true;
          L.hidden = false;
        }
      } else {
        L.hidden = raw.empty();
      }
    }
    L.file = curFile;
    if (L.kind == Kind::Add || L.kind == Kind::Del || L.kind == Kind::Context) {
      size_t skip = (curFile >= 0 && files_[static_cast<size_t>(curFile)].combined) ? 2 : 1;
      L.text = expandTabs(std::string_view(raw).substr(std::min(skip, raw.size())));
    } else if (L.kind != Kind::FileHeader) {
      L.text = expandTabs(raw);
    }
    lines_.push_back(std::move(L));
  }
  finishParse();
  if (lines_.size() >= kMaxDiffLines) {
    Line L;
    L.kind = Kind::Meta;
    L.text = L"… diff truncated (too large to display)";
    lines_.push_back(L);
    rows_.push_back(static_cast<int>(lines_.size()) - 1);
  }
  if (rows_.empty()) placeholder_ = L"No changes";
  top_ = keepScroll ? oldTop : 0;
  hscroll_ = keepScroll ? oldH : 0;
  scrollTo(top_);
  scrollH(hscroll_);
}

void DiffPane::finishParse() {
  for (size_t i = 0; i < files_.size(); ++i) {
    FileInfo& f = files_[i];
    f.end = i + 1 < files_.size() ? files_[i + 1].header : static_cast<int>(lines_.size());
    std::wstring label = Widen(f.path);
    if (!f.origPath.empty() && f.origPath != f.path) label = Widen(f.origPath) + L"  →  " + label;
    std::wstring tags;
    if (f.added) tags += L"  [added]";
    if (f.deleted) tags += L"  [deleted]";
    if (f.binary) tags += L"  [binary]";
    if (f.combined) tags += L"  [conflict]";
    if (!f.modeChange.empty()) tags += L"  [mode " + f.modeChange + L"]";
    lines_[static_cast<size_t>(f.header)].text = label + tags;
  }
  for (size_t i = 0; i < lines_.size(); ++i) {
    if (lines_[i].hidden) continue;
    rows_.push_back(static_cast<int>(i));
    maxChars_ = std::max(maxChars_, lines_[i].text.size());
  }
}

void DiffPane::setText(const std::wstring& text) {
  reset();
  diffMode_ = false;
  top_ = 0;
  hscroll_ = 0;
  appendText(text);
}

void DiffPane::appendText(const std::wstring& text) {
  if (diffMode_) reset();
  diffMode_ = false;
  placeholder_.clear();
  const bool atEnd = top_ >= static_cast<int>(rows_.size()) - visibleRows();
  // One row per line; a final newline ends the last line rather than starting another.
  size_t pos = 0;
  while (pos < text.size() || (pos == 0 && text.empty())) {
    size_t nl = text.find(L'\n', pos);
    size_t end = nl == std::wstring::npos ? text.size() : nl;
    Line L;
    L.kind = Kind::Text;
    for (size_t i = pos; i < end; ++i) {
      wchar_t c = text[i];
      if (c == L'\t') L.text.append(4 - L.text.size() % 4, L' ');
      else if (c != L'\r') L.text.push_back(c);
    }
    maxChars_ = std::max(maxChars_, L.text.size());
    lines_.push_back(std::move(L));
    rows_.push_back(static_cast<int>(lines_.size()) - 1);
    if (nl == std::wstring::npos) break;
    pos = nl + 1;
  }
  if (console && lines_.size() > kMaxConsoleLines) {
    size_t drop = lines_.size() - kMaxConsoleLines / 2;
    lines_.erase(lines_.begin(), lines_.begin() + static_cast<long>(drop));
    rows_.resize(lines_.size());
    for (size_t i = 0; i < rows_.size(); ++i) rows_[i] = static_cast<int>(i);
    anchor_ = cursor_ = -1;
    top_ = std::max(0, top_ - static_cast<int>(drop));
  }
  if (console && atEnd) scrollToEnd();
}

void DiffPane::scrollToEnd() { scrollTo(static_cast<int>(rows_.size())); }

void DiffPane::setPlaceholder(const std::wstring& message) {
  reset();
  diffMode_ = false;
  placeholder_ = message;
  top_ = 0;
  hscroll_ = 0;
}

bool DiffPane::scrollToFile(const std::string& path) {
  for (const auto& f : files_) {
    if (f.path != path) continue;
    auto it = std::lower_bound(rows_.begin(), rows_.end(), f.header);
    if (it == rows_.end()) return false;
    int row = static_cast<int>(it - rows_.begin());
    scrollTo(row);
    anchor_ = cursor_ = row;
    return true;
  }
  return false;
}

bool DiffPane::cursorInHunk() const {
  if (!diffMode_ || cursor_ < 0 || cursor_ >= static_cast<int>(rows_.size())) return false;
  const Line& l = lines_[static_cast<size_t>(rows_[static_cast<size_t>(cursor_)])];
  if (l.file < 0 || files_[static_cast<size_t>(l.file)].combined || files_[static_cast<size_t>(l.file)].binary)
    return false;
  return l.kind != Kind::Text;
}

bool DiffPane::hasMultiLineSelection() const { return anchor_ >= 0 && cursor_ >= 0 && anchor_ != cursor_; }

std::string DiffPane::cursorFilePath() const {
  if (cursor_ < 0 || cursor_ >= static_cast<int>(rows_.size())) return {};
  int f = lines_[static_cast<size_t>(rows_[static_cast<size_t>(cursor_)])].file;
  return f >= 0 ? files_[static_cast<size_t>(f)].path : std::string();
}

std::wstring DiffPane::selectedText() const {
  if (cursor_ < 0) return {};
  int a = std::min(anchor_, cursor_), b = std::max(anchor_, cursor_);
  std::wstring out;
  for (int r = std::max(a, 0); r <= b && r < static_cast<int>(rows_.size()); ++r) {
    out += lines_[static_cast<size_t>(rows_[static_cast<size_t>(r)])].text;
    out += L"\n";
  }
  return out;
}

void DiffPane::selectionLines(int& a, int& b) const {
  int n = static_cast<int>(rows_.size()) - 1;
  int ra = std::min(anchor_, cursor_), rb = std::max(anchor_, cursor_);
  a = rows_[static_cast<size_t>(std::clamp(ra, 0, n))];
  b = rows_[static_cast<size_t>(std::clamp(rb, 0, n))];
}

void DiffPane::widenToHunks(int& a, int& b) const {
  auto hunkStart = [&](int i) {
    int f = lines_[static_cast<size_t>(i)].file;
    for (int k = i; k >= 0 && lines_[static_cast<size_t>(k)].file == f; --k) {
      if (lines_[static_cast<size_t>(k)].kind == Kind::Hunk) return k;
      if (lines_[static_cast<size_t>(k)].kind == Kind::FileHeader) return k;  // header: whole file
    }
    return i;
  };
  auto hunkEnd = [&](int i) {
    int f = lines_[static_cast<size_t>(i)].file;
    int k = i + 1;
    bool fromHeader = lines_[static_cast<size_t>(i)].kind == Kind::FileHeader;
    for (; k < static_cast<int>(lines_.size()) && lines_[static_cast<size_t>(k)].file == f; ++k)
      if (lines_[static_cast<size_t>(k)].kind == Kind::Hunk && !fromHeader) break;
    return k - 1;
  };
  int sa = hunkStart(a);
  a = sa;
  b = hunkEnd(std::max(b, sa));
}

std::string DiffPane::buildPatch(bool reverse, bool wholeHunks) const {
  if (!diffMode_ || cursor_ < 0 || rows_.empty()) return {};
  int a, b;
  selectionLines(a, b);
  if (wholeHunks) widenToHunks(a, b);
  std::string out;
  for (const FileInfo& f : files_) {
    if (f.end <= a || f.header > b || f.combined || f.binary) continue;
    int firstHunk = f.end;
    for (int i = f.header; i < f.end; ++i)
      if (lines_[static_cast<size_t>(i)].kind == Kind::Hunk) {
        firstHunk = i;
        break;
      }
    std::string header;
    for (int i = f.header; i < firstHunk; ++i) header += lines_[static_cast<size_t>(i)].raw + "\n";
    std::string body;
    for (int h = firstHunk; h < f.end;) {
      int e = h + 1;
      while (e < f.end && lines_[static_cast<size_t>(e)].kind != Kind::Hunk) ++e;
      if (e > a && h <= b) {
        std::string hb;
        int oldC = 0, newC = 0;
        bool any = false, lastKept = false;
        for (int i = h + 1; i < e; ++i) {
          const Line& L = lines_[static_cast<size_t>(i)];
          bool sel = i >= a && i <= b;
          switch (L.kind) {
            case Kind::Context:
              hb += L.raw + "\n";
              ++oldC;
              ++newC;
              lastKept = true;
              break;
            case Kind::Add:
              if (sel) {
                hb += L.raw + "\n";
                ++newC;
                any = lastKept = true;
              } else if (reverse) {
                hb += " " + L.raw.substr(1) + "\n";
                ++oldC;
                ++newC;
                lastKept = true;
              } else {
                lastKept = false;
              }
              break;
            case Kind::Del:
              if (sel) {
                hb += L.raw + "\n";
                ++oldC;
                any = lastKept = true;
              } else if (!reverse) {
                hb += " " + L.raw.substr(1) + "\n";
                ++oldC;
                ++newC;
                lastKept = true;
              } else {
                lastKept = false;
              }
              break;
            case Kind::NoNewline:
              if (lastKept) hb += L.raw + "\n";
              break;
            default:
              break;
          }
        }
        if (any) {
          const std::string& hr = lines_[static_cast<size_t>(h)].raw;
          size_t minus = hr.find(" -"), plus = hr.find(" +");
          int oldStart = minus != std::string::npos ? atoi(hr.c_str() + minus + 2) : 0;
          int newStart = plus != std::string::npos ? atoi(hr.c_str() + plus + 2) : 0;
          body += "@@ -" + std::to_string(oldStart) + "," + std::to_string(oldC) + " +" + std::to_string(newStart) +
                  "," + std::to_string(newC) + " @@\n" + hb;
        }
      }
      h = e;
    }
    if (!body.empty()) out += header + body;
  }
  return out;
}

int DiffPane::visibleRows() const {
  float h = rect.h() - (hsb_.visible() ? kBar : 0);
  return std::max(1, static_cast<int>(h / lineH_));
}

int DiffPane::rowAt(float y) const { return top_ + (y < rect.t ? -1 : static_cast<int>((y - rect.t) / lineH_)); }

float DiffPane::gutterWidth() const { return diffMode_ ? charW_ * 13 + 10 : 6; }

float DiffPane::contentWidth() const { return static_cast<float>(maxChars_) * charW_ + gutterWidth() + 20; }

void DiffPane::scrollTo(int top) {
  top_ = std::clamp(top, 0, std::max(0, static_cast<int>(rows_.size()) - visibleRows()));
}

void DiffPane::scrollH(float x) { hscroll_ = std::clamp(x, 0.0f, std::max(0.0f, contentWidth() - rect.w())); }

void DiffPane::ensureVisible(int row) {
  if (row < top_) scrollTo(row);
  else if (row >= top_ + visibleRows()) scrollTo(row - visibleRows() + 1);
}

void DiffPane::setCursor(int row, bool extend) {
  if (rows_.empty()) return;
  row = std::clamp(row, 0, static_cast<int>(rows_.size()) - 1);
  cursor_ = row;
  if (!extend || anchor_ < 0) anchor_ = row;
  ensureVisible(row);
}

bool DiffPane::press(float x, float y, bool shift, bool right) {
  if (!right && vsb_.press(x, y)) {
    top_ = static_cast<int>(std::lround(vsb_.pos));
    return true;
  }
  if (!right && hsb_.press(x, y)) {
    hscroll_ = hsb_.pos;
    return true;
  }
  if (!rect.contains(x, y)) return false;
  int row = rowAt(y);
  bool valid = row >= 0 && row < static_cast<int>(rows_.size());
  if (right) {
    int a = std::min(anchor_, cursor_), b = std::max(anchor_, cursor_);
    if (valid && (cursor_ < 0 || row < a || row > b)) setCursor(row, false);
    return true;
  }
  if (valid) {
    setCursor(row, shift);
    dragging_ = true;
  }
  return true;
}

void DiffPane::dragTo(float x, float y) {
  if (vsb_.dragging) {
    vsb_.dragTo(x, y);
    top_ = static_cast<int>(std::lround(vsb_.pos));
  } else if (hsb_.dragging) {
    hsb_.dragTo(x, y);
    hscroll_ = hsb_.pos;
  } else if (dragging_) {
    setCursor(rowAt(y), true);
  }
}

bool DiffPane::key(unsigned vk, bool ctrl, bool shift) {
  switch (vk) {
    case VK_UP: setCursor(cursor_ - 1, shift); return true;
    case VK_DOWN: setCursor(cursor_ + 1, shift); return true;
    case VK_PRIOR: setCursor(cursor_ - visibleRows(), shift); return true;
    case VK_NEXT: setCursor(cursor_ + visibleRows(), shift); return true;
    case VK_HOME: setCursor(0, shift); return true;
    case VK_END: setCursor(static_cast<int>(rows_.size()) - 1, shift); return true;
    case VK_LEFT: scrollH(hscroll_ - charW_ * 4); return true;
    case VK_RIGHT: scrollH(hscroll_ + charW_ * 4); return true;
    case 'C':
    case VK_INSERT:
      if (!ctrl) return false;
      setClipboardText(nullptr, selectedText());
      return true;
    case 'A':
      if (!ctrl || rows_.empty()) return false;
      anchor_ = 0;
      cursor_ = static_cast<int>(rows_.size()) - 1;
      return true;
    default: return false;
  }
}

void DiffPane::wheel(float notches, bool horizontal) {
  if (horizontal) scrollH(hscroll_ - notches * charW_ * 8);
  else scrollTo(top_ - wheelSteps(gWheelAcc, notches, 3));
}

void DiffPane::paint(Gfx& g, const Palette& p, const Fonts& f, bool focused) {
  charW_ = f.monoW;
  lineH_ = std::round(f.monoH + 3);
  g.fill(rect, p.panel);
  // Scroll bars first: the visible row count depends on the horizontal one.
  hsb_.track = RectF{rect.l + 2, rect.b - kBar, rect.r - kBar, rect.b - 1};
  hsb_.total = contentWidth();
  hsb_.page = rect.w();
  scrollTo(top_);
  scrollH(hscroll_);
  hsb_.pos = hscroll_;
  vsb_.track = RectF{rect.r - kBar, rect.t + 2, rect.r - 1, rect.b - (hsb_.visible() ? kBar : 2)};
  vsb_.total = static_cast<float>(rows_.size());
  vsb_.page = static_cast<float>(visibleRows());
  vsb_.pos = static_cast<float>(top_);
  if (rows_.empty()) {
    drawText(g, placeholder_, rect.inset(12, 0), p.dim, f.ui, Align::Center);
    return;
  }
  const float gutter = gutterWidth();
  const int selA = std::min(anchor_, cursor_), selB = std::max(anchor_, cursor_);
  const int last = std::min(static_cast<int>(rows_.size()), top_ + visibleRows() + 1);
  const float textY = std::floor((lineH_ - f.monoH) / 2);
  // Characters past the right edge are never drawn: very long lines stay cheap.
  const size_t maxDraw = static_cast<size_t>((hscroll_ + rect.w()) / std::max(1.0f, charW_)) + 64;
  g.pushClip(rect);
  for (int r = top_; r < last; ++r) {
    const Line& L = lines_[static_cast<size_t>(rows_[static_cast<size_t>(r)])];
    float y = rect.t + static_cast<float>(r - top_) * lineH_;
    RectF row{rect.l, y, rect.r, y + lineH_};
    RectF gut{rect.l, y, rect.l + gutter, y + lineH_};
    Color fg = p.text;
    bool selected = cursor_ >= 0 && r >= selA && r <= selB;
    if (selected) {
      g.fill(row, focused ? p.select : p.selectDim);
    } else {
      switch (L.kind) {
        case Kind::Add:
          g.fill(row, p.addBg);
          if (diffMode_) {
            g.fill(gut, p.panel);
            g.fill(gut, p.addGutter);
          }
          break;
        case Kind::Del:
          g.fill(row, p.delBg);
          if (diffMode_) {
            g.fill(gut, p.panel);
            g.fill(gut, p.delGutter);
          }
          break;
        case Kind::Hunk: g.fill(row, p.hunkBg); break;
        case Kind::FileHeader: g.fill(row, p.fileBg); break;
        default: break;
      }
    }
    if (L.kind == Kind::Hunk) fg = p.hunkText;
    if (L.kind == Kind::Meta || L.kind == Kind::NoNewline) fg = p.dim;
    if (L.kind == Kind::FileHeader) {
      drawText(g, L.text, RectF{rect.l + 10, y, rect.r - 6 - kBar, y + lineH_}, p.text, f.bold, Align::Left,
               Elide::Middle);
      g.fill(RectF{rect.l, y, rect.r, y + 1}, p.border);
      continue;
    }
    if (diffMode_ && (L.kind == Kind::Add || L.kind == Kind::Del || L.kind == Kind::Context)) {
      if (L.oldNo)
        drawText(g, std::to_wstring(L.oldNo), RectF{rect.l, y, rect.l + charW_ * 5 + 2, y + lineH_}, p.dim, f.mono,
                 Align::Right);
      if (L.newNo)
        drawText(g, std::to_wstring(L.newNo), RectF{rect.l + charW_ * 5 + 4, y, rect.l + charW_ * 10 + 6, y + lineH_},
                 p.dim, f.mono, Align::Right);
      if (L.kind != Kind::Context)
        drawRaw(g, L.kind == Kind::Add ? L"+" : L"−", rect.l + charW_ * 11 + 8, y + textY,
                L.kind == Kind::Add ? p.addText : p.delText, f.mono);
    }
    if (!L.text.empty()) {
      g.pushClip(RectF{rect.l + gutter, y, rect.r, y + lineH_});
      std::wstring_view t = L.text;
      if (t.size() > maxDraw) t = t.substr(0, maxDraw);
      drawRaw(g, t, rect.l + gutter + 4 - hscroll_, y + textY, fg, f.mono);
      g.popClip();
    }
  }
  g.popClip();
  vsb_.paint(g, p, -1, -1);
  hsb_.paint(g, p, -1, -1);
}

}  // namespace verso::gitui
