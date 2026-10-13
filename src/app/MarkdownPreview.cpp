#include "app/MarkdownPreview.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>

#include "app/MarkdownLinks.h"
#include "core/TextUtil.h"
#include "syntax/Highlighter.h"

namespace verso {

namespace {

constexpr float kPadX = 36.0f;
constexpr float kPadTop = 26.0f;
constexpr float kReadWidth = 820.0f;
constexpr float kBody = 15.0f;
constexpr float kCode = 13.5f;
constexpr float kListIndent = 26.0f;
constexpr float kQuoteIndent = 20.0f;

const float kHeadingSize[6] = {28, 22, 18.5f, 16.5f, 15, 14};

void setLineSpacing(IDWriteTextFormat* f, float size, float factor) {
  if (f) f->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * factor, size * factor * 0.8f);
}

const Language* codeLanguage(const std::wstring& tag) {
  if (tag.empty()) return nullptr;
  std::wstring t = toLower(tag);
  for (const auto& l : allLanguages())
    if (toLower(l.name) == t) return &l;
  static const std::pair<const wchar_t*, const wchar_t*> aliases[] = {
      {L"c++", L"cpp"},   {L"csharp", L"cs"},  {L"javascript", L"js"}, {L"typescript", L"ts"},
      {L"shell", L"sh"},  {L"console", L"sh"}, {L"golang", L"go"},     {L"rs", L"rs"},
      {L"yml", L"yml"},   {L"text", L"txt"},   {L"plaintext", L"txt"}, {L"objc", L"m"},
      {L"kotlin", L"kt"}, {L"ruby", L"rb"},    {L"perl", L"pl"},       {L"docker", L"dockerfile"}};
  for (const auto& [name, ext] : aliases)
    if (t == name) t = ext;
  const Language& l = detectLanguage(L"x." + t, L"");
  return l.mode == LexMode::Plain ? nullptr : &l;
}

// A link to a local Markdown note ("a.md", "../b.md#part"), as opposed to a URL,
// an in-page "#anchor" or another kind of file.
bool isLocalNoteLink(const std::wstring& url) {
  if (url.empty() || url[0] == L'#') return false;
  size_t colon = url.find(L':');
  if (colon != std::wstring::npos && colon > 1) return false;  // a URL scheme
  std::wstring path = url.substr(0, url.find_first_of(L"#?"));
  return path.size() > 3 && _wcsicmp(path.c_str() + path.size() - 3, L".md") == 0;
}

// An image placed in a DirectWrite layout. Direct2D calls Draw() while drawing the
// layout, at the object's top-left corner.
class ImageObject final : public IDWriteInlineObject {
 public:
  ImageObject(MarkdownImages* images, std::wstring key, float w, float h)
      : images_(images), key_(std::move(key)), w_(w), h_(h) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteInlineObject)) {
      *out = static_cast<IDWriteInlineObject*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }

  HRESULT STDMETHODCALLTYPE Draw(void*, IDWriteTextRenderer*, FLOAT x, FLOAT y, BOOL, BOOL, IUnknown*) override {
    images_->draw(key_, {x, y, x + w_, y + h_});
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* m) override {
    m->width = w_;
    m->height = h_;
    m->baseline = h_;  // the image sits on the text baseline
    m->supportsSideways = FALSE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* o) override {
    *o = {};
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION* before, DWRITE_BREAK_CONDITION* after) override {
    *before = *after = DWRITE_BREAK_CONDITION_NEUTRAL;
    return S_OK;
  }

 private:
  ~ImageObject() = default;
  std::atomic<ULONG> refs_{1};
  MarkdownImages* images_;
  std::wstring key_;
  float w_, h_;
};

}  // namespace

float MarkdownPreview::contentLeft(const RectF& area) const {
  return area.l + std::max(kPadX * zoom_, (area.w() - width_) / 2.0f);
}

float MarkdownPreview::maxScroll(const RectF& area) const {
  return std::max(0.0f, height_ + kPadTop * zoom_ * 2 - area.h());
}

