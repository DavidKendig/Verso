#pragma once
// Linux menus: what CreateMenu/AppendMenuW build (Win32Compat.h), kept as data so Verso can
// draw the menu bar and popup menus itself. AppLinux.cpp presents them.

#include <string>
#include <vector>

#include "app/linux/Win32Compat.h"

namespace verso::platform {

struct MenuItem {
  UINT flags = 0;   // MF_* (MF_POPUP, MF_SEPARATOR, MF_CHECKED, MF_GRAYED...)
  UINT_PTR id = 0;  // command id; for MF_POPUP the submenu (HMENU)
  std::wstring text;  // "&File\tCtrl+O": & marks the mnemonic, \t starts the shortcut
  HMENU sub() const { return (flags & MF_POPUP) ? reinterpret_cast<HMENU>(id) : nullptr; }
  bool separator() const { return flags & MF_SEPARATOR; }
  bool disabled() const { return flags & (MF_GRAYED | MF_DISABLED); }
  bool checked() const { return flags & MF_CHECKED; }
};

std::vector<MenuItem>* menuItems(HMENU menu);  // nullptr for a destroyed/unknown menu
HMENU windowMenu();                            // the menu bar set with SetMenu

// The app shows popups: returns the chosen command id, or 0.
using PopupPresenter = UINT (*)(void* ctx, HMENU menu, int x, int y, UINT flags);
using MenuBarChanged = void (*)(void* ctx);
void setMenuPresenter(PopupPresenter present, MenuBarChanged changed, void* ctx);

}  // namespace verso::platform
