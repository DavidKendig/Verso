// Gfx on Linux: Cairo for shapes and images, Pango (with fontconfig) for text.
// Coordinates are DIPs, which on a scale-1 Wayland output are pixels, as on Windows at 96 DPI.
#include "app/Gfx.h"

#include <cairo.h>
#include <fontconfig/fontconfig.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

IDWriteTextFormat::~IDWriteTextFormat() {
  if (desc) pango_font_description_free(desc);
}

namespace verso {

namespace {

// UTF-8 for Pango, plus the byte offset of every character so wchar_t indices (what the
// shared code uses) map to Pango's byte indices and back.
std::string toUtf8(std::wstring_view s, std::vector<int>* offsets = nullptr) {
  std::string out;
  out.reserve(s.size());
  if (offsets) offsets->assign(s.size() + 1, 0);
  for (size_t i = 0; i < s.size(); ++i) {
    if (offsets) (*offsets)[i] = static_cast<int>(out.size());
    uint32_t c = static_cast<uint32_t>(s[i]);
    if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;  // a lone surrogate (UTF-16 leftovers)
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

std::string familyFor(const std::wstring& face) {
  std::wstring f = face;
  for (auto& c : f) c = static_cast<wchar_t>(std::towlower(c));
  // Windows faces the app names, mapped to what Linux has.
  if (f == L"segoe ui") return "Inter, Noto Sans, DejaVu Sans, sans-serif";
  if (f == L"consolas" || f == L"courier new") return "JetBrains Mono Slashed, monospace";
  if (f.empty()) return "sans-serif";
  return toUtf8(face) + ", monospace";
}

PangoContext* sharedContext() {
  static PangoContext* ctx = [] {
    PangoFontMap* map = pango_cairo_font_map_get_default();
    PangoContext* c = pango_font_map_create_context(map);
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

PangoLayout* L(void* p) { return static_cast<PangoLayout*>(p); }

void setColor(cairo_t* cr, Color c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }

void roundedPath(cairo_t* cr, double l, double t, double r, double b, double radius) {
  double w = r - l, h = b - t;
  radius = std::min({radius, w / 2, h / 2});
  if (radius <= 0) {
    cairo_rectangle(cr, l, t, w, h);
    return;
  }
  cairo_new_sub_path(cr);
  cairo_arc(cr, r - radius, t + radius, radius, -M_PI / 2, 0);
  cairo_arc(cr, r - radius, b - radius, radius, 0, M_PI / 2);
  cairo_arc(cr, l + radius, b - radius, radius, M_PI / 2, M_PI);
  cairo_arc(cr, l + radius, t + radius, radius, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

}  // namespace

Gfx::Gfx() = default;

Gfx::~Gfx() {
  if (measure_) g_object_unref(L(measure_));
  for (IDWriteTextFormat* f : fonts_) delete f;
}

bool Gfx::init() {
  measure_ = pango_layout_new(sharedContext());
  pango_layout_set_width(L(measure_), -1);
  ui_ = makeFormat(L"Segoe UI", 12.0f, 400, false);
  uiSmall_ = makeFormat(L"Segoe UI", 11.0f, 400, false);
  uiBold_ = makeFormat(L"Segoe UI", 12.0f, 600, false);
  uiLarge_ = makeFormat(L"Segoe UI", 14.0f, 400, false);
  icon_ = makeFormat(kEmbeddedMonoFamily, 7.5f, 700, false);
  return true;
}

IDWriteTextFormat* Gfx::makeFormat(const std::wstring& face, float sizeDip, int weight, bool italic) {
  for (IDWriteTextFormat* f : fonts_)
    if (f->face == face && f->sizeDip == sizeDip && f->weight == weight && f->italic == italic) return f;
  auto* f = new IDWriteTextFormat;
  f->face = face;
  f->sizeDip = sizeDip;
  f->weight = weight;
  f->italic = italic;
  f->desc = pango_font_description_new();
  pango_font_description_set_family(f->desc, familyFor(face).c_str());
  pango_font_description_set_absolute_size(f->desc, sizeDip * PANGO_SCALE);
  pango_font_description_set_weight(f->desc, static_cast<PangoWeight>(weight));
  pango_font_description_set_style(f->desc, italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
  fonts_.push_back(f);
  return f;
}

IDWriteTextFormat* Gfx::createFont(const std::wstring& face, float sizeDip, int weight, bool italic) {
  return makeFormat(face, sizeDip, weight, italic);
}

void Gfx::setTarget(cairo_t* cr, float width, float height) {
  cr_ = cr;
  width_ = width;
  height_ = height;
  clipDepth_ = 0;
}

void Gfx::resize() {}
void Gfx::setDpi(float dpi) { dpi_ = dpi; }

bool Gfx::beginDraw() { return cr_ != nullptr; }

void Gfx::endDraw() {
  while (clipDepth_ > 0) popClip();
}

void Gfx::clear(Color c) {
  cairo_save(cr_);
  cairo_set_operator(cr_, CAIRO_OPERATOR_SOURCE);
  setColor(cr_, c);
  cairo_paint(cr_);
  cairo_restore(cr_);
}

void Gfx::fill(const RectF& r, Color c) {
  if (r.empty()) return;
  setColor(cr_, c);
  cairo_rectangle(cr_, r.l, r.t, r.w(), r.h());
  cairo_fill(cr_);
}

void Gfx::fillRounded(const RectF& r, float radius, Color c) {
  if (r.empty()) return;
  setColor(cr_, c);
  roundedPath(cr_, r.l, r.t, r.r, r.b, radius);
  cairo_fill(cr_);
}

void Gfx::stroke(const RectF& r, Color c, float width) {
  float h = width / 2;
  setColor(cr_, c);
  cairo_set_line_width(cr_, width);
  cairo_rectangle(cr_, r.l + h, r.t + h, r.w() - width, r.h() - width);
  cairo_stroke(cr_);
}

void Gfx::strokeRounded(const RectF& r, float radius, Color c, float width) {
  float h = width / 2;
  setColor(cr_, c);
  cairo_set_line_width(cr_, width);
  roundedPath(cr_, r.l + h, r.t + h, r.r - h, r.b - h, radius);
  cairo_stroke(cr_);
}

void Gfx::line(float x1, float y1, float x2, float y2, Color c, float width) {
  setColor(cr_, c);
  cairo_set_line_width(cr_, width);
  cairo_set_line_cap(cr_, CAIRO_LINE_CAP_BUTT);
  cairo_move_to(cr_, x1, y1);
  cairo_line_to(cr_, x2, y2);
  cairo_stroke(cr_);
}

void Gfx::path(const D2D1_POINT_2F* pts, size_t count, Color c, float width, bool closed, bool filled) {
  if (count < 2) return;
  cairo_move_to(cr_, pts[0].x, pts[0].y);
  for (size_t i = 1; i < count; ++i) cairo_line_to(cr_, pts[i].x, pts[i].y);
  if (closed || filled) cairo_close_path(cr_);
  setColor(cr_, c);
  if (filled) {
    cairo_fill(cr_);
  } else {
    cairo_set_line_width(cr_, width);
    cairo_set_line_cap(cr_, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr_, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr_);
    cairo_set_line_cap(cr_, CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(cr_, CAIRO_LINE_JOIN_MITER);
  }
}

void Gfx::fillEllipse(float cx, float cy, float rx, float ry, Color c) {
  if (rx <= 0 || ry <= 0) return;
  cairo_save(cr_);
  cairo_translate(cr_, cx, cy);
  cairo_scale(cr_, rx, ry);
  cairo_arc(cr_, 0, 0, 1, 0, 2 * M_PI);
  cairo_restore(cr_);
  setColor(cr_, c);
  cairo_fill(cr_);
}

void Gfx::pushClip(const RectF& r) {
  // Direct2D's aliased axis-aligned clip: snap to whole pixels.
  double l = std::round(r.l), t = std::round(r.t), rr = std::round(r.r), b = std::round(r.b);
  cairo_save(cr_);
  cairo_rectangle(cr_, l, t, std::max(0.0, rr - l), std::max(0.0, b - t));
  cairo_clip(cr_);
  ++clipDepth_;
}

void Gfx::popClip() {
  if (clipDepth_ <= 0) return;
  cairo_restore(cr_);
  --clipDepth_;
}

void Gfx::setEditorFont(const std::wstring& face, float sizePt, float linePadding) {
  float sizeDip = sizePt * 96.0f / 72.0f;
  editor_[0] = makeFormat(face, sizeDip, 400, false);
  editor_[1] = makeFormat(face, sizeDip, 400, true);
  editor_[2] = makeFormat(face, sizeDip, 700, false);
  pango_layout_set_attributes(L(measure_), nullptr);
  pango_layout_set_font_description(L(measure_), editor_[0]->desc);
  pango_layout_set_text(L(measure_), "0000000000", -1);
  PangoRectangle logical;
  pango_layout_get_extents(L(measure_), nullptr, &logical);
  float w = static_cast<float>(logical.width) / PANGO_SCALE;
  charWidth_ = w > 0 ? w / 10.0f : sizeDip * 0.55f;
  float natural = logical.height > 0 ? static_cast<float>(logical.height) / PANGO_SCALE : sizeDip * 1.2f;
  lineHeight_ = std::round(natural + linePadding);
  textOffsetY_ = std::floor((lineHeight_ - natural) / 2.0f);
}

IDWriteTextFormat* Gfx::editorFormat(FontStyle s) const {
  IDWriteTextFormat* f = editor_[static_cast<int>(s)];
  return f ? f : editor_[0];
}

void Gfx::text(std::wstring_view s, float x, float y, Color c, IDWriteTextFormat* fmt, float maxW, float maxH) {
  if (s.empty() || !fmt || !cr_) return;
  PangoLayout* l = L(measure_);
  pango_layout_set_attributes(l, nullptr);
  pango_layout_set_font_description(l, fmt->desc);
  std::string u = toUtf8(s);
  pango_layout_set_text(l, u.data(), static_cast<int>(u.size()));
  setColor(cr_, c);
  cairo_move_to(cr_, x, y);
  pango_cairo_show_layout(cr_, l);
}

void Gfx::textIn(std::wstring_view s, const RectF& r, Color c, IDWriteTextFormat* fmt, bool center) {
  if (s.empty() || !fmt || r.empty()) return;
  float h = formatHeight(fmt);
  float y = r.t + (r.h() - h) / 2.0f;
  float x = r.l;
  if (center) x = r.l + (r.w() - textWidth(s, fmt)) / 2.0f;
  pushClip(r);
  text(s, x, y, c, fmt, r.r - x + 1000, h + 4);
  popClip();
}

void Gfx::textMarked(std::wstring_view s, const std::vector<size_t>& marks, float x, float y, Color base,
                     Color markColor, IDWriteTextFormat* fmt, float maxW) {
  if (s.empty() || !fmt || !cr_) return;
  std::vector<int> off;
  std::string u = toUtf8(s, &off);
  PangoAttrList* attrs = pango_attr_list_new();
  auto u16 = [](float v) { return static_cast<guint16>(std::clamp(v, 0.0f, 1.0f) * 65535.0f); };
  for (size_t m : marks) {
    if (m >= s.size()) continue;
    PangoAttribute* fg = pango_attr_foreground_new(u16(markColor.r), u16(markColor.g), u16(markColor.b));
    fg->start_index = off[m];
    fg->end_index = off[m + 1];
    pango_attr_list_insert(attrs, fg);
    PangoAttribute* wt = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
    wt->start_index = off[m];
    wt->end_index = off[m + 1];
    pango_attr_list_insert(attrs, wt);
  }
  PangoLayout* l = L(measure_);
  pango_layout_set_font_description(l, fmt->desc);
  pango_layout_set_text(l, u.data(), static_cast<int>(u.size()));
  pango_layout_set_attributes(l, attrs);
  pango_attr_list_unref(attrs);
  pushClip(RectF{x, y - 2, x + maxW, y + 1000});
  setColor(cr_, base);
  cairo_move_to(cr_, x, y);
  pango_cairo_show_layout(cr_, l);
  popClip();
  pango_layout_set_attributes(l, nullptr);
}

float Gfx::textWidth(std::wstring_view s, IDWriteTextFormat* fmt) {
  if (!fmt || s.empty()) return 0;
  PangoLayout* l = L(measure_);
  pango_layout_set_attributes(l, nullptr);
  pango_layout_set_font_description(l, fmt->desc);
  std::string u = toUtf8(s);
  pango_layout_set_text(l, u.data(), static_cast<int>(u.size()));
  PangoRectangle logical;
  pango_layout_get_extents(l, nullptr, &logical);
  return static_cast<float>(logical.width) / PANGO_SCALE;
}

float Gfx::formatHeight(IDWriteTextFormat* fmt) {
  if (!fmt) return 14;
  PangoLayout* l = L(measure_);
  pango_layout_set_attributes(l, nullptr);
  pango_layout_set_font_description(l, fmt->desc);
  pango_layout_set_text(l, "Ag", -1);
  PangoRectangle logical;
  pango_layout_get_extents(l, nullptr, &logical);
  return static_cast<float>(logical.height) / PANGO_SCALE;
}

size_t Gfx::hitTest(std::wstring_view s, IDWriteTextFormat* fmt, float x) {
  if (!fmt || s.empty() || x <= 0) return 0;
  std::vector<int> off;
  std::string u = toUtf8(s, &off);
  PangoLayout* l = L(measure_);
  pango_layout_set_attributes(l, nullptr);
  pango_layout_set_font_description(l, fmt->desc);
  pango_layout_set_text(l, u.data(), static_cast<int>(u.size()));
  int index = 0, trailing = 0;
  pango_layout_xy_to_index(l, static_cast<int>(x * PANGO_SCALE), 0, &index, &trailing);
  size_t ch = static_cast<size_t>(std::lower_bound(off.begin(), off.end(), index) - off.begin());
  ch = std::min(ch + (trailing > 0 ? 1 : 0), s.size());
  return ch;
}

// --- rich text (DWriteCompat.h) --------------------------------------------------------------

ComPtr<IDWriteTextLayout> Gfx::layout(std::wstring_view s, IDWriteTextFormat* fmt, float maxW, float maxH) {
  if (!fmt) return {};
  return ComPtr<IDWriteTextLayout>(new IDWriteTextLayout(s, fmt, maxW, maxH));
}

ComPtr<IDWriteTextFormat> Gfx::createFormat(const std::wstring& face, float sizeDip, DWRITE_FONT_WEIGHT w,
                                            DWRITE_FONT_STYLE s) {
  auto* f = new IDWriteTextFormat;
  f->face = face;
  f->sizeDip = sizeDip;
  f->weight = w;
  f->italic = s != DWRITE_FONT_STYLE_NORMAL;
  f->desc = pango_font_description_new();
  pango_font_description_set_family(f->desc, familyFor(face).c_str());
  pango_font_description_set_absolute_size(f->desc, sizeDip * PANGO_SCALE);
  pango_font_description_set_weight(f->desc, static_cast<PangoWeight>(w));
  pango_font_description_set_style(f->desc, f->italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
  return ComPtr<IDWriteTextFormat>(f);
}

ID2D1SolidColorBrush* Gfx::brush(Color c) {
  auto q = [](float v) { return static_cast<uint64_t>(std::clamp(v, 0.0f, 1.0f) * 65535.0f); };
  uint64_t key = (q(c.r) << 48) | (q(c.g) << 32) | (q(c.b) << 16) | q(c.a);
  auto& slot = brushes_[key];
  if (!slot) slot = std::make_unique<ID2D1SolidColorBrush>(ID2D1SolidColorBrush{c.r, c.g, c.b, c.a});
  return slot.get();
}

void Gfx::drawLayout(IDWriteTextLayout* layout, float x, float y, Color c) {
  if (!layout || !cr_) return;
  PangoLayout* l = layout->pango();
  setColor(cr_, c);
  cairo_move_to(cr_, x, y);
  pango_cairo_show_layout(cr_, l);
  // Inline images fill the boxes their shape attributes reserved.
  for (const auto& img : layout->images()) {
    if (!img.draw) continue;
    PangoRectangle pos;
    pango_layout_index_to_pos(l, img.byteStart, &pos);
    // The box's bottom sits on its line's baseline.
    int baseline = pos.y + pos.height;
    PangoLayoutIter* it = pango_layout_get_iter(l);
    do {
      PangoLayoutLine* line = pango_layout_iter_get_line_readonly(it);
      if (img.byteStart >= line->start_index && img.byteStart < line->start_index + std::max(1, line->length)) {
        baseline = pango_layout_iter_get_baseline(it);
        break;
      }
    } while (pango_layout_iter_next_line(it));
    pango_layout_iter_free(it);
    float ix = x + static_cast<float>(pos.x) / PANGO_SCALE;
    float iy = y + static_cast<float>(baseline) / PANGO_SCALE - img.h;
    if (pos.width < 0) ix += static_cast<float>(pos.width) / PANGO_SCALE;  // right-to-left run
    img.draw(ix, iy, img.w, img.h);
  }
}

}  // namespace verso

// --- IDWriteTextLayout over Pango --------------------------------------------------------------

namespace {
guint16 channel(float v) { return static_cast<guint16>(std::clamp(v, 0.0f, 1.0f) * 65535.0f); }
}  // namespace

IDWriteTextLayout::IDWriteTextLayout(std::wstring_view text, IDWriteTextFormat* fmt, float maxW, float) {
  std::string u = verso::toUtf8(text, &offsets_);
  layout_ = pango_layout_new(verso::sharedContext());
  pango_layout_set_font_description(layout_, fmt->desc);
  pango_layout_set_text(layout_, u.data(), static_cast<int>(u.size()));
  pango_layout_set_wrap(layout_, PANGO_WRAP_WORD_CHAR);
  // Wrapping formats break lines at maxW; others still need the width for alignment.
  pango_layout_set_width(layout_, fmt->wrap ? static_cast<int>(std::max(1.0f, maxW) * PANGO_SCALE) : -1);
  attrs_ = pango_attr_list_new();
  spacing_ = fmt->uniformSpacing;
}

IDWriteTextLayout::~IDWriteTextLayout() {
  if (attrs_) pango_attr_list_unref(attrs_);
  if (layout_) g_object_unref(layout_);
}

int IDWriteTextLayout::byteAt(UINT32 pos) const {
  if (offsets_.empty()) return 0;
  return offsets_[std::min<size_t>(pos, offsets_.size() - 1)];
}

UINT32 IDWriteTextLayout::posAt(int byte) const {
  return static_cast<UINT32>(std::lower_bound(offsets_.begin(), offsets_.end(), byte) - offsets_.begin());
}

void IDWriteTextLayout::addAttr(void* attr, DWRITE_TEXT_RANGE r) {
  auto* a = static_cast<PangoAttribute*>(attr);
  a->start_index = static_cast<guint>(byteAt(r.startPosition));
  a->end_index = static_cast<guint>(byteAt(r.startPosition + r.length));
  pango_attr_list_change(attrs_, a);  // later settings win over earlier ones, as in DirectWrite
  dirty_ = true;
}

HRESULT IDWriteTextLayout::SetFontWeight(DWRITE_FONT_WEIGHT w, DWRITE_TEXT_RANGE r) {
  addAttr(pango_attr_weight_new(static_cast<PangoWeight>(w)), r);
  return S_OK;
}

HRESULT IDWriteTextLayout::SetFontStyle(DWRITE_FONT_STYLE s, DWRITE_TEXT_RANGE r) {
  addAttr(pango_attr_style_new(s == DWRITE_FONT_STYLE_NORMAL ? PANGO_STYLE_NORMAL : PANGO_STYLE_ITALIC), r);
  return S_OK;
}

HRESULT IDWriteTextLayout::SetStrikethrough(BOOL on, DWRITE_TEXT_RANGE r) {
  addAttr(pango_attr_strikethrough_new(on ? TRUE : FALSE), r);
  return S_OK;
}

HRESULT IDWriteTextLayout::SetUnderline(BOOL on, DWRITE_TEXT_RANGE r) {
  addAttr(pango_attr_underline_new(on ? PANGO_UNDERLINE_SINGLE : PANGO_UNDERLINE_NONE), r);
  return S_OK;
}

HRESULT IDWriteTextLayout::SetFontFamilyName(const wchar_t* family, DWRITE_TEXT_RANGE r) {
  addAttr(pango_attr_family_new(verso::familyFor(family ? family : L"").c_str()), r);
  return S_OK;
}

HRESULT IDWriteTextLayout::SetFontSize(FLOAT size, DWRITE_TEXT_RANGE r) {
  addAttr(pango_attr_size_new_absolute(static_cast<int>(size * PANGO_SCALE)), r);
  return S_OK;
}

HRESULT IDWriteTextLayout::SetDrawingEffect(ID2D1SolidColorBrush* b, DWRITE_TEXT_RANGE r) {
  if (!b) return E_FAIL;
  addAttr(pango_attr_foreground_new(channel(b->r), channel(b->g), channel(b->b)), r);
  addAttr(pango_attr_foreground_alpha_new(channel(b->a)), r);
  return S_OK;
}

HRESULT IDWriteTextLayout::SetTextAlignment(DWRITE_TEXT_ALIGNMENT a) {
  pango_layout_set_alignment(layout_, a == DWRITE_TEXT_ALIGNMENT_CENTER     ? PANGO_ALIGN_CENTER
                                      : a == DWRITE_TEXT_ALIGNMENT_TRAILING ? PANGO_ALIGN_RIGHT
                                                                            : PANGO_ALIGN_LEFT);
  pango_layout_set_justify(layout_, a == DWRITE_TEXT_ALIGNMENT_JUSTIFIED);
  return S_OK;
}

HRESULT IDWriteTextLayout::SetLineSpacing(DWRITE_LINE_SPACING_METHOD method, FLOAT spacing, FLOAT) {
  spacing_ = method == DWRITE_LINE_SPACING_METHOD_UNIFORM ? spacing : 0;
  dirty_ = true;
  return S_OK;
}

HRESULT IDWriteTextLayout::SetMaxWidth(FLOAT w) {
  if (pango_layout_get_width(layout_) >= 0) pango_layout_set_width(layout_, static_cast<int>(std::max(1.0f, w) * PANGO_SCALE));
  return S_OK;
}

HRESULT IDWriteTextLayout::SetWordWrapping(DWRITE_WORD_WRAPPING w) {
  if (w == DWRITE_WORD_WRAPPING_NO_WRAP) pango_layout_set_width(layout_, -1);
  return S_OK;
}

HRESULT IDWriteTextLayout::SetInlineImage(DrawFn draw, FLOAT w, FLOAT h, DWRITE_TEXT_RANGE r) {
  if (r.length == 0) return E_FAIL;
  // The first character becomes the w x h box on the baseline; the rest of the range
  // takes no space, and no line break may split the range.
  PangoRectangle box{0, static_cast<int>(-h * PANGO_SCALE), static_cast<int>(w * PANGO_SCALE),
                     static_cast<int>(h * PANGO_SCALE)};
  PangoRectangle none{0, 0, 0, 0};
  addAttr(pango_attr_shape_new(&box, &box), {r.startPosition, 1});
  if (r.length > 1) addAttr(pango_attr_shape_new(&none, &none), {r.startPosition + 1, r.length - 1});
  addAttr(pango_attr_allow_breaks_new(FALSE), r);
  images_.push_back({byteAt(r.startPosition), w, h, std::move(draw)});
  return S_OK;
}

PangoLayout* IDWriteTextLayout::pango() {
  if (dirty_) {
    PangoAttrList* list = pango_attr_list_copy(attrs_);
    if (spacing_ > 0) {
      // DirectWrite's uniform spacing: every line is `spacing_` tall.
      PangoAttribute* lh = pango_attr_line_height_new_absolute(static_cast<int>(spacing_ * PANGO_SCALE));
      lh->start_index = 0;
      lh->end_index = PANGO_ATTR_INDEX_TO_TEXT_END;
      pango_attr_list_insert_before(list, lh);
    }
    pango_layout_set_attributes(layout_, list);
    pango_attr_list_unref(list);
    dirty_ = false;
  }
  return layout_;
}

HRESULT IDWriteTextLayout::GetMetrics(DWRITE_TEXT_METRICS* m) {
  PangoLayout* l = pango();
  PangoRectangle ink, logical;
  pango_layout_get_extents(l, &ink, &logical);
  *m = {};
  m->left = static_cast<float>(logical.x) / PANGO_SCALE;
  m->top = static_cast<float>(logical.y) / PANGO_SCALE;
  m->width = m->widthIncludingTrailingWhitespace = static_cast<float>(logical.width) / PANGO_SCALE;
  m->height = static_cast<float>(logical.height) / PANGO_SCALE;
  const int lw = pango_layout_get_width(l);
  m->layoutWidth = lw < 0 ? m->width : static_cast<float>(lw) / PANGO_SCALE;
  m->layoutHeight = m->height;
  m->lineCount = static_cast<UINT32>(pango_layout_get_line_count(l));
  return S_OK;
}

HRESULT IDWriteTextLayout::GetLineMetrics(DWRITE_LINE_METRICS* lines, UINT32 maxLines, UINT32* count) {
  PangoLayout* l = pango();
  const UINT32 n = static_cast<UINT32>(pango_layout_get_line_count(l));
  if (count) *count = n;
  if (!lines || maxLines < n) return lines ? E_FAIL : S_OK;
  PangoLayoutIter* it = pango_layout_get_iter(l);
  UINT32 i = 0;
  do {
    PangoLayoutLine* line = pango_layout_iter_get_line_readonly(it);
    PangoRectangle logical;
    pango_layout_iter_get_line_extents(it, nullptr, &logical);
    DWRITE_LINE_METRICS& lm = lines[i];
    lm = {};
    const int start = line->start_index, end = line->start_index + line->length;
    lm.length = posAt(end) - posAt(start);
    lm.height = static_cast<float>(logical.height) / PANGO_SCALE;
    lm.baseline = static_cast<float>(pango_layout_iter_get_baseline(it) - logical.y) / PANGO_SCALE;
  } while (++i < n && pango_layout_iter_next_line(it));
  pango_layout_iter_free(it);
  return S_OK;
}

HRESULT IDWriteTextLayout::HitTestTextRange(UINT32 pos, UINT32 len, FLOAT ox, FLOAT oy, DWRITE_HIT_TEST_METRICS* out,
                                            UINT32 maxCount, UINT32* count) {
  PangoLayout* l = pango();
  const int from = byteAt(pos), to = byteAt(pos + len);
  std::vector<DWRITE_HIT_TEST_METRICS> rects;
  PangoLayoutIter* it = pango_layout_get_iter(l);
  do {
    PangoLayoutLine* line = pango_layout_iter_get_line_readonly(it);
    const int ls = line->start_index, le = line->start_index + line->length;
    const int a = std::max(from, ls), b = std::min(to, le);
    if (a >= b) continue;
    PangoRectangle logical;
    pango_layout_iter_get_line_extents(it, nullptr, &logical);
    int* ranges = nullptr;
    int n = 0;
    pango_layout_line_get_x_ranges(line, a, b, &ranges, &n);
    for (int k = 0; k < n; ++k) {
      DWRITE_HIT_TEST_METRICS m{};
      m.textPosition = posAt(a);
      m.length = posAt(b) - posAt(a);
      m.left = ox + static_cast<float>(logical.x + ranges[2 * k]) / PANGO_SCALE;
      m.width = static_cast<float>(ranges[2 * k + 1] - ranges[2 * k]) / PANGO_SCALE;
      m.top = oy + static_cast<float>(logical.y) / PANGO_SCALE;
      m.height = static_cast<float>(logical.height) / PANGO_SCALE;
      m.isText = TRUE;
      rects.push_back(m);
    }
    g_free(ranges);
  } while (pango_layout_iter_next_line(it));
  pango_layout_iter_free(it);
  if (count) *count = static_cast<UINT32>(rects.size());
  if (!out) return S_OK;
  if (maxCount < rects.size()) return E_FAIL;
  std::copy(rects.begin(), rects.end(), out);
  return S_OK;
}

HRESULT IDWriteTextLayout::HitTestPoint(FLOAT x, FLOAT y, BOOL* trailing, BOOL* inside, DWRITE_HIT_TEST_METRICS* m) {
  PangoLayout* l = pango();
  int index = 0, trail = 0;
  gboolean in = pango_layout_xy_to_index(l, static_cast<int>(x * PANGO_SCALE), static_cast<int>(y * PANGO_SCALE),
                                         &index, &trail);
  if (inside) *inside = in ? TRUE : FALSE;
  if (trailing) *trailing = trail > 0 ? TRUE : FALSE;
  if (m) {
    *m = {};
    m->textPosition = posAt(index);
    // `trail` counts characters past the index (a whole grapheme on its trailing side).
    m->length = trail > 0 ? static_cast<UINT32>(trail) : 1;
    PangoRectangle r;
    pango_layout_index_to_pos(l, index, &r);
    m->left = static_cast<float>(r.x) / PANGO_SCALE;
    m->top = static_cast<float>(r.y) / PANGO_SCALE;
    m->width = static_cast<float>(r.width) / PANGO_SCALE;
    m->height = static_cast<float>(r.height) / PANGO_SCALE;
    m->isText = TRUE;
  }
  return S_OK;
}