void MarkdownPreview::update(Gfx& g, const Theme& theme, std::wstring_view text, uint64_t revision, float areaWidth,
                             const std::wstring& baseDir, const std::wstring& noteRoot, float zoom,
                             bool fullWidth) {
  if (revision == revision_ && std::fabs(areaWidth - areaWidth_) < 0.5f && zoom == zoom_ && theme.dark == dark_ &&
      fullWidth == fullWidth_ &&
      baseDir == baseDir_ && noteRoot == noteRoot_ && g.targetGeneration() == generation_ &&
      text.size() == text_.size() &&
      images_.revision() == imagesRevision_)
    return;
  imagesRevision_ = images_.revision();
  bool formatsStale = zoom != zoom_ || !body_;
  text_.assign(text);
  revision_ = revision;
  areaWidth_ = areaWidth;
  zoom_ = zoom;
  dark_ = theme.dark;
  baseDir_ = baseDir;
  noteRoot_ = noteRoot;
  generation_ = g.targetGeneration();
  fullWidth_ = fullWidth;
  width_ = fullWidth ? std::max(120.0f, areaWidth - 2 * kPadX * zoom)
                     : std::max(120.0f, std::min(areaWidth - 2 * kPadX * zoom, kReadWidth * zoom));
  if (formatsStale) {
    body_ = g.createFormat(L"Segoe UI", kBody * zoom, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL);
    setLineSpacing(body_.Get(), kBody * zoom, 1.6f);
    for (int i = 0; i < 6; ++i) {
      heading_[i] = g.createFormat(L"Segoe UI", kHeadingSize[i] * zoom, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                   DWRITE_FONT_STYLE_NORMAL);
      setLineSpacing(heading_[i].Get(), kHeadingSize[i] * zoom, 1.3f);
    }
    if (body_) body_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    for (auto& h : heading_)
      if (h) h->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
  }
  layoutAll(g, theme);
}

