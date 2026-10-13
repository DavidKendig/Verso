# Verso git mode

Git mode is Verso's git client: a Git client written in C++20 with plain Win32 and no third-party libraries. It runs as a child window inside Verso's main window (`MergeView` in `src/merge_view.h`), and Ctrl+Shift+G switches between it and the editor. It has a commit graph in the middle, staging on the right, and hunk- and line-level staging in the diff.

All of the code is original. Git mode runs every repository operation through the `git` command line, so it respects your git config, hooks, credential manager and SSH setup.

## Features

- **Commit graph:** lane-based graph with merge nodes, branch/remote/tag/HEAD badges, author, short hash and relative dates. Up to 5,000 commits across all branches, remotes and tags.
- **Search** (Ctrl+F): filters commits by message, author, e-mail, hash or ref name.
- **Working directory:** separate unstaged and staged lists, a commit message box, Commit (Ctrl+Enter) and Amend.
- **Hunk and line staging:** right-click in the diff to stage, unstage or discard a single hunk, line or selection. This is built on `git apply --cached --recount`. It also works on untracked files, where you can stage only part of a new file.
- **Commit view:** full commit details, the list of changed files (click one to jump to its diff), and a combined diff. Merge commits are diffed against their first parent.
- **Sidebar:** local branches with ahead/behind counts, remotes, tags and stashes. Click an item to jump to its commit, double-click to check it out, right-click for actions.
- **Branching:** create, checkout (including tracking a remote branch), merge, rebase, rename and delete (with force-delete when the branch isn't merged); also delete a remote branch.
- **Remotes:** fetch, pull, pull with rebase, push (sets upstream automatically), and force push with lease. Network operations run on a background thread.
- **Commit actions:** checkout, create a branch or tag here, cherry-pick, revert, reset (soft, mixed or hard), copy the hash or message.
- **Conflicts:** shows merge, rebase, cherry-pick and revert state. Resolve with ours or theirs, mark resolved, continue or abort.
- **Stash:** stash, stash including untracked files, pop, apply, drop, and view a stash's diff.
- **File tools:** file history, blame (at the working copy or at any commit), open the file, show it in Explorer, add it to `.gitignore`.
- **Git console** (Ctrl+`): every command git mode runs, with its output.
- **Command palette** (Ctrl+P) and a recent-repositories list.
- Clone and init new repositories.
- Dark theme, per-monitor DPI awareness, draggable pane splitters.
- Discarding untracked files moves them to the Recycle Bin.

## Building

Requirements: Windows 10 or 11, Visual Studio 2019 or newer (or the Build Tools) with the C++ workload, and [Git for Windows](https://git-scm.com/download/win).

Git mode is built into Verso as the `verso_gitview` library (with the shared `verso_git` layer). From the repository root:

```powershell
.\build.ps1          # Release -> build\release\Verso.exe
.\build.ps1 -Test    # also runs verso_git_selftest and the editor tests
```

`build.ps1` finds MSVC through `vswhere`, so you don't need a Developer Command Prompt.

## Running

Open git mode from Verso with Ctrl+Shift+G, the **Git** status bar switch, the branch in the status bar, or Git > File History / Blame File. Git mode opens the repository of the file you're editing; with nothing open, it reopens the most recent repository. **Open File** switches back to the editor with that file open, and File > Switch to Text Editor (Ctrl+Shift+G) returns without opening anything.

```bat
verso_git_selftest.exe   :: runs the git/graph/staging self-test
```

The self-test builds a throwaway repository in `%TEMP%`. It checks log parsing, decorations, the graph layout, annotated tags, line/hunk stage, unstage and discard, partial staging of untracked files, CRLF preservation and UTF-8 commit messages.

## Keyboard shortcuts

| Keys | Action |
|---|---|
| Ctrl+O | Open repository |
| Ctrl+P | Command palette |
| F5 | Refresh |
| F7 | Pull |
| Ctrl+Enter | Commit |
| Ctrl+S / Ctrl+Shift+S | Stash / Pop stash |
| Ctrl+Shift+A / Ctrl+Shift+R | Stage all / Unstage all |
| Ctrl+Shift+B | Create branch |
| Ctrl+Shift+C | Checkout branch |
| Ctrl+F | Search commits |
| Ctrl+0 / Ctrl+9 | Go to HEAD / working directory |
| Ctrl+1 / 2 / 3 | Focus sidebar / commit list / diff |
| Ctrl+` | Toggle git console |
| F9 | Toggle sidebar |
| Esc | Clear search or file-history filter |
| Space / Enter (file list) | Stage or unstage the selected files |
| Delete (unstaged list) | Discard the selected files |
| Double-click a file | Stage or unstage it |
| Shift+click / drag in the diff | Select lines for line staging |

## Source layout

| File | Purpose |
|---|---|
| `src/git_process.*` | Runs child processes with piped stdin/stdout/stderr. Only the child's own pipe handles are inherited, so it's safe to call from several threads. |
| `src/git.*` | Git wrapper: log, status, refs, stashes, diffs and synthetic new-file patches. |
| `src/graph.*` | Assigns commits to graph lanes. |
| `src/commit_list.*` | Owner-drawn, virtualised commit list (graph drawn with GDI+). |
| `src/diff_view.*` | Owner-drawn diff viewer and the patch builder used for hunk and line staging. |
| `src/dialogs.*` | Text prompt, filterable picker (command palette), folder chooser. |
| `src/main_window.cpp` | The git view: layout, menus, context menus and all actions, plus the `MergeView` wrapper. |
| `src/merge_view.h` | Embedding interface used by Verso (menu, accelerators, mode-switch and open-file callbacks). |
| `src/theme.*` | Colours, fonts and dark-mode helpers. |
| `tests/selftest.cpp` | Console self-test. |

Recent repositories are saved in `%APPDATA%\Verso\GitRecent.txt` (copied from `%APPDATA%\MergePoint\recent.txt` on first run after the rename).
