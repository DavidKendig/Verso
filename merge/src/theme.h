#pragma once
#include <windows.h>

#include <string>

// Dark colour scheme and shared GDI resources.
namespace theme {

constexpr COLORREF kBg = RGB(0x21, 0x24, 0x2b);
constexpr COLORREF kPanel = RGB(0x29, 0x2d, 0x35);
constexpr COLORREF kInput = RGB(0x1c, 0x1f, 0x25);
constexpr COLORREF kBorder = RGB(0x17, 0x19, 0x1e);
constexpr COLORREF kText = RGB(0xd8, 0xdc, 0xe3);
constexpr COLORREF kTextDim = RGB(0x8a, 0x93, 0xa1);
constexpr COLORREF kAccent = RGB(0x5a, 0x9c, 0xf8);
constexpr COLORREF kSelect = RGB(0x34, 0x47, 0x66);
constexpr COLORREF kSelectDim = RGB(0x30, 0x36, 0x42);
constexpr COLORREF kAddBg = RGB(0x22, 0x3a, 0x2a);
constexpr COLORREF kAddGutter = RGB(0x1d, 0x30, 0x24);
constexpr COLORREF kAddText = RGB(0x8f, 0xd9, 0x9c);
constexpr COLORREF kDelBg = RGB(0x45, 0x25, 0x2a);
constexpr COLORREF kDelGutter = RGB(0x38, 0x1f, 0x23);
constexpr COLORREF kDelText = RGB(0xf0, 0x9a, 0x9a);
constexpr COLORREF kHunkBg = RGB(0x27, 0x30, 0x40);
constexpr COLORREF kHunkText = RGB(0x8f, 0xb4, 0xf0);
constexpr COLORREF kFileBg = RGB(0x34, 0x39, 0x45);
constexpr COLORREF kButton = RGB(0x38, 0x3e, 0x4a);
constexpr COLORREF kButtonDown = RGB(0x2c, 0x31, 0x3b);
constexpr COLORREF kButtonOn = RGB(0x2f, 0x5b, 0x96);
constexpr COLORREF kBadgeLocal = RGB(0x2f, 0x7a, 0x4a);
constexpr COLORREF kBadgeCurrent = RGB(0x2f, 0x6f, 0xc2);
constexpr COLORREF kBadgeRemote = RGB(0x4a, 0x4f, 0x8f);
constexpr COLORREF kBadgeTag = RGB(0x8a, 0x6a, 0x22);
constexpr COLORREF kBadgeHead = RGB(0xa8, 0x4f, 0x2a);
constexpr COLORREF kWarn = RGB(0xe5, 0xa5, 0x4b);

struct Fonts {
    HFONT ui = nullptr;
    HFONT bold = nullptr;
    HFONT caption = nullptr;
    HFONT mono = nullptr;
    HFONT monoBold = nullptr;
};

extern Fonts fonts;
extern int dpi;
extern HBRUSH bgBrush, panelBrush, inputBrush;

void Init(int newDpi);  // (re)creates fonts and brushes for a DPI
inline int S(int v) { return MulDiv(v, dpi, 96); }

COLORREF LaneColor(int lane);
void Fill(HDC dc, const RECT& rc, COLORREF color);
void FillRounded(HDC dc, const RECT& rc, COLORREF color, int radius);
void DrawOwnerButton(const DRAWITEMSTRUCT* dis, bool toggled);
void UseDarkTitleBar(HWND wnd);
void UseDarkControl(HWND wnd);  // dark scrollbars for list/tree/edit controls
SIZE TextSize(HDC dc, HFONT font, const std::wstring& text);

}  // namespace theme
