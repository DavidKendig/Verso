// Linux git mode: MergeView (merge/src/merge_view.h) drawn by Verso inside its own window.
//
// This is the Linux counterpart of merge/src/main_window.cpp, with the same layout,
// commands, menus and git calls. Where Windows uses child windows and common controls,
// the panes here are drawn with Gfx (GitWidgets.h) and take their input from the host
// (AppLinux.cpp); dialogs are drawn over the view and run a modal loop on the window.
#include "merge_view.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <thread>

#include "app/Clipboard.h"
#include "app/Gfx.h"
#include "app/LineEdit.h"
#include "app/Theme.h"
#include "app/linux/GitWidgets.h"
#include "app/linux/Window.h"
#include "git.h"
#include "graph.h"
#include "util.h"

using namespace verso;
using namespace verso::gitui;

namespace {

const wchar_t kAppName[] = L"Verso Git";

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
  CM_REMOTE_FETCH, CM_EDIT_CUT, CM_EDIT_COPY, CM_EDIT_PASTE, CM_EDIT_SELECT_ALL,
};

constexpr int kMaxCommits = 5000;
constexpr uintptr_t kBlinkTimer = 0x6701;  // the app's own timers use small ids

struct CommandDef {
  int id;
  const wchar_t* name;
  const wchar_t* keys;
};

const CommandDef kCommands[] = {
    {IDM_OPEN, L"Open Repository...", L"Ctrl+O"},
    {IDM_CLONE, L"Clone Repository...", L""},
    {IDM_INIT, L"New Repository (git init)...", L""},
    {IDM_OPEN_EXPLORER, L"Open Repository Folder", L""},
    {IDM_OPEN_TERMINAL, L"Open Terminal in Repository", L""},
    {IDM_EXIT, L"Exit", L""},
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

// The keyboard shortcuts (the Windows view's accelerator table): exact modifier match.
struct Accel {
  bool ctrl, shift;
  unsigned vk;
  int id;
};
const Accel kAccels[] = {
    {true, false, 'O', IDM_OPEN},
    {true, true, 'G', IDM_SWITCH_MODE},
    {false, false, VK_F5, IDM_REFRESH},
    {false, false, VK_F7, IDM_PULL},
    {true, false, 'S', IDM_STASH},
    {true, true, 'S', IDM_POP_STASH},
    {true, true, 'A', IDM_STAGE_ALL},
    {true, true, 'R', IDM_UNSTAGE_ALL},
    {true, false, VK_RETURN, IDM_COMMIT},
    {true, true, 'B', IDM_NEW_BRANCH},
    {true, true, 'C', IDM_CHECKOUT_BRANCH},
    {true, false, 'F', IDM_SEARCH},
    {true, false, '0', IDM_GOTO_HEAD},
    {true, false, '9', IDM_GOTO_WORKDIR},
    {true, false, VK_OEM_3, IDM_TOGGLE_CONSOLE},
    {false, false, VK_F9, IDM_TOGGLE_SIDEBAR},
    {true, false, '1', IDM_FOCUS_LEFT},
    {true, false, '2', IDM_FOCUS_MIDDLE},
    {true, false, '3', IDM_FOCUS_RIGHT},
    {true, false, 'P', IDM_PALETTE},
    {false, false, VK_ESCAPE, IDM_ESCAPE},
};

struct SideItem {
  enum Kind { Header, Local, Remote, RemoteGroup, Tag, Stash } kind;
  std::string name;
  std::string target;
};

enum class Mode { None, WorkDir, Commit };
enum class DiffSource { None, Unstaged, Staged, Commit, Text };
enum class Focus { None, Search, Sidebar, Commits, Message, Unstaged, Staged, Details, Files, Diff, Console };
enum class Capture { None, Split1, Split2, Button, Search, Sidebar, Commits, Message, Unstaged, Staged, Details,
                     Files, Diff, Console };

struct Button {
  int cmd = 0;
  std::wstring label;
  RectF r;
  bool visible = true;
};

std::string HomeDir() {
  const char* h = getenv("HOME");
  return h && *h ? h : "/tmp";
}

// Verso's settings folder ($XDG_CONFIG_HOME/verso), as App::dataDir.
std::wstring ConfigDir() {
  std::string base;
  if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x) base = x;
  else base = HomeDir() + "/.config";
  return Widen(base + "/verso");
}

std::wstring RecentFilePath() { return ConfigDir() + L"/GitRecent.txt"; }

std::string FindInPath(const std::string& name) {
  if (name.find('/') != std::string::npos) return access(name.c_str(), X_OK) == 0 ? name : std::string();
  const char* path = getenv("PATH");
  for (const std::string& d : Split(path && *path ? path : "/usr/local/bin:/usr/bin:/bin", ':')) {
    std::string p = (d.empty() ? std::string(".") : d) + "/" + name;
    if (access(p.c_str(), X_OK) == 0) return p;
  }
  return {};
}

// Starts the user's terminal in `dir` (Ryū OS ships foot); false when none is installed.
bool OpenTerminal(const std::wstring& dir) {
  std::vector<std::string> candidates;
  if (const char* t = getenv("TERMINAL"); t && *t) candidates.push_back(t);
  for (const char* t : {"foot", "x-terminal-emulator", "kgx", "gnome-terminal", "konsole", "xfce4-terminal",
                        "alacritty", "kitty", "wezterm", "xterm"})
    candidates.push_back(t);
  std::string d = Narrow(dir);
  for (const auto& c : candidates) {
    std::string exe = FindInPath(c);
    if (exe.empty()) continue;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
      setsid();
      if (chdir(d.c_str()) != 0) _exit(127);
      if (fork() == 0) {
        execl(exe.c_str(), exe.c_str(), static_cast<char*>(nullptr));
        _exit(127);
      }
      _exit(0);
    }
    waitpid(pid, nullptr, 0);
    return true;
  }
  return false;
}

// Moves files to the desktop's Trash (SHFileOperationW over GIO, Win32Compat.cpp).
bool TrashFiles(const std::vector<std::wstring>& files) {
  if (files.empty()) return true;
  std::wstring list;
  for (auto& f : files) {
    list += f;
    list.push_back(L'\0');
  }
  list.push_back(L'\0');
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = list.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
  return SHFileOperationW(&op) == 0;
}

void Invalidate() {
  if (platform::Window* w = platform::mainWindow()) w->invalidate();
}

std::wstring SanitizeRefName(std::wstring s) {
  s = TrimW(s);
  for (auto& c : s)
    if (c == L' ') c = L'-';
  return s;
}

bool IsModifier(unsigned vk) {
  return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LSHIFT || vk == VK_RSHIFT ||
         vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN ||
         vk == VK_RWIN || vk == VK_CAPITAL;
}

// A dialog drawn over the git view: a text prompt, a filterable list, or a wait for an
// external picker. RunModal keeps the window's loop going until it is done.
struct Modal {
  enum Kind { Prompt, Picker, Wait } kind = Prompt;
  std::wstring title, label;
  LineEdit edit;
  const std::vector<std::wstring>* items = nullptr;
  std::vector<int> visible;
  int sel = 0, top = 0;
  bool done = false, accepted = false;
  pid_t child = -1;  // Wait: the picker process, ended by Escape
  RectF card, editRect, listRect, okRect, cancelRect;
  int pressed = 0;  // IDOK / IDCANCEL while its button is held
  bool editDrag = false;
  ClickCounter clicks;
  static constexpr float kItemH = 26;
  int listRows() const { return std::max(1, static_cast<int>(listRect.h() / kItemH)); }
};

// ---------------------------------------------------------------------------------------
// The git view
// ---------------------------------------------------------------------------------------
class GitWindow {
 public:
  ~GitWindow();
  bool Create(MergeViewCallbacks callbacks, std::wstring* error);

  // Host integration (MergeView).
  void Paint(Gfx& g, const Theme& theme, const RectF& area);
  bool KeyDown(unsigned vk, bool ctrl, bool shift, bool alt);
  void Char(wchar_t c);
  bool MouseButton(int button, bool down, float x, float y);
  bool MouseMove(float x, float y, MergeView::Pointer* pointer);
  bool Wheel(float notches, bool horizontal, float x, float y);
  bool InModal() const { return modal_ != nullptr; }

  // layout and painting
  HMENU BuildMenu();
  void RebuildRecentMenu();
  void Layout();
  void LayoutRight(float x, float y, float w, float h);
  void PaintButton(const Button& b, bool toggled);
  void PaintLineEdit(LineEdit& e, const RectF& r, bool focused, const std::wstring& placeholder);
  size_t LineEditHit(LineEdit& e, const RectF& r, float x);
  void PaintStatus();
  void PaintModal();
  void UpdateTitle();
  void SetFocus(Focus f);
  void CycleFocus(int dir);
  void Blink();

  // repository state
  bool OpenRepo(const std::wstring& path, bool quiet = false);
  void Refresh();
  void SplitStatus();
  void FillSidebar();
  void FillFileList(FileList& list, const std::vector<FileEntry>& files, bool stagedList,
                    const std::set<std::string>& keep);
  void ApplyFilter();
  void OnSearchChanged();
  void UpdateWorkdirText();
  void OnSelectionChanged();
  void OnSidebarSelect();
  void OnSidebarActivate();
  void OnFileSelection(FileList& list);
  void ScheduleWorkDiff();
  void ShowCommit(int index);
  void ShowWorkDiff(bool keepScroll);
  std::vector<FileEntry> SelectedEntries(const FileList& list, const std::vector<FileEntry>& files) const;
  std::vector<std::string> SelectedPaths(const FileList& list, bool staged) const;
  const RefInfo* CurrentRef() const;
  const RefInfo* FindRef(const std::string& name, RefInfo::Kind kind) const;
  std::string DefaultRemote() const;

  // git execution
  ProcResult RunLogged(const std::vector<std::wstring>& args, const std::string* input = nullptr);
  bool Exec(const std::vector<std::wstring>& args, const std::string* input = nullptr, bool refresh = true);
  void ExecAsync(const std::wstring& label, std::vector<std::wstring> args,
                 std::function<void(const ProcResult&)> done = {}, const Git* with = nullptr);
  void TaskDone(const std::wstring& label, const ProcResult& result,
                const std::function<void(const ProcResult&)>& done);
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

  // dialogs
  void RunModal(Modal& m);
  std::optional<std::wstring> PromptText(const std::wstring& title, const std::wstring& label,
                                         const std::wstring& initial = L"");
  int PickFromList(const std::wstring& title, const std::vector<std::wstring>& items);
  std::optional<std::wstring> ChooseFolder(const std::wstring& title);
  bool Confirm(const std::wstring& text, const std::wstring& title = L"Confirm");
  void Refilter(Modal& m);
  void AcceptModal(Modal& m);
  void ModalKey(unsigned vk, bool ctrl, bool shift);
  void ModalMouse(int button, bool down, float x, float y);

  // context menus (x, y in the view; the menus open at that point)
  int TrackMenu(HMENU m, float x, float y);
  void ContextFileList(FileList& list, float x, float y);
  void ContextCommitFiles(float x, float y);
  void ContextDiff(float x, float y);
  void ContextCommit(int item, float x, float y);
  void ContextSidebar(float x, float y);
  void ContextEdit(Focus which, float x, float y);
  void ContextForFocus();

  void LoadRecent();
  void SaveRecent();
  void AddRecent(const std::wstring& path);

  HMENU menu_ = nullptr;
  HMENU recentMenu_ = nullptr;
  MergeViewCallbacks cb_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  Gfx* g_ = nullptr;
  Palette pal_;
  Fonts fonts_;
  RectF area_;
  uint64_t lastPaint_ = 0;
  bool caretOn_ = true;

  std::vector<Button> toolbar_;
  Button btnCommit_{IDM_COMMIT, L"Commit"}, btnAmend_{IDM_AMEND_TOGGLE, L"Amend"};
  Button btnStageAll_{IDM_STAGE_ALL, L"Stage All"}, btnUnstageAll_{IDM_UNSTAGE_ALL, L"Unstage All"};
  Button btnDiscardAll_{IDM_DISCARD_ALL, L"Discard All"};
  LineEdit search_;
  RectF searchRect_;
  SideTree tree_;
  CommitListPane list_;
  TextArea message_, details_;
  FileList unstagedList_, stagedList_, filesList_;
  DiffPane diff_, console_;

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
  Focus focus_ = Focus::Commits;
  bool busy_ = false, consoleVisible_ = false, sidebarVisible_ = true, amend_ = false;
  bool refreshing_ = false, diffPending_ = false, keepDiffScroll_ = false, forceWorkdir_ = false;
  std::wstring statusMsg_;
  float sidebarW_ = 0, listW_ = 0;
  RectF split1_, split2_, statusRc_;
  std::vector<std::pair<RectF, std::wstring>> labels_;
  std::vector<std::wstring> recent_;
  std::vector<SideItem> sideItems_;
  std::map<std::wstring, bool> collapsed_{{L"TAGS", true}};

  // pointer state
  float mx_ = -1, my_ = -1;
  Capture capture_ = Capture::None;
  int pressedButton_ = 0;
  Focus rightPane_ = Focus::None;
  int rightItem_ = CommitListPane::kNone;
  int hoverKey_ = 0;
  ClickCounter clicks_;
  Modal* modal_ = nullptr;
};

GitWindow::~GitWindow() {
  *alive_ = false;
  if (platform::Window* w = platform::mainWindow()) w->killTimer(kBlinkTimer);
}

// ---------------------------------------------------------------------------
// Setup