MarkdownPreview::TextBlock MarkdownPreview::makeText(Gfx& g, const Theme& theme, const md::Inline& in,
                                                     IDWriteTextFormat* fmt, float maxW, bool allBold,
                                                     DWRITE_TEXT_ALIGNMENT align) {
  TextBlock t;
  std::wstring_view s = in.text.empty() ? std::wstring_view(L" ") : std::wstring_view(in.text);
  t.layout = g.layout(s, fmt, std::max(10.0f, maxW), 100000);
  if (!t.layout) return t;
  IDWriteTextLayout* l = t.layout.Get();
  if (allBold) l->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, {0, static_cast<UINT32>(s.size())});
  l->SetTextAlignment(in.centered ? DWRITE_TEXT_ALIGNMENT_CENTER : align);
  const Color link = theme.token(TokenKind::Link);
  const Color code = theme.token(TokenKind::String);
  float lineH = 0;
  if (fmt) lineH = fmt->GetFontSize() * 1.6f;
  bool tallImage = false;
  for (const auto& sp : in.spans) {
    DWRITE_TEXT_RANGE r{sp.start, sp.len};
    if (sp.style & md::Image) {
      // Images draw in the text flow; until loaded (or when unavailable) the alt text shows.
      MarkdownImages::Info info = images_.request(sp.src, baseDir_, noteRoot_, (sp.style & md::WikiLink) != 0);
      bool ready = info.state == MarkdownImages::State::Ready;
      float w = 0, h = 0;
      if (sp.width > 0 && sp.height > 0) {
        w = sp.width;
        h = sp.height;
      } else if (ready && sp.width > 0) {
        w = sp.width;
        h = info.h * sp.width / info.w;
      } else if (ready && sp.height > 0) {
        h = sp.height;
        w = info.w * sp.height / info.h;
      } else if (ready) {
        w = info.w;
        h = info.h;
      }
      if (w > 0 && h > 0 && info.state != MarkdownImages::State::Failed) {
        w *= zoom_;
        h *= zoom_;
        if (w > maxW) {
          h *= maxW / w;
          w = maxW;
        }
        auto* obj = new ImageObject(&images_, info.key, std::max(1.0f, w), std::max(1.0f, h));
        l->SetInlineObject(obj, r);
        obj->Release();
        if (h > lineH * 0.8f) tallImage = true;
        continue;
      }
      l->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
      if (auto* b = g.brush(theme.overlayDetail)) l->SetDrawingEffect(b, r);
      if (sp.style & md::Link) l->SetUnderline(TRUE, r);
      continue;
    }
    if (sp.style & md::Bold) l->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
    if (sp.style & md::Italic) l->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
    if (sp.style & md::Strike) l->SetStrikethrough(TRUE, r);
    if (sp.style & md::Code) {
      l->SetFontFamilyName(kEmbeddedMonoFamily, r);
      if (g.embeddedCollection()) l->SetFontCollection(g.embeddedCollection(), r);
      l->SetFontSize(kCode * zoom_, r);
      if (auto* b = g.brush(code)) l->SetDrawingEffect(b, r);
    }
    if (sp.style & md::Link) {
      l->SetUnderline(TRUE, r);
      // A wiki link to a note that does not exist yet is dimmed, as in note apps.
      bool unresolved = false;
      if (sp.style & md::WikiLink) {
        WikiTarget target = parseWikiTarget(sp.url);
        unresolved = !target.page.empty() && !baseDir_.empty() &&
                     !resolveWikiLink(target.page, baseDir_, noteRoot_).exists;
      } else if (isLocalNoteLink(sp.url) && !baseDir_.empty()) {
        unresolved = !resolveNoteLink(sp.url.substr(0, sp.url.find(L'#')), baseDir_, noteRoot_).exists;
      }
      if (auto* b = g.brush(unresolved ? link.withAlpha(0.55f) : link)) l->SetDrawingEffect(b, r);
    }
  }
  // Uniform line spacing would clip tall images; let lines grow to fit them.
  if (tallImage) l->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_DEFAULT, 0, 0);
  DWRITE_TEXT_METRICS m{};
  l->GetMetrics(&m);
  t.w = m.widthIncludingTrailingWhitespace;
  t.h = m.height;
  UINT32 lineCount = 0;
  l->GetLineMetrics(nullptr, 0, &lineCount);
  if (lineCount > 0) {
    std::vector<DWRITE_LINE_METRICS> lines(lineCount);
    if (SUCCEEDED(l->GetLineMetrics(lines.data(), lineCount, &lineCount))) t.baseline = lines[0].baseline;
  }
  // Boxes for inline code backgrounds and clickable links.
  for (const auto& sp : in.spans) {
    if (!(sp.style & (md::Code | md::Link))) continue;
    UINT32 count = 0;
    l->HitTestTextRange(sp.start, sp.len, 0, 0, nullptr, 0, &count);
    if (!count) continue;
    std::vector<DWRITE_HIT_TEST_METRICS> hits(count);
    if (FAILED(l->HitTestTextRange(sp.start, sp.len, 0, 0, hits.data(), count, &count))) continue;
    for (UINT32 i = 0; i < count; ++i) {
      Box b{hits[i].left, hits[i].top, hits[i].left + hits[i].width, hits[i].top + hits[i].height};
      if (sp.style & md::Code) t.codeBoxes.push_back(b);
      if (sp.style & md::Link) t.links.push_back({b, {sp.url, (sp.style & md::WikiLink) != 0}});
    }
  }
  return t;
}

