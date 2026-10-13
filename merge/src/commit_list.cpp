#include "commit_list.h"

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>
#include <windowsx.h>

#include "theme.h"
#include "util.h"

using namespace theme;

static const wchar_t kClass[] = L"MP_CommitList";
static constexpr int kMaxLanes = 24;

bool CommitList::Register(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    return RegisterClassExW(&wc) != 0;
}

HWND CommitList::Create(HWND parent, int id) {
    id_ = id;
    wnd_ = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0,
                           10, 10, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), this);
    UseDarkControl(wnd_);
    UpdateMetrics();
    return wnd_;
}

void CommitList::UpdateMetrics() {
    rowH_ = S(26);
    laneW_ = S(14);
    UpdateScroll();
    if (wnd_) InvalidateRect(wnd_, nullptr, FALSE);
}

void CommitList::SetModel(const std::vector<Commit>* commits, const std::vector<GraphRow>* graph,
                          std::vector<int> rows, bool showGraph, bool workdirRow, const std::string& headHash) {
    commits_ = commits;
    graph_ = graph;
    rows_ = std::move(rows);
    showGraph_ = showGraph;
    workdir_ = workdirRow;
    headHash_ = headHash;
    if (selected_ != kNone && RowOfItem(selected_) < 0) selected_ = kNone;
    top_ = std::clamp(top_, 0, std::max(0, RowCount() - VisibleRows()));
    UpdateScroll();
    InvalidateRect(wnd_, nullptr, FALSE);
}

void CommitList::SetWorkdirText(const std::wstring& text, bool dirty) {
    workdirText_ = text;
    workdirDirty_ = dirty;
    InvalidateRect(wnd_, nullptr, FALSE);
}

void CommitList::SetBanner(const std::wstring& text) {
    banner_ = text;
    UpdateScroll();
    InvalidateRect(wnd_, nullptr, FALSE);
}

int CommitList::ItemOfRow(int row) const {
    if (row < 0) return kNone;
    if (workdir_) {
        if (row == 0) return kWorkdir;
        --row;
    }
    return row < (int)rows_.size() ? rows_[row] : kNone;
}

int CommitList::RowOfItem(int item) const {
    if (item == kWorkdir) return workdir_ ? 0 : -1;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i] == item) return (int)i + (workdir_ ? 1 : 0);
    return -1;
}

int CommitList::VisibleRows() const {
    if (!wnd_) return 1;
    RECT rc;
    GetClientRect(wnd_, &rc);
    return std::max(1, (int)(rc.bottom - BannerHeight()) / rowH_);
}

void CommitList::UpdateScroll() {
    if (!wnd_) return;
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
    si.nMin = 0;
    si.nMax = std::max(0, RowCount() - 1);
    si.nPage = VisibleRows();
    si.nPos = top_;
    SetScrollInfo(wnd_, SB_VERT, &si, TRUE);
}

void CommitList::ScrollTo(int top) {
    top = std::clamp(top, 0, std::max(0, RowCount() - VisibleRows()));
    if (top == top_) return;
    top_ = top;
    UpdateScroll();
    InvalidateRect(wnd_, nullptr, FALSE);
}

void CommitList::EnsureVisible(int row) {
    int vis = VisibleRows();
    if (row < top_)
        ScrollTo(row);
    else if (row >= top_ + vis)
        ScrollTo(row - vis + 1);
}

void CommitList::SelectRow(int row, bool notify) {
    if (RowCount() == 0) return;
    row = std::clamp(row, 0, RowCount() - 1);
    int item = ItemOfRow(row);
    bool changed = item != selected_;
    selected_ = item;
    EnsureVisible(row);
    InvalidateRect(wnd_, nullptr, FALSE);
    if (changed && notify)
        SendMessageW(GetParent(wnd_), WM_COMMAND, MAKEWPARAM(id_, kSelChange), (LPARAM)wnd_);
}

void CommitList::Select(int item, bool notify) {
    int row = RowOfItem(item);
    if (row < 0) {
        selected_ = kNone;
        InvalidateRect(wnd_, nullptr, FALSE);
        if (notify) SendMessageW(GetParent(wnd_), WM_COMMAND, MAKEWPARAM(id_, kSelChange), (LPARAM)wnd_);
        return;
    }
    if (ItemOfRow(row) == selected_ && notify) {
        // Re-selecting the same item still refreshes the dependent views.
        SendMessageW(GetParent(wnd_), WM_COMMAND, MAKEWPARAM(id_, kSelChange), (LPARAM)wnd_);
        EnsureVisible(row);
        return;
    }
    SelectRow(row, notify);
}

int CommitList::ItemAtScreenPoint(POINT pt) const {
    ScreenToClient(wnd_, &pt);
    if (pt.y < BannerHeight()) return kNone;
    return ItemOfRow(top_ + (pt.y - BannerHeight()) / rowH_);
}

