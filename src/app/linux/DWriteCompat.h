#pragma once
// The DirectWrite surface Verso's shared renderers use (the Markdown preview's rich text,
// hit testing), implemented over Pango in GfxCairo.cpp. Text positions and ranges are
// wchar_t indices, as on Windows; the layout maps them to Pango's UTF-8 byte indices.

#include <windows.h>  // src/app/linux/win32/windows.h

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

typedef struct _PangoFontDescription PangoFontDescription;
typedef struct _PangoLayout PangoLayout;
typedef struct _PangoAttrList PangoAttrList;

using UINT32 = uint32_t;
using FLOAT = float;

#ifndef SUCCEEDED
#define SUCCEEDED(hr) (static_cast<HRESULT>(hr) >= 0)
#define FAILED(hr) (static_cast<HRESULT>(hr) < 0)
#endif

enum DWRITE_FONT_WEIGHT {
  DWRITE_FONT_WEIGHT_LIGHT = 300,
  DWRITE_FONT_WEIGHT_NORMAL = 400,
  DWRITE_FONT_WEIGHT_MEDIUM = 500,
  DWRITE_FONT_WEIGHT_SEMI_BOLD = 600,
  DWRITE_FONT_WEIGHT_BOLD = 700,
};
enum DWRITE_FONT_STYLE { DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STYLE_OBLIQUE, DWRITE_FONT_STYLE_ITALIC };
enum DWRITE_TEXT_ALIGNMENT {
  DWRITE_TEXT_ALIGNMENT_LEADING,
  DWRITE_TEXT_ALIGNMENT_TRAILING,
  DWRITE_TEXT_ALIGNMENT_CENTER,
  DWRITE_TEXT_ALIGNMENT_JUSTIFIED,
};
enum DWRITE_WORD_WRAPPING { DWRITE_WORD_WRAPPING_WRAP, DWRITE_WORD_WRAPPING_NO_WRAP };
enum DWRITE_LINE_SPACING_METHOD { DWRITE_LINE_SPACING_METHOD_DEFAULT, DWRITE_LINE_SPACING_METHOD_UNIFORM };

struct DWRITE_TEXT_RANGE {
  UINT32 startPosition, length;
};
struct DWRITE_TEXT_METRICS {
  FLOAT left, top, width, widthIncludingTrailingWhitespace, height, layoutWidth, layoutHeight;
  UINT32 maxBidiReorderingDepth, lineCount;
};
struct DWRITE_LINE_METRICS {
  UINT32 length, trailingWhitespaceLength, newlineLength;
  FLOAT height, baseline;
  BOOL isTrimmed;
};
struct DWRITE_HIT_TEST_METRICS {
  UINT32 textPosition, length;
  FLOAT left, top, width, height;
  UINT32 bidiLevel;
  BOOL isText, isTrimmed;
};

struct IDWriteFontCollection;  // fontconfig holds the bundled fonts on Linux; never created

// A solid color for SetDrawingEffect (Gfx::brush caches one per color).
struct ID2D1SolidColorBrush {
  float r = 0, g = 0, b = 0, a = 1;
};

// A font: what DirectWrite calls a text format.
struct IDWriteTextFormat {
  IDWriteTextFormat() = default;
  IDWriteTextFormat(const IDWriteTextFormat&) = delete;
  IDWriteTextFormat& operator=(const IDWriteTextFormat&) = delete;
  ~IDWriteTextFormat();

  FLOAT GetFontSize() const { return sizeDip; }
  HRESULT SetLineSpacing(DWRITE_LINE_SPACING_METHOD method, FLOAT spacing, FLOAT baseline) {
    uniformSpacing = method == DWRITE_LINE_SPACING_METHOD_UNIFORM ? spacing : 0;
    return S_OK;
  }
  HRESULT SetWordWrapping(DWRITE_WORD_WRAPPING w) {
    wrap = w == DWRITE_WORD_WRAPPING_WRAP;
    return S_OK;
  }

  PangoFontDescription* desc = nullptr;
  float sizeDip = 12;
  std::wstring face;
  int weight = 400;
  bool italic = false;
  float uniformSpacing = 0;  // line height in DIPs; 0 for the font's own
  bool wrap = true;
};

