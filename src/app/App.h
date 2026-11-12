#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/Document.h"
#include "app/FileTree.h"
#include "app/Gfx.h"
#include "app/GitService.h"
#include "app/Keymap.h"
#include "app/LineEdit.h"
#include "app/MarkdownPreview.h"
#include "app/McpServer.h"
#include "app/Settings.h"
#include "app/TerminalView.h"
#include "app/Theme.h"
#include "util/Json.h"

class MergeView;
#ifndef _WIN32
namespace verso::platform {
class Window;
}
#endif

namespace verso {

inline constexpr UINT WM_APP_INDEX_READY = WM_APP + 1;
inline constexpr UINT WM_APP_GIT_READY = WM_APP + 2;
// WM_APP + 3 and + 4 are used by PdfView and MarkdownImages.
inline constexpr UINT WM_APP_TERMINAL = WM_APP + 5;  // wParam 0: output, 1: the shell exited
inline constexpr UINT WM_APP_MCP = WM_APP + 6;       // tool calls from AI clients are waiting
inline constexpr UINT_PTR kTimerAutoscroll = 2;  // drag-selection autoscroll (App.cpp, AppInput.cpp)
inline constexpr float kSidebarRow = 24.0f;      // side bar row height (AppInput.cpp, AppRender.cpp)

struct CommandDef {
  std::string id;
  std::wstring caption;  // Command Palette caption; empty hides it from the palette
  std::function<void()> run;
  bool recordable = false;  // editing commands captured by macro recording
};

class App {
 public:
  App();
  ~App();
  int run(HINSTANCE instance, int show, const std::vector<std::wstring>& args);

 private:
  // --- App.cpp: window and lifecycle -----------------------------------------
  static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
  void invalidate();
  void updateTitle();
  void computeLayout();
  void onTimer(UINT_PTR id);
  void resetBlink();
  void setStatus(std::wstring msg, int ms = 4000);
  void toggleFullscreen();
  void setFullscreen(bool on);
  void toggleDistractionFree();
  void onActivateApp(bool active);
  void openPaths(const std::vector<std::wstring>& paths);
  void requestClose();
  void applyDarkTitleBar();
  void updateImePosition();
  // Shared by the Win32 window procedure and the Linux front end.
  bool onAppMessage(UINT msg, WPARAM wp);  // WM_APP+n posted by background work
  void onMenuCommand(UINT id);             // a menu item was chosen
  enum class CursorKind { Arrow, IBeam, SizeWE, Hand };
  CursorKind cursorAt(float x, float y);   // the mouse cursor over a client point

#ifndef _WIN32
  // --- linux/AppLinux.cpp: the Linux front end ----------------------------------
 public:
  // A message box drawn over the window (MessageBoxW on Linux).
  int messageBox(const std::wstring& text, const std::wstring& caption, UINT type);
  static App* current();
  bool menuBarShown() const { return !fullscreen_; }

 private:
  struct LinuxUi;
  std::unique_ptr<platform::Window> window_;
  std::unique_ptr<LinuxUi> lx_;
  void linuxPaint(cairo_t* cr, int w, int h);
  void linuxKeyDown(unsigned vk, bool repeat);
  void linuxChar(char32_t c);
  void linuxMouseButton(int button, bool down, float x, float y);
  void linuxMouseMove(float x, float y);
  void linuxWheel(float notches, bool horizontal, float x, float y);
  UINT linuxPopup(HMENU menu, int x, int y, UINT flags);
  void paintMenuBar();
  void paintPopups();
  void paintDialog();
  bool menuBarMouse(int button, bool down, float x, float y);
  bool popupMouse(int button, bool down, float x, float y);
  bool popupKey(unsigned vk);
  void openMenuBarItem(int index);
  std::vector<std::wstring> runFileDialog(const std::vector<std::string>& args);
#endif

