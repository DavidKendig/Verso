#include "theme.h"

#include <dwmapi.h>
#include <uxtheme.h>

namespace theme {

Fonts fonts;
int dpi = 96;
HBRUSH bgBrush = nullptr, panelBrush = nullptr, inputBrush = nullptr;

static HFONT MakeFont(const wchar_t* face, int points, int weight) {
    return CreateFontW(-MulDiv(points, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

void Init(int newDpi) {
    dpi = newDpi;
    for (HFONT f : {fonts.ui, fonts.bold, fonts.caption, fonts.mono, fonts.monoBold})
        if (f) DeleteObject(f);
    fonts.ui = MakeFont(L"Segoe UI", 9, FW_NORMAL);
    fonts.bold = MakeFont(L"Segoe UI", 9, FW_SEMIBOLD);
    fonts.caption = MakeFont(L"Segoe UI", 8, FW_SEMIBOLD);
    // Verso registers its bundled JetBrains Mono build (slashed zero) for the process.
    fonts.mono = MakeFont(L"JetBrains Mono Slashed", 10, FW_NORMAL);
    fonts.monoBold = MakeFont(L"JetBrains Mono Slashed", 10, FW_BOLD);
    if (!bgBrush) {
        bgBrush = CreateSolidBrush(kBg);
        panelBrush = CreateSolidBrush(kPanel);
        inputBrush = CreateSolidBrush(kInput);
    }
}

COLORREF LaneColor(int lane) {
    static const COLORREF colors[] = {RGB(0x5a, 0x9c, 0xf8), RGB(0xe0, 0x8a, 0x3c), RGB(0x6c, 0xc6, 0x6f),
                                      RGB(0xd0, 0x6a, 0xc8), RGB(0xe5, 0xc0, 0x4b), RGB(0x4f, 0xc9, 0xc4),
                                      RGB(0xe0, 0x5f, 0x5f), RGB(0x9f, 0x8c, 0xf2)};
    return colors[lane % 8];
}

void Fill(HDC dc, const RECT& rc, COLORREF color) {
    SetBkColor(dc, color);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rc, nullptr, 0, nullptr);
}

void FillRounded(HDC dc, const RECT& rc, COLORREF color, int radius) {
    HBRUSH b = CreateSolidBrush(color);
    HPEN p = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

void DrawOwnerButton(const DRAWITEMSTRUCT* dis, bool toggled) {
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    Fill(dc, rc, kBg);
    bool down = (dis->itemState & ODS_SELECTED) != 0;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    COLORREF fill = toggled ? kButtonOn : (down ? kButtonDown : kButton);
    FillRounded(dc, rc, fill, S(6));
    wchar_t text[128];
    GetWindowTextW(dis->hwndItem, text, 128);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? kTextDim : kText);
    HGDIOBJ of = SelectObject(dc, fonts.ui);
    DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
    if (dis->itemState & ODS_FOCUS) {
        RECT f = rc;
        InflateRect(&f, -S(3), -S(3));
        DrawFocusRect(dc, &f);
    }
}

void UseDarkTitleBar(HWND wnd) {
    BOOL on = TRUE;
    DwmSetWindowAttribute(wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof(on));
}

void UseDarkControl(HWND wnd) { SetWindowTheme(wnd, L"DarkMode_Explorer", nullptr); }

SIZE TextSize(HDC dc, HFONT font, const std::wstring& text) {
    HGDIOBJ of = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    SelectObject(dc, of);
    return sz;
}

}  // namespace theme
