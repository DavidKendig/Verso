#include "merge_view.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <thread>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "commit_list.h"
#include "diff_view.h"
#include "dialogs.h"
#include "git.h"
#include "graph.h"
#include "theme.h"
#include "util.h"

using namespace theme;

namespace {

const wchar_t kAppName[] = L"Verso Git";
const wchar_t kMainClass[] = L"MP_Main";

enum : int {
    IDM_OPEN = 100, IDM_CLONE, IDM_INIT, IDM_EXIT, IDM_REFRESH, IDM_FETCH, IDM_PULL, IDM_PULL_REBASE, IDM_PUSH,
    IDM_FORCE_PUSH, IDM_STASH, IDM_STASH_UNTRACKED, IDM_POP_STASH, IDM_STAGE_ALL, IDM_UNSTAGE_ALL,
    IDM_DISCARD_ALL, IDM_COMMIT, IDM_AMEND_TOGGLE, IDM_NEW_BRANCH, IDM_CHECKOUT_BRANCH, IDM_MERGE_BRANCH,
    IDM_REBASE_BRANCH, IDM_DELETE_BRANCH, IDM_NEW_TAG, IDM_CONTINUE_OP, IDM_ABORT_OP, IDM_SEARCH, IDM_GOTO_HEAD,
    IDM_GOTO_WORKDIR, IDM_TOGGLE_CONSOLE, IDM_TOGGLE_SIDEBAR, IDM_FOCUS_LEFT, IDM_FOCUS_MIDDLE, IDM_FOCUS_RIGHT,
    IDM_PALETTE, IDM_OPEN_EXPLORER, IDM_OPEN_TERMINAL, IDM_ABOUT, IDM_ESCAPE, IDM_SWITCH_MODE,
    IDM_RECENT_BASE = 900,  // up to 919

    CM_STAGE = 2000, CM_UNSTAGE, CM_DISCARD, CM_OPEN_FILE, CM_SHOW_IN_EXPLORER, CM_FILE_HISTORY, CM_BLAME,
    CM_COPY_PATH, CM_IGNORE, CM_RESOLVE_OURS, CM_RESOLVE_THEIRS, CM_MARK_RESOLVED, CM_STAGE_HUNK, CM_STAGE_LINES,
    CM_UNSTAGE_HUNK, CM_UNSTAGE_LINES, CM_DISCARD_HUNK, CM_DISCARD_LINES, CM_COPY_TEXT, CM_CHECKOUT_COMMIT,
    CM_BRANCH_HERE, CM_TAG_HERE, CM_CHERRY_PICK, CM_REVERT, CM_RESET_SOFT, CM_RESET_MIXED, CM_RESET_HARD,
    CM_COPY_HASH, CM_COPY_SUBJECT, CM_REF_CHECKOUT, CM_REF_MERGE, CM_REF_REBASE, CM_REF_RENAME, CM_REF_DELETE,
    CM_REF_PUSH, CM_REF_DELETE_REMOTE, CM_TAG_PUSH, CM_TAG_DELETE, CM_STASH_APPLY, CM_STASH_POP, CM_STASH_DROP,
    CM_REMOTE_FETCH,
};

enum : int {
    IDC_SIDEBAR = 3000, IDC_COMMITS, IDC_DIFF, IDC_MSG, IDC_BTN_COMMIT, IDC_BTN_AMEND, IDC_UNSTAGED, IDC_STAGED,
    IDC_BTN_STAGEALL, IDC_BTN_UNSTAGEALL, IDC_BTN_DISCARDALL, IDC_DETAILS, IDC_FILES, IDC_SEARCH, IDC_CONSOLE,
};

constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_TASKDONE = WM_APP + 2;
constexpr UINT WM_APP_UPDATEDIFF = WM_APP + 3;
constexpr int kMaxCommits = 5000;

struct CommandDef {
    int id;
    const wchar_t* name;
    const wchar_t* keys;
};

const CommandDef kCommands[] = {
    {IDM_OPEN, L"Open Repository...", L"Ctrl+O"},
    {IDM_CLONE, L"Clone Repository...", L""},
    {IDM_INIT, L"New Repository (git init)...", L""},
    {IDM_OPEN_EXPLORER, L"Open Repository Folder in Explorer", L""},
    {IDM_OPEN_TERMINAL, L"Open Terminal in Repository", L""},
    {IDM_EXIT, L"Exit", L"Alt+F4"},
    {IDM_REFRESH, L"Refresh", L"F5"},
    {IDM_FETCH, L"Fetch", L""},
    {IDM_PULL, L"Pull", L"F7"},
    {IDM_PULL_REBASE, L"Pull (Rebase)", L""},
    {IDM_PUSH, L"Push", L""},
    {IDM_FORCE_PUSH, L"Push (Force with Lease)", L""},
    {IDM_STASH, L"Stash", L"Ctrl+S"},
    {IDM_STASH_UNTRACKED, L"Stash Including Untracked", L""},
    {IDM_POP_STASH, L"Pop Stash", L"Ctrl+Shift+S"},
    {IDM_STAGE_ALL, L"Stage All", L"Ctrl+Shift+A"},
    {IDM_UNSTAGE_ALL, L"Unstage All", L"Ctrl+Shift+R"},
    {IDM_DISCARD_ALL, L"Discard All Changes...", L""},
    {IDM_COMMIT, L"Commit", L"Ctrl+Enter"},
    {IDM_AMEND_TOGGLE, L"Toggle Amend Previous Commit", L""},
    {IDM_NEW_BRANCH, L"Create Branch...", L"Ctrl+Shift+B"},
    {IDM_CHECKOUT_BRANCH, L"Checkout Branch...", L"Ctrl+Shift+C"},
    {IDM_MERGE_BRANCH, L"Merge Branch into Current...", L""},
    {IDM_REBASE_BRANCH, L"Rebase Current Branch onto...", L""},
    {IDM_DELETE_BRANCH, L"Delete Branch...", L""},
    {IDM_NEW_TAG, L"Create Tag at HEAD...", L""},
    {IDM_CONTINUE_OP, L"Continue Merge / Rebase / Cherry-pick", L""},
    {IDM_ABORT_OP, L"Abort Merge / Rebase / Cherry-pick", L""},
    {IDM_SEARCH, L"Search Commits", L"Ctrl+F"},
    {IDM_GOTO_HEAD, L"Go to HEAD", L"Ctrl+0"},
    {IDM_GOTO_WORKDIR, L"Go to Working Directory", L"Ctrl+9"},
    {IDM_TOGGLE_CONSOLE, L"Toggle Git Console", L"Ctrl+`"},
    {IDM_TOGGLE_SIDEBAR, L"Toggle Sidebar", L"F9"},
    {IDM_FOCUS_LEFT, L"Focus Sidebar", L"Ctrl+1"},
    {IDM_FOCUS_MIDDLE, L"Focus Commit List", L"Ctrl+2"},
    {IDM_FOCUS_RIGHT, L"Focus Diff", L"Ctrl+3"},
    {IDM_PALETTE, L"Command Palette", L"Ctrl+P"},
    {IDM_ABOUT, L"About Verso Git", L""},
    {IDM_SWITCH_MODE, L"Switch to Text Editor", L"Ctrl+Shift+G"},
};

const CommandDef* FindCommand(int id) {
    for (const auto& c : kCommands)
        if (c.id == id) return &c;
    return nullptr;
}

struct Task {
    std::wstring label;
    ProcResult result;
    std::function<void(const ProcResult&)> done;
};

struct SideItem {
    enum Kind { Header, Local, Remote, RemoteGroup, Tag, Stash } kind;
    std::string name;
    std::string target;
};

enum class Mode { None, WorkDir, Commit };
enum class DiffSource { None, Unstaged, Staged, Commit, Text };

std::wstring RecentFilePath() {
    PWSTR appData = nullptr;
    std::wstring p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        std::wstring dir = std::wstring(appData) + L"\\Verso";
        CreateDirectoryW(dir.c_str(), nullptr);
        p = dir + L"\\GitRecent.txt";
        // Before the rename, git mode kept its recent repositories in MergePoint\recent.txt.
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES)
            CopyFileW((std::wstring(appData) + L"\\MergePoint\\recent.txt").c_str(), p.c_str(), TRUE);
    }
    CoTaskMemFree(appData);
    return p;
}

POINT MenuPoint(LPARAM lp, HWND ctl) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (pt.x == -1 && pt.y == -1) {
        RECT rc;
        GetWindowRect(ctl, &rc);
        pt = {rc.left + S(40), rc.top + S(40)};
    }
    return pt;
}

COLORREF StatusColor(wchar_t c) {
    switch (c) {
    case L'A': return RGB(0x6c, 0xc6, 0x6f);
    case L'D': return RGB(0xe0, 0x5f, 0x5f);
    case L'M': return RGB(0xe5, 0xc0, 0x4b);
    case L'R':
    case L'C': return RGB(0x5a, 0x9c, 0xf8);
    case L'U': return RGB(0xe0, 0x8a, 0x3c);
    case L'?': return RGB(0x8a, 0x93, 0xa1);
    default: return kText;
    }
}

std::wstring GetText(HWND w) {
    int n = GetWindowTextLengthW(w);
    std::wstring s(n, L'\0');
    GetWindowTextW(w, s.data(), n + 1);
    return s;
}

class MainWindow {
public:
    bool Create(HINSTANCE inst, HWND parent, MergeViewCallbacks callbacks, std::wstring* error);
    friend class ::MergeView;

private:
    // Owner for dialogs and message boxes: the host's top-level window.
    HWND Root() const { return GetAncestor(wnd_, GA_ROOT); }
    static LRESULT CALLBACK StaticProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK SearchSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    // setup / layout
    void CreateControls();
    HMENU BuildMenu();
    void RebuildRecentMenu();
    HWND MakeButton(int id, const wchar_t* text);
    HWND MakeFileList(int id);
    void ApplyFonts();
    void Layout();
    void LayoutRight(int x, int y, int w, int h);
    void Paint(HDC dc);
    void UpdateTitle();

    // repository state
    bool OpenRepo(const std::wstring& path, bool quiet = false);
    void Refresh();
    void SplitStatus();
    void FillSidebar();
    void FillFileList(HWND lv, const std::vector<FileEntry>& files, bool stagedList,
                      const std::set<std::string>& keep);
    void ApplyFilter();
    void UpdateWorkdirText();
    void OnSelectionChanged();
    void ShowCommit(int index);
    void ShowWorkDiff(bool keepScroll);
    std::vector<FileEntry> SelectedEntries(HWND lv, const std::vector<FileEntry>& files) const;
    const RefInfo* CurrentRef() const;
    const RefInfo* FindRef(const std::string& name, RefInfo::Kind kind) const;
    std::string DefaultRemote() const;

    // git execution
    ProcResult RunLogged(const std::vector<std::wstring>& args, const std::string* input = nullptr);
    bool Exec(const std::vector<std::wstring>& args, const std::string* input = nullptr, bool refresh = true);
    void ExecAsync(const std::wstring& label, std::vector<std::wstring> args,
                   std::function<void(const ProcResult&)> done = {}, const Git* with = nullptr);
    void ShowGitError(const std::wstring& title, const ProcResult& r);
    void AppendConsole(const std::wstring& text);

    // actions
    void OnCommand(int id);
    bool AllowedWhileBusy(int id) const;
    void DoOpen();
    void DoClone();
    void DoInit();
    void DoCommit();
    void ToggleAmend();
    void DoPush(bool force);
    void StagePaths(const std::vector<std::string>& paths);
    void UnstagePaths(const std::vector<std::string>& paths);
    void DiscardEntries(const std::vector<FileEntry>& entries);
    void DiscardAll();
    void ApplySelectionPatch(bool cached, bool reverse, bool wholeHunks);
    void NewBranch(const std::string& startPoint);
    void NewTag(const std::string& at);
    void CheckoutRef(const std::string& name, bool remote);
    void DeleteLocalBranch(const std::string& name);
    int PickBranch(const std::wstring& title, bool includeRemotes, bool excludeCurrent, std::string& name,
                   bool& remote);
    void ContinueOrAbort(bool abort);
    void ShowPalette();
    void ShowFileHistory(const std::string& path);
    void ShowBlame(const std::string& path, const std::string& rev);
    void OpenFile(const std::string& path);
    std::string RepoRelative(const std::wstring& absolutePath) const;
    void RevealFile(const std::string& path);
    void AddToGitignore(const std::vector<std::string>& paths);
    void ClearFilters();

    // context menus
    void ContextFileList(HWND lv, POINT pt);
    void ContextCommitFiles(POINT pt);
    void ContextDiff(POINT pt);
    void ContextCommit(POINT pt);
    void ContextSidebar(POINT pt);

    void LoadRecent();
    void SaveRecent();
    void AddRecent(const std::wstring& path);

    HINSTANCE inst_ = nullptr;
    HWND wnd_ = nullptr;
    HACCEL accel_ = nullptr;
    HMENU menu_ = nullptr;
    HMENU recentMenu_ = nullptr;
    MergeViewCallbacks cb_;

    HWND tree_ = nullptr, search_ = nullptr, msg_ = nullptr, btnCommit_ = nullptr, btnAmend_ = nullptr;
    HWND lvUnstaged_ = nullptr, lvStaged_ = nullptr, btnStageAll_ = nullptr, btnUnstageAll_ = nullptr;
    HWND btnDiscardAll_ = nullptr, details_ = nullptr, lvFiles_ = nullptr, console_ = nullptr;
    std::vector<std::pair<int, HWND>> toolbar_;
    CommitList list_;
    DiffView diff_;