  // --- AppFiles.cpp: documents, files, session and settings -----------------
  Document* doc();
  Document& newDocument(bool activate = true);
  Document* openFile(const std::wstring& path, bool activate = true);
  bool saveDocument(Document& d, bool saveAs);
  void saveAll();
  bool closeDocument(size_t index, bool prompt = true);
  // Shows `path` in the active tab, replacing a document without unsaved changes
  // (otherwise it opens in a new tab).
  Document* openFileInPlace(const std::wstring& path);
  void moveDocument(size_t from, size_t to);  // tab order, which OPEN FILES follows
  void closeAll();
  void activate(size_t index);
  void reloadFromDisk(Document& d);
  void checkFilesOnDisk();
  void setLanguage(Document& d, const Language& lang, bool fromUser);
  void configureEditor(Document& d);
  void detectIndentation(Document& d);
  void openFolder(const std::wstring& folder, bool replace);
  void rebuildIndex();
  std::vector<std::wstring> promptOpenFiles();
  std::wstring promptSaveFile(const Document& d);
  std::wstring promptFolder();
  void addRecent(std::vector<std::wstring>& list, const std::wstring& item);
  std::wstring dataDir();
  void loadSettings(bool showErrors);
  void loadKeymap(bool showErrors);
  void applyTheme();
  void applyFont();
  void saveSession();
  bool restoreSession();
  // Whether this window owns the hot-exit session; extra windows (-n) do not.
  bool ownsSession_ = true;
  void openConfigFile(const std::wstring& path, const std::string& defaults);
  void openScratch(const std::wstring& content, const Language* lang);
  uint64_t fileWriteTime(const std::wstring& path);
  LineEnding defaultLineEnding() const;
  void sidebarContextMenu(TreeNode* node, POINT screen);

  // --- AppCommands.cpp: command registry, menus ------------------------------
  void registerCommands();
  void addCommand(std::string id, std::wstring caption, std::function<void()> fn, bool recordable = false);
  void runCommand(const std::string& id);
  void edit(const std::function<void(Editor&)>& fn, bool reveal = true);
  void afterEdit(Document& d, bool reveal);
  void buildMenu();
  void onInitMenuPopup(HMENU menu);
  void showContextMenu(POINT screen);
  void showStatusMenu(int kind, POINT screen);
  UINT trackMenu(HMENU menu, POINT screen);
  void doCopy(bool cut);
  void doPaste();
  void changeFontSize(float delta);
  void toggleMacroRecording();
  void playMacro();
  std::wstring keyBindingReference();

  // --- AppInput.cpp: keyboard, mouse, scrolling ------------------------------
  void onKeyDown(UINT vk, bool& handled);
  void onChar(wchar_t c);
  void typeText(const std::wstring& s);
  bool widgetKey(UINT vk, bool ctrl, bool shift, bool alt);
  void onMouseDown(int button, float x, float y);
  void onMouseMove(float x, float y);
  void onMouseUp(int button, float x, float y);
  void onMouseWheel(float notches, bool horizontal, float x, float y);
  void updateDrag(float x, float y);
  size_t hitTestText(float x, float y, double* col = nullptr, size_t* line = nullptr);
  // A press in an input field: places the caret (and starts a drag selection), or
  // selects the word under it on a double-click.
  void pressField(LineEdit& e, const RectF& r, float x, float y);
  void revealCaret(bool center = false);
  void scrollBy(float dy, float dx);
  void clampScroll(Document& d);
  float maxScrollY(const Document& d) const;
  float maxScrollX(Document& d);
  int pageLines() const;
  POINT toScreen(float x, float y) const;

  // --- AppPanels.cpp: find panel and overlays --------------------------------
  enum class FindMode { Hidden, Find, Replace };
  enum class Overlay { None, Commands, Files, Line, Symbols, Prompt };
  struct OverlayItem {
    std::wstring title, detail, hint;
    std::vector<size_t> titleMarks, detailMarks;
    int score = 0;
    std::string command;
    std::wstring path;
    int docIndex = -1;
    size_t line = 0;
    std::wstring symbol;
  };
  struct FindLayout {
    RectF toggles[4];
    RectF findBox, replaceBox;
    RectF find, prev, all, replace, replaceAll;
  };

