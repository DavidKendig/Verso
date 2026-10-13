#include "diff_view.h"

#include <windowsx.h>

#include <algorithm>

#include "theme.h"
#include "util.h"

using namespace theme;

static const wchar_t kClass[] = L"MP_DiffView";
static constexpr size_t kMaxLines = 60000;

bool DiffView::Register(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.lpszClassName = kClass;
    return RegisterClassExW(&wc) != 0;
}

HWND DiffView::Create(HWND parent, int id) {
    wnd_ = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP | WS_CLIPSIBLINGS,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), this);
    UseDarkControl(wnd_);
    UpdateMetrics();
    return wnd_;
}

void DiffView::UpdateMetrics() {
    HDC dc = GetDC(wnd_);
    HGDIOBJ of = SelectObject(dc, fonts.mono);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, of);
    ReleaseDC(wnd_, dc);
    lineH_ = tm.tmHeight + S(3);
    charW_ = tm.tmAveCharWidth;
    UpdateScroll();
    InvalidateRect(wnd_, nullptr, FALSE);
}

void DiffView::Reset() {
    lines_.clear();
    rows_.clear();
    files_.clear();
    placeholder_.clear();
    maxChars_ = 0;
    anchor_ = cursor_ = -1;
}

static std::wstring Expand(std::string_view s) {
    std::wstring w = Widen(s), out;
    out.reserve(w.size());
    for (wchar_t c : w) {
        if (c == L'\t')
            out.append(4 - out.size() % 4, L' ');
        else if (c != L'\r')
            out.push_back(c);
    }
    return out;
}

static std::string PathFromDiffGit(const std::string& rest) {
    // "a/<path> b/<path>" - when both halves are equal the split is unambiguous.
    if (rest.size() > 4 && (rest.size() % 2) == 1) {
        size_t half = rest.size() / 2;
        if (rest[half] == ' ' && StartsWith(rest, "a/") && rest.compare(half + 1, 2, "b/") == 0 &&
            rest.compare(2, half - 2, rest, half + 3, std::string::npos) == 0)
            return rest.substr(2, half - 2);
    }
    size_t b = rest.rfind(" b/");
    return b == std::string::npos ? rest : rest.substr(b + 3);
}