bool GitWindow::Create(MergeViewCallbacks callbacks, std::wstring* error) {
  cb_ = std::move(callbacks);
  git_.exe = Git::Locate();
  if (git_.exe.empty()) {
    if (error) *error = L"git was not found. Install git and make sure it is on PATH.";
    return false;
  }
  LoadRecent();
  menu_ = BuildMenu();
  const std::pair<int, const wchar_t*> tb[] = {{IDM_OPEN, L"Open"},         {IDM_FETCH, L"Fetch"},
                                               {IDM_PULL, L"Pull"},         {IDM_PUSH, L"Push"},
                                               {IDM_STASH, L"Stash"},       {IDM_POP_STASH, L"Pop Stash"},
                                               {IDM_NEW_BRANCH, L"Branch"}, {IDM_PALETTE, L"Commands"}};
  for (auto& [id, text] : tb) toolbar_.push_back(Button{id, text});
  list_.onSelChange = [this] { OnSelectionChanged(); };
  message_.placeholder = L"Commit message  (Ctrl+Enter commits)";
  details_.readOnly = true;
  console_.console = true;

  std::shared_ptr<bool> alive = alive_;
  git_.logger = [this, alive](const std::wstring& cmd, const ProcResult& r) {
    // Runs on whichever thread ran git; the console is updated on the UI thread.
    std::wstring text = L"$ " + cmd + L"\n";
    std::wstring body = Widen(r.out + r.err);
    if (body.size() > 20000) body = body.substr(0, 20000) + L"\n… (output truncated)\n";
    text += body;
    if (!body.empty() && body.back() != L'\n') text += L"\n";
    if (!r.ok()) text += L"[exit code " + std::to_wstring(r.exitCode) + L"]\n";
    if (platform::Window* w = platform::mainWindow())
      w->post([this, alive, text] {
        if (*alive) AppendConsole(text);
      });
  };
  AppendConsole(L"Verso git console — using " + git_.exe + L"\n");
  if (platform::Window* w = platform::mainWindow())
    w->setTimer(kBlinkTimer, 530, true, [this, alive] {
      if (*alive) Blink();
    });
  mode_ = Mode::None;
  diff_.setPlaceholder(L"Open a repository with File › Open Repository (Ctrl+O), or clone one.");
  return true;
}

HMENU GitWindow::BuildMenu() {
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
  AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(recentMenu_), L"Open &Recent");
  item(file, IDM_CLONE);
  item(file, IDM_INIT);
  sep(file);
  item(file, IDM_OPEN_EXPLORER);
  item(file, IDM_OPEN_TERMINAL);
  sep(file);
  item(file, IDM_EXIT);
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");

  HMENU repo = CreatePopupMenu();
  for (int id : std::initializer_list<int>{IDM_REFRESH, 0, IDM_FETCH, IDM_PULL, IDM_PULL_REBASE, IDM_PUSH, IDM_FORCE_PUSH, 0, IDM_STAGE_ALL,
                 IDM_UNSTAGE_ALL, IDM_DISCARD_ALL, IDM_COMMIT, IDM_AMEND_TOGGLE, 0, IDM_STASH, IDM_STASH_UNTRACKED,
                 IDM_POP_STASH, 0, IDM_CONTINUE_OP, IDM_ABORT_OP})
    id ? item(repo, id) : sep(repo);
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(repo), L"&Repository");

  HMENU branch = CreatePopupMenu();
  for (int id : std::initializer_list<int>{IDM_NEW_BRANCH, IDM_CHECKOUT_BRANCH, IDM_MERGE_BRANCH, IDM_REBASE_BRANCH, IDM_DELETE_BRANCH, 0,
                 IDM_NEW_TAG})
    id ? item(branch, id) : sep(branch);
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(branch), L"&Branch");

  HMENU view = CreatePopupMenu();
  for (int id : std::initializer_list<int>{IDM_PALETTE, IDM_SEARCH, 0, IDM_GOTO_HEAD, IDM_GOTO_WORKDIR, 0, IDM_FOCUS_LEFT, IDM_FOCUS_MIDDLE,
                 IDM_FOCUS_RIGHT, 0, IDM_TOGGLE_SIDEBAR, IDM_TOGGLE_CONSOLE})
    id ? item(view, id) : sep(view);
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");

  HMENU help = CreatePopupMenu();
  item(help, IDM_ABOUT);
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");

  RebuildRecentMenu();
  return bar;
}

void GitWindow::RebuildRecentMenu() {
  if (!recentMenu_) return;
  while (GetMenuItemCount(recentMenu_) > 0) DeleteMenu(recentMenu_, 0, MF_BYPOSITION);
  if (recent_.empty()) AppendMenuW(recentMenu_, MF_STRING | MF_GRAYED, 0, L"(none)");
  for (size_t i = 0; i < recent_.size() && i < 20; ++i)
    AppendMenuW(recentMenu_, MF_STRING, IDM_RECENT_BASE + i, recent_[i].c_str());
}

// ---------------------------------------------------------------------------
// Layout and painting

void GitWindow::Layout() {
  if (!g_) return;
  const float L0 = area_.l, T0 = area_.t, W = area_.w(), H = area_.h();
  labels_.clear();
  const float tbH = 44, sbH = 26, conH = consoleVisible_ ? 190.0f : 0.0f, gap = 5;
  if (sidebarW_ == 0) sidebarW_ = 230;
  if (listW_ == 0) listW_ = std::max(380.0f, (W - sidebarW_) * 0.46f);

  float x = L0 + 10;
  for (auto& b : toolbar_) {
    float bw = std::round(g_->textWidth(b.label, fonts_.ui)) + 26;
    b.r = RectF{x, T0 + 8, x + bw, T0 + 36};
    x += bw + 6;
  }
  float searchW = std::min(300.0f, std::max(120.0f, L0 + W - x - 20));
  searchRect_ = RectF{L0 + W - searchW - 10, T0 + 10, L0 + W - 10, T0 + 34};

  float top = T0 + tbH, bottom = std::max(top, T0 + H - sbH - conH);
  console_.rect = consoleVisible_ ? RectF{L0 + 6, bottom + 4, L0 + W - 6, bottom + conH - 2} : RectF{};

  float x0 = L0;
  if (sidebarVisible_) {
    sidebarW_ = std::clamp(sidebarW_, 140.0f, std::max(140.0f, W / 3));
    tree_.rect = RectF{L0, top, L0 + sidebarW_, bottom};
    x0 = L0 + sidebarW_;
    split1_ = RectF{x0, top, x0 + gap, bottom};
    x0 += gap;
  } else {
    tree_.rect = RectF{};
    split1_ = RectF{};
  }
  float avail = L0 + W - x0;
  listW_ = std::clamp(listW_, 260.0f, std::max(260.0f, avail - 320));
  list_.rect = RectF{x0, top, x0 + listW_, bottom};
  split2_ = RectF{x0 + listW_, top, x0 + listW_ + gap, bottom};
  float rx = x0 + listW_ + gap;
  LayoutRight(rx, top, std::max(0.0f, L0 + W - rx), bottom - top);
  statusRc_ = RectF{L0, T0 + H - sbH, L0 + W, T0 + H};
}

void GitWindow::LayoutRight(float x, float y, float w, float h) {
  const bool work = mode_ == Mode::WorkDir, com = mode_ == Mode::Commit;
  for (Button* b : {&btnCommit_, &btnAmend_, &btnStageAll_, &btnUnstageAll_, &btnDiscardAll_}) {
    b->visible = work;
    b->r = RectF{};
  }
  message_.rect = details_.rect = RectF{};
  unstagedList_.rect = stagedList_.rect = filesList_.rect = RectF{};
  const float pad = 8, hdr = 30, btnH = 24;
  auto label = [&](float ly, const std::wstring& text, float right) {
    labels_.push_back({RectF{x + pad, ly, right, ly + hdr}, text});
  };
  // Lays out buttons from the right edge; returns where the label must end.
  auto rightButtons = [&](float ly, std::initializer_list<std::pair<Button*, float>> buttons) {
    float bx = x + w - pad;
    for (auto& [b, bw] : buttons) {
      bx -= bw;
      b->r = RectF{bx, ly + (hdr - btnH) / 2, bx + bw, ly + (hdr - btnH) / 2 + btnH};
      bx -= 6;
    }
    return bx - 6;
  };
  float cy = y;
  if (work) {
    std::wstring title = amend_ ? L"COMMIT MESSAGE  —  amending previous commit" : L"COMMIT MESSAGE";
    if (!opState_.empty()) title += L"   [" + opState_ + L"]";
    label(cy, title, rightButtons(cy, {{&btnCommit_, 90.0f}, {&btnAmend_, 80.0f}}));
    cy += hdr;
    message_.rect = RectF{x + pad, cy, x + w - pad, cy + 70};
    cy += 70 + 4;
    float remain = y + h - cy;
    float listH = std::clamp((remain - 2 * hdr) / 4, 60.0f, 220.0f);
    label(cy, L"UNSTAGED FILES (" + std::to_wstring(unstaged_.size()) + L")",
          rightButtons(cy, {{&btnStageAll_, 90.0f}, {&btnDiscardAll_, 96.0f}}));
    cy += hdr;
    unstagedList_.rect = RectF{x + pad, cy, x + w - pad, cy + listH};
    cy += listH;
    label(cy, L"STAGED FILES (" + std::to_wstring(staged_.size()) + L")",
          rightButtons(cy, {{&btnUnstageAll_, 100.0f}}));
    cy += hdr;
    stagedList_.rect = RectF{x + pad, cy, x + w - pad, cy + listH};
    cy += listH + 6;
  } else if (com) {
    const float dh = 130;
    details_.rect = RectF{x + pad, cy + pad, x + w - pad, cy + pad + dh};
    cy += dh + pad;
    label(cy, L"CHANGED FILES (" + std::to_wstring(commitFiles_.size()) + L")", x + w - pad);
    cy += hdr;
    float fh = std::clamp(static_cast<float>(commitFiles_.size()) * FileList::kRowH + 8, 50.0f, 170.0f);
    filesList_.rect = RectF{x + pad, cy, x + w - pad, cy + fh};
    cy += fh + 6;
  }
  diff_.rect = RectF{x, cy, x + w, std::max(cy, y + h)};
}

void GitWindow::PaintButton(const Button& b, bool toggled) {
  if (!b.visible || b.r.empty()) return;
  Gfx& g = *g_;
  bool hot = b.r.contains(mx_, my_) && !modal_;
  bool down = hot && capture_ == Capture::Button && pressedButton_ == b.cmd;
  Color fill = toggled ? pal_.buttonOn : down ? pal_.selectDim : hot ? pal_.buttonHover : pal_.button;
  g.fillRounded(b.r, 5, fill);
  drawText(g, b.label, b.r.inset(6, 0), pal_.buttonText, fonts_.ui, Align::Center);
}

void GitWindow::PaintLineEdit(LineEdit& e, const RectF& r, bool focused, const std::wstring& placeholder) {
  Gfx& g = *g_;
  g.fillRounded(r, 4, pal_.input);
  g.strokeRounded(r, 4, focused ? pal_.inputBorderFocus : pal_.inputBorder, 1);
  RectF inner = r.inset(8, 0);
  IDWriteTextFormat* fmt = fonts_.ui;
  const std::wstring& t = e.text();
  float caretX = g.textWidth(std::wstring_view(t).substr(0, e.caret()), fmt);
  if (caretX - e.scrollX > inner.w() - 4) e.scrollX = caretX - inner.w() + 4;
  if (caretX - e.scrollX < 0) e.scrollX = std::max(0.0f, caretX - 20);
  float th = g.formatHeight(fmt);
  float ty = std::round(r.t + (r.h() - th) / 2);
  g.pushClip(inner);
  if (t.empty()) {
    g.text(placeholder, inner.l, ty, pal_.placeholder, fmt);
  } else {
    if (e.hasSelection()) {
      size_t b = std::min(e.caret(), e.anchor()), en = std::max(e.caret(), e.anchor());
      float xa = g.textWidth(std::wstring_view(t).substr(0, b), fmt);
      float xb = g.textWidth(std::wstring_view(t).substr(0, en), fmt);
      g.fill(RectF{inner.l + xa - e.scrollX, ty, inner.l + xb - e.scrollX, ty + th}, pal_.inputSelection);
    }
    g.text(t, inner.l - e.scrollX, ty, pal_.text, fmt);
  }
  if (focused && caretOn_) {
    float x = std::round(inner.l + caretX - e.scrollX);
    g.fill(RectF{x, ty, x + 1.5f, ty + th}, pal_.text);
  }
  g.popClip();
}

size_t GitWindow::LineEditHit(LineEdit& e, const RectF& r, float x) {
  RectF inner = r.inset(8, 0);
  return std::min(g_->hitTest(e.text(), fonts_.ui, x - inner.l + e.scrollX), e.text().size());
}

void GitWindow::Paint(Gfx& g, const Theme& theme, const RectF& area) {
  if (!g_) {
    fonts_.ui = g.uiFormat();
    fonts_.bold = g.uiBoldFormat();
    fonts_.caption = g.createFont(L"Segoe UI", 11.0f, 600, false);
    fonts_.mono = g.createFont(kEmbeddedMonoFamily, 13.0f, 400, false);
    fonts_.monoW = g.textWidth(L"0000000000", fonts_.mono) / 10;
    fonts_.monoH = textHeight(fonts_.mono);
    message_.font = fonts_.ui;
    details_.font = fonts_.mono;
  }
  g_ = &g;
  pal_ = makePalette(theme);
  area_ = area;
  lastPaint_ = GetTickCount64();
  Layout();
  const Palette& p = pal_;

  g.pushClip(area);
  g.fill(area, p.bg);
  g.fill(RectF{area.l, area.t + 43, area.r, area.t + 44}, p.border);
  for (const auto& b : toolbar_) PaintButton(b, false);
  PaintLineEdit(search_, searchRect_, focus_ == Focus::Search, L"Search commits  (Ctrl+F)");
  if (sidebarVisible_) tree_.paint(g, p, fonts_, focus_ == Focus::Sidebar, mx_, my_);
  list_.paint(g, p, fonts_, focus_ == Focus::Commits);
  for (const auto& [r, text] : labels_) drawText(g, text, r, p.dim, fonts_.caption);
  if (mode_ == Mode::WorkDir) {
    message_.paint(g, p, focus_ == Focus::Message, caretOn_);
    unstagedList_.paint(g, p, fonts_, focus_ == Focus::Unstaged, mx_, my_);
    stagedList_.paint(g, p, fonts_, focus_ == Focus::Staged, mx_, my_);
    for (const Button* b : {&btnCommit_, &btnAmend_, &btnStageAll_, &btnUnstageAll_, &btnDiscardAll_})
      PaintButton(*b, b == &btnAmend_ && amend_);
  } else if (mode_ == Mode::Commit) {
    details_.paint(g, p, focus_ == Focus::Details, caretOn_);
    filesList_.paint(g, p, fonts_, focus_ == Focus::Files, mx_, my_);
  }
  diff_.paint(g, p, fonts_, focus_ == Focus::Diff);
  if (consoleVisible_) {
    g.fill(console_.rect.inset(-1, -1), p.border);
    console_.paint(g, p, fonts_, focus_ == Focus::Console);
  }
  if (capture_ == Capture::Split1 || capture_ == Capture::Split2) {
    const RectF& s = capture_ == Capture::Split1 ? split1_ : split2_;
    g.fill(RectF{s.l + 2, s.t, s.l + 3, s.b}, p.accent);
  }
  PaintStatus();
  if (modal_) PaintModal();
  g.popClip();
}