  void showFind(bool replace, bool incremental = false);
  void hideFind();
  bool compileFind(Searcher& s);
  void findNext(bool forward);
  void findAll();
  void replaceNext();
  void replaceAll();
  void onFindTextChanged();
  void useSelectionForFind();
  void findUnder(bool forward);
  void findInFiles(const std::wstring& query);
  bool openResultAt(Document& results, size_t line);
  void showOverlay(Overlay mode, std::wstring text = {});
  void closeOverlay(bool accepted);
  void refreshOverlay();
  void acceptOverlay(int index);
  void previewOverlay();
  void prompt(std::wstring label, std::wstring initial, std::function<void(const std::wstring&)> done);
  FindLayout findLayout() const;
  RectF overlayRect() const;
  RectF overlayInputRect() const;
  float overlayItemHeight() const;
  int overlayVisibleCount() const;

  // --- AppGit.cpp: git status, gutter change markers, git mode -----------------
  void gitRefresh();
  void onGitResults();
  void requestGitBaseline(Document& d);
  void updateGitHunks(Document& d);
  const GitRepoState* gitRepoForActive();
  bool ensureMergeView();
  void layoutMergeView();
  std::wstring gitTargetPath();
  void setGitMode(bool on, const std::wstring& repoPath = {});

  // --- AppMarkdown.cpp: Markdown mode, word wrap, preview -----------------------
  enum class MdView { Write, Split, Read };
  void setEditorMode(bool markdown);
  void setMdView(MdView view);
  bool markdownReadOnly() const;
  bool wrapActive() const;
  size_t wrapColumns() const;
  void ensureWrap(Document& d);
  double lineAtEditorY(Document& d, float y);
  void scrollEditorToLine(Document& d, double line);
  void moveVisual(long long rows, bool extend);
  float previewZoom() const;
  void paintPreview();
  void scrollPreview(float dy);
  bool previewKey(UINT vk, bool shift, bool alt);
  struct NavPoint;
  NavPoint navPointOf(const Document& d) const;
  void pushMarkdownNav(const NavPoint& p);
  bool createNote(const std::wstring& path);  // a new "# Name" note; false (with status) on failure
  bool navigateMarkdown(bool back);  // follows the link history (mouse back/forward)
  // Follows a link clicked in the preview: in this tab, or in a new tab when `newTab`.
  void openPreviewLink(const MarkdownPreview::Link& link, bool newTab);
  void openLinkedFile(const std::wstring& path, const std::wstring& anchor, bool newTab, bool created = false);
  // The notes collection a document belongs to: its vault or git root, else the open
  // side bar folder containing it, else its own folder.
  std::wstring noteRootFor(const Document& d);
  void followHeading(const std::wstring& anchor);
  void mdToggleWrap(const std::wstring& marker);
  void mdInsertLink();
  bool mdContinueList();
  RectF mdViewRect() const;
  bool editBlocked();  // Markdown Read view or a PDF: the buffer must not change

  // --- AppPdf.cpp: rendered, read-only PDF documents ----------------------------
  Document* openPdf(const std::wstring& path, bool activateIt);
  void onPdfReady();
  void paintPdf();
  bool pdfKey(UINT vk, bool shift);
  void zoomPdf(float factor, float anchorY = -1);  // factor <= 0 resets to fit width
  std::wstring pdfStatusText(Document& d);
  void gitOpenRepository();
  void gitFileTool(bool blame, const std::wstring& path);
  void gotoModification(bool forward);
  void revertHunk();

  // --- AppTerminal.cpp: terminal mode -------------------------------------------
  // Consumed: handled here; Char: the key's character goes to the shell via WM_CHAR;
  // App: one of Verso's own shortcuts.
  enum class TermKey { Consumed, Char, App };
  void setTerminalMode(bool on);
  std::wstring terminalFolder() const;
  void selectTerminal();  // shows (starting if needed) the shell of the current folder
  bool startTerminal();
  void onTerminalMessage(WPARAM kind);
  void layoutTerminal();
  void paintTerminal();
  TermKey terminalKeyDown(UINT vk, bool ctrl, bool shift, bool alt);
  void terminalChar(wchar_t c, bool alt);
  void terminalSend(std::string_view bytes);
  void terminalPaste();
  bool terminalCopy();
  bool terminalCell(float x, float y, size_t& line, int& col);
  void terminalMouseDown(int button, float x, float y);
  void terminalDrag(float x, float y);
  void terminalWheel(float notches);
  RectF terminalToggleRect() const;
  std::wstring terminalShell() const;
  std::wstring terminalStatusText();
  void connectAiTool();  // types the MCP registration command for an AI CLI