void DiffView::SetDiff(const std::string& diff, bool keepScroll) {
    int oldTop = top_, oldH = hscroll_;
    Reset();
    diffMode_ = true;
    int oldNo = 0, newNo = 0, curFile = -1;
    bool inHunk = false;
    size_t pos = 0;
    while (pos < diff.size() && lines_.size() < kMaxLines) {
        size_t nl = diff.find('\n', pos);
        if (nl == std::string::npos) nl = diff.size();
        Line L;
        L.raw = diff.substr(pos, nl - pos);
        pos = nl + 1;
        const std::string& raw = L.raw;

        if (StartsWith(raw, "diff --git ") || StartsWith(raw, "diff --cc ") || StartsWith(raw, "diff --combined ")) {
            FileInfo f;
            f.header = (int)lines_.size();
            f.combined = !StartsWith(raw, "diff --git ");
            f.path = f.combined ? raw.substr(raw.find(' ', 7) + 1) : PathFromDiffGit(raw.substr(11));
            files_.push_back(f);
            curFile = (int)files_.size() - 1;
            L.kind = Kind::FileHeader;
            inHunk = false;
        } else if (StartsWith(raw, "@@")) {
            L.kind = Kind::Hunk;
            inHunk = true;
            // @@ -a,b +c,d @@  (combined diffs have several '-' ranges)
            size_t minus = raw.find(" -");
            size_t plus = raw.find(" +");
            oldNo = minus != std::string::npos ? atoi(raw.c_str() + minus + 2) : 0;
            newNo = plus != std::string::npos ? atoi(raw.c_str() + plus + 2) : 0;
        } else if (inHunk && curFile >= 0 && files_[curFile].combined) {
            std::string prefix = raw.substr(0, std::min<size_t>(2, raw.size()));
            if (prefix.find('+') != std::string::npos) {
                L.kind = Kind::Add;
                L.newNo = newNo++;
            } else if (prefix.find('-') != std::string::npos) {
                L.kind = Kind::Del;
            } else {
                L.kind = Kind::Context;
                L.newNo = newNo++;
            }
        } else if (inHunk && (raw.empty() || raw[0] == ' ' || raw[0] == '+' || raw[0] == '-' || raw[0] == '\\')) {
            char c = raw.empty() ? ' ' : raw[0];
            if (c == '+') {
                L.kind = Kind::Add;
                L.newNo = newNo++;
            } else if (c == '-') {
                L.kind = Kind::Del;
                L.oldNo = oldNo++;
            } else if (c == '\\') {
                L.kind = Kind::NoNewline;
            } else {
                L.kind = Kind::Context;
                L.oldNo = oldNo++;
                L.newNo = newNo++;
            }
        } else {
            inHunk = false;
            L.kind = Kind::Meta;
            L.hidden = true;
            if (curFile >= 0) {
                FileInfo& f = files_[curFile];
                if (StartsWith(raw, "+++ b/")) f.path = raw.substr(6);
                else if (StartsWith(raw, "rename to ")) f.path = raw.substr(10);
                else if (StartsWith(raw, "rename from ")) f.origPath = raw.substr(12);
                else if (StartsWith(raw, "copy from ")) f.origPath = raw.substr(10);
                else if (StartsWith(raw, "new file mode")) f.added = true;
                else if (StartsWith(raw, "deleted file mode")) f.deleted = true;
                else if (StartsWith(raw, "old mode ")) f.modeChange = Widen(raw.substr(9));
                else if (StartsWith(raw, "new mode ")) f.modeChange += L" → " + Widen(raw.substr(9));
                else if (StartsWith(raw, "Binary files") || StartsWith(raw, "GIT binary patch")) {
                    f.binary = true;
                    L.hidden = false;
                }
            } else {
                L.hidden = raw.empty();
            }
        }
        L.file = curFile;
        if (L.kind == Kind::Add || L.kind == Kind::Del || L.kind == Kind::Context) {
            size_t skip = (curFile >= 0 && files_[curFile].combined) ? 2 : 1;
            L.text = Expand(std::string_view(raw).substr(std::min(skip, raw.size())));
        } else if (L.kind != Kind::FileHeader) {
            L.text = Expand(raw);
        }
        lines_.push_back(std::move(L));
    }
    FinishParse();
    if (lines_.size() >= kMaxLines) {
        Line L;
        L.kind = Kind::Meta;
        L.text = L"… diff truncated (too large to display)";
        lines_.push_back(L);
        rows_.push_back((int)lines_.size() - 1);
    }
    if (rows_.empty()) placeholder_ = L"No changes";
    top_ = keepScroll ? oldTop : 0;
    hscroll_ = keepScroll ? oldH : 0;
    ScrollTo(top_);
    UpdateScroll();
    InvalidateRect(wnd_, nullptr, FALSE);
}

void DiffView::FinishParse() {
    for (size_t i = 0; i < files_.size(); ++i) {
        FileInfo& f = files_[i];
        f.end = i + 1 < files_.size() ? files_[i + 1].header : (int)lines_.size();
        std::wstring label = Widen(f.path);
        if (!f.origPath.empty() && f.origPath != f.path) label = Widen(f.origPath) + L"  →  " + label;
        std::wstring tags;
        if (f.added) tags += L"  [added]";
        if (f.deleted) tags += L"  [deleted]";
        if (f.binary) tags += L"  [binary]";
        if (f.combined) tags += L"  [conflict]";
        if (!f.modeChange.empty()) tags += L"  [mode " + f.modeChange + L"]";
        lines_[f.header].text = label + tags;
    }
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (lines_[i].hidden) continue;
        rows_.push_back((int)i);
        maxChars_ = std::max(maxChars_, lines_[i].text.size());
    }
}