void GitWindow::PaintStatus() {
  Gfx& g = *g_;
  g.fill(statusRc_, pal_.panel);
  g.fill(RectF{statusRc_.l, statusRc_.t, statusRc_.r, statusRc_.t + 1}, pal_.border);
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
  RectF l{statusRc_.l + 10, statusRc_.t + 1, statusRc_.r - 10, statusRc_.b};
  float msgW = statusMsg_.empty() ? 0 : std::min(l.w() / 2, g.textWidth(statusMsg_, fonts_.ui) + 24);
  drawText(g, left, RectF{l.l, l.t, l.r - msgW, l.b}, opState_.empty() ? pal_.dim : pal_.warn, fonts_.ui,
           Align::Left, Elide::Middle);
  drawText(g, statusMsg_, RectF{l.r - msgW, l.t, l.r, l.b}, busy_ ? pal_.accent : pal_.dim, fonts_.ui, Align::Right);
}

void GitWindow::PaintModal() {
  Gfx& g = *g_;
  Modal& m = *modal_;
  g.fill(area_, Color::hex(0x000000, 0.35f));
  float w = std::min(m.kind == Modal::Picker ? 560.0f : 520.0f, area_.w() - 40);
  float h = m.kind == Modal::Picker ? std::min(440.0f, area_.h() - 100) : (m.kind == Modal::Wait ? 110.0f : 168.0f);
  float l = std::floor(area_.l + (area_.w() - w) / 2);
  float t = m.kind == Modal::Picker ? area_.t + 60 : std::floor(area_.t + (area_.h() - h) / 3);
  m.card = RectF{l, t, l + w, t + h};
  for (int i = 1; i <= 5; ++i) {
    float s = static_cast<float>(i) * 2;
    g.fillRounded(RectF{l - s + 2, t - s + 6, l + w + s - 2, t + h + s + 2}, 9 + s,
                  pal_.shadow.withAlpha(pal_.shadow.a / 6));
  }
  g.fillRounded(m.card, 9, pal_.overlayBg);
  g.strokeRounded(m.card, 9, pal_.overlayBorder, 1);
  drawText(g, m.title, RectF{l + 18, t + 10, l + w - 18, t + 36}, pal_.text, fonts_.bold);
  float y = t + 38;
  if (m.kind == Modal::Wait) {
    drawText(g, m.label, RectF{l + 18, y, l + w - 18, y + 24}, pal_.dim, fonts_.ui);
    drawText(g, L"Esc cancels", RectF{l + 18, y + 26, l + w - 18, y + 50}, pal_.dim, fonts_.caption);
    return;
  }
  if (!m.label.empty()) {
    drawText(g, m.label, RectF{l + 18, y, l + w - 18, y + 22}, pal_.dim, fonts_.ui);
    y += 24;
  }
  m.editRect = RectF{l + 16, y, l + w - 16, y + 30};
  PaintLineEdit(m.edit, m.editRect, true, m.kind == Modal::Picker ? L"Type to filter" : L"");
  y += 38;
  if (m.kind == Modal::Picker) {
    m.listRect = RectF{l + 8, y, l + w - 8, t + h - 8};
    int rows = m.listRows();
    int n = static_cast<int>(m.visible.size());
    m.sel = n ? std::clamp(m.sel, 0, n - 1) : 0;
    if (m.sel < m.top) m.top = m.sel;
    if (m.sel >= m.top + rows) m.top = m.sel - rows + 1;
    m.top = std::clamp(m.top, 0, std::max(0, n - rows));
    g.pushClip(m.listRect);
    for (int r = m.top; r < n && r < m.top + rows + 1; ++r) {
      float ry = m.listRect.t + static_cast<float>(r - m.top) * Modal::kItemH;
      RectF row{m.listRect.l, ry, m.listRect.r, ry + Modal::kItemH};
      if (r == m.sel) g.fillRounded(row, 5, pal_.select);
      else if (row.contains(mx_, my_)) g.fillRounded(row, 5, pal_.hover);
      drawText(g, (*m.items)[static_cast<size_t>(m.visible[static_cast<size_t>(r)])], row.inset(10, 0), pal_.text,
               fonts_.ui);
    }
    if (n == 0) drawText(g, L"No matches", m.listRect, pal_.dim, fonts_.ui, Align::Center);
    g.popClip();
    if (n > rows) {
      float listH = m.listRect.h();
      float th = std::max(16.0f, listH * static_cast<float>(rows) / static_cast<float>(n));
      float tt = m.listRect.t + (listH - th) * static_cast<float>(m.top) / static_cast<float>(n - rows);
      g.fillRounded(RectF{m.listRect.r - 4, tt, m.listRect.r - 1, tt + th}, 1.5f, pal_.scrollThumb);
    }
  } else {
    m.cancelRect = RectF{l + w - 16 - 90, t + h - 44, l + w - 16, t + h - 16};
    m.okRect = RectF{m.cancelRect.l - 8 - 90, m.cancelRect.t, m.cancelRect.l - 8, m.cancelRect.b};
    bool okHot = m.okRect.contains(mx_, my_), cancelHot = m.cancelRect.contains(mx_, my_);
    g.fillRounded(m.okRect, 5, m.pressed == IDOK && okHot ? pal_.selectDim : pal_.accent);
    drawText(g, L"OK", m.okRect, Color::hex(0xFFFFFF), fonts_.ui, Align::Center);
    g.fillRounded(m.cancelRect, 5,
                  m.pressed == IDCANCEL && cancelHot ? pal_.selectDim : cancelHot ? pal_.buttonHover : pal_.button);
    drawText(g, L"Cancel", m.cancelRect, pal_.buttonText, fonts_.ui, Align::Center);
  }
}

void GitWindow::UpdateTitle() {
  std::wstring subject;
  if (repoOpen_) {
    std::wstring name = git_.root.substr(git_.root.find_last_of(L'/') + 1);
    subject = name + L" (" + (headBranch_.empty() ? L"detached" : Widen(headBranch_)) + L")";
  }
  if (cb_.onTitle) cb_.onTitle(subject);
  const RefInfo* cur = CurrentRef();
  for (auto& b : toolbar_) {
    if (b.cmd == IDM_PULL) b.label = cur && cur->behind ? L"Pull ↓" + std::to_wstring(cur->behind) : L"Pull";
    if (b.cmd == IDM_PUSH) b.label = cur && cur->ahead ? L"Push ↑" + std::to_wstring(cur->ahead) : L"Push";
  }
  Invalidate();
}

void GitWindow::SetFocus(Focus f) {
  focus_ = f;
  caretOn_ = true;
  Invalidate();
}

void GitWindow::CycleFocus(int dir) {
  // Tab order: what is on screen, left to right.
  Focus order[10];
  int n = 0;
  order[n++] = Focus::Search;
  if (sidebarVisible_) order[n++] = Focus::Sidebar;
  order[n++] = Focus::Commits;
  if (mode_ == Mode::WorkDir) {
    order[n++] = Focus::Message;
    order[n++] = Focus::Unstaged;
    order[n++] = Focus::Staged;
  } else if (mode_ == Mode::Commit) {
    order[n++] = Focus::Details;
    order[n++] = Focus::Files;
  }
  order[n++] = Focus::Diff;
  if (consoleVisible_) order[n++] = Focus::Console;
  int i = static_cast<int>(std::find(order, order + n, focus_) - order);
  if (i == n) i = 0;
  SetFocus(order[(i + dir + n) % n]);
}

void GitWindow::Blink() {
  // Only while the view is on screen (it repaints at least as often as this timer fires).
  if (GetTickCount64() - lastPaint_ > 2000) return;
  if (modal_ ? modal_->kind != Modal::Wait : (focus_ == Focus::Search || focus_ == Focus::Message)) {
    caretOn_ = !caretOn_;
    Invalidate();
  }
}

// ---------------------------------------------------------------------------
// Repository state

bool GitWindow::OpenRepo(const std::wstring& path, bool quiet) {
  Git probe = git_;
  std::wstring err;
  if (!probe.Open(path, err)) {
    if (!quiet)
      MessageBoxW(nullptr, (L"Could not open repository:\n" + path + L"\n\n" + err).c_str(), kAppName, MB_ICONERROR);
    return false;
  }
  git_.root = probe.root;
  git_.gitDir = probe.gitDir;
  repoOpen_ = true;
  pathFilter_.clear();
  search_.setText(L"", false);
  message_.setText(L"");
  amend_ = false;
  shownCommit_.clear();
  AddRecent(git_.root);
  AppendConsole(L"\nOpened " + git_.root + L"\n");
  forceWorkdir_ = true;
  Refresh();
  return true;
}