  // --- AppMcp.cpp: tools for AI clients (Model Context Protocol) ----------------
  void startMcp();
  json::Value mcpTools() const;
  json::Value mcpCall(const std::string& tool, const json::Value& args);
  std::wstring mcpResolve(const std::wstring& path) const;
  Document* mcpDocument(const json::Value& args, std::wstring& err);
  json::Value mcpDocInfo(const Document& d) const;

  // --- AppRender.cpp ----------------------------------------------------------
  struct TabRect {
    RectF r, close;
    size_t index;
  };
  struct StatusItem {
    RectF r;
    int kind;  // 0 indentation, 1 line endings, 2 encoding, 3 syntax, 4 git branch
  };
  struct SidebarRow {
    enum class Kind { Header, OpenFile, Tree } kind;
    std::wstring label;
    size_t docIndex = 0;
    TreeNode* node = nullptr;
    int depth = 0;
  };
  std::vector<SidebarRow> sidebarRows();
  RectF sidebarToggleRect() const;
  RectF modeSwitchRect() const;
  void paint();
  void paintTabs();
  void paintSidebar();
  void paintEditor();
  void paintMinimap(Document& d);
  void paintScrollbar(Document& d);
  void paintFind();
  void paintStatus();
  void paintOverlay();
  void paintWelcome();
  void paintLineEdit(LineEdit& e, const RectF& r, bool focused, const std::wstring& placeholder, bool error = false);
  void paintButton(const RectF& r, std::wstring_view label, bool on);
  size_t lineEditHit(LineEdit& e, const RectF& r, float x);
  float gutterWidth(const Document& d) const;
  std::vector<TabRect> tabRects();
  RectF scrollThumb(Document& d) const;
  float textLeft(const Document& d) const;

  // --- State -----------------------------------------------------------------
  HINSTANCE inst_ = nullptr;
  HWND hwnd_ = nullptr;
  HMENU menu_ = nullptr;
  HMENU recentMenu_ = nullptr;
  Gfx gfx_;
  Theme theme_ = Theme::makeDark();
  Settings settings_;
  Keymap keymap_;
  float fontSizeOverride_ = 0;

  std::vector<std::unique_ptr<Document>> docs_;
  size_t active_ = 0;
  uint64_t nextDocId_ = 1;
  std::vector<std::wstring> recentFiles_, recentFolders_, closedFiles_;

  std::vector<CommandDef> commands_;
  std::unordered_map<std::string, size_t> commandIndex_;
  std::vector<std::string> recentCommands_;
  std::unordered_map<UINT, std::string> menuCommands_;
  UINT nextMenuId_ = 1000;