void DiffView::SetText(const std::wstring& text) {
    Reset();
    diffMode_ = false;
    size_t pos = 0;
    while (pos <= text.size() && lines_.size() < kMaxLines) {
        size_t nl = text.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = text.size();
        Line L;
        L.kind = Kind::Text;
        std::wstring s = text.substr(pos, nl - pos);
        for (wchar_t c : s) {
            if (c == L'\t')
                L.text.append(4 - L.text.size() % 4, L' ');
            else if (c != L'\r')
                L.text.push_back(c);
        }
        maxChars_ = std::max(maxChars_, L.text.size());
        lines_.push_back(std::move(L));
        rows_.push_back((int)lines_.size() - 1);
        pos = nl + 1;
    }
    top_ = hscroll_ = 0;
    UpdateScroll();
    InvalidateRect(wnd_, nullptr, FALSE);
}

void DiffView::SetPlaceholder(const std::wstring& message) {
    Reset();
    diffMode_ = false;
    placeholder_ = message;
    top_ = hscroll_ = 0;
    UpdateScroll();
    InvalidateRect(wnd_, nullptr, FALSE);
}

bool DiffView::ScrollToFile(const std::string& path) {
    for (const auto& f : files_) {
        if (f.path != path) continue;
        auto it = std::lower_bound(rows_.begin(), rows_.end(), f.header);
        if (it == rows_.end()) return false;
        int row = (int)(it - rows_.begin());
        ScrollTo(row);
        anchor_ = cursor_ = row;
        InvalidateRect(wnd_, nullptr, FALSE);
        return true;
    }
    return false;
}

int DiffView::FindRow(std::wstring_view text, int startRow) const {
    for (int r = std::max(0, startRow); r < (int)rows_.size(); ++r)
        if (lines_[rows_[r]].text == text) return r;
    return -1;
}

void DiffView::SetSelection(int anchorRow, int cursorRow) {
    if (rows_.empty()) return;
    anchor_ = std::clamp(anchorRow, 0, (int)rows_.size() - 1);
    cursor_ = std::clamp(cursorRow, 0, (int)rows_.size() - 1);
    EnsureVisible(cursor_);
    InvalidateRect(wnd_, nullptr, FALSE);
}


bool DiffView::CursorInHunk() const {
    if (!diffMode_ || cursor_ < 0 || cursor_ >= (int)rows_.size()) return false;
    const Line& l = lines_[rows_[cursor_]];
    if (l.file < 0 || files_[l.file].combined || files_[l.file].binary) return false;
    return l.kind != Kind::Text;
}

bool DiffView::HasMultiLineSelection() const { return anchor_ >= 0 && cursor_ >= 0 && anchor_ != cursor_; }

std::string DiffView::CursorFilePath() const {
    if (cursor_ < 0 || cursor_ >= (int)rows_.size()) return {};
    int f = lines_[rows_[cursor_]].file;
    return f >= 0 ? files_[f].path : std::string();
}

std::wstring DiffView::SelectedText() const {
    if (cursor_ < 0) return {};
    int a = std::min(anchor_, cursor_), b = std::max(anchor_, cursor_);
    std::wstring out;
    for (int r = a; r <= b && r < (int)rows_.size(); ++r) {
        out += lines_[rows_[r]].text;
        out += L"\r\n";
    }
    return out;
}

void DiffView::SelectionLines(int& a, int& b) const {
    int ra = std::min(anchor_, cursor_), rb = std::max(anchor_, cursor_);
    a = rows_[std::clamp(ra, 0, (int)rows_.size() - 1)];
    b = rows_[std::clamp(rb, 0, (int)rows_.size() - 1)];
}

void DiffView::WidenToHunks(int& a, int& b) const {
    auto hunkStart = [&](int i) {
        int f = lines_[i].file;
        for (int k = i; k >= 0 && lines_[k].file == f; --k) {
            if (lines_[k].kind == Kind::Hunk) return k;
            if (lines_[k].kind == Kind::FileHeader) return k;  // header: whole file
        }
        return i;
    };
    auto hunkEnd = [&](int i) {
        int f = lines_[i].file;
        int k = i + 1;
        bool fromHeader = lines_[i].kind == Kind::FileHeader;
        for (; k < (int)lines_.size() && lines_[k].file == f; ++k) {
            if (lines_[k].kind == Kind::Hunk && !fromHeader) break;
        }
        return k - 1;
    };
    int sa = hunkStart(a);
    a = sa;
    b = hunkEnd(std::max(b, sa));
}