void MarkdownPreview::layoutCode(Gfx& g, const Theme& theme, Item& item, float maxW) {
  const std::wstring& code = item.block.code.empty() ? std::wstring(L" ") : item.block.code;
  auto fmt = g.createFormat(kEmbeddedMonoFamily, kCode * zoom_, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL);
  if (!fmt) return;
  setLineSpacing(fmt.Get(), kCode * zoom_, 1.5f);
  fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  const float pad = 14 * zoom_;
  item.text.layout = g.layout(code, fmt.Get(), maxW - 2 * pad, 100000);
  if (!item.text.layout) return;
  // Syntax highlighting with the editor's lexers when the fence names a language.
  if (const Language* lang = codeLanguage(item.block.lang)) {
    uint32_t state = 0;
    std::vector<Token> toks;
    size_t lineStart = 0;
    for (size_t i = 0; i <= code.size(); ++i) {
      if (i < code.size() && code[i] != L'\n') continue;
      toks.clear();
      tokenizeLine(*lang, std::wstring_view(code).substr(lineStart, i - lineStart), state, toks);
      for (const Token& tk : toks) {
        if (tk.kind == TokenKind::Text) continue;
        if (auto* b = g.brush(theme.token(tk.kind)))
          item.text.layout->SetDrawingEffect(b, {static_cast<UINT32>(lineStart + tk.start), tk.len});
        if (tk.kind == TokenKind::Comment && theme.italicComments)
          item.text.layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC,
                                         {static_cast<UINT32>(lineStart + tk.start), tk.len});
      }
      lineStart = i + 1;
    }
  }
  DWRITE_TEXT_METRICS m{};
  item.text.layout->GetMetrics(&m);
  item.text.w = m.widthIncludingTrailingWhitespace;
  item.text.h = m.height;
  item.h = item.text.h + 2 * pad;
}

void MarkdownPreview::layoutTable(Gfx& g, const Theme& theme, Item& item, float maxW) {
  const auto& rows = item.block.rows;
  if (rows.empty()) return;
  const size_t cols = item.block.align.size();
  const float padX = 12 * zoom_, padY = 7 * zoom_;
  // Natural column widths, then shrink to fit.
  std::vector<float> natural(cols, 40 * zoom_);
  for (size_t r = 0; r < rows.size(); ++r)
    for (size_t c = 0; c < cols && c < rows[r].size(); ++c) {
      TextBlock t = makeText(g, theme, rows[r][c], body_.Get(), 100000, r == 0);
      natural[c] = std::max(natural[c], t.w + 2 * padX + 1);
    }
  float total = 0;
  for (float w : natural) total += w;
  std::vector<float> widths = natural;
  if (total > maxW) {
    float scale = maxW / total;
    for (auto& w : widths) w = std::max(56 * zoom_, w * scale);
  }
  item.colX.assign(1, 0);
  for (float w : widths) item.colX.push_back(item.colX.back() + w);
  item.rowY.assign(1, 0);
  for (size_t r = 0; r < rows.size(); ++r) {
    float rowH = 0;
    size_t first = item.cells.size();
    for (size_t c = 0; c < cols; ++c) {
      Cell cell;
      md::Inline empty;
      const md::Inline& in = c < rows[r].size() ? rows[r][c] : empty;
      cell.align = item.block.align[c];
      // Aligned before measuring, so link and code boxes land where the text is drawn.
      cell.text = makeText(g, theme, in, body_.Get(), widths[c] - 2 * padX, r == 0,
                           cell.align == 1   ? DWRITE_TEXT_ALIGNMENT_CENTER
                           : cell.align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                             : DWRITE_TEXT_ALIGNMENT_LEADING);
      cell.x = item.colX[c] + padX;
      cell.w = widths[c] - 2 * padX;
      rowH = std::max(rowH, cell.text.h);
      item.cells.push_back(std::move(cell));
    }
    for (size_t i = first; i < item.cells.size(); ++i) item.cells[i].y = item.rowY.back() + padY;
    item.rowY.push_back(item.rowY.back() + rowH + 2 * padY);
  }
  item.w = item.colX.back();
  item.h = item.rowY.back();
}

