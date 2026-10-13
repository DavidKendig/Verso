#include "app/Gfx.h"

#include <dwrite_3.h>

#include <algorithm>
#include <cmath>

namespace verso {

bool Gfx::init(HWND hwnd) {
  hwnd_ = hwnd;
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf()))) return false;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
    return false;
  dpi_ = static_cast<float>(GetDpiForWindow(hwnd));
  loadEmbeddedFonts();
  ui_ = makeFormat(L"Segoe UI", 12.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL);
  uiSmall_ = makeFormat(L"Segoe UI", 11.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL);
  uiBold_ = makeFormat(L"Segoe UI", 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL);
  uiLarge_ = makeFormat(L"Segoe UI", 14.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL);
  icon_ = makeFormat(kEmbeddedMonoFamily, 7.5f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL);
  D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
      D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
  factory_->CreateStrokeStyle(sp, nullptr, 0, roundStroke_.GetAddressOf());
  return true;
}

ComPtr<IDWriteTextFormat> Gfx::makeFormat(const std::wstring& face, float sizeDip, DWRITE_FONT_WEIGHT w,
                                          DWRITE_FONT_STYLE s) {
  ComPtr<IDWriteTextFormat> f;
  IDWriteFontCollection* collection =
      embedded_ && _wcsicmp(face.c_str(), kEmbeddedMonoFamily) == 0 ? embedded_.Get() : nullptr;
  dwrite_->CreateTextFormat(face.c_str(), collection, w, s, DWRITE_FONT_STRETCH_NORMAL, sizeDip, L"en-us",
                            f.GetAddressOf());
  if (f) {
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  }
  return f;
}

void Gfx::loadEmbeddedFonts() {
  // The fonts live in the executable's resources (IDs 201-204). DirectWrite gets a
  // private collection; GDI (git mode) gets process-private copies of the same files.
  HMODULE module = GetModuleHandleW(nullptr);
  ComPtr<IDWriteFactory5> f5;
  ComPtr<IDWriteInMemoryFontFileLoader> loader;
  ComPtr<IDWriteFontSetBuilder1> builder;
  if (SUCCEEDED(dwrite_.As(&f5)) && SUCCEEDED(f5->CreateInMemoryFontFileLoader(loader.GetAddressOf())) &&
      SUCCEEDED(f5->RegisterFontFileLoader(loader.Get())))
    f5->CreateFontSetBuilder(builder.GetAddressOf());
  bool any = false;
  for (int id = 201; id <= 204; ++id) {
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!res) continue;
    HGLOBAL mem = LoadResource(module, res);
    void* data = mem ? LockResource(mem) : nullptr;
    DWORD size = SizeofResource(module, res);
    if (!data || !size) continue;
    DWORD installed = 0;
    AddFontMemResourceEx(data, size, nullptr, &installed);
    if (builder) {
      ComPtr<IDWriteFontFile> file;
      // The resource stays mapped for the life of the process, so DirectWrite can read it in place.
      if (SUCCEEDED(loader->CreateInMemoryFontFileReference(f5.Get(), data, size, nullptr, file.GetAddressOf())) &&
          SUCCEEDED(builder->AddFontFile(file.Get())))
        any = true;
    }
  }
  if (!any) return;
  ComPtr<IDWriteFontSet> set;
  if (SUCCEEDED(builder->CreateFontSet(set.GetAddressOf()))) {
    ComPtr<IDWriteFontCollection1> collection;
    if (SUCCEEDED(f5->CreateFontCollectionFromFontSet(set.Get(), collection.GetAddressOf()))) embedded_ = collection;
  }
}

void Gfx::createTarget() {
  if (rt_) return;
  RECT rc;
  GetClientRect(hwnd_, &rc);
  D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(1, rc.right - rc.left)),
                                 static_cast<UINT32>(std::max<LONG>(1, rc.bottom - rc.top)));
  D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
  props.dpiX = props.dpiY = dpi_;
  factory_->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, size), rt_.GetAddressOf());
  ++generation_;
  brushes_.clear();
  if (rt_) {
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), brush_.ReleaseAndGetAddressOf());
  }
}