std::string DiffView::BuildPatch(bool reverse, bool wholeHunks) const {
    if (!diffMode_ || cursor_ < 0 || rows_.empty()) return {};
    int a, b;
    SelectionLines(a, b);
    if (wholeHunks) WidenToHunks(a, b);

    std::string out;
    for (const FileInfo& f : files_) {
        if (f.end <= a || f.header > b || f.combined || f.binary) continue;
        int firstHunk = f.end;
        for (int i = f.header; i < f.end; ++i)
            if (lines_[i].kind == Kind::Hunk) {
                firstHunk = i;
                break;
            }
        std::string header;
        for (int i = f.header; i < firstHunk; ++i) header += lines_[i].raw + "\n";

        std::string body;
        for (int h = firstHunk; h < f.end;) {
            int e = h + 1;
            while (e < f.end && lines_[e].kind != Kind::Hunk) ++e;
            if (e > a && h <= b) {
                std::string hb;
                int oldC = 0, newC = 0;
                bool any = false, lastKept = false;
                for (int i = h + 1; i < e; ++i) {
                    const Line& L = lines_[i];
                    bool sel = i >= a && i <= b;
                    switch (L.kind) {
                    case Kind::Context:
                        hb += L.raw + "\n";
                        ++oldC;
                        ++newC;
                        lastKept = true;
                        break;
                    case Kind::Add:
                        if (sel) {
                            hb += L.raw + "\n";
                            ++newC;
                            any = lastKept = true;
                        } else if (reverse) {
                            hb += " " + L.raw.substr(1) + "\n";
                            ++oldC;
                            ++newC;
                            lastKept = true;
                        } else {
                            lastKept = false;
                        }
                        break;
                    case Kind::Del:
                        if (sel) {
                            hb += L.raw + "\n";
                            ++oldC;
                            any = lastKept = true;
                        } else if (!reverse) {
                            hb += " " + L.raw.substr(1) + "\n";
                            ++oldC;
                            ++newC;
                            lastKept = true;
                        } else {
                            lastKept = false;
                        }
                        break;
                    case Kind::NoNewline:
                        if (lastKept) hb += L.raw + "\n";
                        break;
                    default:
                        break;
                    }
                }
                if (any) {
                    const std::string& hr = lines_[h].raw;
                    size_t minus = hr.find(" -"), plus = hr.find(" +");
                    int oldStart = minus != std::string::npos ? atoi(hr.c_str() + minus + 2) : 0;
                    int newStart = plus != std::string::npos ? atoi(hr.c_str() + plus + 2) : 0;
                    body += "@@ -" + std::to_string(oldStart) + "," + std::to_string(oldC) + " +" +
                            std::to_string(newStart) + "," + std::to_string(newC) + " @@\n" + hb;
                }
            }
            h = e;
        }
        if (!body.empty()) out += header + body;
    }
    return out;
}

int DiffView::VisibleRows() const {
    RECT rc;
    GetClientRect(wnd_, &rc);
    return std::max(1, (int)rc.bottom / lineH_);
}

int DiffView::RowAt(int y) const { return top_ + (y < 0 ? -1 : y / lineH_); }

int DiffView::GutterWidth() const { return diffMode_ ? charW_ * 13 + S(10) : S(6); }

void DiffView::UpdateScroll() {
    if (!wnd_) return;
    RECT rc;
    GetClientRect(wnd_, &rc);
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS};
    si.nMax = std::max(0, (int)rows_.size() - 1);
    si.nPage = VisibleRows();
    si.nPos = top_;
    SetScrollInfo(wnd_, SB_VERT, &si, TRUE);
    SCROLLINFO sh{sizeof(sh), SIF_RANGE | SIF_PAGE | SIF_POS};
    sh.nMax = (int)maxChars_ * charW_ + GutterWidth() + S(20);
    sh.nPage = std::max(1L, rc.right);
    sh.nPos = hscroll_;
    SetScrollInfo(wnd_, SB_HORZ, &sh, TRUE);
}

void DiffView::ScrollTo(int top) {
    top = std::clamp(top, 0, std::max(0, (int)rows_.size() - VisibleRows()));
    if (top == top_) return;
    top_ = top;
    UpdateScroll();
    InvalidateRect(wnd_, nullptr, FALSE);
}