void MarkdownPreview::layoutAll(Gfx& g, const Theme& theme) {
  items_.clear();
  const float z = zoom_;
  float y = 0;
  float prevAfter = 0;
  auto blocks = md::parse(text_);
  anchors_ = md::anchors(blocks);
  images_.beginLayout();
  for (size_t bi = 0; bi < blocks.size(); ++bi) {
    Item item;
    item.block = std::move(blocks[bi]);
    const md::Block& b = item.block;
    float quoteIndent = static_cast<float>(b.quote) * kQuoteIndent * z;
    float x = quoteIndent;
    float maxW = width_ - x;
    float before = 0, after = 12 * z;
    switch (b.kind) {
      case md::BlockKind::Heading: {
        int lv = std::clamp(b.level, 1, 6);
        item.text = makeText(g, theme, b.content, heading_[lv - 1].Get(), maxW);
        item.h = item.text.h + (lv <= 2 ? 10 * z : 0);
        before = (lv <= 2 ? 26 : 20) * z;
        after = (lv <= 2 ? 14 : 8) * z;
        break;
      }
      case md::BlockKind::Paragraph: {
        item.text = makeText(g, theme, b.content, body_.Get(), maxW);
        item.h = item.text.h;
        break;
      }
      case md::BlockKind::ListItem: {
        float indent = static_cast<float>(b.depth + 1) * kListIndent * z;
        x += indent;
        maxW -= indent;
        item.text = makeText(g, theme, b.content, body_.Get(), maxW);
        item.h = item.text.h;
        if (b.ordered)
          item.marker = std::to_wstring(b.number) + L".";
        else
          item.marker = b.depth == 0 ? L"•" : (b.depth == 1 ? L"◦" : L"▪");
        bool nextIsItem = bi + 1 < blocks.size() && blocks[bi + 1].kind == md::BlockKind::ListItem &&
                          blocks[bi + 1].quote == b.quote;
        after = nextIsItem ? 4 * z : 12 * z;
        if (bi > 0 && items_.back().block.kind == md::BlockKind::ListItem) before = 0;
        break;
      }
      case md::BlockKind::CodeBlock:
        layoutCode(g, theme, item, maxW);
        before = 4 * z;
        after = 16 * z;
        break;
      case md::BlockKind::Table:
        layoutTable(g, theme, item, maxW);
        before = 4 * z;
        after = 16 * z;
        break;
      case md::BlockKind::Rule:
        item.h = 2 * z;
        before = 14 * z;
        after = 18 * z;
        break;
    }
    if (items_.empty()) before = 0;
    y += std::max(before, prevAfter);
    item.x = x;
    item.y = y;
    if (item.w == 0) item.w = maxW;
    y += item.h;
    prevAfter = after;
    items_.push_back(std::move(item));
  }
  height_ = y;
  images_.endLayout();
}

void MarkdownPreview::paintText(Gfx& g, const Theme& theme, const TextBlock& t, float x, float y, Color base,
                                float mouseX, float mouseY) {
  for (const auto& b : t.codeBoxes)
    g.fillRounded({x + b.l - 2, y + b.t + 1, x + b.r + 2, y + b.b - 1}, 3, theme.inputBg);
  g.drawLayout(t.layout.Get(), x, y, base);
  for (const auto& lk : t.links) {
    if (mouseX >= x + lk.box.l && mouseX < x + lk.box.r && mouseY >= y + lk.box.t && mouseY < y + lk.box.b)
      g.fill({x + lk.box.l, y + lk.box.b - 2, x + lk.box.r, y + lk.box.b - 0.5f}, theme.token(TokenKind::Link));
  }
}

