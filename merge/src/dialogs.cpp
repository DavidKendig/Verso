#include "dialogs.h"

#include <commctrl.h>
#include <shobjidl.h>

#include <algorithm>

#include "theme.h"
#include "util.h"

using namespace theme;

static const wchar_t kModalClass[] = L"MP_Modal";

namespace {

constexpr int kIdEdit = 10, kIdList = 11, kIdOk = IDOK, kIdCancel = IDCANCEL;

struct ModalCtx {
    bool picker = false;
    std::wstring title, label, initial;
    const std::vector<std::wstring>* items = nullptr;
    std::vector<int> visible;
    HWND wnd = nullptr, edit = nullptr, list = nullptr, ok = nullptr, cancel = nullptr;
    bool done = false, accepted = false;
    std::wstring text;
    int pick = -1;
};

void Refilter(ModalCtx* c) {
    wchar_t buf[512];
    GetWindowTextW(c->edit, buf, 512);
    c->visible.clear();
    SendMessageW(c->list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(c->list, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < c->items->size(); ++i) {
        if (!MatchesAllTokens((*c->items)[i], buf)) continue;
        c->visible.push_back((int)i);
        SendMessageW(c->list, LB_ADDSTRING, 0, (LPARAM)(*c->items)[i].c_str());
    }
    if (!c->visible.empty()) SendMessageW(c->list, LB_SETCURSEL, 0, 0);
    SendMessageW(c->list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(c->list, nullptr, TRUE);
}

void Accept(ModalCtx* c) {
    if (c->picker) {
        int sel = (int)SendMessageW(c->list, LB_GETCURSEL, 0, 0);
        if (sel < 0 || sel >= (int)c->visible.size()) return;
        c->pick = c->visible[sel];
    } else {
        int n = GetWindowTextLengthW(c->edit);
        std::wstring t(n, L'\0');
        GetWindowTextW(c->edit, t.data(), n + 1);
        c->text = t;
    }
    c->accepted = true;
    c->done = true;
}

LRESULT CALLBACK EditSubclass(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* c = (ModalCtx*)ref;
    if (msg == WM_KEYDOWN) {
        switch (wp) {
        case VK_RETURN: Accept(c); return 0;
        case VK_ESCAPE: c->done = true; return 0;
        case VK_UP:
        case VK_DOWN:
        case VK_PRIOR:
        case VK_NEXT:
            if (c->list) {
                int sel = (int)SendMessageW(c->list, LB_GETCURSEL, 0, 0);
                int count = (int)SendMessageW(c->list, LB_GETCOUNT, 0, 0);
                int step = (wp == VK_UP || wp == VK_DOWN) ? 1 : 10;
                sel += (wp == VK_UP || wp == VK_PRIOR) ? -step : step;
                if (count > 0) SendMessageW(c->list, LB_SETCURSEL, std::clamp(sel, 0, count - 1), 0);
                return 0;
            }
            break;
        }
    } else if (msg == WM_CHAR && (wp == L'\r' || wp == 27)) {
        return 0;  // swallow to avoid the default beep
    }
    return DefSubclassProc(wnd, msg, wp, lp);
}

void Layout(ModalCtx* c) {
    RECT rc;
    GetClientRect(c->wnd, &rc);
    int pad = S(12), editH = S(28), btnW = S(90), btnH = S(30);
    int y = pad;
    if (!c->label.empty()) y += S(22);
    MoveWindow(c->edit, pad, y, rc.right - 2 * pad, editH, TRUE);
    y += editH + S(8);
    if (c->list) {
        MoveWindow(c->list, pad, y, rc.right - 2 * pad, rc.bottom - y - pad, TRUE);
    } else {
        int by = rc.bottom - pad - btnH;
        MoveWindow(c->cancel, rc.right - pad - btnW, by, btnW, btnH, TRUE);
        MoveWindow(c->ok, rc.right - pad - 2 * btnW - S(8), by, btnW, btnH, TRUE);
    }
}

LRESULT CALLBACK ModalProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* c = (ModalCtx*)GetWindowLongPtrW(wnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        c = (ModalCtx*)((CREATESTRUCTW*)lp)->lpCreateParams;
        c->wnd = wnd;
        SetWindowLongPtrW(wnd, GWLP_USERDATA, (LONG_PTR)c);
        break;
    case WM_CREATE: {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        c->edit = CreateWindowExW(0, L"EDIT", c->initial.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  0, 0, 0, 0, wnd, (HMENU)(INT_PTR)kIdEdit, inst, nullptr);
        SendMessageW(c->edit, WM_SETFONT, (WPARAM)fonts.ui, TRUE);
        SendMessageW(c->edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(6), S(6)));
        SetWindowSubclass(c->edit, EditSubclass, 1, (DWORD_PTR)c);
        if (c->picker) {
            c->list = CreateWindowExW(0, L"LISTBOX", L"",
                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 0, 0,
                                      0, 0, wnd, (HMENU)(INT_PTR)kIdList, inst, nullptr);
            SendMessageW(c->list, WM_SETFONT, (WPARAM)fonts.ui, TRUE);
            SendMessageW(c->list, LB_SETITEMHEIGHT, 0, S(24));
            UseDarkControl(c->list);
            Refilter(c);
        } else {
            c->ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0,
                                    0, wnd, (HMENU)(INT_PTR)kIdOk, inst, nullptr);
            c->cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                        0, 0, 0, 0, wnd, (HMENU)(INT_PTR)kIdCancel, inst, nullptr);
        }
        Layout(c);
        SendMessageW(c->edit, EM_SETSEL, 0, -1);
        SetFocus(c->edit);
        return 0;
    }
    case WM_SIZE:
        if (c && c->edit) Layout(c);
        return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(wnd, &rc);
        Fill((HDC)wp, rc, kBg);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        if (!c->label.empty()) {
            RECT rc;
            GetClientRect(wnd, &rc);
            RECT t{S(12), S(10), rc.right - S(12), S(32)};
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kTextDim);
            SelectObject(dc, fonts.ui);
            DrawTextW(dc, c->label.c_str(), -1, &t, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        EndPaint(wnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, kText);
        SetBkColor((HDC)wp, kInput);
        return (LRESULT)inputBrush;
    case WM_DRAWITEM:
        DrawOwnerButton((const DRAWITEMSTRUCT*)lp, false);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == kIdOk) Accept(c);
        else if (LOWORD(wp) == kIdCancel) c->done = true;
        else if (LOWORD(wp) == kIdEdit && HIWORD(wp) == EN_CHANGE && c->picker) Refilter(c);
        else if (LOWORD(wp) == kIdList && HIWORD(wp) == LBN_DBLCLK) Accept(c);
        return 0;
    case WM_CLOSE:
        c->done = true;
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

void RunModal(HWND owner, ModalCtx& ctx, int w, int h) {
    // Disable the whole top-level window, not just the embedded child that asked.
    if (owner) owner = GetAncestor(owner, GA_ROOT);
    RECT orc;
    GetWindowRect(owner, &orc);
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + (ctx.picker ? S(80) : ((orc.bottom - orc.top) - h) / 3);
    EnableWindow(owner, FALSE);
    HWND wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kModalClass, ctx.title.c_str(),
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | (ctx.picker ? WS_THICKFRAME : 0), x, y, w, h,
                               owner, nullptr, GetModuleHandleW(nullptr), &ctx);
    UseDarkTitleBar(wnd);
    ShowWindow(wnd, SW_SHOW);
    MSG m;
    bool quit = false;
    while (!ctx.done) {
        BOOL r = GetMessageW(&m, nullptr, 0, 0);
        if (r <= 0) {
            quit = true;
            break;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EnableWindow(owner, TRUE);
    DestroyWindow(wnd);
    SetForegroundWindow(owner);
    if (quit) PostQuitMessage(0);
}

}  // namespace

bool RegisterDialogClasses(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = ModalProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kModalClass;
    return RegisterClassExW(&wc) != 0;
}

std::optional<std::wstring> PromptText(HWND owner, const std::wstring& title, const std::wstring& label,
                                       const std::wstring& initial) {
    ModalCtx ctx;
    ctx.title = title;
    ctx.label = label;
    ctx.initial = initial;
    RunModal(owner, ctx, S(520), S(170));
    if (!ctx.accepted) return std::nullopt;
    return ctx.text;
}

int PickFromList(HWND owner, const std::wstring& title, const std::vector<std::wstring>& items,
                 const std::wstring& initialFilter) {
    ModalCtx ctx;
    ctx.picker = true;
    ctx.title = title;
    ctx.items = &items;
    ctx.initial = initialFilter;
    RunModal(owner, ctx, S(560), S(440));
    return ctx.accepted ? ctx.pick : -1;
}

std::optional<std::wstring> ChooseFolder(HWND owner, const std::wstring& title) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return std::nullopt;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title.c_str());
    std::optional<std::wstring> result;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

bool Confirm(HWND owner, const std::wstring& text, const std::wstring& title) {
    if (owner) owner = GetAncestor(owner, GA_ROOT);
    return MessageBoxW(owner, text.c_str(), title.c_str(), MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
}
