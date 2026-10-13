#pragma once

#include <windows.h>

namespace verso {

struct MenuColors {
  COLORREF bar;
  COLORREF text;
  COLORREF textDisabled;
  COLORREF hot;
  COLORREF pressed;
};

// Switches popup menus (menu bar drop-downs and context menus) between the
// system dark and light renderings. No-op on Windows builds without support.
void setDarkPopupMenus(bool dark);

// Paints the window's menu bar with `colors`. Handles the undocumented
// WM_UAHDRAWMENU/WM_UAHDRAWMENUITEM messages Windows sends for menu bars, plus
// the non-client repaint that otherwise leaves a light line under the bar.
// Returns true when the message was handled and `result` should be returned.
bool handleDarkMenuBar(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, const MenuColors& colors, LRESULT& result);

}  // namespace verso
