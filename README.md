<p align="center">
  <img src="res/icons/verso-512.png" alt="Verso logo" width="192" height="192">
</p>

# Verso

A fast, native Windows editor for code, notes and git, in one window.

Verso is inspired by Sublime Text and uses the same commands and shortcuts, so
if you've used Sublime, you'll feel at home.

- **Text** is a code editor with multiple cursors, Goto Anything, the Command
  Palette and a minimap.
- **MD** is the same editor tuned for writing Markdown, with a live preview,
  images and wiki links. Shortcut: Ctrl+K Ctrl+M.
- **Git** is a full git client: commit graph, staging by hunk or line,
  branches, stashes, blame and history. Shortcut: Ctrl+Shift+G.
- **Terminal** (the **>_** button, or Ctrl+\`) is your shell, right in the
  window. AI coding tools you run there can control Verso.

Current version: **0.26.008**.

## What you need

Windows 10 (version 1809 or later) or Windows 11. You'll also want
[Git for Windows](https://gitforwindows.org/) for git mode and the git features
in the editor. Everything else works without it.

## Getting around

```
Verso.exe [files or folders...] [-n | --new-window]
```

Files you open go to the Verso window that's already running. Folders show up in
the side bar. Add `-n` if you really want a second window. (From a source
checkout, `launch.bat` does the same and builds Verso first if needed.)

Verso remembers where you were. When you close it, your open tabs, unsaved
changes, folders, cursor positions and window size are saved, and they come
back next time. This is called hot exit. It also reopens in the mode you left
it in.

Moving between modes keeps your place:

- Git mode opens the repository of the file you're editing. It refreshes each
  time you switch to it.
- **File History**, **Blame File** and clicking the branch name in the status
  bar all jump to git mode for that file.
- **Open File** in git mode takes you back to the editor with that file open.
- Files that git changed while you were away (checkout, reset, pull) reload
  when you come back.
- Leaving git mode returns you to Text or MD, whichever you were using.

The title bar shows "Verso - *file name*", or "Verso - Git: *repo (branch)*" in
git mode. Text and MD mode don't have a Git menu. Git Mode lives in the View
menu, and all the git commands are in the Command Palette.

## Features

### Editing

- **Multiple cursors.** Ctrl+D adds the next match, and Ctrl+K Ctrl+D skips one.
  Alt+F3 selects every match. Ctrl+Click adds a cursor, Ctrl+Alt+Up/Down adds
  one on the next line, and Ctrl+Shift+L splits a selection into lines.
  Column selection works with middle-drag, Shift+right-drag or Shift+Alt+drag.
- **Brackets and quotes** close themselves. You can type over them, delete
  pairs together, and wrap a selection by typing an opening bracket.
- **Smart indent**, including between braces and after `:` in Python.
- **Line tools.** Move lines with Ctrl+Shift+Up/Down. Duplicate is
  Ctrl+Shift+D, delete is Ctrl+Shift+K and join is Ctrl+J. Ctrl+Enter and
  Ctrl+Shift+Enter insert a line, and Ctrl+] / Ctrl+[ indent.
- **Comments.** Ctrl+/ toggles line comments and Ctrl+Shift+/ toggles block
  comments, in the right style for each language.
- **Text tools.** Change case; sort, reverse or de-duplicate lines; transpose;
  trim trailing spaces.
- **Movement** by word, by subword (camelCase and snake_case) and by bracket.
- **Undo** groups your typing sensibly. Undo all the way back and the file
  counts as unchanged again.
- **Clipboard.** With nothing selected, copy and cut take the whole line.
  Pasting several lines onto the same number of cursors puts one on each.
- **Macros.** Ctrl+Q records and Ctrl+Shift+Q plays back.
- **Word wrap** (View > Word Wrap). Wrapped lines keep their indent, and the
  arrow keys move by what you see on screen.

### Finding things

- **Goto Anything** (Ctrl+P) fuzzy-searches your open files and project folders.
  Add `:42` for a line, `@name` for a symbol or `#word` to search.
- **Command Palette** (Ctrl+Shift+P) lists every command with its shortcut.
- **Goto Symbol** (Ctrl+R) and **Goto Line** (Ctrl+G) preview as you type. Press
  Escape to go back to where you were.
- **Find and Replace** (Ctrl+F / Ctrl+H) supports regex, match case, whole word
  and wrap-around (Alt+R/C/W/A). Matches highlight live, with a count.
  Replacements can use `$1` or `\1`, and Up/Down recalls earlier searches.
- **Find in Files** (Ctrl+Shift+F) searches your project folders. Double-click a
  result, or press Enter on it, to jump there.
- Incremental find (Ctrl+I), quick find (Ctrl+F3) and find the selection
  (Ctrl+E).

### The window

- **Tabs** show unsaved changes. Middle-click closes a tab, and you can drag
  tabs to reorder them. Alt+1 to Alt+9 jump straight to one.
- **The side bar** has an OPEN FILES list and a FOLDERS tree. Toggle it with
  Ctrl+K Ctrl+B or the button in the bottom-left corner. Right-click for New
  File/Folder, Rename, Delete (to the Recycle Bin), Copy Path and Reveal in
  Explorer. You can drag OPEN FILES entries to reorder them, and the tabs
  follow. The FOLDERS tree always stays sorted.
- A minimap, indent guides, rulers, current-line highlight and bracket matching.
  Other copies of the selected text get an outline.
- **Syntax highlighting** for 30+ languages, picked from the file's extension,
  name or `#!` line.
- **The status bar** lets you click to change indentation, line endings,
  encoding and language.
- Full screen (F11) and distraction-free mode (Shift+F11).
- Dark and light themes. The dark theme darkens the title bar and menus too.
  Sharp on high-DPI screens, with proper IME support.

### Git in the editor

These need Git for Windows. Status updates when you switch back to Verso, after
you save, and when you open files or folders.

- **Side bar badges**: `M` modified (orange) or staged (green), `A` added,
  `R` renamed, `U` untracked, `D` deleted and `!` conflicted. Folders with
  changes get a dot, and ignored files are dimmed.
- **The status bar** shows your branch, any merge or rebase in progress, and how
  many files changed. Click it to open git mode.
- **Gutter markers** show your changes against git's staged version as you type:
  green for added lines, orange for changed lines and red where lines were
  deleted.
- Jump between changes with Ctrl+. and Ctrl+, and undo a change at the cursor
  with **Revert Hunk** (Ctrl+K Ctrl+Z).
- In git mode, git's own shortcuts apply: Ctrl+Enter commits, F5 refreshes,
  Ctrl+Shift+A stages everything, and Ctrl+P opens the git command palette. See
  [merge/README.md](merge/README.md) for the rest. Ctrl+Shift+G takes you back
  to the editor.

Verso runs the real `git` command, so your git config, hooks and saved
credentials all work as usual.

### Files

- **Encodings**: UTF-8 (with or without BOM), UTF-16 and Windows-1252 are
  detected when you open a file and kept when you save. Line endings (CRLF, LF,
  CR) are kept too, and you can convert them.
- Saves are atomic, so a crash can't leave a half-written file. If a file
  changes on disk, Verso notices and reloads it.
- Indentation (tabs or spaces, and how many) is detected per file.
- Recent files and folders are in the File menu.
- **PDFs** open as pages, not as garbage text. They're read-only. Pages fit the
  window width, and Ctrl+wheel or Ctrl+= / Ctrl+- zooms (Ctrl+0 resets). The
  status bar shows the page you're on. You can't select or search text in a
  PDF yet.
- Binary files open read-only.

## Markdown mode

MD mode is the editor set up for writing: lines wrap, and new files are
Markdown. Next to the mode buttons you get a **Write | Split | Read** switch.
Ctrl+K Ctrl+R cycles through them.

- **Write** shows just the editor, full width.
- **Split** (the default) shows the editor on the left and the preview on the
  right. Scrolling either one scrolls the other.
- **Read** shows just the rendered page, full width. Scroll it with the arrow
  keys, Page Up/Down, Space or Home/End.

The preview handles everything you'd expect: headings, bold, italic,
strikethrough, code, links, lists, task lists, quotes, tables, horizontal rules
and code blocks with syntax highlighting. It also understands the HTML that
READMEs often use, such as `<img>`, `<a>`, `<br>`, `<b>`, `<i>` and centered
blocks (`<p align="center">`, `<div align="center">`, `<center>`). Other raw
HTML and footnotes show as plain text.

**Images** can go anywhere: on their own line, inside a sentence, in lists,
tables and headings, or wrapped in links (so rows of badges line up). They can
come from:

- a file path, relative to the document. A path starting with `/` is relative
  to the git repository, the way GitHub reads it;
- the web, downloaded in the background. Set `"markdown_remote_images": false`
  if you'd rather the preview didn't contact image hosts;
- a `data:` URI.

PNG, JPEG, GIF, BMP, TIFF, ICO and SVG all work, and so does any other format
Windows has a codec for, such as WebP. Big images shrink to fit,
local images update when the file changes, and a broken image shows its alt
text instead. Each image can be up to 32 MB.

**Links.** Click a link to another file, like `docs/guide.md#install`, and
Verso opens it **in the same tab**, scrolled to that heading.
**Shift+click** (or middle-click) opens it **in a new tab** instead. If you
have unsaved changes, the link opens in a new tab so nothing is lost. `#heading`
links scroll the page, web and email links open in your browser, and a link to
a folder opens its README.

**Wiki links** work like they do in Obsidian: `[[Page]]`, `[[Page|label]]`,
`[[Page#Heading]]`, `[[folder/Page]]` and `[[#Heading]]`. `Page` means
`Page.md`. Verso looks for the page next to the current note first, then
anywhere in your notes, choosing the closest match. "Your notes" means the
nearest folder above the note that is an Obsidian vault or a git repository.
If there isn't one, it's the side bar folder the note is in.

- **Clicking a link to a page that doesn't exist creates it** next to the
  current note, with a heading, and opens it. Links to missing pages are drawn
  dimmed, so you can see which ones will make a new note.
- Ordinary Markdown links to notes, like `[Gold Coast](Gold%20Coast.md)` as
  Obsidian writes them, are found the same way.
- `![[image.png]]` embeds an image, and `![[image.png|200]]` or `|200x100` sets
  its size.

To go back and forward through links you've followed, use your mouse's
back/forward buttons. In the Read view, Backspace / Shift+Backspace and
Alt+Left / Alt+Right work too.

While writing:

- Ctrl+B and Ctrl+I make the selection, or the word under the cursor, bold or
  italic. The Command Palette also has Inline Code, Strikethrough and Insert
  Link.
- Enter continues a list, a numbered list, a task list or a quote. Press Enter
  on an empty item to end the list.

## Terminal mode

Click **>_** in the bottom-left corner, or press Ctrl+\`, and your shell takes
the place of the editor. The side bar stays, and clicking a file in it takes
you back to the editor. The shell keeps running while you use the other modes.

**Each project folder gets its own shell**, started in that folder. Switch to
another folder and the terminal switches with you. Switch back and you get the
earlier shell, history and all. Shells for other folders keep running in the
background, and the status bar tells you how many. With no folder open, the
shell starts in the current file's folder, or your home folder.

- It's PowerShell 7 if you have it, otherwise Windows PowerShell. Set
  `"terminal_shell"` in Preferences to use something else, such as `"cmd.exe"`
  or `"wsl.exe"`.
- Full-screen programs, colors and resizing all work, because it uses a real
  Windows pseudo console.
- **Copy and paste**: Ctrl+C copies if text is selected and otherwise stops the
  running program. Ctrl+V, Ctrl+Shift+V, right-click and middle-click all paste.
  Drag to select, and double-click to select a word.
- **Scroll back** through 10,000 lines with the mouse wheel or Shift+Page Up.
  Start typing to jump back to the bottom.
- Ctrl+Shift+P, Ctrl+Shift+G, F11 and Ctrl+\` still go to Verso. Every other
  key goes to the shell.
- If the shell exits, press Enter to start a new one.

## Letting AI tools use Verso

Verso can be driven by AI coding tools such as Claude Code, Codex and Gemini CLI,
using the [Model Context Protocol](https://modelcontextprotocol.io) (MCP). Once
it's connected, an AI tool can work alongside you. It can open files, read what's
in the editor (unsaved changes included), make edits you can undo, save, search,
select text, switch modes, run Verso commands and leave you a message in the
status bar.

**Set it up once per tool.** Open the Command Palette and run **Terminal:
Connect an AI Tool to Verso**, then pick your tool. Verso types the setup
command into the terminal for you to check, and you press Enter to run it. If
you'd rather type it yourself:

```powershell
claude mcp add --scope user verso "C:\path\to\verso-mcp.exe"
codex mcp add verso '--' "C:\path\to\verso-mcp.exe"
gemini mcp add --scope user verso "C:\path\to\verso-mcp.exe"
```

For any other MCP client, **Copy MCP Configuration (JSON)** in the same menu
gives you a ready-made `mcpServers` entry.

A few things worth knowing:

- AI tools started in Verso's terminal connect to that Verso window. Started
  anywhere else, they find the Verso that's running, or open one.
- Only programs running as your Windows user can connect, and nothing can
  connect over the network.
- Edits stay unsaved until the AI tool saves (or you do), and PDFs and binary
  files can't be edited.
- To switch this off, set `"mcp_server": false` in Preferences.

Behind the scenes, the AI tool runs `verso-mcp.exe`, which passes messages to
Verso over a private named pipe. Verso offers 17 tools, including `get_state`,
`open_file`, `read_document`, `replace_text`, `insert_text`, `save_document`,
`find_text`, `set_selection`, `set_mode` and `run_command`.

## Fonts

Code text uses **JetBrains Mono with a slashed zero**, so `0` and `O` never get
confused. Menus and labels use Segoe UI. The font is built into `Verso.exe`, so
there's nothing to install.

It's JetBrains Mono 2.304 with one change: the slashed zero is the default `0`.
Normally you'd turn that on with an OpenType feature, but git mode draws text
with GDI, which can't use those features. The modified font is called
**JetBrains Mono Slashed**, so it won't clash with a JetBrains Mono you
already have installed.

To use a different font, set `"font_face"` in Preferences to any font on your
system.

The font files and their license are in `res/fonts/`. `tools/make_font.py`
rebuilds them from the official release using
[fontTools](https://github.com/fonttools/fonttools):
`python tools/make_font.py <folder with JetBrainsMono-*.ttf>`.

## Settings

Your settings live in `%APPDATA%\Verso\`:

| File | What it's for |
| --- | --- |
| `Preferences.json` | Your settings (Preferences > Settings), such as `font_face`, `font_size`, `tab_size`, `translate_tabs_to_spaces`, `draw_minimap`, `rulers`, `hot_exit`, `theme`, `word_wrap`, `markdown_remote_images`, `terminal_shell`, `mcp_server`, `word_separators`, `folder_exclude_patterns`, `show_git_status` and `mini_diff`. Changes apply as soon as you save. |
| `Keymap.json` | Your own shortcuts (Preferences > Key Bindings), as `{"keys": [...], "command": "..."}` entries. Chords like `["ctrl+k", "ctrl+u"]` work. |
| `Session.json` | What hot exit remembers. Verso writes this itself. |

Help > Key Binding Reference lists every command and its shortcut.

## Building from source

You need Windows 10 (version 1809 or later) or Windows 11, and Visual Studio
2022 or newer (the free Build Tools are enough) with the **Desktop development
with C++** workload. That gives you the compiler, CMake and Ninja.

```powershell
.\build.ps1                    # release build: build\release\Verso.exe
.\build.ps1 -Config Debug      # debug build:   build\debug\
.\build.ps1 -Test              # build and run the tests
```

This builds `Verso.exe` and `verso-mcp.exe` (the helper AI tools use; keep the
two together). Start `build\release\Verso.exe` to run it.

There are two test suites. `verso_tests` covers the editor core: editing,
search, encodings, syntax, Markdown and the terminal. `verso_git_selftest`
covers the git layer against a throwaway repository in `%TEMP%`.

You can also use CMake directly from an "x64 Native Tools" prompt:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

A few more notes:

- The version number lives in one place, `VERSO_VERSION` in `CMakeLists.txt`.
  The About box, the file properties and the MCP server all read it from there.
- After a build, `cpack` makes the release ZIP. It contains both programs, this
  README, the license, the NOTICE file and the font license.
- The C runtime is linked in, so `Verso.exe` has no extra DLLs to ship.
- The icons are made from the logo by `tools/make_icon.py`, which needs
  [Pillow](https://python-pillow.org/).

## Mac and Linux

Verso runs on Windows today. The core of the editor is written in plain C++ so
it can move to other systems later:

| Part | Windows | macOS | Linux |
| --- | --- | --- | --- |
| Editor core: editing, search, encodings, syntax highlighting, Markdown, the terminal emulator (`src/core`, `src/syntax`, `src/util`) | Yes | Should build; not yet tried on a Mac | Yes, builds and passes all tests with g++ 13 |
| The app itself: window, drawing, input, git mode, terminal, PDFs, images, AI connection (`src/app`, `merge/`, `src/mcp`) | Yes | Not yet | Not yet |

On a Mac or Linux, CMake builds the core and runs its tests:
`cmake -S . -B build && cmake --build build && ctest --test-dir build`.

To bring the full app over, someone would write a new front end on top of the
same core. It would need to provide:

- a window and text drawing (Cocoa and Core Text, GTK with Cairo and Pango, or a
  cross-platform toolkit);
- a pseudo terminal (`forkpty`) for the terminal;
- running git, file dialogs, the clipboard, and a Unix socket in place of the
  named pipe for AI tools.

The icons are ready for all three: `res/Verso.ico` for Windows, `res/Verso.icns`
for a Mac app and `res/icons/verso-<size>.png` (16 to 512 px) for Linux.

## How the code is organized

```
src/core/      The editor engine, with no UI
  TextBuffer     the text and its line index
  Selection      multiple cursors and selections
  Editor         every editing command, undo/redo, clipboard and search
  Search         plain and regex search
  Encoding       encoding detection, line endings, safe saving
  LineDiff       the diff behind git change markers
  Markdown       the Markdown parser for the preview
  WrapMap        word-wrapped rows
  VtScreen       the terminal emulator
src/syntax/    languages, highlighting and symbols
src/util/      JSON and fuzzy matching
src/app/       the Windows app: window, drawing, input, panels, commands, sessions
  GitService     git status in the background
  AppGit         git badges, branch, gutter markers, switching to git mode
  AppMarkdown    MD mode: views, scroll sync, links, writing helpers
  MarkdownPreview, MarkdownImages, MarkdownLinks   the preview, its images and links
  PdfView        PDF pages (AppPdf connects it to the app)
  Pty            running a shell in a pseudo console
  AppTerminal    terminal mode
  McpServer      the AI connection (AppMcp: the tools it offers)
src/mcp/       verso-mcp.exe, the helper AI tools run
merge/src/     git mode, plus the git library both modes share
tests/, merge/tests/   tests
res/           logo, icons, fonts, manifest and version templates
tools/         make_icon.py and make_font.py
```

## What's not here (yet)

- plugins and Package Control
- split panes
- code folding
- auto-complete, snippets, build systems, Goto Definition and project-wide
  symbol search
- spell check, bookmarks, and several windows in one process

## License

Verso is Copyright 2026 David Kendig and is licensed under the
[Apache License, Version 2.0](LICENSE). You may use, change and share it,
including commercially. Copies and versions you share must include the
license and the [NOTICE](NOTICE) file, which credits Verso and its font. The
bundled font has its own license; see below.

## Credits and licenses

### Project code

| Component | Origin | License |
| --- | --- | --- |
| The app (`src/`, `tests/`, `tools/`, `merge/`, build scripts) | Written for this project | [Apache License 2.0](LICENSE), Copyright 2026 David Kendig |


No source code was copied from any other project. Where Verso follows another
program's behavior or a published technique, the code was written from scratch:

- **Myers diff** (`src/core/LineDiff.cpp`) is written from the published
  algorithm: E. W. Myers, "An O(ND) Difference Algorithm and Its Variations",
  *Algorithmica*, 1986.
- **The dark menu bar** (`src/app/DarkMenu.cpp`) uses undocumented Windows
  messages (`WM_UAHDRAWMENU`, `WM_UAHDRAWMENUITEM`) and `uxtheme.dll` ordinals
  135 and 136. The details come from public community write-ups, such as the
  MIT-licensed `win32-darkmode` projects; Notepad++ uses the same technique. No
  code from those projects is included.



### Built into Verso.exe

| Component | Version | Copyright | License |
| --- | --- | --- | --- |
| [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) (modified and renamed "JetBrains Mono Slashed") | 2.304 | 2020 The JetBrains Mono Project Authors | [SIL Open Font License 1.1](res/fonts/OFL.txt). Modified versions must stay under the OFL. The license text is in `res/fonts/OFL.txt` and inside the program. |
| Microsoft C++ Standard Library (STL), statically linked | MSVC 14.50 | Microsoft Corporation | Apache License 2.0 with LLVM Exception (no attribution needed for compiled programs) |
| Microsoft C/C++ runtime (UCRT, VCRuntime), statically linked | MSVC 14.50 / Windows SDK 10.0.26100 | Microsoft Corporation | Visual Studio / Windows SDK license terms (distributable code) |

### Used while running, not included

| Component | How Verso uses it | License |
| --- | --- | --- |
| Windows: Win32, Direct2D, DirectWrite, GDI, GDI+, Common Controls, DWM, Shell, IMM, Windows Imaging Component, WinHTTP (web images in the preview), the pseudo console (terminal mode), named pipes (AI tools), and Windows Runtime PDF and imaging (Windows.Data.Pdf, Windows.Graphics.Imaging, through the Windows SDK's C++/WinRT headers) | Parts of the operating system, linked through the Windows SDK | Part of Windows (Windows license) |
| [Git for Windows](https://gitforwindows.org/) (`git.exe`) | Run as a separate program for everything git; you install it yourself | GNU GPL v2. Verso doesn't link to or ship git. |

### Used only to build, not included

| Tool | What for | License |
| --- | --- | --- |
| Microsoft Visual C++ Build Tools (compiler, linker, resource compiler) | Compiling | Visual Studio license terms |
| [CMake](https://cmake.org/) | Build setup | BSD 3-Clause |
| [Ninja](https://ninja-build.org/) | Running the build | Apache License 2.0 |
| [Python](https://www.python.org/) | Runs `tools/make_icon.py` and `tools/make_font.py` | Python Software Foundation License |
| [Pillow](https://python-pillow.org/) | Makes the icons from the logo (`tools/make_icon.py`) | MIT-CMU (HPND) |
| [fontTools](https://github.com/fonttools/fonttools) 4.61 | Builds the slashed-zero font (`tools/make_font.py`) | MIT |