    Git git_;
    bool repoOpen_ = false;
    std::vector<Commit> commits_;
    std::vector<GraphRow> graph_;
    std::vector<FileEntry> status_, unstaged_, staged_;
    std::vector<RefInfo> refs_;
    std::vector<std::string> remotes_;
    std::vector<StashInfo> stashes_;
    std::string headBranch_, headHash_;
    std::wstring opState_;
    std::vector<ChangedFile> commitFiles_;
    std::string shownCommit_;
    std::wstring pathFilter_;

    Mode mode_ = Mode::None;
    DiffSource diffSource_ = DiffSource::None;
    bool busy_ = false, consoleVisible_ = false, sidebarVisible_ = true, amend_ = false;
    bool filling_ = false, refreshing_ = false, diffPending_ = false, keepDiffScroll_ = false, forceWorkdir_ = false;
    std::wstring statusMsg_;
    int sidebarW_ = 0, listW_ = 0, drag_ = 0;
    RECT split1_{}, split2_{}, statusRc_{};
    std::vector<std::pair<RECT, std::wstring>> labels_;
    std::vector<std::wstring> recent_;
    std::vector<SideItem> sideItems_;
    std::map<std::wstring, bool> collapsed_{{L"TAGS", true}};
};

// ---------------------------------------------------------------------------
// Setup

bool MainWindow::Create(HINSTANCE inst, HWND parent, MergeViewCallbacks callbacks, std::wstring* error) {
    inst_ = inst;
    cb_ = std::move(callbacks);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = StaticProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);
    CommitList::Register(inst);
    DiffView::Register(inst);
    RegisterDialogClasses(inst);

    theme::Init(GetDpiForWindow(parent));
    git_.exe = Git::Locate();
    if (git_.exe.empty()) {
        if (error) *error = L"git.exe was not found. Install Git for Windows and make sure it is on PATH.";
        return false;
    }

    LoadRecent();
    menu_ = BuildMenu();
    wnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, kMainClass, kAppName, WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0,
                           0, S(1200), S(800), parent, nullptr, inst, this);
    if (!wnd_) {
        if (error) *error = L"Could not create the git view window.";
        return false;
    }

    ACCEL acc[] = {
        {FVIRTKEY | FCONTROL, 'O', IDM_OPEN},
        {FVIRTKEY | FCONTROL | FSHIFT, 'G', IDM_SWITCH_MODE},
        {FVIRTKEY, VK_F5, IDM_REFRESH},
        {FVIRTKEY, VK_F7, IDM_PULL},
        {FVIRTKEY | FCONTROL, 'S', IDM_STASH},
        {FVIRTKEY | FCONTROL | FSHIFT, 'S', IDM_POP_STASH},
        {FVIRTKEY | FCONTROL | FSHIFT, 'A', IDM_STAGE_ALL},
        {FVIRTKEY | FCONTROL | FSHIFT, 'R', IDM_UNSTAGE_ALL},
        {FVIRTKEY | FCONTROL, VK_RETURN, IDM_COMMIT},
        {FVIRTKEY | FCONTROL | FSHIFT, 'B', IDM_NEW_BRANCH},
        {FVIRTKEY | FCONTROL | FSHIFT, 'C', IDM_CHECKOUT_BRANCH},
        {FVIRTKEY | FCONTROL, 'F', IDM_SEARCH},
        {FVIRTKEY | FCONTROL, '0', IDM_GOTO_HEAD},
        {FVIRTKEY | FCONTROL, '9', IDM_GOTO_WORKDIR},
        {FVIRTKEY | FCONTROL, VK_OEM_3, IDM_TOGGLE_CONSOLE},
        {FVIRTKEY, VK_F9, IDM_TOGGLE_SIDEBAR},
        {FVIRTKEY | FCONTROL, '1', IDM_FOCUS_LEFT},
        {FVIRTKEY | FCONTROL, '2', IDM_FOCUS_MIDDLE},
        {FVIRTKEY | FCONTROL, '3', IDM_FOCUS_RIGHT},
        {FVIRTKEY | FCONTROL, 'P', IDM_PALETTE},
        {FVIRTKEY, VK_ESCAPE, IDM_ESCAPE},
    };
    accel_ = CreateAcceleratorTableW(acc, ARRAYSIZE(acc));

    // The host opens a repository (or the most recent one) when the view is first shown.
    mode_ = Mode::None;
    diff_.SetPlaceholder(L"Open a repository with File › Open Repository (Ctrl+O), or clone one.");
    Layout();
    return true;
}

HWND MainWindow::MakeButton(int id, const wchar_t* text) {
    HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, wnd_,
                             (HMENU)(INT_PTR)id, inst_, nullptr);
    return b;
}

HWND MainWindow::MakeFileList(int id) {
    HWND lv = CreateWindowExW(0, WC_LISTVIEWW, L"",
                              WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS, 0, 0, 0,
                              0, wnd_, (HMENU)(INT_PTR)id, inst_, nullptr);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col{LVCF_WIDTH};
    col.cx = S(30);
    ListView_InsertColumn(lv, 0, &col);
    col.cx = S(500);
    ListView_InsertColumn(lv, 1, &col);
    ListView_SetBkColor(lv, kInput);
    ListView_SetTextBkColor(lv, kInput);
    ListView_SetTextColor(lv, kText);
    UseDarkControl(lv);
    return lv;
}

void MainWindow::CreateControls() {
    const std::pair<int, const wchar_t*> tb[] = {{IDM_OPEN, L"Open"},        {IDM_FETCH, L"Fetch"},
                                                 {IDM_PULL, L"Pull"},        {IDM_PUSH, L"Push"},
                                                 {IDM_STASH, L"Stash"},      {IDM_POP_STASH, L"Pop Stash"},
                                                 {IDM_NEW_BRANCH, L"Branch"}, {IDM_PALETTE, L"Commands"}};
    for (auto& [id, text] : tb) toolbar_.push_back({id, MakeButton(id, text)});

    search_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, wnd_,
                              (HMENU)IDC_SEARCH, inst_, nullptr);
    SendMessageW(search_, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search commits  (Ctrl+F)");
    SetWindowSubclass(search_, SearchSubclass, 1, (DWORD_PTR)this);

    tree_ = CreateWindowExW(0, WC_TREEVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_LINESATROOT |
                                TVS_SHOWSELALWAYS | TVS_FULLROWSELECT | TVS_NOHSCROLL,
                            0, 0, 0, 0, wnd_, (HMENU)IDC_SIDEBAR, inst_, nullptr);
    TreeView_SetExtendedStyle(tree_, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    TreeView_SetBkColor(tree_, kPanel);
    TreeView_SetTextColor(tree_, kText);
    UseDarkControl(tree_);

    list_.Create(wnd_, IDC_COMMITS);
    diff_.Create(wnd_, IDC_DIFF);

    msg_ = CreateWindowExW(0, L"EDIT", L"",
                           WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0, 0,
                           0, wnd_, (HMENU)IDC_MSG, inst_, nullptr);
    UseDarkControl(msg_);
    btnCommit_ = MakeButton(IDC_BTN_COMMIT, L"Commit");
    btnAmend_ = MakeButton(IDC_BTN_AMEND, L"Amend");
    lvUnstaged_ = MakeFileList(IDC_UNSTAGED);
    lvStaged_ = MakeFileList(IDC_STAGED);
    btnStageAll_ = MakeButton(IDC_BTN_STAGEALL, L"Stage All");
    btnDiscardAll_ = MakeButton(IDC_BTN_DISCARDALL, L"Discard All");
    btnUnstageAll_ = MakeButton(IDC_BTN_UNSTAGEALL, L"Unstage All");

    details_ = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 0, 0, 0, 0, wnd_,
                               (HMENU)IDC_DETAILS, inst_, nullptr);
    UseDarkControl(details_);
    lvFiles_ = MakeFileList(IDC_FILES);

    console_ = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                                   ES_READONLY,
                               0, 0, 0, 0, wnd_, (HMENU)IDC_CONSOLE, inst_, nullptr);
    SendMessageW(console_, EM_SETLIMITTEXT, 0, 0);
    UseDarkControl(console_);

    ApplyFonts();

    HWND w = wnd_;
    git_.logger = [w](const std::wstring& cmd, const ProcResult& r) {
        std::wstring text = L"$ " + cmd + L"\r\n";
        std::wstring body = ToCrlf(Widen(r.out + r.err));
        if (body.size() > 20000) body = body.substr(0, 20000) + L"\r\n… (output truncated)\r\n";
        text += body;
        if (!body.empty() && body.back() != L'\n') text += L"\r\n";
        if (!r.ok()) text += L"[exit code " + std::to_wstring(r.exitCode) + L"]\r\n";
        auto* s = new std::wstring(std::move(text));
        if (!PostMessageW(w, WM_APP_LOG, 0, (LPARAM)s)) delete s;
    };
    AppendConsole(L"Verso git console — using " + git_.exe + L"\r\n");
}

void MainWindow::ApplyFonts() {
    for (HWND h : {search_, msg_, tree_, lvUnstaged_, lvStaged_, lvFiles_})
        SendMessageW(h, WM_SETFONT, (WPARAM)fonts.ui, TRUE);
    SendMessageW(details_, WM_SETFONT, (WPARAM)fonts.mono, TRUE);
    SendMessageW(console_, WM_SETFONT, (WPARAM)fonts.mono, TRUE);
    for (HWND h : {search_, msg_, details_, console_})
        SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(6), S(6)));
    TreeView_SetItemHeight(tree_, S(24));
    for (HWND lv : {lvUnstaged_, lvStaged_, lvFiles_}) ListView_SetColumnWidth(lv, 0, S(30));
    list_.UpdateMetrics();
    diff_.UpdateMetrics();
}

HMENU MainWindow::BuildMenu() {
    HMENU bar = CreateMenu();
    auto item = [](HMENU m, int id) {
        const CommandDef* d = FindCommand(id);
        std::wstring s = d->name;
        if (*d->keys) s += std::wstring(L"\t") + d->keys;
        AppendMenuW(m, MF_STRING, id, s.c_str());
    };
    auto sep = [](HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); };

    HMENU file = CreatePopupMenu();
    item(file, IDM_SWITCH_MODE);
    sep(file);
    item(file, IDM_OPEN);
    recentMenu_ = CreatePopupMenu();
    AppendMenuW(file, MF_POPUP, (UINT_PTR)recentMenu_, L"Open &Recent");
    item(file, IDM_CLONE);
    item(file, IDM_INIT);
    sep(file);
    item(file, IDM_OPEN_EXPLORER);
    item(file, IDM_OPEN_TERMINAL);
    sep(file);
    item(file, IDM_EXIT);
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");

    HMENU repo = CreatePopupMenu();
    for (int id : std::initializer_list<int>{IDM_REFRESH, 0, IDM_FETCH, IDM_PULL, IDM_PULL_REBASE, IDM_PUSH, IDM_FORCE_PUSH, 0, IDM_STAGE_ALL,
                   IDM_UNSTAGE_ALL, IDM_DISCARD_ALL, IDM_COMMIT, IDM_AMEND_TOGGLE, 0, IDM_STASH, IDM_STASH_UNTRACKED,
                   IDM_POP_STASH, 0, IDM_CONTINUE_OP, IDM_ABORT_OP})
        id ? item(repo, id) : sep(repo);
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)repo, L"&Repository");

    HMENU branch = CreatePopupMenu();
    for (int id : std::initializer_list<int>{IDM_NEW_BRANCH, IDM_CHECKOUT_BRANCH, IDM_MERGE_BRANCH, IDM_REBASE_BRANCH, IDM_DELETE_BRANCH, 0,
                   IDM_NEW_TAG})
        id ? item(branch, id) : sep(branch);
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)branch, L"&Branch");

    HMENU view = CreatePopupMenu();
    for (int id : std::initializer_list<int>{IDM_PALETTE, IDM_SEARCH, 0, IDM_GOTO_HEAD, IDM_GOTO_WORKDIR, 0, IDM_FOCUS_LEFT, IDM_FOCUS_MIDDLE,
                   IDM_FOCUS_RIGHT, 0, IDM_TOGGLE_SIDEBAR, IDM_TOGGLE_CONSOLE})
        id ? item(view, id) : sep(view);
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&View");

    HMENU help = CreatePopupMenu();
    item(help, IDM_ABOUT);
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");

    RebuildRecentMenu();
    return bar;
}

void MainWindow::RebuildRecentMenu() {
    if (!recentMenu_) return;
    while (GetMenuItemCount(recentMenu_) > 0) DeleteMenu(recentMenu_, 0, MF_BYPOSITION);
    if (recent_.empty()) AppendMenuW(recentMenu_, MF_STRING | MF_GRAYED, 0, L"(none)");
    for (size_t i = 0; i < recent_.size() && i < 20; ++i)
        AppendMenuW(recentMenu_, MF_STRING, IDM_RECENT_BASE + i, recent_[i].c_str());
}

// ---------------------------------------------------------------------------
// Layout and painting