void DiffView::ScrollH(int x) {
    RECT rc;
    GetClientRect(wnd_, &rc);
    int maxX = std::max(0, (int)maxChars_ * charW_ + GutterWidth() + S(20) - (int)rc.right);
    x = std::clamp(x, 0, maxX);
    if (x == hscroll_) return;
    hscroll_ = x;
    UpdateScroll();
    InvalidateRect(wnd_, nullptr, FALSE);
}

void DiffView::EnsureVisible(int row) {
    if (row < top_)
        ScrollTo(row);
    else if (row >= top_ + VisibleRows())
        ScrollTo(row - VisibleRows() + 1);
}

void DiffView::SetCursor(int row, bool extend) {
    if (rows_.empty()) return;
    row = std::clamp(row, 0, (int)rows_.size() - 1);
    cursor_ = row;
    if (!extend || anchor_ < 0) anchor_ = row;
    EnsureVisible(row);
    InvalidateRect(wnd_, nullptr, FALSE);
}

LRESULT CALLBACK DiffView::Proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    DiffView* self;
    if (msg == WM_NCCREATE) {
        self = (DiffView*)((CREATESTRUCTW*)lp)->lpCreateParams;
        self->wnd_ = wnd;
        SetWindowLongPtrW(wnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (DiffView*)GetWindowLongPtrW(wnd, GWLP_USERDATA);
    }
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(wnd, msg, wp, lp);
}

