#include "app/DarkMenu.h"

namespace verso {

namespace {

// Undocumented menu-bar drawing messages and structures (stable since Windows
// Vista; the same approach is used by Notepad++ and other Win32 editors).
constexpr UINT WM_UAHDRAWMENU = 0x0091;
constexpr UINT WM_UAHDRAWMENUITEM = 0x0092;

struct UAHMENU {
  HMENU hmenu;
  HDC hdc;
  DWORD dwFlags;
};

union UAHMENUITEMMETRICS {
  struct {
    DWORD cx;
    DWORD cy;
  } rgsizeBar[2];
  struct {
    DWORD cx;
    DWORD cy;
  } rgsizePopup[4];
};

struct UAHMENUPOPUPMETRICS {
  DWORD rgcx[4];
  DWORD fUpdateMaxWidths : 2;
};

struct UAHMENUITEM {
  int iPosition;
  UAHMENUITEMMETRICS umim;
  UAHMENUPOPUPMETRICS umpm;
};

struct UAHDRAWMENUITEM {
  DRAWITEMSTRUCT dis;
  UAHMENU um;
  UAHMENUITEM umi;
};

enum class PreferredAppMode { Default, AllowDark, ForceDark, ForceLight };
using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using FlushMenuThemesFn = void(WINAPI*)();

DWORD windowsBuild() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  static DWORD build = [] {
    OSVERSIONINFOW v{sizeof(v)};
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
      if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) fn(&v);
    return v.dwBuildNumber;
  }();
  return build;
}

void fill(HDC dc, const RECT& r, COLORREF c) {
  HBRUSH b = CreateSolidBrush(c);
  FillRect(dc, &r, b);
  DeleteObject(b);
}

// Covers the one-pixel light separator Windows draws between menu bar and client.
void paintMenuSeparator(HWND hwnd, COLORREF color) {
  MENUBARINFO mbi{sizeof(mbi)};
  if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return;
  RECT client;
  GetClientRect(hwnd, &client);
  MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&client), 2);
  RECT window;
  GetWindowRect(hwnd, &window);
  OffsetRect(&client, -window.left, -window.top);
  RECT line = client;
  line.bottom = line.top;
  line.top -= 1;
  HDC dc = GetWindowDC(hwnd);
  fill(dc, line, color);
  ReleaseDC(hwnd, dc);
}

}  // namespace

void setDarkPopupMenus(bool dark) {
  if (windowsBuild() < 18362) return;  // SetPreferredAppMode arrived in Windows 10 1903
  static HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!uxtheme) return;
  auto setMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
  auto flush = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
  if (setMode) setMode(dark ? PreferredAppMode::ForceDark : PreferredAppMode::ForceLight);
  if (flush) flush();
}

bool handleDarkMenuBar(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, const MenuColors& colors, LRESULT& result) {
  switch (msg) {
    case WM_UAHDRAWMENU: {
      auto* menu = reinterpret_cast<UAHMENU*>(lp);
      MENUBARINFO mbi{sizeof(mbi)};
      if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return false;
      RECT window;
      GetWindowRect(hwnd, &window);
      RECT bar = mbi.rcBar;
      OffsetRect(&bar, -window.left, -window.top);
      bar.top -= 1;  // include the border pixel above the bar
      fill(menu->hdc, bar, colors.bar);
      result = TRUE;
      return true;
    }
    case WM_UAHDRAWMENUITEM: {
      auto* item = reinterpret_cast<UAHDRAWMENUITEM*>(lp);
      wchar_t text[256] = {};
      MENUITEMINFOW mii{sizeof(mii)};
      mii.fMask = MIIM_STRING;
      mii.dwTypeData = text;
      mii.cch = 255;
      GetMenuItemInfoW(item->um.hmenu, static_cast<UINT>(item->umi.iPosition), TRUE, &mii);

      const UINT state = item->dis.itemState;
      COLORREF bg = colors.bar;
      COLORREF fg = colors.text;
      if (state & (ODS_INACTIVE | ODS_GRAYED | ODS_DISABLED)) fg = colors.textDisabled;
      if (state & ODS_HOTLIGHT) bg = colors.hot;
      if (state & ODS_SELECTED) bg = colors.pressed;
      fill(item->um.hdc, item->dis.rcItem, bg);

      DWORD flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
      if (state & ODS_NOACCEL) flags |= DT_HIDEPREFIX;
      SetBkMode(item->um.hdc, TRANSPARENT);
      SetTextColor(item->um.hdc, fg);
      DrawTextW(item->um.hdc, text, -1, &item->dis.rcItem, flags);
      result = TRUE;
      return true;
    }
    case WM_NCPAINT:
    case WM_NCACTIVATE:
      result = DefWindowProcW(hwnd, msg, wp, lp);
      paintMenuSeparator(hwnd, colors.bar);
      return true;
    default:
      return false;
  }
}

}  // namespace verso