void MarkdownPreview::paint(Gfx& g, const Theme& theme, const RectF& area, float scrollY, float mouseX,
                            float mouseY) {
  g.fill(area, theme.bg);
  g.pushClip(area);
  images_.beginPaint(&g);
  const float left = contentLeft(area);
  const float top = area.t + kPadTop * zoom_ - scrollY;
  const Color text = theme.token(TokenKind::Text);
  const Color dim = theme.overlayDetail;
  const Color headingColor = theme.tabTextActive;
  for (const Item& it : items_) {
    float y = top + it.y;
    if (y + it.h < area.t - 40 || y > area.b + 40) continue;
    float x = left + it.x;
    const md::Block& b = it.block;
    // Blockquote bars.
    for (int q = 0; q < b.quote; ++q) {
      float bx = left + static_cast<float>(q) * kQuoteIndent * zoom_ + 2;
      g.fill({bx, y - 4, bx + 3, y + it.h + 4}, theme.gutterFg.withAlpha(0.8f));
    }
    Color base = b.quote > 0 ? dim : text;
    switch (b.kind) {
      case md::BlockKind::Heading:
        paintText(g, theme, it.text, x, y, b.quote > 0 ? dim : headingColor, mouseX, mouseY);
        if (b.level <= 2) g.fill({x, y + it.h - 1.5f, x + it.w, y + it.h - 0.5f}, theme.guide.withAlpha(0.35f));
        break;
      case md::BlockKind::Paragraph:
        paintText(g, theme, it.text, x, y, base, mouseX, mouseY);
        break;
      case md::BlockKind::ListItem: {
        float lineH = kBody * zoom_ * 1.6f;
        // Markers sit on the first line's baseline, which a tall inline image moves down.
        const float my = y + std::max(0.0f, it.text.baseline - lineH * 0.8f);
        if (b.task >= 0) {
          float s = 13 * zoom_;
          RectF box{x - 20 * zoom_, my + (lineH - s) / 2, x - 20 * zoom_ + s, my + (lineH + s) / 2};
          if (b.task == 1) {
            g.fillRounded(box, 3, theme.accent);
            D2D1_POINT_2F tick[] = {D2D1::Point2F(box.l + s * 0.24f, box.t + s * 0.52f),
                                    D2D1::Point2F(box.l + s * 0.43f, box.t + s * 0.72f),
                                    D2D1::Point2F(box.l + s * 0.78f, box.t + s * 0.3f)};
            g.path(tick, 3, theme.bg, 1.8f * zoom_, false, false);
          } else {
            g.strokeRounded(box, 3, dim, 1.3f);
          }
        } else if (b.ordered) {
          // Numbers are right-aligned against the item text.
          float w = g.textWidth(it.marker, body_.Get());
          g.text(it.marker, x - 8 * zoom_ - w, my, dim, body_.Get());
        } else {
          g.text(it.marker, x - 17 * zoom_, my, dim, body_.Get());
        }
        paintText(g, theme, it.text, x, y, b.task == 1 ? dim : base, mouseX, mouseY);
        break;
      }
      case md::BlockKind::CodeBlock: {
        RectF box{x, y, x + it.w, y + it.h};
        g.fillRounded(box, 6, theme.inputBg);
        g.strokeRounded(box, 6, theme.inputBorder, 1);
        g.pushClip(box.inset(8 * zoom_, 0));
        g.drawLayout(it.text.layout.Get(), x + 14 * zoom_, y + 14 * zoom_, text);
        g.popClip();
        if (!b.lang.empty()) {
          float w = g.textWidth(b.lang, g.uiSmallFormat()) + 12;
          g.textIn(b.lang, {box.r - w - 4, box.t + 3, box.r - 4, box.t + 20}, dim, g.uiSmallFormat(), true);
        }
        break;
      }
      case md::BlockKind::Table: {
        RectF box{x, y, x + it.w, y + it.h};
        if (it.rowY.size() > 1) g.fill({x, y, x + it.w, y + it.rowY[1]}, theme.inputBg);
        for (size_t r = 1; r + 1 < it.rowY.size(); ++r)
          g.line(x, y + it.rowY[r], x + it.w, y + it.rowY[r], theme.inputBorder, 1);
        for (size_t c = 1; c + 1 < it.colX.size(); ++c)
          g.line(x + it.colX[c], y, x + it.colX[c], y + it.h, theme.inputBorder, 1);
        g.strokeRounded(box, 4, theme.inputBorder, 1);
        for (const Cell& cell : it.cells) {
          if (cell.text.layout) cell.text.layout->SetMaxWidth(cell.w);
          paintText(g, theme, cell.text, x + cell.x, y + cell.y, base, mouseX, mouseY);
        }
        break;
      }
      case md::BlockKind::Rule:
        g.fill({x, y, x + it.w, y + it.h}, theme.guide.withAlpha(0.5f));
        break;
    }
  }
  // Scroll indicator.
  float maxS = maxScroll(area);
  if (maxS > 0) {
    float viewH = area.h();
    float thumbH = std::max(28.0f, viewH * viewH / (viewH + maxS));
    float ty = area.t + (viewH - thumbH) * std::clamp(scrollY / maxS, 0.0f, 1.0f);
    g.fillRounded({area.r - 9, ty + 2, area.r - 3, ty + thumbH - 2}, 3, theme.scrollThumb);
  }
  if (items_.empty()) {
    bool blank = std::all_of(text_.begin(), text_.end(), [](wchar_t c) { return std::iswspace(c) != 0; });
    g.textIn(blank ? L"This note is empty. Choose Write or Split in the status bar to add to it."
                   : L"Nothing to show.",
             {left, area.t + 30, area.r, area.t + 60}, dim, g.uiFormat());
  }
  images_.endPaint();
  g.popClip();
}