void GitWindow::SplitStatus() {
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

void GitWindow::Refresh() {
  if (!repoOpen_ || refreshing_) return;
  refreshing_ = true;
  int sel = list_.selected();
  std::string selHash =
      sel >= 0 && sel < static_cast<int>(commits_.size()) ? commits_[static_cast<size_t>(sel)].hash : std::string();
  bool wantWorkdir = forceWorkdir_ || sel == CommitListPane::kWorkdir || sel == CommitListPane::kNone;
  forceWorkdir_ = false;
  std::set<std::string> keepU, keepS;
  for (auto& e : SelectedEntries(unstagedList_, unstaged_)) keepU.insert(e.path);
  for (auto& e : SelectedEntries(stagedList_, staged_)) keepS.insert(e.path);

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

  if (opState_ == L"MERGING" && message_.text().empty()) {
    std::ifstream in(Narrow(git_.gitDir + L"/MERGE_MSG"), std::ios::binary);
    std::string m((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string cleaned;
    for (auto& l : Split(m, '\n'))
      if (!StartsWith(l, "#")) cleaned += l + "\n";
    message_.setText(Widen(Trim(cleaned)));
  }

  FillSidebar();
  FillFileList(unstagedList_, unstaged_, false, keepU);
  FillFileList(stagedList_, staged_, true, keepS);
  ApplyFilter();
  UpdateWorkdirText();

  int target = CommitListPane::kWorkdir;
  if (!wantWorkdir) {
    target = CommitListPane::kNone;
    for (int i = 0; i < static_cast<int>(commits_.size()); ++i)
      if (commits_[static_cast<size_t>(i)].hash == selHash) target = i;
    if (target == CommitListPane::kNone) target = CommitListPane::kWorkdir;
  }
  if (!pathFilter_.empty() && target == CommitListPane::kWorkdir)
    target = commits_.empty() ? CommitListPane::kNone : 0;
  keepDiffScroll_ = true;
  list_.select(target, true);
  keepDiffScroll_ = false;

  UpdateTitle();
  Layout();
  refreshing_ = false;
  Invalidate();
}

void GitWindow::FillSidebar() {
  // Keep the selected entry selected across refreshes (by kind and name).
  int prev = tree_.selectedItem();
  std::optional<SideItem> was;
  if (prev >= 0 && prev < static_cast<int>(sideItems_.size())) was = sideItems_[static_cast<size_t>(prev)];
  sideItems_.clear();
  std::vector<SideTree::Node> nodes;
  int selectNode = -1;
  auto add = [&](int parent, const std::wstring& text, SideItem item, bool bold, bool expandable) {
    if (was && was->kind == item.kind && was->name == item.name) selectNode = static_cast<int>(nodes.size());
    sideItems_.push_back(std::move(item));
    SideTree::Node n;
    n.text = text;
    n.parent = parent;
    n.depth = parent < 0 ? 0 : nodes[static_cast<size_t>(parent)].depth + 1;
    n.bold = bold;
    n.expandable = expandable;
    n.item = static_cast<int>(sideItems_.size()) - 1;
    n.collapseKey = text;
    nodes.push_back(std::move(n));
    return static_cast<int>(nodes.size()) - 1;
  };
  auto header = [&](const wchar_t* name) {
    return add(-1, name, SideItem{SideItem::Header, Narrow(name), ""}, true, true);
  };

  int hb = header(L"BRANCHES");
  for (const auto& r : refs_) {
    if (r.kind != RefInfo::Local) continue;
    std::wstring label = (r.current ? L"● " : L"") + Widen(r.name);
    if (r.ahead) label += L"   ↑" + std::to_wstring(r.ahead);
    if (r.behind) label += L"   ↓" + std::to_wstring(r.behind);
    if (r.gone) label += L"   (gone)";
    add(hb, label, SideItem{SideItem::Local, r.name, r.target}, r.current, false);
  }
  int hr = header(L"REMOTES");
  for (const auto& remote : remotes_) {
    int g = add(hr, Widen(remote), SideItem{SideItem::RemoteGroup, remote, ""}, false, true);
    for (const auto& r : refs_) {
      if (r.kind != RefInfo::Remote || !StartsWith(r.name, remote + "/")) continue;
      add(g, Widen(r.name.substr(remote.size() + 1)), SideItem{SideItem::Remote, r.name, r.target}, false, false);
    }
  }
  int ht = header(L"TAGS");
  for (auto it = refs_.rbegin(); it != refs_.rend(); ++it)
    if (it->kind == RefInfo::Tag) add(ht, Widen(it->name), SideItem{SideItem::Tag, it->name, it->target}, false, false);
  int hs = header(L"STASHES");
  for (const auto& s : stashes_)
    add(hs, Widen(s.ref) + L"  " + s.message, SideItem{SideItem::Stash, s.ref, ""}, false, false);
  tree_.setNodes(std::move(nodes), &collapsed_, selectNode);
}

void GitWindow::FillFileList(FileList& list, const std::vector<FileEntry>& files, bool stagedList,
                             const std::set<std::string>& keep) {
  std::vector<FileList::Row> rows;
  rows.reserve(files.size());
  for (const auto& e : files) {
    FileList::Row r;
    r.status = static_cast<wchar_t>(stagedList ? e.x : (e.untracked ? '?' : e.conflict ? 'U' : e.y));
    std::wstring path = Widen(e.path);
    if (!e.origPath.empty() && stagedList) path = Widen(e.origPath) + L"  →  " + path;
    if (e.untracked) path += L"   (untracked)";
    if (e.conflict) path += L"   (conflict)";
    r.text = std::move(path);
    r.key = e.path;
    rows.push_back(std::move(r));
  }
  list.setRows(std::move(rows), keep);
}

void GitWindow::ApplyFilter() {
  std::wstring query = TrimW(search_.text());
  std::vector<int> rows;
  rows.reserve(commits_.size());
  for (int i = 0; i < static_cast<int>(commits_.size()); ++i) {
    if (!query.empty()) {
      const Commit& c = commits_[static_cast<size_t>(i)];
      std::wstring hay = c.subject + L" " + c.author + L" " + c.email + L" " + Widen(c.hash);
      for (auto& b : c.badges) hay += L" " + b.name;
      if (!MatchesAllTokens(hay, query)) continue;
    }
    rows.push_back(i);
  }
  bool plain = query.empty() && pathFilter_.empty();
  list_.setModel(&commits_, &graph_, std::move(rows), plain, pathFilter_.empty() && query.empty(), headHash_);
  list_.setBanner(pathFilter_.empty() ? L"" : L"History of " + pathFilter_ + L"    (Esc to clear)");
}

void GitWindow::OnSearchChanged() {
  ApplyFilter();
  if (list_.selected() == CommitListPane::kNone) OnSelectionChanged();
  Invalidate();
}

void GitWindow::UpdateWorkdirText() {
  std::wstring t;
  size_t conflicts = std::count_if(status_.begin(), status_.end(), [](auto& e) { return e.conflict; });
  if (status_.empty())
    t = L"Working directory clean";
  else
    t = L"Uncommitted changes:  " + std::to_wstring(unstaged_.size()) + L" unstaged,  " +
        std::to_wstring(staged_.size()) + L" staged";
  if (conflicts) t += L",  " + std::to_wstring(conflicts) + L" conflicted";
  if (!opState_.empty()) t = opState_ + L"  —  " + t;
  list_.setWorkdirText(t, !status_.empty() || !opState_.empty());
}

std::vector<FileEntry> GitWindow::SelectedEntries(const FileList& list, const std::vector<FileEntry>& files) const {
  std::vector<FileEntry> out;
  for (int i : list.selection())
    if (i >= 0 && i < static_cast<int>(files.size())) out.push_back(files[static_cast<size_t>(i)]);
  return out;
}

std::vector<std::string> GitWindow::SelectedPaths(const FileList& list, bool staged) const {
  std::vector<std::string> paths;
  for (auto& e : SelectedEntries(list, staged ? staged_ : unstaged_)) {
    paths.push_back(e.path);
    if (staged && !e.origPath.empty()) paths.push_back(e.origPath);
  }
  return paths;
}

const RefInfo* GitWindow::CurrentRef() const {
  for (const auto& r : refs_)
    if (r.kind == RefInfo::Local && r.current) return &r;
  return nullptr;
}

const RefInfo* GitWindow::FindRef(const std::string& name, RefInfo::Kind kind) const {
  for (const auto& r : refs_)
    if (r.kind == kind && r.name == name) return &r;
  return nullptr;
}

std::string GitWindow::DefaultRemote() const {
  for (const auto& r : remotes_)
    if (r == "origin") return r;
  return remotes_.empty() ? std::string() : remotes_.front();
}

void GitWindow::OnSelectionChanged() {
  int item = list_.selected();
  if (item == CommitListPane::kWorkdir) {
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
    diff_.setPlaceholder(repoOpen_ ? L"Select a commit" : L"Open a repository (Ctrl+O)");
    Layout();
  }
  // Focus cannot stay on a pane that is no longer shown.
  if ((mode_ != Mode::WorkDir && (focus_ == Focus::Message || focus_ == Focus::Unstaged || focus_ == Focus::Staged)) ||
      (mode_ != Mode::Commit && (focus_ == Focus::Details || focus_ == Focus::Files)))
    focus_ = Focus::Commits;
  Invalidate();
}

void GitWindow::ShowCommit(int index) {
  const Commit& c = commits_[static_cast<size_t>(index)];
  if (c.hash == shownCommit_ && keepDiffScroll_) return;
  shownCommit_ = c.hash;
  std::wstring details = git_.CommitDetails(c.hash);
  if (c.parents.size() > 1) details += L"\n(merge commit — changes shown against the first parent)";
  details_.setText(details);
  commitFiles_ = git_.CommitFiles(c);
  std::vector<FileList::Row> rows;
  for (const auto& f : commitFiles_) {
    FileList::Row r;
    r.status = static_cast<wchar_t>(f.status.empty() ? ' ' : f.status[0]);
    r.text = Widen(f.path);
    if (!f.origPath.empty()) r.text = Widen(f.origPath) + L"  →  " + r.text;
    r.key = f.path;
    rows.push_back(std::move(r));
  }
  filesList_.setRows(std::move(rows), {});
  diffSource_ = DiffSource::Commit;
  diff_.setDiff(git_.CommitDiff(c), false);
}

void GitWindow::ShowWorkDiff(bool keepScroll) {
  auto selU = SelectedEntries(unstagedList_, unstaged_);
  auto selS = SelectedEntries(stagedList_, staged_);
  std::string d;
  if (!selU.empty()) {
    diffSource_ = DiffSource::Unstaged;
    std::vector<std::string> tracked;
    std::string untracked;
    for (auto& e : selU) {
      if (e.untracked) untracked += git_.NewFileDiff(e.path);
      else tracked.push_back(e.path);
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
    diff_.setPlaceholder(opState_.empty() ? L"Nothing to commit — working tree clean"
                                          : L"All conflicts resolved. Commit to conclude " + opState_);
    return;
  }
  diff_.setDiff(d, keepScroll);
}

void GitWindow::OnSidebarSelect() {
  int i = tree_.selectedItem();
  if (i < 0 || i >= static_cast<int>(sideItems_.size())) return;
  const SideItem item = sideItems_[static_cast<size_t>(i)];
  if (item.kind == SideItem::Stash) {
    ProcResult r = git_.Run({L"stash", L"show", L"-p", L"--include-untracked", Widen(item.name)});
    if (!r.ok()) r = git_.Run({L"stash", L"show", L"-p", Widen(item.name)});
    mode_ = Mode::None;
    diffSource_ = DiffSource::Text;
    Layout();
    diff_.setDiff(r.out, false);
  } else if (!item.target.empty()) {
    for (int k = 0; k < static_cast<int>(commits_.size()); ++k)
      if (commits_[static_cast<size_t>(k)].hash == item.target) {
        list_.select(k, true);
        break;
      }
  }
  Invalidate();
}

void GitWindow::OnSidebarActivate() {
  int i = tree_.selectedItem();
  if (i < 0 || i >= static_cast<int>(sideItems_.size()) || busy_) return;
  const SideItem item = sideItems_[static_cast<size_t>(i)];
  if (item.kind == SideItem::Local && item.name != headBranch_) CheckoutRef(item.name, false);
  else if (item.kind == SideItem::Remote) CheckoutRef(item.name, true);
}

void GitWindow::OnFileSelection(FileList& list) {
  if (&list == &filesList_) {
    int row = filesList_.caret();
    auto sel = filesList_.selection();
    if (row < 0 || std::find(sel.begin(), sel.end(), row) == sel.end()) row = sel.empty() ? -1 : sel.front();
    if (row >= 0 && row < static_cast<int>(commitFiles_.size()))
      diff_.scrollToFile(commitFiles_[static_cast<size_t>(row)].path);
    return;
  }
  // Selecting in one of the working directory lists clears the other.
  if (list.anySelected()) (&list == &unstagedList_ ? stagedList_ : unstagedList_).clearSelection();
  ScheduleWorkDiff();
}

void GitWindow::ScheduleWorkDiff() {
  if (diffPending_) return;
  diffPending_ = true;
  std::shared_ptr<bool> alive = alive_;
  auto run = [this, alive] {
    if (!*alive) return;
    diffPending_ = false;
    if (mode_ == Mode::WorkDir) ShowWorkDiff(false);
    Invalidate();
  };
  if (platform::Window* w = platform::mainWindow()) w->post(run);
  else run();
}

// ---------------------------------------------------------------------------
// Git execution helpers

ProcResult GitWindow::RunLogged(const std::vector<std::wstring>& args, const std::string* input) {
  return git_.Run(args, input, true);
}

bool GitWindow::Exec(const std::vector<std::wstring>& args, const std::string* input, bool refresh) {
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

void GitWindow::ExecAsync(const std::wstring& label, std::vector<std::wstring> args,
                          std::function<void(const ProcResult&)> done, const Git* with) {
  if (busy_) {
    MessageBeep(MB_ICONWARNING);
    return;
  }
  busy_ = true;
  statusMsg_ = label + L"…";
  Invalidate();
  Git g = with ? *with : git_;
  std::shared_ptr<bool> alive = alive_;
  std::thread([this, alive, g, args = std::move(args), label, done = std::move(done)] {
    ProcResult result = g.Run(args, nullptr, true);
    if (platform::Window* w = platform::mainWindow())
      w->post([this, alive, label, result, done] {
        if (*alive) TaskDone(label, result, done);
      });
  }).detach();
}

void GitWindow::TaskDone(const std::wstring& label, const ProcResult& result,
                         const std::function<void(const ProcResult&)>& done) {
  busy_ = false;
  if (result.ok()) {
    statusMsg_ = label + L" — done";
  } else {
    statusMsg_ = label + L" — failed";
    ShowGitError(label, result);
  }
  if (done) done(result);
  if (repoOpen_) Refresh();
  Invalidate();
}

void GitWindow::ShowGitError(const std::wstring& title, const ProcResult& r) {
  std::string text = Trim(r.out);
  std::string err = Trim(r.err);
  if (!err.empty()) text = text.empty() ? err : text + "\n\n" + err;
  std::wstring w = Widen(text);
  if (w.empty()) w = L"git exited with code " + std::to_wstring(r.exitCode);
  if (w.size() > 3000) w = w.substr(0, 3000) + L"\n…";
  MessageBoxW(nullptr, w.c_str(), (title + L" failed").c_str(), MB_ICONERROR);
}

void GitWindow::AppendConsole(const std::wstring& text) {
  console_.appendText(text);
  if (consoleVisible_) Invalidate();
}

// ---------------------------------------------------------------------------
// Actions

bool GitWindow::AllowedWhileBusy(int id) const {
  switch (id) {
    case IDM_EXIT:
    case IDM_SEARCH:
    case IDM_GOTO_HEAD:
    case IDM_GOTO_WORKDIR:
    case IDM_TOGGLE_CONSOLE:
    case IDM_TOGGLE_SIDEBAR:
    case IDM_FOCUS_LEFT:
    case IDM_FOCUS_MIDDLE:
    case IDM_FOCUS_RIGHT:
    case IDM_PALETTE:
    case IDM_OPEN_EXPLORER:
    case IDM_OPEN_TERMINAL:
    case IDM_ABOUT:
    case IDM_ESCAPE:
    case IDM_SWITCH_MODE: return true;
    default: return false;
  }
}

void GitWindow::OnCommand(int id) {
  if (busy_ && !AllowedWhileBusy(id)) {
    MessageBeep(MB_ICONWARNING);
    statusMsg_ = L"Busy — waiting for: " + statusMsg_;
    Invalidate();
    return;
  }
  bool needsRepo = !(id == IDM_OPEN || id == IDM_CLONE || id == IDM_INIT || id == IDM_EXIT || id == IDM_ABOUT ||
                     id == IDM_PALETTE || id == IDM_TOGGLE_CONSOLE || id == IDM_TOGGLE_SIDEBAR || id == IDM_ESCAPE ||
                     id == IDM_SWITCH_MODE || (id >= IDM_RECENT_BASE && id < IDM_RECENT_BASE + 20));
  if (needsRepo && !repoOpen_) {
    MessageBeep(MB_ICONWARNING);
    return;
  }
  switch (id) {
    case IDM_OPEN: DoOpen(); break;
    case IDM_CLONE: DoClone(); break;
    case IDM_INIT: DoInit(); break;
    case IDM_EXIT: PostMessageW(nullptr, WM_CLOSE, 0, 0); break;
    case IDM_SWITCH_MODE:
      if (cb_.onSwitchMode) cb_.onSwitchMode();
      break;
    case IDM_REFRESH: Refresh(); break;
    case IDM_FETCH: ExecAsync(L"Fetch", {L"fetch", L"--all", L"--prune"}); break;
    case IDM_PULL:
    case IDM_PULL_REBASE:
      if (headBranch_.empty()) {
        MessageBoxW(nullptr, L"HEAD is detached; check out a branch before pulling.", kAppName, MB_ICONINFORMATION);
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
      if (stashes_.empty()) MessageBoxW(nullptr, L"There are no stashes.", kAppName, MB_ICONINFORMATION);
      else Exec({L"stash", L"pop"});
      break;
    case IDM_STAGE_ALL: Exec({L"add", L"-A"}); break;
    case IDM_UNSTAGE_ALL:
      if (headHash_.empty()) Exec({L"rm", L"-r", L"-q", L"--cached", L"."});
      else Exec({L"reset", L"-q"});
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
      SetFocus(Focus::Search);
      search_.selectAll();
      break;
    case IDM_GOTO_HEAD:
      for (int i = 0; i < static_cast<int>(commits_.size()); ++i)
        if (commits_[static_cast<size_t>(i)].hash == headHash_) {
          list_.select(i, true);
          SetFocus(Focus::Commits);
          break;
        }
      break;
    case IDM_GOTO_WORKDIR:
      ClearFilters();
      list_.select(CommitListPane::kWorkdir, true);
      SetFocus(Focus::Message);
      break;
    case IDM_TOGGLE_CONSOLE:
      consoleVisible_ = !consoleVisible_;
      Layout();
      if (consoleVisible_) console_.scrollToEnd();
      else if (focus_ == Focus::Console) focus_ = Focus::Commits;
      break;
    case IDM_TOGGLE_SIDEBAR:
      sidebarVisible_ = !sidebarVisible_;
      if (!sidebarVisible_ && focus_ == Focus::Sidebar) focus_ = Focus::Commits;
      Layout();
      break;
    case IDM_FOCUS_LEFT:
      if (sidebarVisible_) SetFocus(Focus::Sidebar);
      break;
    case IDM_FOCUS_MIDDLE: SetFocus(Focus::Commits); break;
    case IDM_FOCUS_RIGHT: SetFocus(Focus::Diff); break;
    case IDM_PALETTE: ShowPalette(); break;
    case IDM_OPEN_EXPLORER: ShellExecuteW(nullptr, L"open", git_.root.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case IDM_OPEN_TERMINAL:
      if (!OpenTerminal(git_.root))
        MessageBoxW(nullptr, L"No terminal emulator was found. Set $TERMINAL or install foot.", kAppName,
                    MB_ICONINFORMATION);
      break;
    case IDM_ABOUT:
      MessageBoxW(nullptr,
                  (std::wstring(L"Verso Git\n\nThe git mode of Verso, drawn by Verso itself on Linux.\n"
                                L"Every repository operation runs through the git command line.\n\n"
                                L"Copyright 2026 David Kendig\n"
                                L"Licensed under the Apache License, Version 2.0.\n\nGit: ") +
                   git_.exe)
                      .c_str(),
                  kAppName, MB_ICONINFORMATION);
      break;
    case IDM_ESCAPE:
      if (focus_ == Focus::Search || !pathFilter_.empty() || !search_.text().empty()) {
        ClearFilters();
        SetFocus(Focus::Commits);
      }
      break;
    default:
      if (id >= IDM_RECENT_BASE && id < IDM_RECENT_BASE + 20 && id - IDM_RECENT_BASE < static_cast<int>(recent_.size()))
        OpenRepo(recent_[static_cast<size_t>(id - IDM_RECENT_BASE)]);
      break;
  }
  Invalidate();
}

void GitWindow::ClearFilters() {
  bool changed = !pathFilter_.empty();
  pathFilter_.clear();
  if (!search_.text().empty()) {
    search_.setText(L"", false);
    OnSearchChanged();
  }
  if (changed) {
    forceWorkdir_ = true;
    Refresh();
  }
}

void GitWindow::DoOpen() {
  if (auto dir = ChooseFolder(L"Open Git Repository")) OpenRepo(*dir);
}

void GitWindow::DoClone() {
  auto url = PromptText(L"Clone Repository", L"Repository URL:");
  if (!url || TrimW(*url).empty()) return;
  std::wstring u = TrimW(*url);
  auto parent = ChooseFolder(L"Choose the folder to clone into");
  if (!parent) return;
  std::wstring name = u;
  while (!name.empty() && (name.back() == L'/' || name.back() == L'\\')) name.pop_back();
  name = name.substr(name.find_last_of(L"/\\:") + 1);
  if (name.size() > 4 && name.compare(name.size() - 4, 4, L".git") == 0) name.resize(name.size() - 4);
  auto folder = PromptText(L"Clone Repository", L"Folder name inside " + *parent + L":", name);
  if (!folder || TrimW(*folder).empty()) return;
  std::wstring dest = *parent;
  if (dest.empty() || dest.back() != L'/') dest += L'/';
  dest += TrimW(*folder);
  Git g = git_;
  g.root = *parent;
  ExecAsync(
      L"Clone " + u, {L"clone", L"--progress", u, dest},
      [this, dest](const ProcResult& r) {
        if (r.ok()) OpenRepo(dest);
      },
      &g);
}

void GitWindow::DoInit() {
  auto dir = ChooseFolder(L"Choose a folder for the new repository");
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

void GitWindow::DoCommit() {
  if (mode_ != Mode::WorkDir) list_.select(CommitListPane::kWorkdir, true);
  std::wstring message = TrimW(message_.text());
  if (message.empty() && !amend_) {
    MessageBoxW(nullptr, L"Please enter a commit message.", kAppName, MB_ICONINFORMATION);
    SetFocus(Focus::Message);
    return;
  }
  if (staged_.empty() && !amend_ && opState_.empty()) {
    if (unstaged_.empty()) {
      MessageBoxW(nullptr, L"There is nothing to commit.", kAppName, MB_ICONINFORMATION);
      return;
    }
    if (!Confirm(L"No changes are staged.\n\nStage all changes and commit them?", L"Commit")) return;
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
    message_.setText(L"");
    amend_ = false;
    statusMsg_ = L"Committed";
    Layout();
  }
}

void GitWindow::ToggleAmend() {
  if (headHash_.empty()) {
    MessageBoxW(nullptr, L"There is no previous commit to amend.", kAppName, MB_ICONINFORMATION);
    return;
  }
  amend_ = !amend_;
  std::wstring headMsg = Widen(git_.CommitMessage("HEAD"));
  if (amend_ && message_.text().empty()) message_.setText(headMsg);
  if (!amend_ && message_.text() == headMsg) message_.setText(L"");
  if (mode_ != Mode::WorkDir) list_.select(CommitListPane::kWorkdir, true);
  Layout();
}

void GitWindow::DoPush(bool force) {
  if (headBranch_.empty()) {
    MessageBoxW(nullptr, L"HEAD is detached; check out a branch before pushing.", kAppName, MB_ICONINFORMATION);
    return;
  }
  const RefInfo* cur = CurrentRef();
  std::vector<std::wstring> args = {L"push"};
  if (force) {
    if (!Confirm(L"Force push " + Widen(headBranch_) +
                     L"?\n\nThis rewrites the remote branch (uses --force-with-lease).",
                 L"Force Push"))
      return;
    args.push_back(L"--force-with-lease");
  }
  if (!cur || cur->upstream.empty()) {
    std::string remote = DefaultRemote();
    if (remote.empty()) {
      MessageBoxW(nullptr, L"This repository has no remotes configured.", kAppName, MB_ICONINFORMATION);
      return;
    }
    args.insert(args.end(), {L"-u", Widen(remote), Widen(headBranch_)});
  }
  ExecAsync(force ? L"Force push" : L"Push", args);
}

void GitWindow::StagePaths(const std::vector<std::string>& paths) {
  if (paths.empty()) return;
  std::string in = JoinNul(paths);
  keepDiffScroll_ = true;
  Exec({L"add", L"-A", L"--pathspec-from-file=-", L"--pathspec-file-nul"}, &in);
  keepDiffScroll_ = false;
}

void GitWindow::UnstagePaths(const std::vector<std::string>& paths) {
  if (paths.empty()) return;
  std::string in = JoinNul(paths);
  if (headHash_.empty())
    Exec({L"rm", L"--cached", L"-r", L"-q", L"--pathspec-from-file=-", L"--pathspec-file-nul"}, &in);
  else
    Exec({L"reset", L"-q", L"HEAD", L"--pathspec-from-file=-", L"--pathspec-file-nul"}, &in);
}

void GitWindow::DiscardEntries(const std::vector<FileEntry>& entries) {
  if (entries.empty()) return;
  std::wstring what = entries.size() == 1 ? Widen(entries[0].path) : std::to_wstring(entries.size()) + L" files";
  if (!Confirm(L"Discard all unstaged changes to " + what +
                   L"?\n\nUntracked files are moved to the Trash. Tracked changes cannot be recovered.",
               L"Discard Changes"))
    return;
  std::vector<std::string> tracked;
  std::vector<std::wstring> untracked;
  for (auto& e : entries) {
    if (e.untracked) untracked.push_back(git_.FullPath(e.path));
    else tracked.push_back(e.path);
  }
  if (!TrashFiles(untracked))
    MessageBoxW(nullptr, L"Some untracked files could not be moved to the Trash.", kAppName, MB_ICONWARNING);
  if (!tracked.empty()) {
    std::string in = JoinNul(tracked);
    Exec({L"restore", L"--worktree", L"--pathspec-from-file=-", L"--pathspec-file-nul"}, &in);
  } else {
    Refresh();
  }
}

void GitWindow::DiscardAll() {
  std::vector<FileEntry> entries;
  for (auto& e : unstaged_)
    if (!e.conflict) entries.push_back(e);
  if (entries.empty()) return;
  DiscardEntries(entries);
}

void GitWindow::ApplySelectionPatch(bool cached, bool reverse, bool wholeHunks) {
  std::string patch = diff_.buildPatch(reverse, wholeHunks);
  if (patch.empty()) {
    MessageBeep(MB_ICONWARNING);
    return;
  }
  if (!cached && !Confirm(L"Discard the selected changes from the working tree?\n\nThis cannot be undone.", L"Discard"))
    return;
  std::vector<std::wstring> args = {L"apply", L"--recount", L"--whitespace=nowarn"};
  if (cached) args.push_back(L"--cached");
  if (reverse) args.push_back(L"-R");
  args.push_back(L"-");
  keepDiffScroll_ = true;
  Exec(args, &patch);
  keepDiffScroll_ = false;
}

void GitWindow::NewBranch(const std::string& startPoint) {
  std::wstring label = startPoint.empty() ? L"New branch name (created from HEAD and checked out):"
                                          : L"New branch name (from " + Widen(startPoint.substr(0, 10)) + L"):";
  auto name = PromptText(L"Create Branch", label);
  if (!name) return;
  std::wstring n = SanitizeRefName(*name);
  if (n.empty()) return;
  std::vector<std::wstring> args = {L"checkout", L"-b", n};
  if (!startPoint.empty()) args.push_back(Widen(startPoint));
  Exec(args);
}

void GitWindow::NewTag(const std::string& at) {
  auto name = PromptText(L"Create Tag", L"Tag name:");
  if (!name) return;
  std::wstring n = SanitizeRefName(*name);
  if (n.empty()) return;
  auto message = PromptText(L"Create Tag", L"Annotation message (leave empty for a lightweight tag):");
  if (!message) return;
  if (TrimW(*message).empty()) Exec({L"tag", n, Widen(at)});
  else Exec({L"tag", L"-a", n, L"-m", TrimW(*message), Widen(at)});
}

void GitWindow::CheckoutRef(const std::string& name, bool remote) {
  if (!remote) {
    Exec({L"checkout", Widen(name)});
    return;
  }
  std::string local = name.substr(name.find('/') + 1);
  if (FindRef(local, RefInfo::Local)) Exec({L"checkout", Widen(local)});
  else Exec({L"checkout", L"--track", Widen(name)});
}

void GitWindow::DeleteLocalBranch(const std::string& name) {
  if (!Confirm(L"Delete local branch '" + Widen(name) + L"'?", L"Delete Branch")) return;
  ProcResult r = RunLogged({L"branch", L"-d", Widen(name)});
  if (!r.ok()) {
    if (r.err.find("not fully merged") != std::string::npos) {
      if (Confirm(L"Branch '" + Widen(name) +
                      L"' is not fully merged.\n\nForce delete it? Its unmerged commits may be lost.",
                  L"Force Delete Branch"))
        Exec({L"branch", L"-D", Widen(name)}, nullptr, false);
    } else {
      ShowGitError(L"git branch -d", r);
    }
  }
  Refresh();
}

int GitWindow::PickBranch(const std::wstring& title, bool includeRemotes, bool excludeCurrent, std::string& name,
                          bool& remote) {
  std::vector<std::wstring> items;
  std::vector<RefInfo> refs;
  for (const auto& r : refs_) {
    if (r.kind == RefInfo::Tag) continue;
    if (r.kind == RefInfo::Remote && !includeRemotes) continue;
    if (excludeCurrent && r.current) continue;
    items.push_back((r.kind == RefInfo::Remote ? L"remote   " : L"local    ") + Widen(r.name));
    refs.push_back(r);
  }
  if (items.empty()) {
    MessageBoxW(nullptr, L"No other branches.", kAppName, MB_ICONINFORMATION);
    return -1;
  }
  int i = PickFromList(title, items);
  if (i < 0) return -1;
  name = refs[static_cast<size_t>(i)].name;
  remote = refs[static_cast<size_t>(i)].kind == RefInfo::Remote;
  return i;
}

void GitWindow::ContinueOrAbort(bool abort) {
  if (opState_.empty()) {
    MessageBoxW(nullptr, L"No merge, rebase, cherry-pick or revert is in progress.", kAppName, MB_ICONINFORMATION);
    return;
  }
  std::wstring verb = opState_ == L"MERGING"          ? L"merge"
                      : opState_ == L"REBASING"       ? L"rebase"
                      : opState_ == L"CHERRY-PICKING" ? L"cherry-pick"
                                                      : L"revert";
  if (abort) {
    if (!Confirm(L"Abort the " + verb + L" in progress and restore the previous state?", L"Abort")) return;
    Exec({verb, L"--abort"});
  } else if (verb == L"merge") {
    DoCommit();
  } else {
    Exec({verb, L"--continue"});
  }
}

void GitWindow::ShowPalette() {
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
    ids.push_back(IDM_RECENT_BASE + static_cast<int>(i));
  }
  int i = PickFromList(L"Command Palette", items);
  if (i >= 0) OnCommand(ids[static_cast<size_t>(i)]);
}

void GitWindow::ShowFileHistory(const std::string& path) {
  pathFilter_ = Widen(path);
  search_.setText(L"", false);
  Refresh();
  SetFocus(Focus::Commits);
}

void GitWindow::ShowBlame(const std::string& path, const std::string& rev) {
  std::vector<std::wstring> args = {L"blame", L"--date=short"};
  if (!rev.empty()) args.push_back(Widen(rev));
  args.insert(args.end(), {L"--", Widen(path)});
  ProcResult r = RunLogged(args);
  if (!r.ok()) {
    ShowGitError(L"git blame", r);
    return;
  }
  diffSource_ = DiffSource::Text;
  diff_.setText(L"Blame: " + Widen(path) + (rev.empty() ? L"" : L" @ " + Widen(rev.substr(0, 10))) + L"\n\n" +
                Widen(r.out));
  Invalidate();
}

void GitWindow::OpenFile(const std::string& path) {
  if (cb_.onOpenFile) cb_.onOpenFile(git_.FullPath(path));
}

std::string GitWindow::RepoRelative(const std::wstring& absolutePath) const {
  std::wstring full = absolutePath;
  if (DWORD need = GetFullPathNameW(absolutePath.c_str(), 0, nullptr, nullptr)) {
    std::wstring buf(need, L'\0');
    DWORD n = GetFullPathNameW(absolutePath.c_str(), need, buf.data(), nullptr);
    if (n > 0 && n < need) full.assign(buf.data(), n);
  }
  const std::wstring& root = git_.root;
  if (full.size() <= root.size() || full.compare(0, root.size(), root) != 0 || full[root.size()] != L'/') return {};
  return Narrow(full.substr(root.size() + 1));
}

void GitWindow::RevealFile(const std::string& path) {
  std::wstring arg = L"/select,\"" + git_.FullPath(path) + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}

void GitWindow::AddToGitignore(const std::vector<std::string>& paths) {
  std::string file = Narrow(git_.root + L"/.gitignore");
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
// Dialogs

void GitWindow::RunModal(Modal& m) {
  Modal* outer = modal_;
  modal_ = &m;
  capture_ = Capture::None;
  caretOn_ = true;
  Invalidate();
  platform::Window* w = platform::mainWindow();
  while (!m.done && w && w->step(-1)) {
  }
  modal_ = outer;
  Invalidate();
}

std::optional<std::wstring> GitWindow::PromptText(const std::wstring& title, const std::wstring& label,
                                                  const std::wstring& initial) {
  Modal m;
  m.kind = Modal::Prompt;
  m.title = title;
  m.label = label;
  m.edit.setText(initial, true);
  RunModal(m);
  if (!m.accepted) return std::nullopt;
  return m.edit.text();
}

int GitWindow::PickFromList(const std::wstring& title, const std::vector<std::wstring>& items) {
  Modal m;
  m.kind = Modal::Picker;
  m.title = title;
  m.items = &items;
  Refilter(m);
  RunModal(m);
  return m.accepted && m.sel >= 0 && m.sel < static_cast<int>(m.visible.size())
             ? m.visible[static_cast<size_t>(m.sel)]
             : -1;
}

void GitWindow::Refilter(Modal& m) {
  m.visible.clear();
  for (size_t i = 0; i < m.items->size(); ++i)
    if (MatchesAllTokens((*m.items)[i], m.edit.text())) m.visible.push_back(static_cast<int>(i));
  m.sel = 0;
  m.top = 0;
}

bool GitWindow::Confirm(const std::wstring& text, const std::wstring& title) {
  return MessageBoxW(nullptr, text.c_str(), title.c_str(), MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
}

// The desktop's folder picker (zenity, or Ryū OS's ryu-picker, as the editor uses); a
// typed path when neither is installed.
std::optional<std::wstring> GitWindow::ChooseFolder(const std::wstring& title) {
  std::string tool;
  for (const char* t : {"zenity", "ryu-picker"})
    if (!(tool = FindInPath(t)).empty()) break;
  if (tool.empty()) {
    auto typed = PromptText(title, L"Folder path:", Widen(HomeDir() + "/"));
    if (!typed || TrimW(*typed).empty()) return std::nullopt;
    std::wstring p = TrimW(*typed);
    if (p[0] == L'~') p = Widen(HomeDir()) + p.substr(1);
    return p;
  }
  std::vector<std::string> args = {tool, "--file-selection", "--directory", "--title=" + Narrow(title)};
  if (repoOpen_) args.push_back("--filename=" + Narrow(git_.root.substr(0, git_.root.find_last_of(L'/') + 1)));
  int out[2];
  if (pipe2(out, O_CLOEXEC) < 0) return std::nullopt;
  pid_t pid = fork();
  if (pid == 0) {
    dup2(out[1], 1);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(127);
  }
  close(out[1]);
  if (pid < 0) {
    close(out[0]);
    return std::nullopt;
  }
  Modal m;
  m.kind = Modal::Wait;
  m.title = title;
  m.label = L"Choose a folder in the folder picker…";
  m.child = pid;
  std::string data;
  platform::Window* w = platform::mainWindow();
  if (w) {
    w->addWatch(out[0], [&] {
      char buf[4096];
      ssize_t n = read(out[0], buf, sizeof buf);
      if (n > 0) data.append(buf, static_cast<size_t>(n));
      else m.done = true;
    });
    RunModal(m);
    w->removeWatch(out[0]);
  }
  close(out[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return std::nullopt;
  std::string path = Trim(data.substr(0, data.find('\n')));
  if (path.empty()) return std::nullopt;
  return Widen(path);
}

void GitWindow::AcceptModal(Modal& m) {
  if (m.kind == Modal::Picker && (m.sel < 0 || m.sel >= static_cast<int>(m.visible.size()))) return;
  m.accepted = true;
  m.done = true;
}

void GitWindow::ModalKey(unsigned vk, bool ctrl, bool shift) {
  Modal& m = *modal_;
  if (m.kind == Modal::Wait) {
    if (vk == VK_ESCAPE && m.child > 0) kill(m.child, SIGTERM);
    return;
  }
  switch (vk) {
    case VK_RETURN: AcceptModal(m); break;
    case VK_ESCAPE: m.done = true; break;
    case VK_UP:
    case VK_DOWN:
    case VK_PRIOR:
    case VK_NEXT:
      if (m.kind == Modal::Picker) {
        int step = (vk == VK_UP || vk == VK_DOWN) ? 1 : std::max(1, m.listRows() - 1);
        m.sel += (vk == VK_UP || vk == VK_PRIOR) ? -step : step;
        int n = static_cast<int>(m.visible.size());
        m.sel = n ? std::clamp(m.sel, 0, n - 1) : 0;
      }
      break;
    default:
      if (m.edit.onKey(vk, ctrl, shift, nullptr) == LineEdit::KeyResult::Changed && m.kind == Modal::Picker)
        Refilter(m);
      break;
  }
  Invalidate();
}

void GitWindow::ModalMouse(int button, bool down, float x, float y) {
  Modal& m = *modal_;
  if (button < 0) {  // motion
    if (m.editDrag) m.edit.setCaret(LineEditHit(m.edit, m.editRect, x), true);
    Invalidate();
    return;
  }
  if (button != platform::MouseLeft || m.kind == Modal::Wait) return;
  if (down) {
    if (m.editRect.contains(x, y)) {
      int clicks = m.clicks.press(x, y, &m.edit);
      size_t pos = LineEditHit(m.edit, m.editRect, x);
      if (clicks >= 2) m.edit.selectWordAt(pos);
      else m.edit.setCaret(pos, GetKeyState(VK_SHIFT) < 0);
      m.editDrag = clicks < 2;
    } else if (m.kind == Modal::Picker && m.listRect.contains(x, y)) {
      int r = m.top + static_cast<int>((y - m.listRect.t) / Modal::kItemH);
      if (r >= 0 && r < static_cast<int>(m.visible.size())) {
        int clicks = m.clicks.press(x, y, &m.visible);
        m.sel = r;
        if (clicks >= 2) AcceptModal(m);
      }
    } else if (m.kind == Modal::Prompt && m.okRect.contains(x, y)) {
      m.pressed = IDOK;
    } else if (m.kind == Modal::Prompt && m.cancelRect.contains(x, y)) {
      m.pressed = IDCANCEL;
    }
  } else {
    m.editDrag = false;
    if (m.pressed == IDOK && m.okRect.contains(x, y)) AcceptModal(m);
    else if (m.pressed == IDCANCEL && m.cancelRect.contains(x, y)) m.done = true;
    m.pressed = 0;
  }
  caretOn_ = true;
  Invalidate();
}

// ---------------------------------------------------------------------------
// Context menus

int GitWindow::TrackMenu(HMENU m, float x, float y) {
  // Popups open in window coordinates: the view sits below the menu bar.
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, static_cast<int>(x),
                           static_cast<int>(y + platform::menuBarHeight()), 0, nullptr, nullptr);
  DestroyMenu(m);
  return cmd;
}

void GitWindow::ContextFileList(FileList& list, float x, float y) {
  bool stagedList = &list == &stagedList_;
  auto sel = SelectedEntries(list, stagedList ? staged_ : unstaged_);
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
  AppendMenuW(m, MF_STRING, CM_SHOW_IN_EXPLORER, L"Show in File Manager");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (allUntracked ? MF_GRAYED : 0), CM_FILE_HISTORY, L"File History");
  AppendMenuW(m, MF_STRING | (allUntracked ? MF_GRAYED : 0), CM_BLAME, L"Blame");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  if (allUntracked) AppendMenuW(m, MF_STRING, CM_IGNORE, L"Add to .gitignore");
  AppendMenuW(m, MF_STRING, CM_COPY_PATH, L"Copy Path");
  int cmd = TrackMenu(m, x, y);

  std::vector<std::string> paths;
  for (auto& e : sel) {
    paths.push_back(e.path);
    if (stagedList && !e.origPath.empty()) paths.push_back(e.origPath);
  }
  const std::string first = sel.front().path;
  switch (cmd) {
    case CM_STAGE:
    case CM_MARK_RESOLVED: StagePaths(paths); break;
    case CM_UNSTAGE: UnstagePaths(paths); break;
    case CM_DISCARD: DiscardEntries(sel); break;
    case CM_RESOLVE_OURS:
    case CM_RESOLVE_THEIRS: {
      std::string in = JoinNul(paths);
      ProcResult r = RunLogged(
          {L"checkout", cmd == CM_RESOLVE_OURS ? L"--ours" : L"--theirs", L"--pathspec-from-file=-",
           L"--pathspec-file-nul"},
          &in);
      if (!r.ok()) ShowGitError(L"git checkout", r);
      else StagePaths(paths);
      break;
    }
    case CM_OPEN_FILE: OpenFile(first); break;
    case CM_SHOW_IN_EXPLORER: RevealFile(first); break;
    case CM_FILE_HISTORY: ShowFileHistory(first); break;
    case CM_BLAME: ShowBlame(first, ""); break;
    case CM_IGNORE: AddToGitignore(paths); break;
    case CM_COPY_PATH: {
      std::wstring text;
      for (auto& p : paths) text += Widen(p) + L"\n";
      CopyToClipboard(nullptr, TrimW(text));
      break;
    }
    default: break;
  }
}

void GitWindow::ContextCommitFiles(float x, float y) {
  auto sel = filesList_.selection();
  int i = filesList_.caret();
  if (i < 0 || std::find(sel.begin(), sel.end(), i) == sel.end()) i = sel.empty() ? -1 : sel.front();
  int ci = list_.selected();
  if (i < 0 || i >= static_cast<int>(commitFiles_.size()) || ci < 0) return;
  const ChangedFile f = commitFiles_[static_cast<size_t>(i)];
  const std::string hash = commits_[static_cast<size_t>(ci)].hash;
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, CM_OPEN_FILE, L"Open File (working copy)");
  AppendMenuW(m, MF_STRING, CM_SHOW_IN_EXPLORER, L"Show in File Manager");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, CM_FILE_HISTORY, L"File History");
  AppendMenuW(m, MF_STRING | (f.status == "D" ? MF_GRAYED : 0), CM_BLAME, L"Blame at This Commit");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, CM_COPY_PATH, L"Copy Path");
  switch (TrackMenu(m, x, y)) {
    case CM_OPEN_FILE: OpenFile(f.path); break;
    case CM_SHOW_IN_EXPLORER: RevealFile(f.path); break;
    case CM_FILE_HISTORY: ShowFileHistory(f.path); break;
    case CM_BLAME: ShowBlame(f.path, hash); break;
    case CM_COPY_PATH: CopyToClipboard(nullptr, Widen(f.path)); break;
    default: break;
  }
}

void GitWindow::ContextDiff(float x, float y) {
  bool inHunk = diff_.cursorInHunk();
  bool multi = diff_.hasMultiLineSelection();
  std::string path = diff_.cursorFilePath();
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
  switch (TrackMenu(m, x, y)) {
    case CM_STAGE_HUNK: ApplySelectionPatch(true, false, true); break;
    case CM_STAGE_LINES: ApplySelectionPatch(true, false, false); break;
    case CM_UNSTAGE_HUNK: ApplySelectionPatch(true, true, true); break;
    case CM_UNSTAGE_LINES: ApplySelectionPatch(true, true, false); break;
    case CM_DISCARD_HUNK: ApplySelectionPatch(false, true, true); break;
    case CM_DISCARD_LINES: ApplySelectionPatch(false, true, false); break;
    case CM_COPY_TEXT: CopyToClipboard(nullptr, diff_.selectedText()); break;
    case CM_OPEN_FILE: OpenFile(path); break;
    case CM_FILE_HISTORY: ShowFileHistory(path); break;
    case CM_BLAME: {
      int ci = list_.selected();
      ShowBlame(path, mode_ == Mode::Commit && ci >= 0 ? commits_[static_cast<size_t>(ci)].hash : "");
      break;
    }
    default: break;
  }
}

void GitWindow::ContextCommit(int item, float x, float y) {
  if (item == CommitListPane::kWorkdir) {
    HMENU m = CreatePopupMenu();
    for (int id : std::initializer_list<int>{IDM_STAGE_ALL, IDM_UNSTAGE_ALL, IDM_DISCARD_ALL, 0, IDM_STASH, IDM_STASH_UNTRACKED}) {
      if (!id) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
      else AppendMenuW(m, MF_STRING, id, FindCommand(id)->name);
    }
    if (int cmd = TrackMenu(m, x, y)) OnCommand(cmd);
    return;
  }
  if (item < 0 || item >= static_cast<int>(commits_.size())) return;
  const Commit c = commits_[static_cast<size_t>(item)];
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
  AppendMenuW(m, MF_POPUP | (headBranch_.empty() ? MF_GRAYED : 0), reinterpret_cast<UINT_PTR>(reset),
              (L"Reset " + (headBranch_.empty() ? std::wstring(L"HEAD") : Widen(headBranch_)) + L" to This Commit")
                  .c_str());
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, CM_COPY_HASH, L"Copy Commit Hash");
  AppendMenuW(m, MF_STRING, CM_COPY_SUBJECT, L"Copy Commit Message");
  int cmd = TrackMenu(m, x, y);
  std::wstring h = Widen(c.hash);
  switch (cmd) {
    case CM_CHECKOUT_COMMIT: {
      for (const auto& b : c.badges)
        if (b.kind == RefBadge::Local) {
          Exec({L"checkout", b.name});
          return;
        }
      if (Confirm(L"Check out commit " + shortHash + L"?\n\nThis puts the repository in 'detached HEAD' state.",
                  L"Checkout Commit"))
        Exec({L"checkout", h});
      break;
    }
    case CM_BRANCH_HERE: NewBranch(c.hash); break;
    case CM_TAG_HERE: NewTag(c.hash); break;
    case CM_CHERRY_PICK:
      if (merge) Exec({L"cherry-pick", L"-m", L"1", h});
      else Exec({L"cherry-pick", h});
      break;
    case CM_REVERT:
      if (merge) Exec({L"revert", L"--no-edit", L"-m", L"1", h});
      else Exec({L"revert", L"--no-edit", h});
      break;
    case CM_RESET_SOFT: Exec({L"reset", L"--soft", h}); break;
    case CM_RESET_MIXED: Exec({L"reset", L"--mixed", h}); break;
    case CM_RESET_HARD:
      if (Confirm(L"Hard reset to " + shortHash + L"?\n\nAll uncommitted changes will be permanently lost.",
                  L"Hard Reset"))
        Exec({L"reset", L"--hard", h});
      break;
    case CM_COPY_HASH: CopyToClipboard(nullptr, h); break;
    case CM_COPY_SUBJECT: CopyToClipboard(nullptr, Widen(git_.CommitMessage(c.hash))); break;
    default: break;
  }
}

void GitWindow::ContextSidebar(float x, float y) {
  int i = tree_.selectedItem();
  if (i < 0 || i >= static_cast<int>(sideItems_.size())) return;
  const SideItem item = sideItems_[static_cast<size_t>(i)];
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
    case SideItem::RemoteGroup: AppendMenuW(m, MF_STRING, CM_REMOTE_FETCH, (L"Fetch " + name).c_str()); break;
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
  int cmd = TrackMenu(m, x, y);
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
        if (Confirm(L"Check out tag " + name + L"?\n\nThis puts the repository in 'detached HEAD' state.",
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
        MessageBoxW(nullptr, L"This repository has no remotes configured.", kAppName, MB_ICONINFORMATION);
      }
      break;
    }
    case CM_BRANCH_HERE: NewBranch(item.target); break;
    case CM_REF_RENAME: {
      auto n = PromptText(L"Rename Branch", L"New name for '" + name + L"':", name);
      if (n && !SanitizeRefName(*n).empty()) Exec({L"branch", L"-m", name, SanitizeRefName(*n)});
      break;
    }
    case CM_REF_DELETE: DeleteLocalBranch(item.name); break;
    case CM_REF_DELETE_REMOTE: {
      std::string remote = item.name.substr(0, item.name.find('/'));
      std::string branch = item.name.substr(item.name.find('/') + 1);
      if (Confirm(L"Delete branch '" + Widen(branch) + L"' from remote '" + Widen(remote) + L"'?",
                  L"Delete Remote Branch"))
        ExecAsync(L"Delete " + name, {L"push", Widen(remote), L"--delete", Widen(branch)});
      break;
    }
    case CM_REMOTE_FETCH: ExecAsync(L"Fetch " + name, {L"fetch", L"--prune", name}); break;
    case CM_TAG_PUSH: {
      std::string remote = DefaultRemote();
      if (remote.empty())
        MessageBoxW(nullptr, L"This repository has no remotes configured.", kAppName, MB_ICONINFORMATION);
      else
        ExecAsync(L"Push tag " + name, {L"push", Widen(remote), L"refs/tags/" + name});
      break;
    }
    case CM_TAG_DELETE:
      if (Confirm(L"Delete local tag '" + name + L"'?", L"Delete Tag")) Exec({L"tag", L"-d", name});
      break;
    case CM_STASH_APPLY: Exec({L"stash", L"apply", name}); break;
    case CM_STASH_POP: Exec({L"stash", L"pop", name}); break;
    case CM_STASH_DROP:
      if (Confirm(L"Drop " + name + L"? The stashed changes will be lost.", L"Drop Stash"))
        Exec({L"stash", L"drop", name});
      break;
    default: break;
  }
}

// Cut / Copy / Paste for the text fields (the edit controls' own menu on Windows).
void GitWindow::ContextEdit(Focus which, float x, float y) {
  bool editable = which == Focus::Message || which == Focus::Search;
  HMENU m = CreatePopupMenu();
  if (editable) AppendMenuW(m, MF_STRING, CM_EDIT_CUT, L"Cut\tCtrl+X");
  AppendMenuW(m, MF_STRING, CM_EDIT_COPY, L"Copy\tCtrl+C");
  if (editable) AppendMenuW(m, MF_STRING, CM_EDIT_PASTE, L"Paste\tCtrl+V");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, CM_EDIT_SELECT_ALL, L"Select All\tCtrl+A");
  int cmd = TrackMenu(m, x, y);
  if (!cmd) return;
  unsigned vk = cmd == CM_EDIT_CUT ? 'X' : cmd == CM_EDIT_COPY ? 'C' : cmd == CM_EDIT_PASTE ? 'V' : 'A';
  switch (which) {
    case Focus::Message: message_.key(vk, true, false); break;
    case Focus::Details: details_.key(vk, true, false); break;
    case Focus::Console: console_.key(vk, true, false); break;
    case Focus::Search:
      if (search_.onKey(vk, true, false, nullptr) == LineEdit::KeyResult::Changed) OnSearchChanged();
      break;
    default: break;
  }
  Invalidate();
}

void GitWindow::ContextForFocus() {
  auto menuAt = [](const RectF& r) { return std::make_pair(r.l + 40, r.t + 40); };
  if (!repoOpen_ && focus_ != Focus::Search && focus_ != Focus::Console) return;
  switch (focus_) {
    case Focus::Unstaged:
    case Focus::Staged: {
      FileList& list = focus_ == Focus::Staged ? stagedList_ : unstagedList_;
      auto [x, y] = menuAt(list.rect);
      ContextFileList(list, x, y);
      break;
    }
    case Focus::Files: {
      auto [x, y] = menuAt(filesList_.rect);
      ContextCommitFiles(x, y);
      break;
    }
    case Focus::Diff: {
      auto [x, y] = menuAt(diff_.rect);
      ContextDiff(x, y);
      break;
    }
    case Focus::Commits: {
      auto [x, y] = menuAt(list_.rect);
      ContextCommit(list_.selected(), x, y);
      break;
    }
    case Focus::Sidebar: {
      auto [x, y] = menuAt(tree_.rect);
      ContextSidebar(x, y);
      break;
    }
    case Focus::Message:
    case Focus::Details:
    case Focus::Console:
    case Focus::Search: {
      RectF r = focus_ == Focus::Message   ? message_.rect
                : focus_ == Focus::Details ? details_.rect
                : focus_ == Focus::Console ? console_.rect
                                           : searchRect_;
      ContextEdit(focus_, r.l + 20, r.t + 20);
      break;
    }
    default: break;
  }
}

// ---------------------------------------------------------------------------
// Recent repositories

void GitWindow::LoadRecent() {
  std::ifstream in(Narrow(RecentFilePath()), std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    std::wstring p = Widen(Trim(line));
    if (!p.empty() && std::find(recent_.begin(), recent_.end(), p) == recent_.end()) recent_.push_back(p);
  }
}

void GitWindow::SaveRecent() {
  std::string dir = Narrow(ConfigDir());
  mkdir(dir.c_str(), 0755);
  std::ofstream out(Narrow(RecentFilePath()), std::ios::binary | std::ios::trunc);
  for (auto& p : recent_) out << Narrow(p) << "\n";
}

void GitWindow::AddRecent(const std::wstring& path) {
  recent_.erase(std::remove(recent_.begin(), recent_.end(), path), recent_.end());
  recent_.insert(recent_.begin(), path);
  if (recent_.size() > 20) recent_.resize(20);
  SaveRecent();
  RebuildRecentMenu();
}

// ---------------------------------------------------------------------------
// Input

bool GitWindow::KeyDown(unsigned vk, bool ctrl, bool shift, bool alt) {
  caretOn_ = true;
  if (modal_) {
    ModalKey(vk, ctrl, shift);
    return true;
  }
  if (IsModifier(vk)) return false;
  if (!alt) {
    for (const auto& a : kAccels)
      if (a.vk == vk && a.ctrl == ctrl && a.shift == shift) {
        OnCommand(a.id);
        return true;
      }
  }
  if (alt) return false;  // Alt+letter opens the git menus
  if (vk == VK_APPS || (vk == VK_F10 && shift)) {
    ContextForFocus();
    return true;
  }
  if (vk == VK_TAB && !ctrl) {
    CycleFocus(shift ? -1 : 1);
    return true;
  }
  switch (focus_) {
    case Focus::Search:
      if (vk == VK_RETURN || vk == VK_DOWN) {
        SetFocus(Focus::Commits);
      } else if (search_.onKey(vk, ctrl, shift, nullptr) == LineEdit::KeyResult::Changed) {
        OnSearchChanged();
      }
      break;
    case Focus::Sidebar: {
      bool changed = false;
      tree_.key(vk, &changed);
      if (changed) OnSidebarSelect();
      break;
    }
    case Focus::Commits: list_.key(vk); break;
    case Focus::Message: message_.key(vk, ctrl, shift); break;
    case Focus::Details: details_.key(vk, ctrl, shift); break;
    case Focus::Unstaged:
    case Focus::Staged: {
      bool staged = focus_ == Focus::Staged;
      FileList& list = staged ? stagedList_ : unstagedList_;
      if ((vk == VK_SPACE || vk == VK_RETURN) && !busy_) {
        auto paths = SelectedPaths(list, staged);
        staged ? UnstagePaths(paths) : StagePaths(paths);
      } else if (vk == VK_DELETE && !staged && !busy_) {
        DiscardEntries(SelectedEntries(list, unstaged_));
      } else {
        bool changed = false;
        list.key(vk, ctrl, shift, &changed);
        if (changed) OnFileSelection(list);
      }
      break;
    }
    case Focus::Files: {
      bool changed = false;
      filesList_.key(vk, ctrl, shift, &changed);
      if (changed) OnFileSelection(filesList_);
      break;
    }
    case Focus::Diff: diff_.key(vk, ctrl, shift); break;
    case Focus::Console: console_.key(vk, ctrl, shift); break;
    default: break;
  }
  Invalidate();
  return true;
}

void GitWindow::Char(wchar_t c) {
  if (c < 0x20 || c == 0x7F) return;
  // Ctrl+key are commands; AltGr (Ctrl+Alt) still types.
  if (GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0) return;
  caretOn_ = true;
  if (modal_) {
    if (modal_->kind == Modal::Wait) return;
    modal_->edit.insert(std::wstring(1, c));
    if (modal_->kind == Modal::Picker) Refilter(*modal_);
  } else if (focus_ == Focus::Search) {
    search_.insert(std::wstring(1, c));
    OnSearchChanged();
  } else if (focus_ == Focus::Message) {
    message_.typeChar(c);
  }
  Invalidate();
}

bool GitWindow::MouseButton(int button, bool down, float x, float y) {
  mx_ = x;
  my_ = y;
  if (modal_) {
    ModalMouse(button, down, x, y);
    return true;
  }
  if (!g_) return false;
  Layout();
  const bool inside = area_.contains(x, y) && !statusRc_.contains(x, y);
  const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;

  if (!down) {
    const bool held = capture_ != Capture::None || rightPane_ != Focus::None;
    if (button == platform::MouseLeft) {
      int cmd = 0;
      if (capture_ == Capture::Button) {
        auto over = [&](const Button& b) { return b.cmd == pressedButton_ && b.visible && b.r.contains(x, y); };
        for (const Button* b : {&btnCommit_, &btnAmend_, &btnStageAll_, &btnUnstageAll_, &btnDiscardAll_})
          if (over(*b)) cmd = b->cmd;
        for (const auto& b : toolbar_)
          if (over(b)) cmd = b.cmd;
      }
      message_.release();
      details_.release();
      diff_.release();
      console_.release();
      tree_.release();
      list_.release();
      unstagedList_.release();
      stagedList_.release();
      filesList_.release();
      capture_ = Capture::None;
      pressedButton_ = 0;
      if (cmd) OnCommand(cmd);
    } else if (button == platform::MouseRight && rightPane_ != Focus::None) {
      Focus pane = rightPane_;
      rightPane_ = Focus::None;
      bool editPane = pane == Focus::Message || pane == Focus::Details || pane == Focus::Console ||
                      pane == Focus::Search;
      if (repoOpen_ || editPane) {
        switch (pane) {
          case Focus::Sidebar:
            if (tree_.rect.contains(x, y)) ContextSidebar(x, y);
            break;
          case Focus::Commits:
            if (list_.rect.contains(x, y))
              ContextCommit(rightItem_ == CommitListPane::kNone ? list_.selected() : rightItem_, x, y);
            break;
          case Focus::Unstaged:
          case Focus::Staged: {
            FileList& list = pane == Focus::Staged ? stagedList_ : unstagedList_;
            if (list.rect.contains(x, y)) ContextFileList(list, x, y);
            break;
          }
          case Focus::Files:
            if (filesList_.rect.contains(x, y)) ContextCommitFiles(x, y);
            break;
          case Focus::Diff:
            if (diff_.rect.contains(x, y)) ContextDiff(x, y);
            break;
          default: ContextEdit(pane, x, y); break;
        }
      }
    }
    Invalidate();
    return inside || held;
  }

  if (!inside) return false;
  caretOn_ = true;
  if (button == platform::MouseRight) {
    rightItem_ = CommitListPane::kNone;
    rightPane_ = Focus::None;
    if (tree_.rect.contains(x, y)) {
      SetFocus(Focus::Sidebar);
      int node = tree_.nodeAt(x, y);
      if (node >= 0 && node != tree_.selected()) {
        tree_.select(node);
        OnSidebarSelect();
      }
      if (node >= 0) rightPane_ = Focus::Sidebar;
    } else if (list_.rect.contains(x, y)) {
      SetFocus(Focus::Commits);
      rightItem_ = list_.itemAt(x, y);
      if (rightItem_ != CommitListPane::kNone) list_.press(x, y);
      rightPane_ = Focus::Commits;
    } else if (unstagedList_.rect.contains(x, y) || stagedList_.rect.contains(x, y)) {
      bool staged = stagedList_.rect.contains(x, y);
      FileList& list = staged ? stagedList_ : unstagedList_;
      SetFocus(staged ? Focus::Staged : Focus::Unstaged);
      if (list.press(x, y, false, false, true)) OnFileSelection(list);
      if (list.rowAt(x, y) >= 0) rightPane_ = staged ? Focus::Staged : Focus::Unstaged;
    } else if (filesList_.rect.contains(x, y)) {
      SetFocus(Focus::Files);
      if (filesList_.press(x, y, false, false, true)) OnFileSelection(filesList_);
      if (filesList_.rowAt(x, y) >= 0) rightPane_ = Focus::Files;
    } else if (diff_.rect.contains(x, y)) {
      SetFocus(Focus::Diff);
      diff_.press(x, y, false, true);
      rightPane_ = Focus::Diff;
    } else if (message_.rect.contains(x, y)) {
      SetFocus(Focus::Message);
      rightPane_ = Focus::Message;
    } else if (details_.rect.contains(x, y)) {
      SetFocus(Focus::Details);
      rightPane_ = Focus::Details;
    } else if (console_.rect.contains(x, y)) {
      SetFocus(Focus::Console);
      rightPane_ = Focus::Console;
    } else if (searchRect_.contains(x, y)) {
      SetFocus(Focus::Search);
      rightPane_ = Focus::Search;
    }
    Invalidate();
    return true;
  }
  if (button != platform::MouseLeft) return true;

  if (split1_.contains(x, y)) {
    capture_ = Capture::Split1;
  } else if (split2_.contains(x, y)) {
    capture_ = Capture::Split2;
  } else if (searchRect_.contains(x, y)) {
    SetFocus(Focus::Search);
    int clicks = clicks_.press(x, y, &search_);
    size_t pos = LineEditHit(search_, searchRect_, x);
    if (clicks >= 2) search_.selectWordAt(pos);
    else search_.setCaret(pos, shift);
    if (clicks < 2) capture_ = Capture::Search;
  } else if (tree_.rect.contains(x, y)) {
    SetFocus(Focus::Sidebar);
    int clicks = clicks_.press(x, y, &tree_);
    bool toggled = false;
    if (tree_.press(x, y, clicks, &toggled)) OnSidebarSelect();
    if (clicks == 2 && !toggled) OnSidebarActivate();
    capture_ = Capture::Sidebar;
  } else if (list_.rect.contains(x, y)) {
    SetFocus(Focus::Commits);
    list_.press(x, y);
    capture_ = Capture::Commits;
  } else if (message_.rect.contains(x, y)) {
    SetFocus(Focus::Message);
    message_.press(x, y, shift, clicks_.press(x, y, &message_));
    capture_ = Capture::Message;
  } else if (details_.rect.contains(x, y)) {
    SetFocus(Focus::Details);
    details_.press(x, y, shift, clicks_.press(x, y, &details_));
    capture_ = Capture::Details;
  } else if (unstagedList_.rect.contains(x, y) || stagedList_.rect.contains(x, y)) {
    bool staged = stagedList_.rect.contains(x, y);
    FileList& list = staged ? stagedList_ : unstagedList_;
    SetFocus(staged ? Focus::Staged : Focus::Unstaged);
    int clicks = clicks_.press(x, y, &list);
    int row = list.rowAt(x, y);
    if (list.press(x, y, ctrl, shift, false)) OnFileSelection(list);
    if (clicks == 2 && row >= 0 && !busy_) {
      auto paths = SelectedPaths(list, staged);
      staged ? UnstagePaths(paths) : StagePaths(paths);
    }
    capture_ = staged ? Capture::Staged : Capture::Unstaged;
  } else if (filesList_.rect.contains(x, y)) {
    SetFocus(Focus::Files);
    if (filesList_.press(x, y, ctrl, shift, false)) OnFileSelection(filesList_);
    capture_ = Capture::Files;
  } else if (diff_.rect.contains(x, y)) {
    SetFocus(Focus::Diff);
    diff_.press(x, y, shift, false);
    capture_ = Capture::Diff;
  } else if (console_.rect.contains(x, y)) {
    SetFocus(Focus::Console);
    console_.press(x, y, shift, false);
    capture_ = Capture::Console;
  } else {
    auto hit = [&](const Button& b) { return b.visible && b.r.contains(x, y); };
    for (const auto& b : toolbar_)
      if (hit(b)) pressedButton_ = b.cmd;
    for (const Button* b : {&btnCommit_, &btnAmend_, &btnStageAll_, &btnUnstageAll_, &btnDiscardAll_})
      if (hit(*b)) pressedButton_ = b->cmd;
    if (pressedButton_) capture_ = Capture::Button;
  }
  Invalidate();
  return true;
}

bool GitWindow::MouseMove(float x, float y, MergeView::Pointer* pointer) {
  mx_ = x;
  my_ = y;
  *pointer = MergeView::Pointer::Arrow;
  if (modal_) {
    if (modal_->editRect.contains(x, y)) *pointer = MergeView::Pointer::IBeam;
    ModalMouse(-1, false, x, y);
    return true;
  }
  if (!g_) return false;
  Layout();
  const bool inside = area_.contains(x, y) && !statusRc_.contains(x, y);
  switch (capture_) {
    case Capture::Split1:
      sidebarW_ = x - area_.l;
      *pointer = MergeView::Pointer::ResizeH;
      Invalidate();
      return true;
    case Capture::Split2:
      listW_ = x - area_.l - (sidebarVisible_ ? sidebarW_ + 5 : 0);
      *pointer = MergeView::Pointer::ResizeH;
      Invalidate();
      return true;
    case Capture::Search: search_.setCaret(LineEditHit(search_, searchRect_, x), true); break;
    case Capture::Message: message_.dragTo(x, y); break;
    case Capture::Details: details_.dragTo(x, y); break;
    case Capture::Diff: diff_.dragTo(x, y); break;
    case Capture::Console: console_.dragTo(x, y); break;
    case Capture::Sidebar: tree_.dragTo(x, y); break;
    case Capture::Commits: list_.dragTo(x, y); break;
    case Capture::Unstaged: unstagedList_.dragTo(x, y); break;
    case Capture::Staged: stagedList_.dragTo(x, y); break;
    case Capture::Files: filesList_.dragTo(x, y); break;
    default: break;
  }
  if (capture_ != Capture::None && capture_ != Capture::Button) {
    Invalidate();
    if (capture_ == Capture::Search || capture_ == Capture::Message || capture_ == Capture::Details)
      *pointer = MergeView::Pointer::IBeam;
    return true;
  }
  if (split1_.contains(x, y) || split2_.contains(x, y)) *pointer = MergeView::Pointer::ResizeH;
  else if (searchRect_.contains(x, y) || message_.rect.contains(x, y) || details_.rect.contains(x, y))
    *pointer = MergeView::Pointer::IBeam;
  // Repaint only when what lies under the pointer changes (hover highlights).
  int key = 0;
  for (const auto& b : toolbar_)
    if (b.r.contains(x, y)) key = 1000 + b.cmd;
  for (const Button* b : {&btnCommit_, &btnAmend_, &btnStageAll_, &btnUnstageAll_, &btnDiscardAll_})
    if (b->visible && b->r.contains(x, y)) key = 1000 + b->cmd;
  if (tree_.rect.contains(x, y)) key = 100000 + tree_.nodeAt(x, y) + (x > tree_.rect.r - 10 ? 50000 : 0);
  const std::pair<FileList*, int> lists[] = {{&unstagedList_, 200000}, {&stagedList_, 300000}, {&filesList_, 400000}};
  for (const auto& [list, base] : lists)
    if (list->rect.contains(x, y)) key = base + list->rowAt(x, y) + (x > list->rect.r - 10 ? 50000 : 0);
  if (key != hoverKey_ || capture_ == Capture::Button) {
    hoverKey_ = key;
    Invalidate();
  }
  return inside || capture_ != Capture::None;
}

bool GitWindow::Wheel(float notches, bool horizontal, float x, float y) {
  if (modal_) {
    if (modal_->kind == Modal::Picker) {
      modal_->top = std::max(0, modal_->top - static_cast<int>(std::lround(notches * 3)));
      modal_->sel = std::clamp(modal_->sel, modal_->top, modal_->top + modal_->listRows() - 1);
      Invalidate();
    }
    return true;
  }
  if (!g_ || !area_.contains(x, y)) return false;
  if (GetKeyState(VK_SHIFT) < 0) horizontal = true;
  if (tree_.rect.contains(x, y)) tree_.wheel(notches);
  else if (list_.rect.contains(x, y)) list_.wheel(notches);
  else if (message_.rect.contains(x, y)) message_.wheel(notches);
  else if (details_.rect.contains(x, y)) details_.wheel(notches);
  else if (unstagedList_.rect.contains(x, y)) unstagedList_.wheel(notches);
  else if (stagedList_.rect.contains(x, y)) stagedList_.wheel(notches);
  else if (filesList_.rect.contains(x, y)) filesList_.wheel(notches);
  else if (diff_.rect.contains(x, y)) diff_.wheel(notches, horizontal);
  else if (console_.rect.contains(x, y)) console_.wheel(notches, horizontal);
  Invalidate();
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// MergeView: the embeddable wrapper used by Verso.

struct MergeView::Impl {
  GitWindow win;
};

std::unique_ptr<MergeView> MergeView::Create(HINSTANCE, HWND, MergeViewCallbacks callbacks, std::wstring* error) {
  auto impl = std::make_unique<Impl>();
  if (!impl->win.Create(std::move(callbacks), error)) return nullptr;
  return std::unique_ptr<MergeView>(new MergeView(std::move(impl)));
}

MergeView::MergeView(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MergeView::~MergeView() {
  if (impl_ && impl_->win.menu_) DestroyMenu(impl_->win.menu_);
}

HWND MergeView::hwnd() const { return nullptr; }
HMENU MergeView::menu() const { return impl_->win.menu_; }
bool MergeView::busy() const { return impl_->win.busy_; }
bool MergeView::modal() const { return impl_->win.InModal(); }

void MergeView::paint(verso::Gfx& g, const verso::Theme& theme, const verso::RectF& area) {
  impl_->win.Paint(g, theme, area);
}

bool MergeView::keyDown(unsigned vk, bool ctrl, bool shift, bool alt) {
  return impl_->win.KeyDown(vk, ctrl, shift, alt);
}
void MergeView::charTyped(wchar_t c) { impl_->win.Char(c); }
bool MergeView::mouseButton(int button, bool down, float x, float y) {
  return impl_->win.MouseButton(button, down, x, y);
}
bool MergeView::mouseMove(float x, float y, Pointer* pointer) { return impl_->win.MouseMove(x, y, pointer); }
bool MergeView::wheel(float notches, bool horizontal, float x, float y) {
  return impl_->win.Wheel(notches, horizontal, x, y);
}

void MergeView::onMenuCommand(WPARAM wp, LPARAM) {
  GitWindow& w = impl_->win;
  if (w.InModal()) return;  // the menu bar stays usable over a dialog; its commands wait
  w.OnCommand(LOWORD(wp));
}

void MergeView::activate() {
  GitWindow& w = impl_->win;
  if (!w.repoOpen_) {
    // Try the recent repositories in order. Unavailable ones stay listed: a removable
    // or network drive may come back.
    const std::vector<std::wstring> recent = w.recent_;
    for (const auto& r : recent)
      if (w.OpenRepo(r, true)) break;
  } else if (!w.busy_ && !w.InModal()) {
    w.Refresh();
  }
  w.SetFocus(Focus::Commits);
}

bool MergeView::openRepository(const std::wstring& path) {
  GitWindow& w = impl_->win;
  Git probe = w.git_;
  std::wstring err;
  if (!probe.Open(path, err)) return false;
  if (w.repoOpen_ && probe.root == w.git_.root) {
    if (!w.busy_ && !w.InModal()) w.Refresh();
    return true;
  }
  if (w.busy_ || w.InModal()) return false;  // the running operation's completion expects its repository
  return w.OpenRepo(probe.root);
}

void MergeView::refresh() {
  GitWindow& w = impl_->win;
  if (w.repoOpen_ && !w.busy_ && !w.InModal()) w.Refresh();
}

void MergeView::focus() { impl_->win.SetFocus(Focus::Commits); }

bool MergeView::showFileHistory(const std::wstring& absolutePath) {
  GitWindow& w = impl_->win;
  if (w.busy_ || w.InModal()) return false;
  std::string rel = w.RepoRelative(absolutePath);
  if (!rel.empty()) w.ShowFileHistory(rel);
  return true;
}

bool MergeView::showBlame(const std::wstring& absolutePath) {
  GitWindow& w = impl_->win;
  if (w.busy_ || w.InModal()) return false;
  std::string rel = w.RepoRelative(absolutePath);
  if (!rel.empty()) w.ShowBlame(rel, "");
  return true;
}