  struct Layout {
    RectF tabs, sidebar, splitter, editor, gutter, text, minimap, vscroll, find, status, preview, pdf, terminal;
  } lay_;
  float width_ = 0, height_ = 0;
  bool sidebarVisible_ = false;
  float sidebarWidth_ = 240;
  float sidebarScroll_ = 0;
  bool minimapVisible_ = true;
  bool fullscreen_ = false, distractionFree_ = false;
#ifdef _WIN32
  WINDOWPLACEMENT savedPlacement_{sizeof(WINDOWPLACEMENT)};
#endif
  FileTree tree_;
  FileIndex index_;
  GitService git_;
  std::unique_ptr<MergeView> merge_;  // git mode, created on first use
  bool gitMode_ = false;
  bool startInGitMode_ = false;
  std::wstring mergeTitle_;
  bool mdMode_ = false;  // Markdown mode (stays set while git mode is shown)
  bool terminalMode_ = false, startInTerminalMode_ = false;
  // One shell per project folder; the others keep running in the background.
  std::vector<std::unique_ptr<TerminalView>> terms_ = [] {
    std::vector<std::unique_ptr<TerminalView>> v;
    v.push_back(std::make_unique<TerminalView>());
    return v;
  }();
  TerminalView* term_ = terms_.front().get();  // the terminal terminal mode shows
  McpServer mcp_;
  MdView mdView_ = MdView::Split;
  MarkdownPreview preview_;
  // Link history for the Markdown preview, and a heading to scroll to once a
  // followed document is laid out.
  struct NavPoint {
    uint64_t docId;
    std::wstring path;  // reopens the file when its tab was replaced or closed
    float previewScroll, scrollY;
  };
  std::vector<NavPoint> navBack_, navForward_;
  uint64_t pendingAnchorDoc_ = 0;
  std::wstring pendingAnchor_;
  bool indexStarted_ = false;
  float tabScroll_ = 0;

  enum class Focus { Editor, Find, Replace, Overlay };
  Focus focus_ = Focus::Editor;
  FindMode findMode_ = FindMode::Hidden;
  bool findIncremental_ = false;
  LineEdit findEdit_, replaceEdit_;
  SearchOptions findOpts_;
  bool findWrap_ = true;
  std::wstring findError_;
  std::vector<Selection> findOrigin_;
  std::wstring findCountKey_;
  size_t findCount_ = 0;

  Overlay overlay_ = Overlay::None;
  LineEdit overlayEdit_;
  std::vector<OverlayItem> overlayItems_;
  int overlaySel_ = 0;
  int overlayTop_ = 0;
  uint64_t overlayDocId_ = 0;
  std::vector<Selection> overlayRestoreSels_;
  float overlayRestoreX_ = 0, overlayRestoreY_ = 0;
  bool overlayPreviewed_ = false;
  std::wstring promptLabel_;
  std::function<void(const std::wstring&)> promptDone_;
  std::vector<FileIndex::Entry> indexSnapshot_;
  std::vector<Symbol> overlaySymbols_;

  std::optional<KeyCombo> pendingChord_;
  bool suppressChar_ = false;
  wchar_t highSurrogate_ = 0;

  enum class Drag { None, Text, Column, Gutter, Minimap, Scrollbar, Splitter, Tab, OpenFile, Field, Terminal };
  Drag drag_ = Drag::None;
  int clickCount_ = 0;
  // Double-clicks in input fields (Find, Replace, Goto Anything).
  const LineEdit* fieldClicked_ = nullptr;
  DWORD fieldClickTime_ = 0;
  float fieldClickX_ = 0, fieldClickY_ = 0;
  DWORD lastClickTime_ = 0;
  float lastClickX_ = 0, lastClickY_ = 0;
  Selection dragAnchor_;
  int dragGranularity_ = 0;  // 0 characters, 1 words, 2 lines
  std::vector<Selection> dragBaseSels_;
  size_t dragLine_ = 0;
  double dragCol_ = 0;
  float dragOffset_ = 0;
  LineEdit* dragField_ = nullptr;
  RectF dragFieldRect_;
  float mouseX_ = -1, mouseY_ = -1;
  bool trackingMouse_ = false;

  bool caretVisible_ = true;
  std::wstring status_;
  ULONGLONG statusUntil_ = 0;
  bool sessionDirty_ = false;
  std::wstring lastCopied_;
  bool lastCopiedWholeLines_ = false;
  struct MacroStep {
    std::string command;
    std::wstring text;
  };
  bool recording_ = false;
  bool playingMacro_ = false;
  std::vector<MacroStep> macro_, recordingMacro_;
  bool checkingFiles_ = false;
  std::vector<StatusItem> statusItems_;

  // Bracket-match cache keyed by document state.
  uint64_t bracketDoc_ = 0, bracketState_ = UINT64_MAX, bracketSel_ = UINT64_MAX;
  size_t bracketA_ = static_cast<size_t>(-1), bracketB_ = static_cast<size_t>(-1);
};

}  // namespace verso