void Gfx::resize() {
  if (!rt_) return;
  RECT rc;
  GetClientRect(hwnd_, &rc);
  rt_->Resize(D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(1, rc.right)), static_cast<UINT32>(std::max<LONG>(1, rc.bottom))));
}

void Gfx::setDpi(float dpi) {
  dpi_ = dpi;
  if (rt_) rt_->SetDpi(dpi, dpi);
}

bool Gfx::beginDraw() {
  createTarget();
  if (!rt_) return false;
  rt_->BeginDraw();
  rt_->SetTransform(D2D1::Matrix3x2F::Identity());
  return true;
}

void Gfx::endDraw() {
  if (!rt_) return;
  HRESULT hr = rt_->EndDraw();
  if (hr == D2DERR_RECREATE_TARGET) {
    brushes_.clear();
    brush_.Reset();
    rt_.Reset();
  }
}

void Gfx::clear(Color c) { rt_->Clear(c.d2d()); }

void Gfx::fill(const RectF& r, Color c) {
  if (r.empty()) return;
  brush_->SetColor(c.d2d());
  rt_->FillRectangle(D2D1::RectF(r.l, r.t, r.r, r.b), brush_.Get());
}

void Gfx::fillRounded(const RectF& r, float radius, Color c) {
  if (r.empty()) return;
  brush_->SetColor(c.d2d());
  rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.l, r.t, r.r, r.b), radius, radius), brush_.Get());
}

void Gfx::stroke(const RectF& r, Color c, float width) {
  brush_->SetColor(c.d2d());
  float h = width / 2;
  rt_->DrawRectangle(D2D1::RectF(r.l + h, r.t + h, r.r - h, r.b - h), brush_.Get(), width);
}

void Gfx::strokeRounded(const RectF& r, float radius, Color c, float width) {
  brush_->SetColor(c.d2d());
  float h = width / 2;
  rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.l + h, r.t + h, r.r - h, r.b - h), radius, radius),
                            brush_.Get(), width);
}

void Gfx::line(float x1, float y1, float x2, float y2, Color c, float width) {
  brush_->SetColor(c.d2d());
  rt_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush_.Get(), width);
}