// Rich text laid out for drawing and hit testing (DirectWrite's IDWriteTextLayout).
class IDWriteTextLayout {
 public:
  IDWriteTextLayout(std::wstring_view text, IDWriteTextFormat* fmt, float maxW, float maxH);
  ~IDWriteTextLayout();
  IDWriteTextLayout(const IDWriteTextLayout&) = delete;
  IDWriteTextLayout& operator=(const IDWriteTextLayout&) = delete;

  HRESULT SetFontWeight(DWRITE_FONT_WEIGHT w, DWRITE_TEXT_RANGE r);
  HRESULT SetFontStyle(DWRITE_FONT_STYLE s, DWRITE_TEXT_RANGE r);
  HRESULT SetStrikethrough(BOOL on, DWRITE_TEXT_RANGE r);
  HRESULT SetUnderline(BOOL on, DWRITE_TEXT_RANGE r);
  HRESULT SetFontFamilyName(const wchar_t* family, DWRITE_TEXT_RANGE r);
  HRESULT SetFontCollection(IDWriteFontCollection*, DWRITE_TEXT_RANGE) { return S_OK; }
  HRESULT SetFontSize(FLOAT size, DWRITE_TEXT_RANGE r);
  HRESULT SetDrawingEffect(ID2D1SolidColorBrush* brush, DWRITE_TEXT_RANGE r);
  HRESULT SetTextAlignment(DWRITE_TEXT_ALIGNMENT a);
  HRESULT SetLineSpacing(DWRITE_LINE_SPACING_METHOD method, FLOAT spacing, FLOAT baseline);
  HRESULT SetMaxWidth(FLOAT w);
  HRESULT SetWordWrapping(DWRITE_WORD_WRAPPING w);
  // DirectWrite's inline object: the range becomes one w x h box sitting on the baseline,
  // which `draw` fills (called from Gfx::drawLayout with the box's position).
  using DrawFn = std::function<void(float x, float y, float w, float h)>;
  HRESULT SetInlineImage(DrawFn draw, FLOAT w, FLOAT h, DWRITE_TEXT_RANGE r);

  HRESULT GetMetrics(DWRITE_TEXT_METRICS* m);
  HRESULT GetLineMetrics(DWRITE_LINE_METRICS* lines, UINT32 maxLines, UINT32* count);
  HRESULT HitTestTextRange(UINT32 pos, UINT32 len, FLOAT originX, FLOAT originY, DWRITE_HIT_TEST_METRICS* out,
                           UINT32 maxCount, UINT32* count);
  HRESULT HitTestPoint(FLOAT x, FLOAT y, BOOL* trailing, BOOL* inside, DWRITE_HIT_TEST_METRICS* m);

  // Linux: for Gfx::drawLayout.
  PangoLayout* pango();  // with every attribute applied
  struct Image {
    int byteStart = 0;
    float w = 0, h = 0;
    DrawFn draw;
  };
  const std::vector<Image>& images() const { return images_; }

 private:
  void addAttr(void* attr, DWRITE_TEXT_RANGE r);  // a PangoAttribute*, over the wchar_t range
  int byteAt(UINT32 pos) const;
  UINT32 posAt(int byte) const;

  PangoLayout* layout_ = nullptr;
  PangoAttrList* attrs_ = nullptr;
  std::vector<int> offsets_;  // byte offset of each wchar_t, plus the end
  float spacing_ = 0;         // uniform line height (DIPs), 0 for the default
  bool dirty_ = true;
  std::vector<Image> images_;
};

namespace verso {
// Minimal ComPtr for the Linux build: shared ownership of a compat object.
template <typename T>
class ComPtr {
 public:
  ComPtr() = default;
  ComPtr(std::nullptr_t) {}
  explicit ComPtr(T* p) : p_(p) {}
  T* Get() const { return p_.get(); }
  T* operator->() const { return p_.get(); }
  explicit operator bool() const { return p_ != nullptr; }
  void Reset() { p_.reset(); }

 private:
  std::shared_ptr<T> p_;
};
}  // namespace verso