LRESULT CALLBACK CommitList::Proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    CommitList* self;
    if (msg == WM_NCCREATE) {
        self = (CommitList*)((CREATESTRUCTW*)lp)->lpCreateParams;
        self->wnd_ = wnd;
        SetWindowLongPtrW(wnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (CommitList*)GetWindowLongPtrW(wnd, GWLP_USERDATA);
    }
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(wnd, msg, wp, lp);
}

LRESULT CommitList::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd_, &ps);
        RECT rc;
        GetClientRect(wnd_, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
        HGDIOBJ old = SelectObject(mem, bmp);
        Paint(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(wnd_, &ps);
        return 0;
    }
    case WM_SIZE:
        ScrollTo(top_);
        UpdateScroll();
        InvalidateRect(wnd_, nullptr, FALSE);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO si{sizeof(si), SIF_ALL};
        GetScrollInfo(wnd_, SB_VERT, &si);
        int pos = top_;
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos--; break;
        case SB_LINEDOWN: pos++; break;
        case SB_PAGEUP: pos -= VisibleRows(); break;
        case SB_PAGEDOWN: pos += VisibleRows(); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = RowCount(); break;
        }
        ScrollTo(pos);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        ScrollTo(top_ - delta * 3 / WHEEL_DELTA);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        SetFocus(wnd_);
        int y = GET_Y_LPARAM(lp);
        if (y >= BannerHeight()) {
            int row = top_ + (y - BannerHeight()) / rowH_;
            if (row < RowCount()) SelectRow(row, true);
        }
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_KEYDOWN: {
        int row = RowOfItem(selected_);
        switch (wp) {
        case VK_UP: SelectRow(row < 0 ? 0 : row - 1, true); return 0;
        case VK_DOWN: SelectRow(row < 0 ? 0 : row + 1, true); return 0;
        case VK_PRIOR: SelectRow(row - VisibleRows(), true); return 0;
        case VK_NEXT: SelectRow(row + VisibleRows(), true); return 0;
        case VK_HOME: SelectRow(0, true); return 0;
        case VK_END: SelectRow(RowCount() - 1, true); return 0;
        }
        break;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(wnd_, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(wnd_, msg, wp, lp);
}

void CommitList::PaintGraph(HDC dc, int row, const RECT& rowRc, int item) {
    if (!graph_ || item < 0 || item >= (int)graph_->size()) return;
    const GraphRow& g = (*graph_)[item];
    Gdiplus::Graphics gfx(dc);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    auto x = [&](int lane) { return (Gdiplus::REAL)(rowRc.left + S(8) + std::min(lane, kMaxLanes) * laneW_ + laneW_ / 2); };
    auto color = [](int lane) {
        COLORREF c = LaneColor(lane);
        return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
    };
    Gdiplus::REAL yTop = (Gdiplus::REAL)rowRc.top, yBot = (Gdiplus::REAL)rowRc.bottom;
    Gdiplus::REAL yMid = (yTop + yBot) / 2;
    Gdiplus::REAL penW = (Gdiplus::REAL)std::max(1.5, S(2) * 0.9);
    for (const auto& s : g.top) {
        Gdiplus::Pen pen(color(s.from), penW);
        gfx.DrawLine(&pen, x(s.from), yTop, x(s.to), yMid);
    }
    for (const auto& s : g.bottom) {
        Gdiplus::Pen pen(color(s.to), penW);
        gfx.DrawLine(&pen, x(s.from), yMid, x(s.to), yBot);
    }
    Gdiplus::REAL r = (Gdiplus::REAL)S(5);
    Gdiplus::REAL cx = x(g.col);
    bool isHead = (*commits_)[item].hash == headHash_;
    Gdiplus::SolidBrush fill(color(g.col));
    if (g.merge) {
        COLORREF panel = kPanel;
        Gdiplus::SolidBrush bg(Gdiplus::Color(255, GetRValue(panel), GetGValue(panel), GetBValue(panel)));
        gfx.FillEllipse(&bg, cx - r, yMid - r, 2 * r, 2 * r);
        Gdiplus::Pen ring(color(g.col), penW);
        gfx.DrawEllipse(&ring, cx - r + 1, yMid - r + 1, 2 * r - 2, 2 * r - 2);
    } else {
        gfx.FillEllipse(&fill, cx - r, yMid - r, 2 * r, 2 * r);
    }
    if (isHead) {
        Gdiplus::Pen white(Gdiplus::Color(255, 255, 255, 255), penW);
        gfx.DrawEllipse(&white, cx - r - 2, yMid - r - 2, 2 * r + 4, 2 * r + 4);
    }
    (void)row;
}

void CommitList::Paint(HDC dc, const RECT& rc) {
    Fill(dc, rc, kPanel);
    SetBkMode(dc, TRANSPARENT);
    bool focused = GetFocus() == wnd_;

    int bannerH = BannerHeight();
    if (bannerH) {
        RECT b{0, 0, rc.right, bannerH};
        Fill(dc, b, kHunkBg);
        SelectObject(dc, fonts.bold);
        SetTextColor(dc, kHunkText);
        RECT t = b;
        t.left += S(10);
        DrawTextW(dc, banner_.c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    int laneCount = 1;
    if (showGraph_ && graph_) {
        int last = std::min(RowCount(), top_ + VisibleRows() + 1);
        for (int r = top_; r < last; ++r) {
            int item = ItemOfRow(r);
            if (item >= 0 && item < (int)graph_->size()) laneCount = std::max(laneCount, (*graph_)[item].width);
        }
        laneCount = std::min(laneCount, kMaxLanes + 1);
    }
    int graphW = showGraph_ ? S(8) + laneCount * laneW_ + S(8) : S(10);
    int w = rc.right;
    bool showAuthor = w > S(520);
    bool showHash = w > S(680);
    int dateW = S(96), authorW = showAuthor ? S(130) : 0, hashW = showHash ? S(70) : 0;
    int rightCols = dateW + authorW + hashW + S(8);

    int rows = RowCount();
    for (int r = top_; r < rows; ++r) {
        int y = bannerH + (r - top_) * rowH_;
        if (y > rc.bottom) break;
        RECT rowRc{0, y, w, y + rowH_};
        int item = ItemOfRow(r);
        if (item == selected_) Fill(dc, rowRc, focused ? kSelect : kSelectDim);

        if (item == kWorkdir) {
            int cx = (showGraph_ ? S(8) + laneW_ / 2 : S(14));
            int cy = y + rowH_ / 2, rr = S(5);
            HPEN pen = CreatePen(PS_SOLID, S(2), workdirDirty_ ? kWarn : kTextDim);
            HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Ellipse(dc, cx - rr, cy - rr, cx + rr, cy + rr);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pen);
            RECT t{graphW, y, w - S(8), y + rowH_};
            SelectObject(dc, fonts.bold);
            SetTextColor(dc, workdirDirty_ ? kText : kTextDim);
            DrawTextW(dc, workdirText_.c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            continue;
        }
        if (item < 0 || !commits_) continue;
        const Commit& c = (*commits_)[item];
        if (showGraph_) PaintGraph(dc, r, rowRc, item);

        int x = graphW;
        int textRight = w - rightCols;
        SelectObject(dc, fonts.caption);
        for (const auto& b : c.badges) {
            if (x > textRight - S(40)) break;
            std::wstring label = b.name;
            COLORREF col = kBadgeLocal;
            switch (b.kind) {
            case RefBadge::Head: col = kBadgeHead; break;
            case RefBadge::Local: col = b.current ? kBadgeCurrent : kBadgeLocal; break;
            case RefBadge::Remote: col = kBadgeRemote; break;
            case RefBadge::Tag: col = kBadgeTag; label = L"⚑ " + label; break;
            }
            SIZE sz = TextSize(dc, fonts.caption, label);
            int bw = std::min<int>(sz.cx + S(12), textRight - x);
            RECT br{x, y + S(4), x + bw, y + rowH_ - S(4)};
            FillRounded(dc, br, col, S(8));
            SetTextColor(dc, RGB(255, 255, 255));
            DrawTextW(dc, label.c_str(), -1, &br,
                      DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            x += bw + S(4);
        }

        SelectObject(dc, fonts.ui);
        SetTextColor(dc, kText);
        RECT t{x + S(2), y, textRight - S(6), y + rowH_};
        DrawTextW(dc, c.subject.c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

        SetTextColor(dc, kTextDim);
        int cx = w - rightCols + S(4);
        if (showAuthor) {
            RECT a{cx, y, cx + authorW - S(8), y + rowH_};
            DrawTextW(dc, c.author.c_str(), -1, &a, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            cx += authorW;
        }
        if (showHash) {
            SelectObject(dc, fonts.mono);
            std::wstring h = Widen(c.hash.substr(0, 7));
            RECT a{cx, y, cx + hashW, y + rowH_};
            DrawTextW(dc, h.c_str(), -1, &a, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            SelectObject(dc, fonts.ui);
            cx += hashW;
        }
        std::wstring when = RelativeTime(c.time);
        RECT d{cx, y, w - S(8), y + rowH_};
        DrawTextW(dc, when.c_str(), -1, &d, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    }

    if (rows == 0 || (rows == 1 && workdir_ && commits_ && commits_->empty())) {
        RECT t{0, bannerH + (workdir_ ? rowH_ : 0), w, rc.bottom};
        SelectObject(dc, fonts.ui);
        SetTextColor(dc, kTextDim);
        DrawTextW(dc, rows_.empty() && !workdir_ ? L"No matching commits" : L"No commits yet", -1, &t,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }
}
