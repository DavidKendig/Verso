#include <windows.h>

#include <algorithm>

#include "app/App.h"
#include "merge_view.h"

namespace verso {

namespace {

constexpr size_t kMaxDiffLines = 200000;

}  // namespace

// ---------------------------------------------------------------------------
// Background status and gutter change markers

void App::gitRefresh() {
  if (!git_.available() || (!settings_.showGitStatus && !settings_.miniDiff)) return;
  std::vector<std::wstring> paths = tree_.folders();
  for (const auto& d : docs_)
    if (!d->path.empty()) paths.push_back(d->path);
  git_.refresh(std::move(paths));
}

void App::requestGitBaseline(Document& d) {
  if (!settings_.miniDiff || d.path.empty()) return;
  git_.requestBaseline(d.id, d.path);
}

void App::onGitResults() {
  std::vector<GitBaseline> baselines;
  bool reposChanged = git_.collect(baselines);
  for (auto& b : baselines) {
    for (auto& d : docs_) {
      if (d->id != b.docId || d->path != b.path) continue;
      d->hasGitBaseline = b.tracked;
      d->gitBaseline = std::move(b.text);
      d->gitHunks.clear();
      d->gitHunksState = UINT64_MAX;
    }
  }
  if (reposChanged) {
    // The index may have changed (commits, staging in git mode); reload the bases.
    for (auto& d : docs_) requestGitBaseline(*d);
  }
  invalidate();
}

void App::updateGitHunks(Document& d) {
  if (!d.hasGitBaseline || d.gitHunksState == d.ed.stateId()) return;
  d.gitHunksState = d.ed.stateId();
  const TextBuffer& b = d.ed.buffer();
  if (b.lineCount() > kMaxDiffLines) {
    d.gitHunks.clear();
    return;
  }
  std::vector<std::wstring_view> cur;
  cur.reserve(b.lineCount());
  for (size_t l = 0; l < b.lineCount(); ++l) cur.push_back(b.lineText(l));
  // A trailing newline leaves an empty final line in the buffer; ignore it like the baseline does.
  if (!cur.empty() && cur.back().empty() && b.size() > 0) cur.pop_back();
  d.gitHunks = diffLines(splitLineViews(d.gitBaseline), cur);
}

const GitRepoState* App::gitRepoForActive() {
  if (Document* d = doc(); d && !d->path.empty())
    if (const GitRepoState* r = git_.repoFor(d->path)) return r;
  for (const auto& f : tree_.folders())
    if (const GitRepoState* r = git_.repoFor(f)) return r;
  return nullptr;
}

// ---------------------------------------------------------------------------
// Git mode: the embedded git client

bool App::ensureMergeView() {
  if (merge_) return true;
  MergeViewCallbacks cb;
  cb.onTitle = [this](const std::wstring& title) {
    mergeTitle_ = title;
    if (gitMode_) updateTitle();
  };
  cb.onOpenFile = [this](const std::wstring& path) {
    setGitMode(false);
    openFile(path);
  };
  cb.onSwitchMode = [this] { setGitMode(false); };
  std::wstring err;
  merge_ = MergeView::Create(inst_, hwnd_, std::move(cb), &err);
  if (!merge_) {
    MessageBoxW(hwnd_, (L"Git mode is unavailable.\n\n" + err).c_str(), L"Verso", MB_ICONWARNING);
    return false;
  }
  return true;
}

void App::layoutMergeView() {
  if (!merge_) return;
  RECT rc;
  GetClientRect(hwnd_, &rc);
  int statusPx = static_cast<int>(lay_.status.h() * gfx_.dpi() / 96.0f + 0.5f);
  MoveWindow(merge_->hwnd(), 0, 0, rc.right, std::max(0L, rc.bottom - statusPx), TRUE);
}

std::wstring App::gitTargetPath() {
  if (const GitRepoState* r = gitRepoForActive()) return r->root;
  if (Document* d = doc(); d && !d->path.empty()) return parentOf(d->path);
  if (!tree_.empty()) return tree_.folders().front();
  return {};
}

void App::setGitMode(bool on, const std::wstring& repoPath) {
  if (on) {
    if (!ensureMergeView()) return;
    if (overlay_ != Overlay::None) closeOverlay(false);
    pendingChord_.reset();
    std::wstring target = repoPath.empty() ? gitTargetPath() : repoPath;
    bool wasOn = gitMode_;
    terminalMode_ = false;  // git mode takes the window; the shell keeps running
    gitMode_ = true;
    computeLayout();
    layoutMergeView();
    ShowWindow(merge_->hwnd(), SW_SHOW);
    if (!fullscreen_) SetMenu(hwnd_, merge_->menu());
    // Follow the repository of the file being edited; otherwise reopen the last one.
    if (target.empty() || !merge_->openRepository(target)) {
      if (!target.empty() && merge_->busy())
        setStatus(L"A git operation is still running; showing its repository until it finishes.", 4000);
      merge_->activate();
    } else {
      merge_->focus();
    }
    if (!wasOn) sessionDirty_ = true;
  } else {
    if (!gitMode_) return;
    if (GetFocus() && merge_ && IsChild(merge_->hwnd(), GetFocus())) SetFocus(hwnd_);
    gitMode_ = false;
    if (merge_) ShowWindow(merge_->hwnd(), SW_HIDE);
    if (!fullscreen_) SetMenu(hwnd_, menu_);
    SetFocus(hwnd_);
    // Git operations may have changed files and the index.
    checkFilesOnDisk();
    for (auto& d : docs_) requestGitBaseline(*d);
    gitRefresh();
    sessionDirty_ = true;
  }
  computeLayout();
  updateTitle();
  DrawMenuBar(hwnd_);
  resetBlink();
  invalidate();
}

void App::gitOpenRepository() { setGitMode(true); }

void App::gitFileTool(bool blame, const std::wstring& path) {
  if (path.empty()) {
    setStatus(L"Save the file first to see its git history.");
    return;
  }
  const GitRepoState* r = git_.repoFor(path);
  if (!r) {
    setStatus(fileNameOf(path) + L" is not in a git repository.");
    return;
  }
  setGitMode(true, r->root);
  if (!gitMode_ || !merge_) return;
  if (!(blame ? merge_->showBlame(path) : merge_->showFileHistory(path)))
    setStatus(L"A git operation is still running; try again when it finishes.", 4000);
}

// ---------------------------------------------------------------------------
// Change navigation

void App::gotoModification(bool forward) {
  Document* d = doc();
  if (!d) return;
  updateGitHunks(*d);
  if (d->gitHunks.empty()) {
    setStatus(d->hasGitBaseline ? L"No changes since the git index." : L"This file is not tracked by git.");
    return;
  }
  const TextBuffer& b = d->ed.buffer();
  size_t line = b.lineOf(d->ed.selections().primary().caret);
  const LineHunk* target = nullptr;
  if (forward) {
    for (const auto& h : d->gitHunks)
      if (h.newStart > line) {
        target = &h;
        break;
      }
    if (!target) target = &d->gitHunks.front();
  } else {
    for (auto it = d->gitHunks.rbegin(); it != d->gitHunks.rend(); ++it)
      if (it->newStart + std::max<size_t>(it->newCount, 1) - 1 < line) {
        target = &*it;
        break;
      }
    if (!target) target = &d->gitHunks.back();
  }
  size_t l = std::min(target->newStart, b.lineCount() - 1);
  d->ed.setSelections({Selection::at(b.lineStart(l))}, 0);
  afterEdit(*d, false);
  revealCaret(true);
}

void App::revertHunk() {
  Document* d = doc();
  if (!d || editBlocked()) return;
  updateGitHunks(*d);
  const TextBuffer& b = d->ed.buffer();
  size_t line = b.lineOf(d->ed.selections().primary().caret);
  const LineHunk* hunk = nullptr;
  for (const auto& h : d->gitHunks) {
    bool inside = h.newCount > 0 ? (line >= h.newStart && line < h.newStart + h.newCount)
                                 : (line == h.newStart || line + 1 == h.newStart);
    if (inside) {
      hunk = &h;
      break;
    }
  }
  if (!hunk) {
    setStatus(L"No change at the cursor to revert.");
    return;
  }
  auto old = splitLineViews(d->gitBaseline);
  std::wstring repl;
  for (size_t i = hunk->oldStart; i < hunk->oldStart + hunk->oldCount && i < old.size(); ++i) {
    repl += old[i];
    repl += L'\n';
  }
  const size_t count = b.lineCount();
  size_t from = hunk->newStart < count ? b.lineStart(hunk->newStart) : b.size();
  size_t end = hunk->newStart + hunk->newCount;
  size_t to = end < count ? b.lineStart(end) : b.size();
  bool atEof = to == b.size() && (b.size() == 0 || b.at(b.size() - 1) != L'\n');
  if (atEof && !repl.empty()) {
    repl.pop_back();  // keep the file's missing final newline
    if (from == b.size() && from > 0) repl.insert(repl.begin(), L'\n');
  }
  d->ed.commit({{from, to - from, repl}}, {Selection::at(from)}, 0, EditKind::Other);
  afterEdit(*d, true);
  setStatus(L"Reverted the change to its git index version.", 2500);
}

}  // namespace verso