void MainWindow::Layout() {
    if (!wnd_ || !list_.hwnd()) return;
    RECT rc;
    GetClientRect(wnd_, &rc);
    int W = rc.right, H = rc.bottom;
    labels_.clear();
    int tbH = S(44), sbH = S(26), conH = consoleVisible_ ? S(190) : 0, gap = S(5);
    if (sidebarW_ == 0) sidebarW_ = S(230);
    if (listW_ == 0) listW_ = std::max(S(380), (W - sidebarW_) * 46 / 100);

    HDC dc = GetDC(wnd_);
    int x = S(10);
    for (auto& [id, h] : toolbar_) {
        int bw = TextSize(dc, fonts.ui, GetText(h)).cx + S(26);
        MoveWindow(h, x, S(8), bw, S(28), FALSE);
        x += bw + S(6);
    }
    ReleaseDC(wnd_, dc);
    int searchW = std::min(S(300), std::max(S(120), W - x - S(20)));
    MoveWindow(search_, W - searchW - S(10), S(10), searchW, S(24), FALSE);

    int top = tbH, bottom = H - sbH - conH;
    ShowWindow(console_, consoleVisible_ ? SW_SHOW : SW_HIDE);
    if (consoleVisible_) MoveWindow(console_, S(6), bottom + S(4), W - S(12), conH - S(6), FALSE);

    int x0 = 0;
    ShowWindow(tree_, sidebarVisible_ ? SW_SHOW : SW_HIDE);
    if (sidebarVisible_) {
        sidebarW_ = std::clamp(sidebarW_, S(140), std::max(S(140), W / 3));
        MoveWindow(tree_, 0, top, sidebarW_, bottom - top, FALSE);
        x0 = sidebarW_;
        split1_ = {x0, top, x0 + gap, bottom};
        x0 += gap;
    } else {
        split1_ = {};
    }
    int avail = W - x0;
    listW_ = std::clamp(listW_, S(260), std::max(S(260), avail - S(320)));
    MoveWindow(list_.hwnd(), x0, top, listW_, bottom - top, FALSE);
    split2_ = {x0 + listW_, top, x0 + listW_ + gap, bottom};
    int rx = x0 + listW_ + gap;
    LayoutRight(rx, top, W - rx, bottom - top);
    statusRc_ = {0, H - sbH, W, H};
    InvalidateRect(wnd_, nullptr, TRUE);
    RedrawWindow(wnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void MainWindow::LayoutRight(int x, int y, int w, int h) {
    bool work = mode_ == Mode::WorkDir, com = mode_ == Mode::Commit;
    for (HWND c : {msg_, btnCommit_, btnAmend_, lvUnstaged_, lvStaged_, btnStageAll_, btnUnstageAll_, btnDiscardAll_})
        ShowWindow(c, work ? SW_SHOW : SW_HIDE);
    for (HWND c : {details_, lvFiles_}) ShowWindow(c, com ? SW_SHOW : SW_HIDE);

    int pad = S(8), hdr = S(30), btnH = S(24);
    auto label = [&](int ly, const std::wstring& text) { labels_.push_back({{x + pad, ly, x + w - pad, ly + hdr}, text}); };
    auto rightButtons = [&](int ly, std::initializer_list<std::pair<HWND, int>> buttons) {
        int bx = x + w - pad;
        for (auto& [b, bw] : buttons) {
            bx -= bw;
            MoveWindow(b, bx, ly + (hdr - btnH) / 2, bw, btnH, FALSE);
            bx -= S(6);
        }
    };

    int cy = y;
    if (work) {
        std::wstring title = amend_ ? L"COMMIT MESSAGE  —  amending previous commit" : L"COMMIT MESSAGE";
        if (!opState_.empty()) title += L"   [" + opState_ + L"]";
        label(cy, title);
        rightButtons(cy, {{btnCommit_, S(90)}, {btnAmend_, S(80)}});
        cy += hdr;
        MoveWindow(msg_, x + pad, cy, w - 2 * pad, S(70), FALSE);
        cy += S(70) + S(4);
        int remain = y + h - cy;
        int listH = std::clamp((remain - 2 * hdr) / 4, S(60), S(220));
        label(cy, L"UNSTAGED FILES (" + std::to_wstring(unstaged_.size()) + L")");
        rightButtons(cy, {{btnStageAll_, S(90)}, {btnDiscardAll_, S(96)}});
        cy += hdr;
        MoveWindow(lvUnstaged_, x + pad, cy, w - 2 * pad, listH, FALSE);
        cy += listH;
        label(cy, L"STAGED FILES (" + std::to_wstring(staged_.size()) + L")");
        rightButtons(cy, {{btnUnstageAll_, S(100)}});
        cy += hdr;
        MoveWindow(lvStaged_, x + pad, cy, w - 2 * pad, listH, FALSE);
        cy += listH + S(6);
        for (HWND lv : {lvUnstaged_, lvStaged_}) ListView_SetColumnWidth(lv, 1, w - 2 * pad - S(30) - S(24));
    } else if (com) {
        int dh = S(130);
        MoveWindow(details_, x + pad, cy + pad, w - 2 * pad, dh, FALSE);
        cy += dh + pad;
        label(cy, L"CHANGED FILES (" + std::to_wstring(commitFiles_.size()) + L")");
        cy += hdr;
        int fh = std::clamp((int)commitFiles_.size() * S(22) + S(8), S(50), S(170));
        MoveWindow(lvFiles_, x + pad, cy, w - 2 * pad, fh, FALSE);
        ListView_SetColumnWidth(lvFiles_, 1, w - 2 * pad - S(30) - S(24));
        cy += fh + S(6);
    }
    MoveWindow(diff_.hwnd(), x, cy, w, std::max(0, y + h - cy), FALSE);
}

void MainWindow::Paint(HDC dc) {
    RECT rc;
    GetClientRect(wnd_, &rc);
    Fill(dc, rc, kBg);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, fonts.caption);
    SetTextColor(dc, kTextDim);
    for (auto& [r, text] : labels_) {
        RECT t = r;
        DrawTextW(dc, text.c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    Fill(dc, statusRc_, kPanel);
    RECT line{statusRc_.left, statusRc_.top, statusRc_.right, statusRc_.top + 1};
    Fill(dc, line, kBorder);
    std::wstring left;
    if (repoOpen_) {
        left = git_.root;
        if (!headBranch_.empty())
            left += L"    branch: " + Widen(headBranch_);
        else if (!headHash_.empty())
            left += L"    detached HEAD at " + Widen(headHash_.substr(0, 7));
        if (const RefInfo* cur = CurrentRef(); cur && !cur->upstream.empty()) {
            left += L"  →  " + Widen(cur->upstream);
            if (cur->ahead) left += L"  ↑" + std::to_wstring(cur->ahead);
            if (cur->behind) left += L"  ↓" + std::to_wstring(cur->behind);
            if (cur->gone) left += L"  (upstream gone)";
        }
        if (!opState_.empty()) left += L"    [" + opState_ + L"]";
    } else {
        left = L"No repository open";
    }
    SelectObject(dc, fonts.ui);
    RECT l = statusRc_;
    l.left += S(10);
    l.right -= S(10);
    SetTextColor(dc, opState_.empty() ? kTextDim : kWarn);
    DrawTextW(dc, left.c_str(), -1, &l, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    SetTextColor(dc, busy_ ? kAccent : kTextDim);
    DrawTextW(dc, statusMsg_.c_str(), -1, &l, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
}

void MainWindow::UpdateTitle() {
    std::wstring subject;
    if (repoOpen_) {
        std::wstring name = git_.root.substr(git_.root.find_last_of(L'\\') + 1);
        subject = name + L" (" + (headBranch_.empty() ? L"detached" : Widen(headBranch_)) + L")";
    }
    if (cb_.onTitle) cb_.onTitle(subject);
    const RefInfo* cur = CurrentRef();
    for (auto& [id, h] : toolbar_) {
        if (id == IDM_PULL)
            SetWindowTextW(h, cur && cur->behind ? (L"Pull ↓" + std::to_wstring(cur->behind)).c_str() : L"Pull");
        if (id == IDM_PUSH)
            SetWindowTextW(h, cur && cur->ahead ? (L"Push ↑" + std::to_wstring(cur->ahead)).c_str() : L"Push");
    }
}

// ---------------------------------------------------------------------------
// Repository state

bool MainWindow::OpenRepo(const std::wstring& path, bool quiet) {
    Git probe = git_;
    std::wstring err;
    if (!probe.Open(path, err)) {
        if (!quiet)
            MessageBoxW(Root(), (L"Could not open repository:\n" + path + L"\n\n" + err).c_str(), kAppName,
                        MB_ICONERROR);
        return false;
    }
    git_.root = probe.root;
    git_.gitDir = probe.gitDir;
    repoOpen_ = true;
    pathFilter_.clear();
    SetWindowTextW(search_, L"");
    SetWindowTextW(msg_, L"");
    amend_ = false;
    shownCommit_.clear();
    AddRecent(git_.root);
    AppendConsole(L"\r\nOpened " + git_.root + L"\r\n");
    forceWorkdir_ = true;
    Refresh();
    return true;
}

void MainWindow::SplitStatus() {
    unstaged_.clear();
    staged_.clear();
    for (const auto& e : status_) {
        if (e.untracked || e.conflict) {
            unstaged_.push_back(e);
            continue;
        }
        if (e.x != ' ') staged_.push_back(e);
        if (e.y != ' ') unstaged_.push_back(e);
    }
}

void MainWindow::Refresh() {
    if (!repoOpen_ || refreshing_) return;
    refreshing_ = true;
    HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    int sel = list_.Selected();
    std::string selHash = sel >= 0 && sel < (int)commits_.size() ? commits_[sel].hash : std::string();
    bool wantWorkdir = forceWorkdir_ || sel == CommitList::kWorkdir || sel == CommitList::kNone;
    forceWorkdir_ = false;
    std::set<std::string> keepU, keepS;
    for (auto& e : SelectedEntries(lvUnstaged_, unstaged_)) keepU.insert(e.path);
    for (auto& e : SelectedEntries(lvStaged_, staged_)) keepS.insert(e.path);

    headBranch_ = git_.HeadBranch();
    headHash_ = git_.HeadHash();
    opState_ = git_.OperationState();
    refs_ = git_.Refs();
    remotes_ = git_.Remotes();
    stashes_ = git_.Stashes();
    status_ = git_.Status();
    SplitStatus();
    commits_ = git_.Log(kMaxCommits, pathFilter_, !headHash_.empty());
    graph_ = BuildGraph(commits_);

    if (opState_ == L"MERGING" && GetWindowTextLengthW(msg_) == 0) {
        std::ifstream in(git_.gitDir + L"\\MERGE_MSG", std::ios::binary);
        std::string m((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string cleaned;
        for (auto& l : Split(m, '\n'))
            if (!StartsWith(l, "#")) cleaned += l + "\n";
        SetWindowTextW(msg_, ToCrlf(Widen(Trim(cleaned))).c_str());
    }

    FillSidebar();
    filling_ = true;
    FillFileList(lvUnstaged_, unstaged_, false, keepU);
    FillFileList(lvStaged_, staged_, true, keepS);
    filling_ = false;
    ApplyFilter();
    UpdateWorkdirText();

    int target = CommitList::kWorkdir;
    if (!wantWorkdir) {
        target = CommitList::kNone;
        for (int i = 0; i < (int)commits_.size(); ++i)
            if (commits_[i].hash == selHash) target = i;
        if (target == CommitList::kNone) target = CommitList::kWorkdir;
    }
    if (!pathFilter_.empty() && target == CommitList::kWorkdir) target = commits_.empty() ? CommitList::kNone : 0;
    keepDiffScroll_ = true;
    list_.Select(target, true);
    keepDiffScroll_ = false;

    UpdateTitle();
    Layout();
    SetCursor(oldCursor);
    refreshing_ = false;
}

void MainWindow::FillSidebar() {
    filling_ = true;
    SendMessageW(tree_, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(tree_);
    sideItems_.clear();
    std::vector<HTREEITEM> expandable;

    auto add = [&](HTREEITEM parent, const std::wstring& text, SideItem item, bool bold) {
        sideItems_.push_back(std::move(item));
        TVINSERTSTRUCTW is{};
        is.hParent = parent;
        is.hInsertAfter = TVI_LAST;
        is.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
        is.item.pszText = const_cast<LPWSTR>(text.c_str());
        is.item.lParam = (LPARAM)(sideItems_.size() - 1);
        is.item.stateMask = TVIS_BOLD;
        is.item.state = bold ? TVIS_BOLD : 0;
        return TreeView_InsertItem(tree_, &is);
    };
    auto header = [&](const wchar_t* name) {
        HTREEITEM h = add(TVI_ROOT, name, {SideItem::Header, Narrow(name), ""}, true);
        if (!collapsed_[name]) expandable.push_back(h);
        return h;
    };

    HTREEITEM hb = header(L"BRANCHES");
    for (const auto& r : refs_) {
        if (r.kind != RefInfo::Local) continue;
        std::wstring label = (r.current ? L"● " : L"") + Widen(r.name);
        if (r.ahead) label += L"   ↑" + std::to_wstring(r.ahead);
        if (r.behind) label += L"   ↓" + std::to_wstring(r.behind);
        if (r.gone) label += L"   (gone)";
        add(hb, label, {SideItem::Local, r.name, r.target}, r.current);
    }
    HTREEITEM hr = header(L"REMOTES");
    for (const auto& remote : remotes_) {
        std::wstring rname = Widen(remote);
        HTREEITEM g = add(hr, rname, {SideItem::RemoteGroup, remote, ""}, false);
        if (!collapsed_[rname]) expandable.push_back(g);
        for (const auto& r : refs_) {
            if (r.kind != RefInfo::Remote || !StartsWith(r.name, remote + "/")) continue;
            add(g, Widen(r.name.substr(remote.size() + 1)), {SideItem::Remote, r.name, r.target}, false);
        }
    }
    HTREEITEM ht = header(L"TAGS");
    for (auto it = refs_.rbegin(); it != refs_.rend(); ++it)
        if (it->kind == RefInfo::Tag) add(ht, Widen(it->name), {SideItem::Tag, it->name, it->target}, false);
    HTREEITEM hs = header(L"STASHES");
    for (const auto& s : stashes_)
        add(hs, Widen(s.ref) + L"  " + s.message, {SideItem::Stash, s.ref, ""}, false);

    for (HTREEITEM h : expandable) TreeView_Expand(tree_, h, TVE_EXPAND);
    SendMessageW(tree_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(tree_, nullptr, TRUE);
    filling_ = false;
}

void MainWindow::FillFileList(HWND lv, const std::vector<FileEntry>& files, bool stagedList,
                              const std::set<std::string>& keep) {
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < files.size(); ++i) {
        const FileEntry& e = files[i];
        wchar_t st[2] = {(wchar_t)(stagedList ? e.x : (e.untracked ? '?' : e.conflict ? 'U' : e.y)), 0};
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = (int)i;
        it.pszText = st;
        it.lParam = (LPARAM)i;
        int idx = ListView_InsertItem(lv, &it);
        std::wstring path = Widen(e.path);
        if (!e.origPath.empty() && stagedList) path = Widen(e.origPath) + L"  →  " + path;
        if (e.untracked) path += L"   (untracked)";
        if (e.conflict) path += L"   (conflict)";
        ListView_SetItemText(lv, idx, 1, path.data());
        if (keep.count(e.path)) ListView_SetItemState(lv, idx, LVIS_SELECTED, LVIS_SELECTED);
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, nullptr, TRUE);
}

void MainWindow::ApplyFilter() {
    std::wstring query = TrimW(GetText(search_));
    std::vector<int> rows;
    rows.reserve(commits_.size());
    for (int i = 0; i < (int)commits_.size(); ++i) {
        if (!query.empty()) {
            const Commit& c = commits_[i];
            std::wstring hay = c.subject + L" " + c.author + L" " + c.email + L" " + Widen(c.hash);
            for (auto& b : c.badges) hay += L" " + b.name;
            if (!MatchesAllTokens(hay, query)) continue;
        }
        rows.push_back(i);
    }
    bool plain = query.empty() && pathFilter_.empty();
    list_.SetModel(&commits_, &graph_, std::move(rows), plain, pathFilter_.empty() && query.empty(), headHash_);
    list_.SetBanner(pathFilter_.empty() ? L"" : L"History of " + pathFilter_ + L"    (Esc to clear)");
}

void MainWindow::UpdateWorkdirText() {
    std::wstring t;
    size_t conflicts = std::count_if(status_.begin(), status_.end(), [](auto& e) { return e.conflict; });
    if (status_.empty())
        t = L"Working directory clean";
    else
        t = L"Uncommitted changes:  " + std::to_wstring(unstaged_.size()) + L" unstaged,  " +
            std::to_wstring(staged_.size()) + L" staged";
    if (conflicts) t += L",  " + std::to_wstring(conflicts) + L" conflicted";
    if (!opState_.empty()) t = opState_ + L"  —  " + t;
    list_.SetWorkdirText(t, !status_.empty() || !opState_.empty());
}

std::vector<FileEntry> MainWindow::SelectedEntries(HWND lv, const std::vector<FileEntry>& files) const {
    std::vector<FileEntry> out;
    int i = -1;
    while ((i = ListView_GetNextItem(lv, i, LVNI_SELECTED)) >= 0) {
        LVITEMW it{};
        it.mask = LVIF_PARAM;
        it.iItem = i;
        ListView_GetItem(lv, &it);
        if (it.lParam >= 0 && it.lParam < (LPARAM)files.size()) out.push_back(files[it.lParam]);
    }
    return out;
}

const RefInfo* MainWindow::CurrentRef() const {
    for (const auto& r : refs_)
        if (r.kind == RefInfo::Local && r.current) return &r;
    return nullptr;
}

const RefInfo* MainWindow::FindRef(const std::string& name, RefInfo::Kind kind) const {
    for (const auto& r : refs_)
        if (r.kind == kind && r.name == name) return &r;
    return nullptr;
}

std::string MainWindow::DefaultRemote() const {
    for (const auto& r : remotes_)
        if (r == "origin") return r;
    return remotes_.empty() ? std::string() : remotes_.front();
}

void MainWindow::OnSelectionChanged() {
    int item = list_.Selected();
    if (item == CommitList::kWorkdir) {
        mode_ = Mode::WorkDir;
        shownCommit_.clear();
        Layout();
        ShowWorkDiff(keepDiffScroll_);
    } else if (item >= 0) {
        mode_ = Mode::Commit;
        ShowCommit(item);
        Layout();
    } else {
        mode_ = Mode::None;
        shownCommit_.clear();
        diffSource_ = DiffSource::None;
        diff_.SetPlaceholder(repoOpen_ ? L"Select a commit" : L"Open a repository (Ctrl+O)");
        Layout();
    }
}

void MainWindow::ShowCommit(int index) {
    const Commit& c = commits_[index];
    if (c.hash == shownCommit_ && keepDiffScroll_) return;
    shownCommit_ = c.hash;
    std::wstring details = git_.CommitDetails(c.hash);
    if (c.parents.size() > 1) details += L"\r\n(merge commit — changes shown against the first parent)";
    SetWindowTextW(details_, details.c_str());
    commitFiles_ = git_.CommitFiles(c);
    SendMessageW(lvFiles_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lvFiles_);
    for (size_t i = 0; i < commitFiles_.size(); ++i) {
        const ChangedFile& f = commitFiles_[i];
        wchar_t st[2] = {(wchar_t)(f.status.empty() ? ' ' : f.status[0]), 0};
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = (int)i;
        it.pszText = st;
        it.lParam = (LPARAM)i;
        int idx = ListView_InsertItem(lvFiles_, &it);
        std::wstring path = Widen(f.path);
        if (!f.origPath.empty()) path = Widen(f.origPath) + L"  →  " + path;
        ListView_SetItemText(lvFiles_, idx, 1, path.data());
    }
    SendMessageW(lvFiles_, WM_SETREDRAW, TRUE, 0);
    diffSource_ = DiffSource::Commit;
    diff_.SetDiff(git_.CommitDiff(c), false);
}

void MainWindow::ShowWorkDiff(bool keepScroll) {
    auto selU = SelectedEntries(lvUnstaged_, unstaged_);
    auto selS = SelectedEntries(lvStaged_, staged_);
    std::string d;
    if (!selU.empty()) {
        diffSource_ = DiffSource::Unstaged;
        std::vector<std::string> tracked;
        std::string untracked;
        for (auto& e : selU) {
            if (e.untracked)
                untracked += git_.NewFileDiff(e.path);
            else
                tracked.push_back(e.path);
        }
        if (!tracked.empty()) d = git_.WorkingDiff(tracked, false);
        d += untracked;
    } else if (!selS.empty()) {
        diffSource_ = DiffSource::Staged;
        std::vector<std::string> paths;
        for (auto& e : selS) {
            paths.push_back(e.path);
            if (!e.origPath.empty()) paths.push_back(e.origPath);
        }
        d = git_.WorkingDiff(paths, true);
    } else if (!unstaged_.empty()) {
        diffSource_ = DiffSource::Unstaged;
        d = git_.WorkingDiff({}, false);
        int shown = 0;
        for (auto& e : unstaged_)
            if (e.untracked && shown++ < 50) d += git_.NewFileDiff(e.path);
    } else if (!staged_.empty()) {
        diffSource_ = DiffSource::Staged;
        d = git_.WorkingDiff({}, true);
    } else {
        diffSource_ = DiffSource::None;
        diff_.SetPlaceholder(opState_.empty() ? L"Nothing to commit — working tree clean"
                                              : L"All conflicts resolved. Commit to conclude " + opState_);
        return;
    }
    diff_.SetDiff(d, keepScroll);
}

// ---------------------------------------------------------------------------
// Git execution helpers

ProcResult MainWindow::RunLogged(const std::vector<std::wstring>& args, const std::string* input) {
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    ProcResult r = git_.Run(args, input, true);
    SetCursor(old);
    return r;
}

bool MainWindow::Exec(const std::vector<std::wstring>& args, const std::string* input, bool refresh) {
    if (!repoOpen_) return false;
    if (busy_) {
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    ProcResult r = RunLogged(args, input);
    if (!r.ok()) ShowGitError(L"git " + args[0], r);
    if (refresh) Refresh();
    return r.ok();
}

void MainWindow::ExecAsync(const std::wstring& label, std::vector<std::wstring> args,
                           std::function<void(const ProcResult&)> done, const Git* with) {
    if (busy_) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    busy_ = true;
    statusMsg_ = label + L"…";
    InvalidateRect(wnd_, &statusRc_, FALSE);
    Git g = with ? *with : git_;
    auto* task = new Task{label, {}, std::move(done)};
    HWND w = wnd_;
    std::thread([g, args = std::move(args), task, w] {
        task->result = g.Run(args, nullptr, true);
        if (!PostMessageW(w, WM_APP_TASKDONE, 0, (LPARAM)task)) delete task;
    }).detach();
}

void MainWindow::ShowGitError(const std::wstring& title, const ProcResult& r) {
    std::string text = Trim(r.out);
    std::string err = Trim(r.err);
    if (!err.empty()) text = text.empty() ? err : text + "\n\n" + err;
    std::wstring w = Widen(text);
    if (w.empty()) w = L"git exited with code " + std::to_wstring(r.exitCode);
    if (w.size() > 3000) w = w.substr(0, 3000) + L"\n…";
    MessageBoxW(Root(), w.c_str(), (title + L" failed").c_str(), MB_ICONERROR);
}

void MainWindow::AppendConsole(const std::wstring& text) {
    int len = GetWindowTextLengthW(console_);
    if (len > 400000) {
        SendMessageW(console_, EM_SETSEL, 0, len / 2);
        SendMessageW(console_, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(console_);
    }
    SendMessageW(console_, EM_SETSEL, len, len);
    SendMessageW(console_, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
}

// ---------------------------------------------------------------------------
// Actions

bool MainWindow::AllowedWhileBusy(int id) const {
    switch (id) {
    case IDM_EXIT: case IDM_SEARCH: case IDM_GOTO_HEAD: case IDM_GOTO_WORKDIR: case IDM_TOGGLE_CONSOLE:
    case IDM_TOGGLE_SIDEBAR: case IDM_FOCUS_LEFT: case IDM_FOCUS_MIDDLE: case IDM_FOCUS_RIGHT: case IDM_PALETTE:
    case IDM_OPEN_EXPLORER: case IDM_OPEN_TERMINAL: case IDM_ABOUT: case IDM_ESCAPE: case IDM_SWITCH_MODE:
        return true;
    }
    return false;
}

void MainWindow::OnCommand(int id) {
    if (busy_ && !AllowedWhileBusy(id)) {
        MessageBeep(MB_ICONWARNING);
        statusMsg_ = L"Busy — waiting for: " + statusMsg_;
        InvalidateRect(wnd_, &statusRc_, FALSE);
        return;
    }
    bool needsRepo = !(id == IDM_OPEN || id == IDM_CLONE || id == IDM_INIT || id == IDM_EXIT || id == IDM_ABOUT ||
                       id == IDM_PALETTE || id == IDM_TOGGLE_CONSOLE || id == IDM_TOGGLE_SIDEBAR ||
                       id == IDM_ESCAPE || id == IDM_SWITCH_MODE || (id >= IDM_RECENT_BASE && id < IDM_RECENT_BASE + 20));
    if (needsRepo && !repoOpen_) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    switch (id) {
    case IDM_OPEN: DoOpen(); break;
    case IDM_CLONE: DoClone(); break;
    case IDM_INIT: DoInit(); break;
    case IDM_EXIT: PostMessageW(Root(), WM_CLOSE, 0, 0); break;
    case IDM_SWITCH_MODE:
        if (cb_.onSwitchMode) cb_.onSwitchMode();
        break;
    case IDM_REFRESH: Refresh(); break;
    case IDM_FETCH: ExecAsync(L"Fetch", {L"fetch", L"--all", L"--prune"}); break;
    case IDM_PULL:
    case IDM_PULL_REBASE:
        if (headBranch_.empty()) {
            MessageBoxW(Root(), L"HEAD is detached; check out a branch before pulling.", kAppName, MB_ICONINFORMATION);
            break;
        }
        ExecAsync(L"Pull", id == IDM_PULL ? std::vector<std::wstring>{L"pull", L"--no-edit"}
                                          : std::vector<std::wstring>{L"pull", L"--rebase"});
        break;
    case IDM_PUSH: DoPush(false); break;
    case IDM_FORCE_PUSH: DoPush(true); break;
    case IDM_STASH: Exec({L"stash", L"push"}); break;
    case IDM_STASH_UNTRACKED: Exec({L"stash", L"push", L"--include-untracked"}); break;
    case IDM_POP_STASH:
        if (stashes_.empty())
            MessageBoxW(Root(), L"There are no stashes.", kAppName, MB_ICONINFORMATION);
        else
            Exec({L"stash", L"pop"});
        break;
    case IDM_STAGE_ALL: Exec({L"add", L"-A"}); break;
    case IDM_UNSTAGE_ALL:
        if (headHash_.empty())
            Exec({L"rm", L"-r", L"-q", L"--cached", L"."});
        else
            Exec({L"reset", L"-q"});
        break;
    case IDM_DISCARD_ALL: DiscardAll(); break;
    case IDM_COMMIT: DoCommit(); break;
    case IDM_AMEND_TOGGLE: ToggleAmend(); break;
    case IDM_NEW_BRANCH: NewBranch(""); break;
    case IDM_CHECKOUT_BRANCH: {
        std::string name;
        bool remote = false;
        if (PickBranch(L"Checkout Branch", true, true, name, remote) >= 0) CheckoutRef(name, remote);
        break;
    }
    case IDM_MERGE_BRANCH: {
        std::string name;
        bool remote = false;
        if (PickBranch(L"Merge into " + Widen(headBranch_), true, true, name, remote) >= 0)
            Exec({L"merge", L"--no-edit", Widen(name)});
        break;
    }
    case IDM_REBASE_BRANCH: {
        std::string name;
        bool remote = false;
        if (PickBranch(L"Rebase " + Widen(headBranch_) + L" onto", true, true, name, remote) >= 0)
            Exec({L"rebase", Widen(name)});
        break;
    }
    case IDM_DELETE_BRANCH: {
        std::string name;
        bool remote = false;
        if (PickBranch(L"Delete Branch", false, true, name, remote) >= 0) DeleteLocalBranch(name);
        break;
    }
    case IDM_NEW_TAG: NewTag("HEAD"); break;
    case IDM_CONTINUE_OP: ContinueOrAbort(false); break;
    case IDM_ABORT_OP: ContinueOrAbort(true); break;
    case IDM_SEARCH:
        SetFocus(search_);
        SendMessageW(search_, EM_SETSEL, 0, -1);
        break;
    case IDM_GOTO_HEAD:
        for (int i = 0; i < (int)commits_.size(); ++i)
            if (commits_[i].hash == headHash_) {
                list_.Select(i, true);
                SetFocus(list_.hwnd());
                break;
            }
        break;
    case IDM_GOTO_WORKDIR:
        ClearFilters();
        list_.Select(CommitList::kWorkdir, true);
        SetFocus(msg_);
        break;
    case IDM_TOGGLE_CONSOLE:
        consoleVisible_ = !consoleVisible_;
        Layout();
        if (consoleVisible_) SendMessageW(console_, EM_SCROLLCARET, 0, 0);
        break;
    case IDM_TOGGLE_SIDEBAR:
        sidebarVisible_ = !sidebarVisible_;
        Layout();
        break;
    case IDM_FOCUS_LEFT: SetFocus(tree_); break;
    case IDM_FOCUS_MIDDLE: SetFocus(list_.hwnd()); break;
    case IDM_FOCUS_RIGHT: SetFocus(diff_.hwnd()); break;
    case IDM_PALETTE: ShowPalette(); break;
    case IDM_OPEN_EXPLORER: ShellExecuteW(wnd_, L"open", git_.root.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case IDM_OPEN_TERMINAL:
        if ((INT_PTR)ShellExecuteW(wnd_, L"open", L"wt.exe", (L"-d " + QuoteArg(git_.root)).c_str(), nullptr,
                                   SW_SHOWNORMAL) <= 32)
            ShellExecuteW(wnd_, L"open", L"cmd.exe", nullptr, git_.root.c_str(), SW_SHOWNORMAL);
        break;
    case IDM_ABOUT:
        MessageBoxW(Root(),
                    (std::wstring(L"Verso Git\n\nThe git mode of Verso, a native Win32 Git client written in C++20.\n"
                                  L"Every repository operation runs through the git command line.\n\n"
                                  L"Copyright 2026 David Kendig\n"
                                  L"Licensed under the Apache License, Version 2.0.\n\nGit: ") +
                     git_.exe)
                        .c_str(),
                    kAppName, MB_ICONINFORMATION);
        break;
    case IDM_ESCAPE:
        if (GetFocus() == search_ || !pathFilter_.empty() || GetWindowTextLengthW(search_) > 0) {
            ClearFilters();
            SetFocus(list_.hwnd());
        }
        break;
    default:
        if (id >= IDM_RECENT_BASE && id < IDM_RECENT_BASE + 20 && id - IDM_RECENT_BASE < (int)recent_.size())
            OpenRepo(recent_[id - IDM_RECENT_BASE]);
        break;
    }
}

void MainWindow::ClearFilters() {
    bool changed = !pathFilter_.empty();
    pathFilter_.clear();
    if (GetWindowTextLengthW(search_) > 0) SetWindowTextW(search_, L"");  // EN_CHANGE re-filters
    if (changed) {
        forceWorkdir_ = true;
        Refresh();
    }
}

void MainWindow::DoOpen() {
    if (auto dir = ChooseFolder(Root(), L"Open Git Repository")) OpenRepo(*dir);
}

void MainWindow::DoClone() {
    auto url = PromptText(Root(), L"Clone Repository", L"Repository URL:");
    if (!url || TrimW(*url).empty()) return;
    std::wstring u = TrimW(*url);
    auto parent = ChooseFolder(Root(), L"Choose the folder to clone into");
    if (!parent) return;
    std::wstring name = u;
    while (!name.empty() && (name.back() == L'/' || name.back() == L'\\')) name.pop_back();
    name = name.substr(name.find_last_of(L"/\\:") + 1);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, L".git") == 0) name.resize(name.size() - 4);
    auto folder = PromptText(Root(), L"Clone Repository", L"Folder name inside " + *parent + L":", name);
    if (!folder || TrimW(*folder).empty()) return;
    std::wstring dest = *parent;
    if (dest.back() != L'\\') dest += L'\\';
    dest += TrimW(*folder);
    Git g = git_;
    g.root = *parent;
    ExecAsync(L"Clone " + u, {L"clone", L"--progress", u, dest},
              [this, dest](const ProcResult& r) {
                  if (r.ok()) OpenRepo(dest);
              },
              &g);
}

void MainWindow::DoInit() {
    auto dir = ChooseFolder(Root(), L"Choose a folder for the new repository");
    if (!dir) return;
    Git g = git_;
    g.root = *dir;
    ProcResult r = g.Run({L"init"}, nullptr, true);
    if (!r.ok()) {
        ShowGitError(L"git init", r);
        return;
    }
    OpenRepo(*dir);
}

void MainWindow::DoCommit() {
    if (mode_ != Mode::WorkDir) list_.Select(CommitList::kWorkdir, true);
    std::wstring message = TrimW(GetText(msg_));
    if (message.empty() && !amend_) {
        MessageBoxW(Root(), L"Please enter a commit message.", kAppName, MB_ICONINFORMATION);
        SetFocus(msg_);
        return;
    }
    if (staged_.empty() && !amend_ && opState_.empty()) {
        if (unstaged_.empty()) {
            MessageBoxW(Root(), L"There is nothing to commit.", kAppName, MB_ICONINFORMATION);
            return;
        }
        if (!Confirm(Root(), L"No changes are staged.\n\nStage all changes and commit them?", L"Commit")) return;
        ProcResult r = RunLogged({L"add", L"-A"});
        if (!r.ok()) {
            ShowGitError(L"git add", r);
            return;
        }
    }
    std::vector<std::wstring> args = {L"commit"};
    std::string input;
    if (message.empty()) {
        args.push_back(L"--no-edit");
    } else {
        args.insert(args.end(), {L"-F", L"-"});
        input = ToLf(Narrow(message)) + "\n";
    }
    if (amend_) args.push_back(L"--amend");
    if (Exec(args, input.empty() ? nullptr : &input)) {
        SetWindowTextW(msg_, L"");
        amend_ = false;
        statusMsg_ = L"Committed";
        InvalidateRect(btnAmend_, nullptr, FALSE);
        Layout();
    }
}

void MainWindow::ToggleAmend() {
    if (headHash_.empty()) {
        MessageBoxW(Root(), L"There is no previous commit to amend.", kAppName, MB_ICONINFORMATION);
        return;
    }
    amend_ = !amend_;
    std::wstring headMsg = ToCrlf(Widen(git_.CommitMessage("HEAD")));
    if (amend_ && GetWindowTextLengthW(msg_) == 0) SetWindowTextW(msg_, headMsg.c_str());
    if (!amend_ && GetText(msg_) == headMsg) SetWindowTextW(msg_, L"");
    InvalidateRect(btnAmend_, nullptr, FALSE);
    if (mode_ != Mode::WorkDir) list_.Select(CommitList::kWorkdir, true);
    Layout();
}

void MainWindow::DoPush(bool force) {
    if (headBranch_.empty()) {
        MessageBoxW(Root(), L"HEAD is detached; check out a branch before pushing.", kAppName, MB_ICONINFORMATION);
        return;
    }
    const RefInfo* cur = CurrentRef();
    std::vector<std::wstring> args = {L"push"};
    if (force) {
        if (!Confirm(Root(), L"Force push " + Widen(headBranch_) +
                               L"?\n\nThis rewrites the remote branch (uses --force-with-lease).",
                     L"Force Push"))
            return;
        args.push_back(L"--force-with-lease");
    }
    if (!cur || cur->upstream.empty()) {
        std::string remote = DefaultRemote();
        if (remote.empty()) {
            MessageBoxW(Root(), L"This repository has no remotes configured.", kAppName, MB_ICONINFORMATION);
            return;
        }
        args.insert(args.end(), {L"-u", Widen(remote), Widen(headBranch_)});
    }
    ExecAsync(force ? L"Force push" : L"Push", args);
}

void MainWindow::StagePaths(const std::vector<std::string>& paths) {
    if (paths.empty()) return;
    std::string in = JoinNul(paths);
    keepDiffScroll_ = true;
    Exec({L"add", L"-A", L"--pathspec-from-file=-", L"--pathspec-file-nul"}, &in);
    keepDiffScroll_ = false;
}

void MainWindow::UnstagePaths(const std::vector<std::string>& paths) {
    if (paths.empty()) return;
    std::string in = JoinNul(paths);
    if (headHash_.empty())
        Exec({L"rm", L"--cached", L"-r", L"-q", L"--pathspec-from-file=-", L"--pathspec-file-nul"}, &in);
    else
        Exec({L"reset", L"-q", L"HEAD", L"--pathspec-from-file=-", L"--pathspec-file-nul"}, &in);
}

static bool RecycleFiles(HWND owner, const std::vector<std::wstring>& files) {
    if (files.empty()) return true;
    std::wstring list;
    for (auto& f : files) {
        list += f;
        list.push_back(L'\0');
    }
    list.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.hwnd = owner;
    op.wFunc = FO_DELETE;
    op.pFrom = list.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0;
}

void MainWindow::DiscardEntries(const std::vector<FileEntry>& entries) {
    if (entries.empty()) return;
    std::wstring what = entries.size() == 1 ? Widen(entries[0].path) : std::to_wstring(entries.size()) + L" files";
    if (!Confirm(Root(), L"Discard all unstaged changes to " + what +
                           L"?\n\nUntracked files are moved to the Recycle Bin. Tracked changes cannot be recovered.",
                 L"Discard Changes"))
        return;
    std::vector<std::string> tracked;
    std::vector<std::wstring> untracked;
    for (auto& e : entries) {
        if (e.untracked)
            untracked.push_back(git_.FullPath(e.path));
        else
            tracked.push_back(e.path);
    }
    if (!RecycleFiles(wnd_, untracked))
        MessageBoxW(Root(), L"Some untracked files could not be removed.", kAppName, MB_ICONWARNING);
    if (!tracked.empty()) {
        std::string in = JoinNul(tracked);
        Exec({L"restore", L"--worktree", L"--pathspec-from-file=-", L"--pathspec-file-nul"}, &in);
    } else {
        Refresh();
    }
}

void MainWindow::DiscardAll() {
    std::vector<FileEntry> entries;
    for (auto& e : unstaged_)
        if (!e.conflict) entries.push_back(e);
    if (entries.empty()) return;
    DiscardEntries(entries);
}

void MainWindow::ApplySelectionPatch(bool cached, bool reverse, bool wholeHunks) {
    std::string patch = diff_.BuildPatch(reverse, wholeHunks);
    if (patch.empty()) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    if (!cached && !Confirm(Root(), L"Discard the selected changes from the working tree?\n\nThis cannot be undone.",
                            L"Discard"))
        return;
    std::vector<std::wstring> args = {L"apply", L"--recount", L"--whitespace=nowarn"};
    if (cached) args.push_back(L"--cached");
    if (reverse) args.push_back(L"-R");
    args.push_back(L"-");
    keepDiffScroll_ = true;
    Exec(args, &patch);
    keepDiffScroll_ = false;
}

static std::wstring SanitizeRefName(std::wstring s) {
    s = TrimW(s);
    for (auto& c : s)
        if (c == L' ') c = L'-';
    return s;
}

void MainWindow::NewBranch(const std::string& startPoint) {
    std::wstring label = startPoint.empty() ? L"New branch name (created from HEAD and checked out):"
                                            : L"New branch name (from " + Widen(startPoint.substr(0, 10)) + L"):";
    auto name = PromptText(Root(), L"Create Branch", label);
    if (!name) return;
    std::wstring n = SanitizeRefName(*name);
    if (n.empty()) return;
    std::vector<std::wstring> args = {L"checkout", L"-b", n};
    if (!startPoint.empty()) args.push_back(Widen(startPoint));
    Exec(args);
}

void MainWindow::NewTag(const std::string& at) {
    auto name = PromptText(Root(), L"Create Tag", L"Tag name:");
    if (!name) return;
    std::wstring n = SanitizeRefName(*name);
    if (n.empty()) return;
    auto message = PromptText(Root(), L"Create Tag", L"Annotation message (leave empty for a lightweight tag):");
    if (!message) return;
    if (TrimW(*message).empty())
        Exec({L"tag", n, Widen(at)});
    else
        Exec({L"tag", L"-a", n, L"-m", TrimW(*message), Widen(at)});
}

void MainWindow::CheckoutRef(const std::string& name, bool remote) {
    if (!remote) {
        Exec({L"checkout", Widen(name)});
        return;
    }
    std::string local = name.substr(name.find('/') + 1);
    if (FindRef(local, RefInfo::Local))
        Exec({L"checkout", Widen(local)});
    else
        Exec({L"checkout", L"--track", Widen(name)});
}

void MainWindow::DeleteLocalBranch(const std::string& name) {
    if (!Confirm(Root(), L"Delete local branch '" + Widen(name) + L"'?", L"Delete Branch")) return;
    ProcResult r = RunLogged({L"branch", L"-d", Widen(name)});
    if (!r.ok()) {
        if (r.err.find("not fully merged") != std::string::npos) {
            if (Confirm(Root(), L"Branch '" + Widen(name) +
                                  L"' is not fully merged.\n\nForce delete it? Its unmerged commits may be lost.",
                        L"Force Delete Branch"))
                Exec({L"branch", L"-D", Widen(name)}, nullptr, false);
        } else {
            ShowGitError(L"git branch -d", r);
        }
    }
    Refresh();
}

int MainWindow::PickBranch(const std::wstring& title, bool includeRemotes, bool excludeCurrent, std::string& name,
                           bool& remote) {
    std::vector<std::wstring> items;
    std::vector<const RefInfo*> refs;
    for (const auto& r : refs_) {
        if (r.kind == RefInfo::Tag) continue;
        if (r.kind == RefInfo::Remote && !includeRemotes) continue;
        if (excludeCurrent && r.current) continue;
        items.push_back((r.kind == RefInfo::Remote ? L"remote   " : L"local    ") + Widen(r.name));
        refs.push_back(&r);
    }
    if (items.empty()) {
        MessageBoxW(Root(), L"No other branches.", kAppName, MB_ICONINFORMATION);
        return -1;
    }
    int i = PickFromList(Root(), title, items);
    if (i < 0) return -1;
    name = refs[i]->name;
    remote = refs[i]->kind == RefInfo::Remote;
    return i;
}

void MainWindow::ContinueOrAbort(bool abort) {
    if (opState_.empty()) {
        MessageBoxW(Root(), L"No merge, rebase, cherry-pick or revert is in progress.", kAppName, MB_ICONINFORMATION);
        return;
    }
    std::wstring verb = opState_ == L"MERGING"          ? L"merge"
                        : opState_ == L"REBASING"       ? L"rebase"
                        : opState_ == L"CHERRY-PICKING" ? L"cherry-pick"
                                                        : L"revert";
    if (abort) {
        if (!Confirm(Root(), L"Abort the " + verb + L" in progress and restore the previous state?", L"Abort")) return;
        Exec({verb, L"--abort"});
    } else if (verb == L"merge") {
        DoCommit();
    } else {
        Exec({verb, L"--continue"});
    }
}

void MainWindow::ShowPalette() {
    std::vector<std::wstring> items;
    std::vector<int> ids;
    for (const auto& c : kCommands) {
        std::wstring s = c.name;
        if (*c.keys) s += std::wstring(L"      ") + c.keys;
        items.push_back(s);
        ids.push_back(c.id);
    }
    for (size_t i = 0; i < recent_.size(); ++i) {
        items.push_back(L"Open Recent: " + recent_[i]);
        ids.push_back(IDM_RECENT_BASE + (int)i);
    }
    int i = PickFromList(Root(), L"Command Palette", items);
    if (i >= 0) PostMessageW(wnd_, WM_COMMAND, MAKEWPARAM(ids[i], 0), 0);
}

void MainWindow::ShowFileHistory(const std::string& path) {
    pathFilter_ = Widen(path);
    SetWindowTextW(search_, L"");
    Refresh();
    SetFocus(list_.hwnd());
}

void MainWindow::ShowBlame(const std::string& path, const std::string& rev) {
    std::vector<std::wstring> args = {L"blame", L"--date=short"};
    if (!rev.empty()) args.push_back(Widen(rev));
    args.insert(args.end(), {L"--", Widen(path)});
    ProcResult r = RunLogged(args);
    if (!r.ok()) {
        ShowGitError(L"git blame", r);
        return;
    }
    diffSource_ = DiffSource::Text;
    diff_.SetText(L"Blame: " + Widen(path) + (rev.empty() ? L"" : L" @ " + Widen(rev.substr(0, 10))) + L"\n\n" +
                  Widen(r.out));
}

void MainWindow::OpenFile(const std::string& path) {
    if (cb_.onOpenFile) cb_.onOpenFile(git_.FullPath(path));
}

std::string MainWindow::RepoRelative(const std::wstring& absolutePath) const {
    std::wstring full = absolutePath;
    if (DWORD need = GetFullPathNameW(absolutePath.c_str(), 0, nullptr, nullptr)) {
        std::wstring buf(need, L'\0');
        DWORD n = GetFullPathNameW(absolutePath.c_str(), need, buf.data(), nullptr);
        if (n > 0 && n < need) full.assign(buf.data(), n);
    }
    const std::wstring& root = git_.root;
    if (full.size() <= root.size() || _wcsnicmp(full.c_str(), root.c_str(), root.size()) != 0 ||
        (full[root.size()] != L'\\' && full[root.size()] != L'/'))
        return {};
    std::wstring rel = full.substr(root.size() + 1);
    for (auto& c : rel)
        if (c == L'\\') c = L'/';
    return Narrow(rel);
}

void MainWindow::RevealFile(const std::string& path) {
    std::wstring arg = L"/select,\"" + git_.FullPath(path) + L"\"";
    ShellExecuteW(wnd_, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}

void MainWindow::AddToGitignore(const std::vector<std::string>& paths) {
    std::wstring file = git_.root + L"\\.gitignore";
    std::string existing;
    {
        std::ifstream in(file, std::ios::binary);
        existing.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (!existing.empty() && existing.back() != '\n') out << "\n";
    for (auto& p : paths) out << "/" << p << "\n";
    out.close();
    Refresh();
}

// ---------------------------------------------------------------------------
// Context menus

void MainWindow::ContextFileList(HWND lv, POINT pt) {
    bool stagedList = lv == lvStaged_;
    auto sel = SelectedEntries(lv, stagedList ? staged_ : unstaged_);
    if (sel.empty()) return;
    bool anyConflict = std::any_of(sel.begin(), sel.end(), [](auto& e) { return e.conflict; });
    bool allUntracked = std::all_of(sel.begin(), sel.end(), [](auto& e) { return e.untracked; });
    HMENU m = CreatePopupMenu();
    if (stagedList) {
        AppendMenuW(m, MF_STRING, CM_UNSTAGE, L"Unstage");
    } else {
        AppendMenuW(m, MF_STRING, CM_STAGE, anyConflict ? L"Stage (mark resolved)" : L"Stage");
        AppendMenuW(m, MF_STRING, CM_DISCARD, L"Discard Changes...");
    }
    if (anyConflict) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CM_RESOLVE_OURS, L"Resolve Using Ours (current branch)");
        AppendMenuW(m, MF_STRING, CM_RESOLVE_THEIRS, L"Resolve Using Theirs (incoming)");
        AppendMenuW(m, MF_STRING, CM_MARK_RESOLVED, L"Mark Resolved");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CM_OPEN_FILE, L"Open File");
    AppendMenuW(m, MF_STRING, CM_SHOW_IN_EXPLORER, L"Show in Explorer");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (allUntracked ? MF_GRAYED : 0), CM_FILE_HISTORY, L"File History");
    AppendMenuW(m, MF_STRING | (allUntracked ? MF_GRAYED : 0), CM_BLAME, L"Blame");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (allUntracked) AppendMenuW(m, MF_STRING, CM_IGNORE, L"Add to .gitignore");
    AppendMenuW(m, MF_STRING, CM_COPY_PATH, L"Copy Path");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, wnd_, nullptr);
    DestroyMenu(m);

    std::vector<std::string> paths;
    for (auto& e : sel) {
        paths.push_back(e.path);
        if (stagedList && !e.origPath.empty()) paths.push_back(e.origPath);
    }
    const std::string& first = sel.front().path;
    switch (cmd) {
    case CM_STAGE:
    case CM_MARK_RESOLVED: StagePaths(paths); break;
    case CM_UNSTAGE: UnstagePaths(paths); break;
    case CM_DISCARD: DiscardEntries(sel); break;
    case CM_RESOLVE_OURS:
    case CM_RESOLVE_THEIRS: {
        std::string in = JoinNul(paths);
        ProcResult r = RunLogged({L"checkout", cmd == CM_RESOLVE_OURS ? L"--ours" : L"--theirs",
                                  L"--pathspec-from-file=-", L"--pathspec-file-nul"},
                                 &in);
        if (!r.ok())
            ShowGitError(L"git checkout", r);
        else
            StagePaths(paths);
        break;
    }
    case CM_OPEN_FILE: OpenFile(first); break;
    case CM_SHOW_IN_EXPLORER: RevealFile(first); break;
    case CM_FILE_HISTORY: ShowFileHistory(first); break;
    case CM_BLAME: ShowBlame(first, ""); break;
    case CM_IGNORE: AddToGitignore(paths); break;
    case CM_COPY_PATH: {
        std::wstring text;
        for (auto& p : paths) text += Widen(p) + L"\r\n";
        CopyToClipboard(wnd_, TrimW(text));
        break;
    }
    }
}

void MainWindow::ContextCommitFiles(POINT pt) {
    int i = ListView_GetNextItem(lvFiles_, -1, LVNI_SELECTED);
    int ci = list_.Selected();
    if (i < 0 || i >= (int)commitFiles_.size() || ci < 0) return;
    const ChangedFile& f = commitFiles_[i];
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CM_OPEN_FILE, L"Open File (working copy)");
    AppendMenuW(m, MF_STRING, CM_SHOW_IN_EXPLORER, L"Show in Explorer");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CM_FILE_HISTORY, L"File History");
    AppendMenuW(m, MF_STRING | (f.status == "D" ? MF_GRAYED : 0), CM_BLAME, L"Blame at This Commit");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CM_COPY_PATH, L"Copy Path");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, wnd_, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case CM_OPEN_FILE: OpenFile(f.path); break;
    case CM_SHOW_IN_EXPLORER: RevealFile(f.path); break;
    case CM_FILE_HISTORY: ShowFileHistory(f.path); break;
    case CM_BLAME: ShowBlame(f.path, commits_[ci].hash); break;
    case CM_COPY_PATH: CopyToClipboard(wnd_, Widen(f.path)); break;
    }
}

void MainWindow::ContextDiff(POINT pt) {
    bool inHunk = diff_.CursorInHunk();
    bool multi = diff_.HasMultiLineSelection();
    std::string path = diff_.CursorFilePath();
    HMENU m = CreatePopupMenu();
    const wchar_t* lines = multi ? L" Selected Lines" : L" Line";
    if (diffSource_ == DiffSource::Unstaged && inHunk) {
        AppendMenuW(m, MF_STRING, CM_STAGE_HUNK, L"Stage Hunk");
        AppendMenuW(m, MF_STRING, CM_STAGE_LINES, (std::wstring(L"Stage") + lines).c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CM_DISCARD_HUNK, L"Discard Hunk...");
        AppendMenuW(m, MF_STRING, CM_DISCARD_LINES, (std::wstring(L"Discard") + lines + L"...").c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    } else if (diffSource_ == DiffSource::Staged && inHunk) {
        AppendMenuW(m, MF_STRING, CM_UNSTAGE_HUNK, L"Unstage Hunk");
        AppendMenuW(m, MF_STRING, CM_UNSTAGE_LINES, (std::wstring(L"Unstage") + lines).c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(m, MF_STRING, CM_COPY_TEXT, L"Copy\tCtrl+C");
    if (!path.empty()) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CM_OPEN_FILE, L"Open File");
        AppendMenuW(m, MF_STRING, CM_FILE_HISTORY, L"File History");
        AppendMenuW(m, MF_STRING, CM_BLAME, L"Blame");
    }
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, wnd_, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case CM_STAGE_HUNK: ApplySelectionPatch(true, false, true); break;
    case CM_STAGE_LINES: ApplySelectionPatch(true, false, false); break;
    case CM_UNSTAGE_HUNK: ApplySelectionPatch(true, true, true); break;
    case CM_UNSTAGE_LINES: ApplySelectionPatch(true, true, false); break;
    case CM_DISCARD_HUNK: ApplySelectionPatch(false, true, true); break;
    case CM_DISCARD_LINES: ApplySelectionPatch(false, true, false); break;
    case CM_COPY_TEXT: CopyToClipboard(wnd_, diff_.SelectedText()); break;
    case CM_OPEN_FILE: OpenFile(path); break;
    case CM_FILE_HISTORY: ShowFileHistory(path); break;
    case CM_BLAME: {
        int ci = list_.Selected();
        ShowBlame(path, mode_ == Mode::Commit && ci >= 0 ? commits_[ci].hash : "");
        break;
    }
    }
}

void MainWindow::ContextCommit(POINT pt) {
    int item = list_.ItemAtScreenPoint(pt);
    if (pt.x == -1 || item == CommitList::kNone) item = list_.Selected();
    if (item == CommitList::kWorkdir) {
        HMENU m = CreatePopupMenu();
        for (int id : std::initializer_list<int>{IDM_STAGE_ALL, IDM_UNSTAGE_ALL, IDM_DISCARD_ALL, 0, IDM_STASH, IDM_STASH_UNTRACKED}) {
            if (!id) {
                AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
                continue;
            }
            AppendMenuW(m, MF_STRING, id, FindCommand(id)->name);
        }
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, wnd_, nullptr);
        DestroyMenu(m);
        if (cmd) OnCommand(cmd);
        return;
    }
    if (item < 0) return;
    const Commit c = commits_[item];
    bool merge = c.parents.size() > 1;
    std::wstring shortHash = Widen(c.hash.substr(0, 7));
    HMENU m = CreatePopupMenu();
    HMENU reset = CreatePopupMenu();
    AppendMenuW(reset, MF_STRING, CM_RESET_SOFT, L"Soft (keep index and working tree)");
    AppendMenuW(reset, MF_STRING, CM_RESET_MIXED, L"Mixed (keep working tree)");
    AppendMenuW(reset, MF_STRING, CM_RESET_HARD, L"Hard (discard all changes)...");
    AppendMenuW(m, MF_STRING, CM_CHECKOUT_COMMIT, (L"Checkout " + shortHash).c_str());
    AppendMenuW(m, MF_STRING, CM_BRANCH_HERE, L"Create Branch Here...");
    AppendMenuW(m, MF_STRING, CM_TAG_HERE, L"Create Tag Here...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CM_CHERRY_PICK, L"Cherry-pick Commit");
    AppendMenuW(m, MF_STRING, CM_REVERT, L"Revert Commit");
    AppendMenuW(m, MF_POPUP | (headBranch_.empty() ? MF_GRAYED : 0), (UINT_PTR)reset,
                (L"Reset " + (headBranch_.empty() ? std::wstring(L"HEAD") : Widen(headBranch_)) + L" to This Commit")
                    .c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CM_COPY_HASH, L"Copy Commit Hash");
    AppendMenuW(m, MF_STRING, CM_COPY_SUBJECT, L"Copy Commit Message");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, wnd_, nullptr);
    DestroyMenu(m);
    std::wstring h = Widen(c.hash);
    switch (cmd) {
    case CM_CHECKOUT_COMMIT: {
        for (const auto& b : c.badges)
            if (b.kind == RefBadge::Local) {
                Exec({L"checkout", b.name});
                return;
            }
        if (Confirm(Root(), L"Check out commit " + shortHash +
                              L"?\n\nThis puts the repository in 'detached HEAD' state.",
                    L"Checkout Commit"))
            Exec({L"checkout", h});
        break;
    }
    case CM_BRANCH_HERE: NewBranch(c.hash); break;
    case CM_TAG_HERE: NewTag(c.hash); break;
    case CM_CHERRY_PICK:
        if (merge)
            Exec({L"cherry-pick", L"-m", L"1", h});
        else
            Exec({L"cherry-pick", h});
        break;
    case CM_REVERT:
        if (merge)
            Exec({L"revert", L"--no-edit", L"-m", L"1", h});
        else
            Exec({L"revert", L"--no-edit", h});
        break;
    case CM_RESET_SOFT: Exec({L"reset", L"--soft", h}); break;
    case CM_RESET_MIXED: Exec({L"reset", L"--mixed", h}); break;
    case CM_RESET_HARD:
        if (Confirm(Root(), L"Hard reset to " + shortHash +
                              L"?\n\nAll uncommitted changes will be permanently lost.",
                    L"Hard Reset"))
            Exec({L"reset", L"--hard", h});
        break;
    case CM_COPY_HASH: CopyToClipboard(wnd_, h); break;
    case CM_COPY_SUBJECT: CopyToClipboard(wnd_, ToCrlf(Widen(git_.CommitMessage(c.hash)))); break;
    }
}

void MainWindow::ContextSidebar(POINT pt) {
    HTREEITEM hit = nullptr;
    if (pt.x == -1 && pt.y == -1) {
        hit = TreeView_GetSelection(tree_);
        RECT rc;
        GetWindowRect(tree_, &rc);
        pt = {rc.left + S(40), rc.top + S(40)};
    } else {
        TVHITTESTINFO ht{};
        ht.pt = pt;
        ScreenToClient(tree_, &ht.pt);
        hit = TreeView_HitTest(tree_, &ht);
        if (hit) TreeView_SelectItem(tree_, hit);
    }
    if (!hit) return;
    TVITEMW it{};
    it.mask = TVIF_PARAM;
    it.hItem = hit;
    TreeView_GetItem(tree_, &it);
    if (it.lParam < 0 || it.lParam >= (LPARAM)sideItems_.size()) return;
    const SideItem item = sideItems_[it.lParam];
    std::wstring name = Widen(item.name);
    HMENU m = CreatePopupMenu();
    switch (item.kind) {
    case SideItem::Local: {
        bool current = item.name == headBranch_;
        AppendMenuW(m, MF_STRING | (current ? MF_GRAYED : 0), CM_REF_CHECKOUT, (L"Checkout " + name).c_str());
        AppendMenuW(m, MF_STRING | (current ? MF_GRAYED : 0), CM_REF_MERGE,
                    (L"Merge " + name + L" into " + Widen(headBranch_)).c_str());
        AppendMenuW(m, MF_STRING | (current ? MF_GRAYED : 0), CM_REF_REBASE,
                    (L"Rebase " + Widen(headBranch_) + L" onto " + name).c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CM_REF_PUSH, (L"Push " + name).c_str());
        AppendMenuW(m, MF_STRING, CM_BRANCH_HERE, L"Create Branch From Here...");
        AppendMenuW(m, MF_STRING, CM_REF_RENAME, L"Rename...");
        AppendMenuW(m, MF_STRING | (current ? MF_GRAYED : 0), CM_REF_DELETE, L"Delete...");
        break;
    }
    case SideItem::Remote:
        AppendMenuW(m, MF_STRING, CM_REF_CHECKOUT, (L"Checkout " + name).c_str());
        AppendMenuW(m, MF_STRING, CM_REF_MERGE, (L"Merge " + name + L" into " + Widen(headBranch_)).c_str());
        AppendMenuW(m, MF_STRING, CM_REF_REBASE, (L"Rebase " + Widen(headBranch_) + L" onto " + name).c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CM_BRANCH_HERE, L"Create Branch From Here...");
        AppendMenuW(m, MF_STRING, CM_REF_DELETE_REMOTE, L"Delete Remote Branch...");
        break;
    case SideItem::RemoteGroup:
        AppendMenuW(m, MF_STRING, CM_REMOTE_FETCH, (L"Fetch " + name).c_str());
        break;
    case SideItem::Tag:
        AppendMenuW(m, MF_STRING, CM_REF_CHECKOUT, (L"Checkout " + name).c_str());
        AppendMenuW(m, MF_STRING, CM_TAG_PUSH, (L"Push Tag " + name).c_str());
        AppendMenuW(m, MF_STRING, CM_TAG_DELETE, L"Delete Tag...");
        break;
    case SideItem::Stash:
        AppendMenuW(m, MF_STRING, CM_STASH_APPLY, L"Apply Stash");
        AppendMenuW(m, MF_STRING, CM_STASH_POP, L"Pop Stash");
        AppendMenuW(m, MF_STRING, CM_STASH_DROP, L"Drop Stash...");
        break;
    case SideItem::Header:
        if (item.name == "BRANCHES") AppendMenuW(m, MF_STRING, IDM_NEW_BRANCH, L"Create Branch...");
        if (item.name == "REMOTES") AppendMenuW(m, MF_STRING, IDM_FETCH, L"Fetch All");
        if (item.name == "TAGS") AppendMenuW(m, MF_STRING, IDM_NEW_TAG, L"Create Tag at HEAD...");
        if (item.name == "STASHES") AppendMenuW(m, MF_STRING, IDM_STASH, L"Stash Changes");
        break;
    }
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, wnd_, nullptr);
    DestroyMenu(m);
    if (cmd >= IDM_OPEN && cmd < IDM_RECENT_BASE) {
        OnCommand(cmd);
        return;
    }
    if (cmd && busy_) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    switch (cmd) {
    case CM_REF_CHECKOUT:
        if (item.kind == SideItem::Tag) {
            if (Confirm(Root(), L"Check out tag " + name + L"?\n\nThis puts the repository in 'detached HEAD' state.",
                        L"Checkout Tag"))
                Exec({L"checkout", L"refs/tags/" + name});
        } else {
            CheckoutRef(item.name, item.kind == SideItem::Remote);
        }
        break;
    case CM_REF_MERGE: Exec({L"merge", L"--no-edit", name}); break;
    case CM_REF_REBASE: Exec({L"rebase", name}); break;
    case CM_REF_PUSH: {
        const RefInfo* r = FindRef(item.name, RefInfo::Local);
        std::string remote = DefaultRemote();
        if (r && !r->upstream.empty()) {
            std::string up = r->upstream;
            std::string upRemote = up.substr(0, up.find('/'));
            std::string upBranch = up.substr(up.find('/') + 1);
            ExecAsync(L"Push " + name, {L"push", Widen(upRemote), name + L":" + Widen(upBranch)});
        } else if (!remote.empty()) {
            ExecAsync(L"Push " + name, {L"push", L"-u", Widen(remote), name});
        } else {
            MessageBoxW(Root(), L"This repository has no remotes configured.", kAppName, MB_ICONINFORMATION);
        }
        break;
    }
    case CM_BRANCH_HERE: NewBranch(item.target); break;
    case CM_REF_RENAME: {
        auto n = PromptText(Root(), L"Rename Branch", L"New name for '" + name + L"':", name);
        if (n && !SanitizeRefName(*n).empty()) Exec({L"branch", L"-m", name, SanitizeRefName(*n)});
        break;
    }
    case CM_REF_DELETE: DeleteLocalBranch(item.name); break;
    case CM_REF_DELETE_REMOTE: {
        std::string remote = item.name.substr(0, item.name.find('/'));
        std::string branch = item.name.substr(item.name.find('/') + 1);
        if (Confirm(Root(), L"Delete branch '" + Widen(branch) + L"' from remote '" + Widen(remote) + L"'?",
                    L"Delete Remote Branch"))
            ExecAsync(L"Delete " + name, {L"push", Widen(remote), L"--delete", Widen(branch)});
        break;
    }
    case CM_REMOTE_FETCH: ExecAsync(L"Fetch " + name, {L"fetch", L"--prune", name}); break;
    case CM_TAG_PUSH: {
        std::string remote = DefaultRemote();
        if (remote.empty())
            MessageBoxW(Root(), L"This repository has no remotes configured.", kAppName, MB_ICONINFORMATION);
        else
            ExecAsync(L"Push tag " + name, {L"push", Widen(remote), L"refs/tags/" + name});
        break;
    }
    case CM_TAG_DELETE:
        if (Confirm(Root(), L"Delete local tag '" + name + L"'?", L"Delete Tag")) Exec({L"tag", L"-d", name});
        break;
    case CM_STASH_APPLY: Exec({L"stash", L"apply", name}); break;
    case CM_STASH_POP: Exec({L"stash", L"pop", name}); break;
    case CM_STASH_DROP:
        if (Confirm(Root(), L"Drop " + name + L"? The stashed changes will be lost.", L"Drop Stash"))
            Exec({L"stash", L"drop", name});
        break;
    }
}

// ---------------------------------------------------------------------------
// Recent repositories

void MainWindow::LoadRecent() {
    std::ifstream in(RecentFilePath(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        std::wstring p = Widen(Trim(line));
        if (!p.empty() && std::find(recent_.begin(), recent_.end(), p) == recent_.end()) recent_.push_back(p);
    }
}

void MainWindow::SaveRecent() {
    std::ofstream out(RecentFilePath(), std::ios::binary | std::ios::trunc);
    for (auto& p : recent_) out << Narrow(p) << "\n";
}

void MainWindow::AddRecent(const std::wstring& path) {
    recent_.erase(std::remove(recent_.begin(), recent_.end(), path), recent_.end());
    recent_.insert(recent_.begin(), path);
    if (recent_.size() > 20) recent_.resize(20);
    SaveRecent();
    RebuildRecentMenu();
}

// ---------------------------------------------------------------------------
// Window procedure

LRESULT CALLBACK MainWindow::SearchSubclass(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* self = (MainWindow*)ref;
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_DOWN)) {
        SetFocus(self->list_.hwnd());
        return 0;
    }
    if (msg == WM_CHAR && wp == L'\r') return 0;
    return DefSubclassProc(wnd, msg, wp, lp);
}

LRESULT CALLBACK MainWindow::StaticProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self;
    if (msg == WM_NCCREATE) {
        self = (MainWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
        self->wnd_ = wnd;
        SetWindowLongPtrW(wnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (MainWindow*)GetWindowLongPtrW(wnd, GWLP_USERDATA);
    }
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(wnd, msg, wp, lp);
}

LRESULT MainWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        CreateControls();  // theme::Init already ran in MergeView::Create
        return 0;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_DPICHANGED_AFTERPARENT: {
        int oldDpi = theme::dpi;
        theme::Init(GetDpiForWindow(wnd_));
        sidebarW_ = MulDiv(sidebarW_, theme::dpi, oldDpi);
        listW_ = MulDiv(listW_, theme::dpi, oldDpi);
        ApplyFonts();
        Layout();
        InvalidateRect(wnd_, nullptr, TRUE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd_, &ps);
        Paint(dc);
        EndPaint(wnd_, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, kText);
        SetBkColor((HDC)wp, kInput);
        return (LRESULT)inputBrush;
    case WM_DRAWITEM: {
        auto* dis = (const DRAWITEMSTRUCT*)lp;
        DrawOwnerButton(dis, dis->CtlID == IDC_BTN_AMEND && amend_);
        return TRUE;
    }
    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(wnd_, &pt);
        if ((HWND)wp == wnd_ && (PtInRect(&split1_, pt) || PtInRect(&split2_, pt))) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        drag_ = PtInRect(&split1_, pt) ? 1 : PtInRect(&split2_, pt) ? 2 : 0;
        if (drag_) SetCapture(wnd_);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (drag_) {
            int x = GET_X_LPARAM(lp);
            if (drag_ == 1)
                sidebarW_ = x;
            else
                listW_ = x - (sidebarVisible_ ? sidebarW_ + S(5) : 0);
            Layout();
        }
        return 0;
    case WM_LBUTTONUP:
        if (drag_) {
            drag_ = 0;
            ReleaseCapture();
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case IDC_COMMITS:
            if (code == CommitList::kSelChange) OnSelectionChanged();
            return 0;
        case IDC_SEARCH:
            if (code == EN_CHANGE) {
                ApplyFilter();
                if (list_.Selected() == CommitList::kNone) OnSelectionChanged();
            }
            return 0;
        case IDC_BTN_COMMIT: OnCommand(IDM_COMMIT); return 0;
        case IDC_BTN_AMEND: OnCommand(IDM_AMEND_TOGGLE); return 0;
        case IDC_BTN_STAGEALL: OnCommand(IDM_STAGE_ALL); return 0;
        case IDC_BTN_UNSTAGEALL: OnCommand(IDM_UNSTAGE_ALL); return 0;
        case IDC_BTN_DISCARDALL: OnCommand(IDM_DISCARD_ALL); return 0;
        case IDC_MSG:
        case IDC_DETAILS:
        case IDC_CONSOLE:
            return 0;
        }
        if (lp == 0 || code == BN_CLICKED) OnCommand(id);
        return 0;
    }
    case WM_NOTIFY: {
        auto* hdr = (NMHDR*)lp;
        if (hdr->idFrom == IDC_SIDEBAR) {
            if (hdr->code == TVN_SELCHANGEDW && !filling_) {
                auto* tv = (NMTREEVIEWW*)lp;
                LPARAM i = tv->itemNew.lParam;
                if (i >= 0 && i < (LPARAM)sideItems_.size()) {
                    const SideItem& item = sideItems_[i];
                    if (item.kind == SideItem::Stash) {
                        ProcResult r = git_.Run({L"stash", L"show", L"-p", L"--include-untracked", Widen(item.name)});
                        if (!r.ok()) r = git_.Run({L"stash", L"show", L"-p", Widen(item.name)});
                        mode_ = Mode::None;
                        diffSource_ = DiffSource::Text;
                        Layout();
                        diff_.SetDiff(r.out, false);
                    } else if (!item.target.empty()) {
                        for (int k = 0; k < (int)commits_.size(); ++k)
                            if (commits_[k].hash == item.target) {
                                list_.Select(k, true);
                                break;
                            }
                    }
                }
            } else if (hdr->code == TVN_ITEMEXPANDEDW && !filling_) {
                auto* tv = (NMTREEVIEWW*)lp;
                wchar_t text[256] = {};
                TVITEMW it{};
                it.mask = TVIF_TEXT;
                it.hItem = tv->itemNew.hItem;
                it.pszText = text;
                it.cchTextMax = 256;
                TreeView_GetItem(tree_, &it);
                collapsed_[text] = tv->action == TVE_COLLAPSE;
            } else if (hdr->code == NM_DBLCLK) {
                HTREEITEM sel = TreeView_GetSelection(tree_);
                TVITEMW it{};
                it.mask = TVIF_PARAM;
                it.hItem = sel;
                if (sel && TreeView_GetItem(tree_, &it) && it.lParam < (LPARAM)sideItems_.size() && !busy_) {
                    const SideItem item = sideItems_[it.lParam];
                    if (item.kind == SideItem::Local && item.name != headBranch_) {
                        CheckoutRef(item.name, false);
                        return TRUE;
                    }
                    if (item.kind == SideItem::Remote) {
                        CheckoutRef(item.name, true);
                        return TRUE;
                    }
                }
            }
            return 0;
        }
        if (hdr->idFrom == IDC_UNSTAGED || hdr->idFrom == IDC_STAGED || hdr->idFrom == IDC_FILES) {
            if (hdr->code == NM_CUSTOMDRAW) {
                auto* cd = (NMLVCUSTOMDRAW*)lp;
                switch (cd->nmcd.dwDrawStage) {
                case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
                case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
                case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
                    wchar_t t[4] = {};
                    if (cd->iSubItem == 0) {
                        ListView_GetItemText(hdr->hwndFrom, (int)cd->nmcd.dwItemSpec, 0, t, 4);
                        cd->clrText = StatusColor(t[0]);
                    } else {
                        cd->clrText = kText;
                    }
                    return CDRF_DODEFAULT;
                }
                }
                return CDRF_DODEFAULT;
            }
            if (hdr->code == LVN_ITEMCHANGED && !filling_) {
                auto* lv = (NMLISTVIEW*)lp;
                if (!(lv->uChanged & LVIF_STATE) || ((lv->uOldState ^ lv->uNewState) & LVIS_SELECTED) == 0) return 0;
                if (hdr->idFrom == IDC_FILES) {
                    if ((lv->uNewState & LVIS_SELECTED) && lv->lParam >= 0 &&
                        lv->lParam < (LPARAM)commitFiles_.size())
                        diff_.ScrollToFile(commitFiles_[lv->lParam].path);
                    return 0;
                }
                if (lv->uNewState & LVIS_SELECTED) {
                    HWND other = hdr->idFrom == IDC_UNSTAGED ? lvStaged_ : lvUnstaged_;
                    filling_ = true;
                    ListView_SetItemState(other, -1, 0, LVIS_SELECTED);
                    filling_ = false;
                }
                if (!diffPending_) {
                    diffPending_ = true;
                    PostMessageW(wnd_, WM_APP_UPDATEDIFF, 0, 0);
                }
                return 0;
            }
            if (hdr->code == NM_DBLCLK && hdr->idFrom != IDC_FILES && !busy_) {
                bool staged = hdr->idFrom == IDC_STAGED;
                auto sel = SelectedEntries(hdr->hwndFrom, staged ? staged_ : unstaged_);
                std::vector<std::string> paths;
                for (auto& e : sel) {
                    paths.push_back(e.path);
                    if (staged && !e.origPath.empty()) paths.push_back(e.origPath);
                }
                staged ? UnstagePaths(paths) : StagePaths(paths);
                return 0;
            }
            if (hdr->code == LVN_KEYDOWN && hdr->idFrom != IDC_FILES && !busy_) {
                auto* kd = (NMLVKEYDOWN*)lp;
                bool staged = hdr->idFrom == IDC_STAGED;
                if (kd->wVKey == VK_SPACE || kd->wVKey == VK_RETURN) {
                    std::vector<std::string> paths;
                    for (auto& e : SelectedEntries(hdr->hwndFrom, staged ? staged_ : unstaged_)) {
                        paths.push_back(e.path);
                        if (staged && !e.origPath.empty()) paths.push_back(e.origPath);
                    }
                    staged ? UnstagePaths(paths) : StagePaths(paths);
                } else if (kd->wVKey == VK_DELETE && !staged) {
                    DiscardEntries(SelectedEntries(hdr->hwndFrom, unstaged_));
                }
                return 0;
            }
        }
        break;
    }
    case WM_CONTEXTMENU: {
        HWND src = (HWND)wp;
        if (!repoOpen_) return 0;
        if (src == lvUnstaged_ || src == lvStaged_)
            ContextFileList(src, MenuPoint(lp, src));
        else if (src == lvFiles_)
            ContextCommitFiles(MenuPoint(lp, src));
        else if (src == diff_.hwnd())
            ContextDiff(MenuPoint(lp, src));
        else if (src == list_.hwnd())
            ContextCommit(MenuPoint(lp, src));
        else if (src == tree_)
            ContextSidebar(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        else
            break;
        return 0;
    }
    case WM_APP_UPDATEDIFF:
        diffPending_ = false;
        if (mode_ == Mode::WorkDir) ShowWorkDiff(false);
        return 0;
    case WM_APP_LOG: {
        std::unique_ptr<std::wstring> s((std::wstring*)lp);
        AppendConsole(*s);
        return 0;
    }
    case WM_APP_TASKDONE: {
        std::unique_ptr<Task> t((Task*)lp);
        busy_ = false;
        if (t->result.ok()) {
            statusMsg_ = t->label + L" — done";
        } else {
            statusMsg_ = t->label + L" — failed";
            ShowGitError(t->label, t->result);
        }
        if (t->done) t->done(t->result);
        if (repoOpen_) Refresh();
        InvalidateRect(wnd_, &statusRc_, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(wnd_, msg, wp, lp);
}

}  // namespace

// ---------------------------------------------------------------------------
// MergeView: the embeddable wrapper used by Verso.

struct MergeView::Impl {
    MainWindow win;
};

std::unique_ptr<MergeView> MergeView::Create(HINSTANCE inst, HWND parent, MergeViewCallbacks callbacks,
                                             std::wstring* error) {
    static const bool initialized = [] {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;  // GDI+ stays up for the rest of the process
        Gdiplus::GdiplusStartup(&token, &input, nullptr);
        return true;
    }();
    (void)initialized;
    auto impl = std::make_unique<Impl>();
    if (!impl->win.Create(inst, parent, std::move(callbacks), error)) return nullptr;
    return std::unique_ptr<MergeView>(new MergeView(std::move(impl)));
}

MergeView::MergeView(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MergeView::~MergeView() {
    if (impl_ && impl_->win.wnd_ && IsWindow(impl_->win.wnd_)) DestroyWindow(impl_->win.wnd_);
    if (impl_ && impl_->win.menu_) DestroyMenu(impl_->win.menu_);
}

HWND MergeView::hwnd() const { return impl_->win.wnd_; }
HMENU MergeView::menu() const { return impl_->win.menu_; }
bool MergeView::busy() const { return impl_->win.busy_; }

bool MergeView::translateAccelerator(MSG* msg) {
    MainWindow& w = impl_->win;
    return w.accel_ && TranslateAcceleratorW(w.wnd_, w.accel_, msg) != 0;
}

void MergeView::onMenuCommand(WPARAM wp, LPARAM lp) { SendMessageW(impl_->win.wnd_, WM_COMMAND, wp, lp); }

void MergeView::activate() {
    MainWindow& w = impl_->win;
    if (!w.repoOpen_) {
        // Try the recent repositories in order. Unavailable ones stay listed: a
        // removable or network drive may come back.
        const std::vector<std::wstring> recent = w.recent_;
        for (const auto& r : recent)
            if (w.OpenRepo(r, true)) break;
    } else if (!w.busy_) {
        w.Refresh();
    }
    SetFocus(w.list_.hwnd());
}

bool MergeView::openRepository(const std::wstring& path) {
    MainWindow& w = impl_->win;
    Git probe = w.git_;
    std::wstring err;
    if (!probe.Open(path, err)) return false;
    if (w.repoOpen_ && _wcsicmp(probe.root.c_str(), w.git_.root.c_str()) == 0) {
        if (!w.busy_) w.Refresh();
        return true;
    }
    if (w.busy_) return false;  // the running operation's completion expects its repository
    return w.OpenRepo(probe.root);
}

void MergeView::refresh() {
    MainWindow& w = impl_->win;
    if (w.repoOpen_ && !w.busy_) w.Refresh();
}

void MergeView::focus() { SetFocus(impl_->win.list_.hwnd()); }


bool MergeView::showFileHistory(const std::wstring& absolutePath) {
    MainWindow& w = impl_->win;
    if (w.busy_) return false;
    std::string rel = w.RepoRelative(absolutePath);
    if (!rel.empty()) w.ShowFileHistory(rel);
    return true;
}

bool MergeView::showBlame(const std::wstring& absolutePath) {
    MainWindow& w = impl_->win;
    if (w.busy_) return false;
    std::string rel = w.RepoRelative(absolutePath);
    if (!rel.empty()) w.ShowBlame(rel, "");
    return true;
}