LRESULT DiffView::Handle(UINT msg, WPARAM wp, LPARAM lp) {
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
        case SB_BOTTOM: pos = (int)rows_.size(); break;
        }
        ScrollTo(pos);
        return 0;
    }
    case WM_HSCROLL: {
        SCROLLINFO si{sizeof(si), SIF_ALL};
        GetScrollInfo(wnd_, SB_HORZ, &si);
        int pos = hscroll_;
        switch (LOWORD(wp)) {
        case SB_LINELEFT: pos -= charW_ * 4; break;
        case SB_LINERIGHT: pos += charW_ * 4; break;
        case SB_PAGELEFT: pos -= (int)si.nPage; break;
        case SB_PAGERIGHT: pos += (int)si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        }
        ScrollH(pos);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT)
            ScrollH(hscroll_ - delta * charW_ * 8 / WHEEL_DELTA);
        else
            ScrollTo(top_ - delta * 3 / WHEEL_DELTA);
        return 0;
    }
    case WM_MOUSEHWHEEL:
        ScrollH(hscroll_ + GET_WHEEL_DELTA_WPARAM(wp) * charW_ * 8 / WHEEL_DELTA);
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(wnd_);
        int row = RowAt(GET_Y_LPARAM(lp));
        if (row >= 0 && row < (int)rows_.size()) {
            SetCursor(row, (wp & MK_SHIFT) != 0);
            dragging_ = true;
            SetCapture(wnd_);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (dragging_) {
            int y = GET_Y_LPARAM(lp);
            SetCursor(RowAt(y), true);
        }
        return 0;
    case WM_LBUTTONUP:
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
        }
        return 0;
    case WM_RBUTTONDOWN: {
        SetFocus(wnd_);
        int row = RowAt(GET_Y_LPARAM(lp));
        int a = std::min(anchor_, cursor_), b = std::max(anchor_, cursor_);
        if (row >= 0 && row < (int)rows_.size() && (cursor_ < 0 || row < a || row > b)) SetCursor(row, false);
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_KEYDOWN: {
        bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
        switch (wp) {
        case VK_UP: SetCursor(cursor_ - 1, shift); return 0;
        case VK_DOWN: SetCursor(cursor_ + 1, shift); return 0;
        case VK_PRIOR: SetCursor(cursor_ - VisibleRows(), shift); return 0;
        case VK_NEXT: SetCursor(cursor_ + VisibleRows(), shift); return 0;
        case VK_HOME: SetCursor(0, shift); return 0;
        case VK_END: SetCursor((int)rows_.size() - 1, shift); return 0;
        case VK_LEFT: ScrollH(hscroll_ - charW_ * 4); return 0;
        case VK_RIGHT: ScrollH(hscroll_ + charW_ * 4); return 0;
        case 'C':
            if (ctrl) CopyToClipboard(wnd_, SelectedText());
            return 0;
        case 'A':
            if (ctrl && !rows_.empty()) {
                anchor_ = 0;
                cursor_ = (int)rows_.size() - 1;
                InvalidateRect(wnd_, nullptr, FALSE);
            }
            return 0;
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

void DiffView::Paint(HDC dc, const RECT& rc) {
    Fill(dc, rc, kPanel);
    SetBkMode(dc, TRANSPARENT);
    if (rows_.empty()) {
        SelectObject(dc, fonts.ui);
        SetTextColor(dc, kTextDim);
        RECT t = rc;
        DrawTextW(dc, placeholder_.c_str(), -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    int gutter = GutterWidth();
    int selA = std::min(anchor_, cursor_), selB = std::max(anchor_, cursor_);
    bool focused = GetFocus() == wnd_;
    int last = std::min((int)rows_.size(), top_ + VisibleRows() + 1);
    for (int r = top_; r < last; ++r) {
        const Line& L = lines_[rows_[r]];
        int y = (r - top_) * lineH_;
        RECT row{0, y, rc.right, y + lineH_};
        RECT gut{0, y, gutter, y + lineH_};
        COLORREF bg = kPanel, gbg = kPanel, fg = kText;
        HFONT font = fonts.mono;
        switch (L.kind) {
        case Kind::Add: bg = kAddBg; gbg = kAddGutter; break;
        case Kind::Del: bg = kDelBg; gbg = kDelGutter; break;
        case Kind::Hunk: bg = gbg = kHunkBg; fg = kHunkText; break;
        case Kind::FileHeader: bg = gbg = kFileBg; font = fonts.bold; break;
        case Kind::Meta:
        case Kind::NoNewline: fg = kTextDim; break;
        default: break;
        }
        bool selected = cursor_ >= 0 && r >= selA && r <= selB;
        if (selected) bg = gbg = focused ? kSelect : kSelectDim;
        Fill(dc, row, bg);
        if (diffMode_ && L.kind != Kind::FileHeader) Fill(dc, gut, gbg);

        if (L.kind == Kind::FileHeader) {
            RECT t{S(10), y, rc.right - S(6), y + lineH_};
            SelectObject(dc, fonts.bold);
            SetTextColor(dc, kText);
            DrawTextW(dc, L.text.c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
            RECT line{0, y, rc.right, y + 1};
            Fill(dc, line, kBorder);
            continue;
        }

        SelectObject(dc, fonts.mono);
        if (diffMode_ && (L.kind == Kind::Add || L.kind == Kind::Del || L.kind == Kind::Context)) {
            SetTextColor(dc, kTextDim);
            if (L.oldNo) {
                std::wstring n = std::to_wstring(L.oldNo);
                RECT t{0, y, charW_ * 5 + S(2), y + lineH_};
                DrawTextW(dc, n.c_str(), -1, &t, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            }
            if (L.newNo) {
                std::wstring n = std::to_wstring(L.newNo);
                RECT t{charW_ * 5 + S(4), y, charW_ * 10 + S(6), y + lineH_};
                DrawTextW(dc, n.c_str(), -1, &t, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            }
            if (L.kind != Kind::Context) {
                SetTextColor(dc, L.kind == Kind::Add ? kAddText : kDelText);
                const wchar_t* sign = L.kind == Kind::Add ? L"+" : L"−";
                RECT t{charW_ * 11 + S(8), y, gutter, y + lineH_};
                DrawTextW(dc, sign, -1, &t, DT_SINGLELINE | DT_VCENTER);
            }
        }
        SelectObject(dc, font);
        SetTextColor(dc, fg);
        RECT clip{gutter, y, rc.right, y + lineH_};
        int x = gutter + S(4) - hscroll_;
        int ty = y + (lineH_ - (lineH_ - S(3))) / 2;
        ExtTextOutW(dc, x, ty, ETO_CLIPPED, &clip, L.text.c_str(), (UINT)L.text.size(), nullptr);
    }
}