void Gfx::path(const D2D1_POINT_2F* pts, size_t count, Color c, float width, bool closed, bool filled) {
  if (count < 2) return;
  ComPtr<ID2D1PathGeometry> geo;
  if (FAILED(factory_->CreatePathGeometry(geo.GetAddressOf()))) return;
  ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(geo->Open(sink.GetAddressOf()))) return;
  sink->BeginFigure(pts[0], filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddLines(pts + 1, static_cast<UINT32>(count - 1));
  sink->EndFigure(closed || filled ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
  sink->Close();
  brush_->SetColor(c.d2d());
  if (filled)
    rt_->FillGeometry(geo.Get(), brush_.Get());
  else
    rt_->DrawGeometry(geo.Get(), brush_.Get(), width, roundStroke_.Get());
}

ID2D1SolidColorBrush* Gfx::brush(Color c) {
  auto q = [](float v) { return static_cast<uint64_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
  uint64_t key = (q(c.r) << 24) | (q(c.g) << 16) | (q(c.b) << 8) | q(c.a);
  auto it = brushes_.find(key);
  if (it != brushes_.end()) return it->second.Get();
  ComPtr<ID2D1SolidColorBrush> b;
  if (rt_) rt_->CreateSolidColorBrush(c.d2d(), b.GetAddressOf());
  ID2D1SolidColorBrush* raw = b.Get();
  if (b) brushes_[key] = std::move(b);
  return raw;
}

void Gfx::drawLayout(IDWriteTextLayout* layout, float x, float y, Color c) {
  if (!layout) return;
  brush_->SetColor(c.d2d());
  rt_->DrawTextLayout(D2D1::Point2F(x, y), layout, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Gfx::fillEllipse(float cx, float cy, float rx, float ry, Color c) {
  brush_->SetColor(c.d2d());
  rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), brush_.Get());
}

void Gfx::pushClip(const RectF& r) {
  rt_->PushAxisAlignedClip(D2D1::RectF(r.l, r.t, r.r, r.b), D2D1_ANTIALIAS_MODE_ALIASED);
}

void Gfx::popClip() { rt_->PopAxisAlignedClip(); }

void Gfx::setEditorFont(const std::wstring& face, float sizePt, float linePadding) {
  float sizeDip = sizePt * 96.0f / 72.0f;
  editor_[0] = makeFormat(face, sizeDip, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL);
  editor_[1] = makeFormat(face, sizeDip, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC);
  editor_[2] = makeFormat(face, sizeDip, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL);
  if (!editor_[0]) {
    editor_[0] = makeFormat(L"Consolas", sizeDip, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL);
    editor_[1] = makeFormat(L"Consolas", sizeDip, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC);
    editor_[2] = makeFormat(L"Consolas", sizeDip, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL);
  }
  auto l = layout(L"0000000000", editor_[0].Get(), 10000, 1000);
  DWRITE_TEXT_METRICS tm{};
  if (l) l->GetMetrics(&tm);
  charWidth_ = tm.widthIncludingTrailingWhitespace > 0 ? tm.widthIncludingTrailingWhitespace / 10.0f : sizeDip * 0.55f;
  float natural = tm.height > 0 ? tm.height : sizeDip * 1.2f;
  lineHeight_ = std::round(natural + linePadding);
  textOffsetY_ = std::floor((lineHeight_ - natural) / 2.0f);
}

IDWriteTextFormat* Gfx::editorFormat(FontStyle s) const {
  return editor_[static_cast<int>(s)].Get() ? editor_[static_cast<int>(s)].Get() : editor_[0].Get();
}

ComPtr<IDWriteTextLayout> Gfx::layout(std::wstring_view s, IDWriteTextFormat* fmt, float maxW, float maxH) {
  ComPtr<IDWriteTextLayout> l;
  if (fmt) dwrite_->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), fmt, maxW, maxH, l.GetAddressOf());
  return l;
}

void Gfx::text(std::wstring_view s, float x, float y, Color c, IDWriteTextFormat* fmt, float maxW, float maxH) {
  if (s.empty() || !fmt) return;
  brush_->SetColor(c.d2d());
  rt_->DrawText(s.data(), static_cast<UINT32>(s.size()), fmt, D2D1::RectF(x, y, x + maxW, y + maxH), brush_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_NONE);
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
  if (s.empty() || !fmt) return;
  auto l = layout(s, fmt, maxW, 1000);
  if (!l) return;
  ComPtr<ID2D1SolidColorBrush> markBrush;
  rt_->CreateSolidColorBrush(markColor.d2d(), markBrush.GetAddressOf());
  for (size_t m : marks) {
    if (m >= s.size()) continue;
    DWRITE_TEXT_RANGE r{static_cast<UINT32>(m), 1};
    l->SetDrawingEffect(markBrush.Get(), r);
    l->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
  }
  brush_->SetColor(base.d2d());
  rt_->DrawTextLayout(D2D1::Point2F(x, y), l.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Gfx::textWidth(std::wstring_view s, IDWriteTextFormat* fmt) {
  auto l = layout(s, fmt, 100000, 1000);
  if (!l) return 0;
  DWRITE_TEXT_METRICS tm{};
  l->GetMetrics(&tm);
  return tm.widthIncludingTrailingWhitespace;
}

float Gfx::formatHeight(IDWriteTextFormat* fmt) {
  auto l = layout(L"Ag", fmt, 1000, 1000);
  if (!l) return 14;
  DWRITE_TEXT_METRICS tm{};
  l->GetMetrics(&tm);
  return tm.height;
}

}  // namespace verso
