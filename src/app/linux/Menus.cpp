#include "app/linux/Menus.h"

#include <memory>
#include <unordered_map>

namespace verso::platform {

namespace {
struct MenuData {
  std::vector<MenuItem> items;
};
std::unordered_map<HMENU, std::unique_ptr<MenuData>>& menus() {
  static std::unordered_map<HMENU, std::unique_ptr<MenuData>> m;
  return m;
}
HMENU gBar = nullptr;
PopupPresenter gPresent = nullptr;
MenuBarChanged gChanged = nullptr;
void* gCtx = nullptr;

HMENU newMenu() {
  auto data = std::make_unique<MenuData>();
  HMENU h = reinterpret_cast<HMENU>(data.get());
  menus()[h] = std::move(data);
  return h;
}

// Finds an item by command id (searching submenus, like MF_BYCOMMAND) or by position.
MenuItem* findItem(HMENU menu, UINT item, UINT flags) {
  auto* items = menuItems(menu);
  if (!items) return nullptr;
  if (flags & MF_BYPOSITION) return item < items->size() ? &(*items)[item] : nullptr;
  for (auto& it : *items) {
    if (!(it.flags & MF_POPUP) && it.id == item) return &it;
    if (HMENU sub = it.sub())
      if (MenuItem* found = findItem(sub, item, flags)) return found;
  }
  return nullptr;
}

void changed(HMENU menu) {
  if (gChanged && menu == gBar) gChanged(gCtx);
}
}  // namespace

std::vector<MenuItem>* menuItems(HMENU menu) {
  auto it = menus().find(menu);
  return it == menus().end() ? nullptr : &it->second->items;
}

HMENU windowMenu() { return gBar; }

void setMenuPresenter(PopupPresenter present, MenuBarChanged onChange, void* ctx) {
  gPresent = present;
  gChanged = onChange;
  gCtx = ctx;
}

}  // namespace verso::platform

using namespace verso::platform;

HMENU CreateMenu() { return newMenu(); }
HMENU CreatePopupMenu() { return newMenu(); }

BOOL DestroyMenu(HMENU menu) {
  auto* items = menuItems(menu);
  if (!items) return FALSE;
  for (auto& it : *items)
    if (HMENU sub = it.sub()) DestroyMenu(sub);
  if (menu == gBar) gBar = nullptr;
  menus().erase(menu);
  return TRUE;
}

BOOL AppendMenuW(HMENU menu, UINT flags, UINT_PTR idOrSub, LPCWSTR text) {
  auto* items = menuItems(menu);
  if (!items) return FALSE;
  items->push_back(MenuItem{flags, idOrSub, text ? text : L""});
  changed(menu);
  return TRUE;
}

BOOL InsertMenuW(HMENU menu, UINT position, UINT flags, UINT_PTR idOrSub, LPCWSTR text) {
  auto* items = menuItems(menu);
  if (!items) return FALSE;
  size_t at = items->size();
  if (flags & MF_BYPOSITION) {
    if (position < items->size()) at = position;
  } else {
    for (size_t i = 0; i < items->size(); ++i)
      if ((*items)[i].id == position) at = i;
  }
  items->insert(items->begin() + static_cast<long>(at), MenuItem{flags & ~MF_BYPOSITION, idOrSub, text ? text : L""});
  changed(menu);
  return TRUE;
}

static BOOL removeItem(HMENU menu, UINT position, UINT flags, bool destroy) {
  auto* items = menuItems(menu);
  if (!items) return FALSE;
  for (size_t i = 0; i < items->size(); ++i) {
    bool match = (flags & MF_BYPOSITION) ? i == position : (!((*items)[i].flags & MF_POPUP) && (*items)[i].id == position);
    if (!match) continue;
    if (destroy)
      if (HMENU sub = (*items)[i].sub()) DestroyMenu(sub);
    items->erase(items->begin() + static_cast<long>(i));
    changed(menu);
    return TRUE;
  }
  return FALSE;
}

BOOL DeleteMenu(HMENU menu, UINT position, UINT flags) { return removeItem(menu, position, flags, true); }
BOOL RemoveMenu(HMENU menu, UINT position, UINT flags) { return removeItem(menu, position, flags, false); }

int GetMenuItemCount(HMENU menu) {
  auto* items = menuItems(menu);
  return items ? static_cast<int>(items->size()) : -1;
}

HMENU GetSubMenu(HMENU menu, int pos) {
  auto* items = menuItems(menu);
  if (!items || pos < 0 || pos >= static_cast<int>(items->size())) return nullptr;
  return (*items)[static_cast<size_t>(pos)].sub();
}

UINT GetMenuItemID(HMENU menu, int pos) {
  auto* items = menuItems(menu);
  if (!items || pos < 0 || pos >= static_cast<int>(items->size())) return static_cast<UINT>(-1);
  const MenuItem& it = (*items)[static_cast<size_t>(pos)];
  return it.sub() ? static_cast<UINT>(-1) : static_cast<UINT>(it.id);
}

DWORD CheckMenuItem(HMENU menu, UINT item, UINT check) {
  MenuItem* it = findItem(menu, item, check);
  if (!it) return static_cast<DWORD>(-1);
  DWORD prev = it->flags & MF_CHECKED;
  it->flags = (check & MF_CHECKED) ? (it->flags | MF_CHECKED) : (it->flags & ~MF_CHECKED);
  return prev;
}

BOOL EnableMenuItem(HMENU menu, UINT item, UINT enable) {
  MenuItem* it = findItem(menu, item, enable);
  if (!it) return -1;
  BOOL prev = static_cast<BOOL>(it->flags & (MF_GRAYED | MF_DISABLED));
  it->flags &= ~(MF_GRAYED | MF_DISABLED);
  it->flags |= enable & (MF_GRAYED | MF_DISABLED);
  return prev;
}

BOOL ModifyMenuW(HMENU menu, UINT position, UINT flags, UINT_PTR idOrSub, LPCWSTR text) {
  MenuItem* it = findItem(menu, position, flags);
  if (!it) return FALSE;
  it->flags = flags & ~MF_BYPOSITION;
  it->id = idOrSub;
  it->text = text ? text : L"";
  changed(menu);
  return TRUE;
}

BOOL SetMenu(HWND, HMENU menu) {
  gBar = menu;
  if (gChanged) gChanged(gCtx);
  return TRUE;
}

HMENU GetMenu(HWND) { return gBar; }

BOOL DrawMenuBar(HWND) {
  if (gChanged) gChanged(gCtx);
  return TRUE;
}

BOOL TrackPopupMenu(HMENU menu, UINT flags, int x, int y, int, HWND hwnd, const RECT*) {
  if (!gPresent) return FALSE;
  UINT cmd = gPresent(gCtx, menu, x, y, flags);
  if (flags & TPM_RETURNCMD) return static_cast<BOOL>(cmd);
  if (cmd) PostMessageW(hwnd, WM_COMMAND, cmd, 0);
  return cmd != 0;
}
