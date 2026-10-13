#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace verso {

// Bundled monospace family (JetBrains Mono with a slashed zero), embedded as resources.
inline constexpr const wchar_t* kEmbeddedMonoFamily = L"JetBrains Mono Slashed";

using Microsoft::WRL::ComPtr;

struct Color {
  float r = 0, g = 0, b = 0, a = 1;
  static constexpr Color hex(uint32_t rgb, float alpha = 1.0f) {
    return Color{((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha};
  }
  constexpr Color withAlpha(float na) const { return Color{r, g, b, na}; }
  D2D1_COLOR_F d2d() const { return D2D1::ColorF(r, g, b, a); }
};

struct RectF {
  float l = 0, t = 0, r = 0, b = 0;
  float w() const { return r - l; }
  float h() const { return b - t; }
  bool contains(float x, float y) const { return x >= l && x < r && y >= t && y < b; }
  bool empty() const { return r <= l || b <= t; }
  RectF inset(float dx, float dy) const { return {l + dx, t + dy, r - dx, b - dy}; }
};

enum class FontStyle { Regular, Italic, Bold };

// Direct2D/DirectWrite drawing in device-independent pixels (DIPs).
class Gfx {
 public:
  bool init(HWND hwnd);
  void resize();
  void setDpi(float dpi);
  float dpi() const { return dpi_; }
  float pxToDip(int px) const { return static_cast<float>(px) * 96.0f / dpi_; }

  bool beginDraw();
  void endDraw();

  void clear(Color c);
  void fill(const RectF& r, Color c);
  void fillRounded(const RectF& r, float radius, Color c);
  void stroke(const RectF& r, Color c, float width = 1.0f);
  void strokeRounded(const RectF& r, float radius, Color c, float width = 1.0f);
  void line(float x1, float y1, float x2, float y2, Color c, float width = 1.0f);
  // Polyline/polygon with round joins and caps; `filled` fills the closed shape.
  void path(const D2D1_POINT_2F* pts, size_t count, Color c, float width, bool closed, bool filled);
  void fillEllipse(float cx, float cy, float rx, float ry, Color c);
  void pushClip(const RectF& r);
  void popClip();

  void setEditorFont(const std::wstring& face, float sizePt, float linePadding);
  IDWriteTextFormat* editorFormat(FontStyle s) const;
  IDWriteTextFormat* uiFormat() const { return ui_.Get(); }
  IDWriteTextFormat* uiSmallFormat() const { return uiSmall_.Get(); }
  IDWriteTextFormat* uiBoldFormat() const { return uiBold_.Get(); }
  IDWriteTextFormat* uiLargeFormat() const { return uiLarge_.Get(); }
  IDWriteTextFormat* iconFormat() const { return icon_.Get(); }

  void text(std::wstring_view s, float x, float y, Color c, IDWriteTextFormat* fmt, float maxW = 100000.0f,
            float maxH = 1000.0f);
  // Draws text vertically centered inside `r`, clipped with an ellipsis-free cut.
  void textIn(std::wstring_view s, const RectF& r, Color c, IDWriteTextFormat* fmt, bool center = false);
  // Draws text with the characters at `marks` in `markColor` and bold.
  void textMarked(std::wstring_view s, const std::vector<size_t>& marks, float x, float y, Color base,
                  Color markColor, IDWriteTextFormat* fmt, float maxW);
  float textWidth(std::wstring_view s, IDWriteTextFormat* fmt);
  float formatHeight(IDWriteTextFormat* fmt);
  ComPtr<IDWriteTextLayout> layout(std::wstring_view s, IDWriteTextFormat* fmt, float maxW, float maxH);

  // Extra access for richer renderers (the Markdown preview).
  ComPtr<IDWriteTextFormat> createFormat(const std::wstring& face, float sizeDip, DWRITE_FONT_WEIGHT w,
                                         DWRITE_FONT_STYLE s) {
    return makeFormat(face, sizeDip, w, s);
  }
  IDWriteFontCollection* embeddedCollection() const { return embedded_.Get(); }
  ID2D1SolidColorBrush* brush(Color c);  // cached per render target
  void drawLayout(IDWriteTextLayout* layout, float x, float y, Color c);
  ID2D1RenderTarget* target() const { return rt_.Get(); }
  uint64_t targetGeneration() const { return generation_; }

  float charWidth() const { return charWidth_; }
  float lineHeight() const { return lineHeight_; }
  float textOffsetY() const { return textOffsetY_; }

 private:
  ComPtr<IDWriteTextFormat> makeFormat(const std::wstring& face, float sizeDip, DWRITE_FONT_WEIGHT w,
                                       DWRITE_FONT_STYLE s);
  void createTarget();
  void loadEmbeddedFonts();

  HWND hwnd_ = nullptr;
  float dpi_ = 96.0f;
  ComPtr<ID2D1Factory> factory_;
  ComPtr<IDWriteFactory> dwrite_;
  ComPtr<IDWriteFontCollection> embedded_;  // the bundled fonts, for DirectWrite
  ComPtr<ID2D1HwndRenderTarget> rt_;
  ComPtr<ID2D1SolidColorBrush> brush_;
  std::unordered_map<uint64_t, ComPtr<ID2D1SolidColorBrush>> brushes_;
  uint64_t generation_ = 0;
  ComPtr<IDWriteTextFormat> editor_[3];
  ComPtr<IDWriteTextFormat> ui_, uiSmall_, uiBold_, uiLarge_, icon_;
  ComPtr<ID2D1StrokeStyle> roundStroke_;
  float charWidth_ = 8, lineHeight_ = 18, textOffsetY_ = 0;
};

}  // namespace verso