MarkdownPreview::Link MarkdownPreview::linkAt(float x, float y, const RectF& area, float scrollY) const {
  const float left = contentLeft(area);
  const float top = area.t + kPadTop * zoom_ - scrollY;
  auto hit = [&](const TextBlock& t, float ox, float oy) -> const Link* {
    for (const auto& lk : t.links)
      if (x >= ox + lk.box.l && x < ox + lk.box.r && y >= oy + lk.box.t && y < oy + lk.box.b) return &lk.link;
    return nullptr;
  };
  for (const Item& it : items_) {
    float iy = top + it.y;
    if (y < iy - 2 || y > iy + it.h + 2) continue;
    float ix = left + it.x;
    if (const Link* l = hit(it.text, ix, iy)) return *l;
    for (const Cell& c : it.cells)
      if (const Link* l = hit(c.text, ix + c.x, iy + c.y)) return *l;
  }
  return {};
}

float MarkdownPreview::yForLine(double line) const {
  for (const Item& it : items_) {
    double first = static_cast<double>(it.block.line), last = static_cast<double>(it.block.lastLine) + 1;
    if (line < first) return it.y;
    if (line < last) return it.y + static_cast<float>((line - first) / (last - first)) * it.h;
  }
  return height_;
}

double MarkdownPreview::lineAtY(float y) const {
  for (const Item& it : items_) {
    if (y < it.y) return static_cast<double>(it.block.line);
    if (y < it.y + it.h) {
      double first = static_cast<double>(it.block.line), last = static_cast<double>(it.block.lastLine) + 1;
      return first + (last - first) * (y - it.y) / std::max(1.0f, it.h);
    }
  }
  return items_.empty() ? 0.0 : static_cast<double>(items_.back().block.lastLine + 1);
}

float MarkdownPreview::scrollForLine(double line) const {
  return std::max(0.0f, yForLine(line) + kPadTop * zoom_ - 8 * zoom_);
}

double MarkdownPreview::lineForScroll(float scroll) const {
  return lineAtY(scroll - kPadTop * zoom_ + 8 * zoom_);
}

float MarkdownPreview::yForAnchor(const std::wstring& anchor) const {
  std::wstring want = anchor;
  for (auto& c : want) c = static_cast<wchar_t>(std::towlower(c));
  size_t heading = 0;
  for (const Item& it : items_) {
    if (it.block.kind != md::BlockKind::Heading) continue;
    if (heading < anchors_.size() && anchors_[heading] == want) return it.y;
    ++heading;
  }
  return -1;
}

}  // namespace verso
