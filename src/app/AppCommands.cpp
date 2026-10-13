#include <windows.h>
#include <shellapi.h>

#include <algorithm>

#include "app/App.h"
#include "app/Clipboard.h"
#include "verso_version.h"

namespace verso {

namespace {

constexpr size_t npos = static_cast<size_t>(-1);

struct EncodingChoice {
  Encoding enc;
  const char* id;
};
const EncodingChoice kEncodings[] = {{Encoding::Utf8, "utf-8"},
                                     {Encoding::Utf8Bom, "utf-8-bom"},
                                     {Encoding::Utf16LE, "utf-16le"},
                                     {Encoding::Utf16BE, "utf-16be"},
                                     {Encoding::Windows1252, "windows-1252"}};

}  // namespace

void App::addCommand(std::string id, std::wstring caption, std::function<void()> fn, bool recordable) {
  commandIndex_[id] = commands_.size();
  commands_.push_back({std::move(id), std::move(caption), std::move(fn), recordable});
}

void App::runCommand(const std::string& id) {
  auto it = commandIndex_.find(id);
  if (it == commandIndex_.end()) {
    setStatus(L"Unknown command: " + utf8ToWide(id));
    return;
  }
  const CommandDef& c = commands_[it->second];
  if (recording_ && c.recordable && !playingMacro_) recordingMacro_.push_back({id, L""});
  auto fn = c.run;
  fn();
}

void App::edit(const std::function<void(Editor&)>& fn, bool reveal) {
  Document* d = doc();
  if (!d) return;
  if (d->pdf || d->binary) {
    setStatus(d->pdf ? L"PDF files open read-only." : L"Binary files open read-only.", 2500);
    return;
  }
  if (markdownReadOnly()) {
    setStatus(L"Read view: choose Write or Split in the status bar to edit.", 2500);
    return;
  }
  fn(d->ed);
  afterEdit(*d, reveal);
}

void App::afterEdit(Document& d, bool reveal) {
  size_t first = d.ed.takeFirstChangedLine();
  if (first != npos) d.syntax.invalidateFrom(first);
  if (reveal && &d == doc()) revealCaret();
  sessionDirty_ = true;
  computeLayout();
  updateTitle();
  resetBlink();
  invalidate();
}

void App::registerCommands() {
  commands_.clear();
  commandIndex_.clear();
  using Fn = std::function<void(Editor&)>;
  auto E = [this](Fn fn, bool reveal = true) { return [this, fn, reveal] { edit(fn, reveal); }; };
  auto mv = [this](MoveBy by, bool fwd, bool extend) {
    return [this, by, fwd, extend] {
      int page = pageLines();
      if (wrapActive() && (by == MoveBy::Lines || by == MoveBy::Pages)) {
        moveVisual(static_cast<long long>(by == MoveBy::Lines ? 1 : page) * (fwd ? 1 : -1), extend);
        return;
      }
      edit([=](Editor& e) { e.move(by, fwd, extend, page); });
    };
  };

  // File ---------------------------------------------------------------------
  addCommand("new_file", L"File: New File", [this] { newDocument(); });
  addCommand("open_file", L"File: Open File...", [this] {
    for (const auto& p : promptOpenFiles()) openFile(p);
  });
  addCommand("open_folder", L"File: Open Folder...", [this] {
    std::wstring f = promptFolder();
    if (!f.empty()) openFolder(f, true);
  });
  addCommand("add_folder", L"Project: Add Folder...", [this] {
    std::wstring f = promptFolder();
    if (!f.empty()) openFolder(f, false);
  });
  addCommand("reveal_in_side_bar", L"File: Reveal in Side Bar", [this] {
    Document* d = doc();
    if (!d || d->path.empty()) return;
    tree_.reveal(d->path);
    TreeNode* node = tree_.find(d->path);
    if (!node) {
      setStatus(L"The file is not in any open folder.", 2500);
      return;
    }
    if (!sidebarVisible_) runCommand("toggle_sidebar");
    auto rows = sidebarRows();
    for (size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].node != node) continue;
      float y = static_cast<float>(i) * kSidebarRow;
      if (y < sidebarScroll_ || y + kSidebarRow > sidebarScroll_ + lay_.sidebar.h() - 8)
        sidebarScroll_ = std::max(0.0f, y - lay_.sidebar.h() / 2);
    }
    invalidate();
  });
  addCommand("close_folders", L"Project: Close All Folders", [this] {
    tree_.setFolders({});
    index_.stop();
    indexSnapshot_.clear();
    indexStarted_ = false;
    if (terminalMode_) selectTerminal();
    computeLayout();
    sessionDirty_ = true;
    invalidate();
  });
  addCommand("save", L"File: Save", [this] {
    if (Document* d = doc()) saveDocument(*d, false);
  });
  addCommand("save_as", L"File: Save As...", [this] {
    if (Document* d = doc()) saveDocument(*d, true);
  });
  addCommand("save_all", L"File: Save All", [this] { saveAll(); });
  addCommand("revert_file", L"File: Revert", [this] {
    Document* d = doc();
    if (d && !d->path.empty() && GetFileAttributesW(d->path.c_str()) != INVALID_FILE_ATTRIBUTES) reloadFromDisk(*d);
  });
  addCommand("close_file", L"File: Close", [this] {
    if (!docs_.empty()) closeDocument(active_);
  });
  addCommand("close_all", L"File: Close All", [this] { closeAll(); });
  addCommand("close_others", L"File: Close Other Tabs", [this] {
    if (docs_.empty()) return;
    uint64_t keep = docs_[active_]->id;
    for (size_t i = docs_.size(); i-- > 0;)
      if (docs_[i]->id != keep && !closeDocument(i)) return;
  });
  addCommand("reopen_last_file", L"File: Reopen Closed File", [this] {
    while (!closedFiles_.empty()) {
      std::wstring p = closedFiles_.back();
      closedFiles_.pop_back();
      if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
        openFile(p);
        return;
      }
    }
  });
  addCommand("exit", L"File: Exit", [this] { PostMessageW(hwnd_, WM_CLOSE, 0, 0); });
  addCommand("copy_file_path", L"File: Copy Path", [this] {
    if (Document* d = doc(); d && !d->path.empty()) {
      setClipboardText(hwnd_, d->path);
      setStatus(L"Copied " + d->path);
    }
  });
  addCommand("open_containing_folder", L"File: Open Containing Folder", [this] {
    if (Document* d = doc(); d && !d->path.empty()) {
      std::wstring arg = L"/select,\"" + d->path + L"\"";
      ShellExecuteW(hwnd_, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }
  });
  addCommand("rename_file", L"File: Rename...", [this] {
    Document* d = doc();
    if (!d || d->path.empty()) return;
    uint64_t id = d->id;
    prompt(L"New Name:", fileNameOf(d->path), [this, id](const std::wstring& name) {
      for (auto& dp : docs_) {
        if (dp->id != id || trim(name).empty()) continue;
        std::wstring target = parentOf(dp->path) + L"\\" + std::wstring(trim(name));
        if (!MoveFileExW(dp->path.c_str(), target.c_str(), 0)) {
          setStatus(L"Rename failed: " + lastErrorMessage(GetLastError()));
          return;
        }
        dp->path = target;
        dp->diskWriteTime = fileWriteTime(target);
        if (!dp->syntaxFromUser)
          setLanguage(*dp, detectLanguage(target, std::wstring(dp->ed.buffer().lineText(0).substr(0, 200))), false);
        if (!tree_.empty()) tree_.refresh();
        updateTitle();
        invalidate();
      }
    });
  });

  // Edit ---------------------------------------------------------------------
  addCommand("undo", L"Edit: Undo", E([](Editor& e) { e.undo(); }), true);
  addCommand("redo", L"Edit: Redo", E([](Editor& e) { e.redo(); }), true);
  addCommand("cut", L"Edit: Cut", [this] { doCopy(true); }, true);
  addCommand("copy", L"Edit: Copy", [this] { doCopy(false); }, true);
  addCommand("paste", L"Edit: Paste", [this] { doPaste(); }, true);
  addCommand("select_all", L"Selection: Select All", E([](Editor& e) { e.selectAll(); }, false), true);
  addCommand("left_delete", L"", E([](Editor& e) { e.backspace(); }), true);
  addCommand("right_delete", L"", E([](Editor& e) { e.deleteForward(); }), true);
  addCommand("insert_newline", L"", [this] {
    Document* d = doc();
    if (d && d->findResults && d->ed.selections().size() == 1 &&
        openResultAt(*d, d->ed.buffer().lineOf(d->ed.selections().primary().caret)))
      return;
    if (mdMode_ && !gitMode_ && !editBlocked() && mdContinueList()) return;
    edit([](Editor& e) { e.newline(); });
  }, true);
  addCommand("insert_line_after", L"Edit: Insert Line After", E([](Editor& e) { e.insertLineAfter(); }), true);
  addCommand("insert_line_before", L"Edit: Insert Line Before", E([](Editor& e) { e.insertLineBefore(); }), true);
  addCommand("tab", L"", E([](Editor& e) { e.tabKey(); }), true);
  addCommand("indent", L"Indentation: Indent", E([](Editor& e) { e.indent(); }), true);
  addCommand("unindent", L"Indentation: Unindent", E([](Editor& e) { e.unindent(); }), true);
  addCommand("delete_word_back", L"Edit: Delete Word Backward", E([](Editor& e) { e.deleteWord(false); }), true);
  addCommand("delete_word_forward", L"Edit: Delete Word Forward", E([](Editor& e) { e.deleteWord(true); }), true);
  addCommand("delete_to_bol", L"Edit: Delete to Beginning of Line",
             E([](Editor& e) { e.deleteToLineBoundary(false); }), true);
  addCommand("delete_to_eol", L"Edit: Delete to End of Line", E([](Editor& e) { e.deleteToLineBoundary(true); }),
             true);
  addCommand("toggle_comment", L"Edit: Toggle Comment", E([](Editor& e) { e.toggleComment(false); }), true);
  addCommand("toggle_block_comment", L"Edit: Toggle Block Comment", E([](Editor& e) { e.toggleComment(true); }),
             true);
  addCommand("swap_line_up", L"Edit: Swap Line Up", E([](Editor& e) { e.swapLines(true); }), true);
  addCommand("swap_line_down", L"Edit: Swap Line Down", E([](Editor& e) { e.swapLines(false); }), true);
  addCommand("duplicate_line", L"Edit: Duplicate Line", E([](Editor& e) { e.duplicate(); }), true);
  addCommand("delete_line", L"Edit: Delete Line", E([](Editor& e) { e.deleteLine(); }), true);
  addCommand("join_lines", L"Edit: Join Lines", E([](Editor& e) { e.joinLines(); }), true);
  addCommand("transpose", L"Edit: Transpose", E([](Editor& e) { e.transpose(); }), true);
  addCommand("upper_case", L"Convert Case: Upper Case", E([](Editor& e) { e.transformCase(CaseMode::Upper); }), true);
  addCommand("lower_case", L"Convert Case: Lower Case", E([](Editor& e) { e.transformCase(CaseMode::Lower); }), true);
  addCommand("title_case", L"Convert Case: Title Case", E([](Editor& e) { e.transformCase(CaseMode::Title); }), true);
  addCommand("swap_case", L"Convert Case: Swap Case", E([](Editor& e) { e.transformCase(CaseMode::Swap); }), true);
  addCommand("sort_lines", L"Sort Lines", E([](Editor& e) { e.permuteLines(LineOrder::Sort); }), true);
  addCommand("sort_lines_case_sensitive", L"Sort Lines (Case Sensitive)",
             E([](Editor& e) { e.permuteLines(LineOrder::SortCaseSensitive); }), true);
  addCommand("reverse_lines", L"Permute Lines: Reverse", E([](Editor& e) { e.permuteLines(LineOrder::Reverse); }),
             true);
  addCommand("unique_lines", L"Permute Lines: Unique", E([](Editor& e) { e.permuteLines(LineOrder::Unique); }), true);
  addCommand("trim_trailing_whitespace", L"Edit: Trim Trailing White Space",
             E([](Editor& e) { e.trimTrailingWhitespace(); }, false), true);
  addCommand("toggle_record_macro", L"Tools: Record Macro (Toggle)", [this] { toggleMacroRecording(); });
  addCommand("run_macro", L"Tools: Playback Macro", [this] { playMacro(); });

  // Movement -----------------------------------------------------------------
  addCommand("move_left", L"", mv(MoveBy::Chars, false, false), true);
  addCommand("move_right", L"", mv(MoveBy::Chars, true, false), true);
  addCommand("move_up", L"", mv(MoveBy::Lines, false, false), true);
  addCommand("move_down", L"", mv(MoveBy::Lines, true, false), true);
  addCommand("select_left", L"", mv(MoveBy::Chars, false, true), true);
  addCommand("select_right", L"", mv(MoveBy::Chars, true, true), true);
  addCommand("select_up", L"", mv(MoveBy::Lines, false, true), true);
  addCommand("select_down", L"", mv(MoveBy::Lines, true, true), true);
  addCommand("word_left", L"", mv(MoveBy::Words, false, false), true);
  addCommand("word_right", L"", mv(MoveBy::WordEnds, true, false), true);
  addCommand("select_word_left", L"", mv(MoveBy::Words, false, true), true);
  addCommand("select_word_right", L"", mv(MoveBy::WordEnds, true, true), true);
  addCommand("subword_left", L"", mv(MoveBy::Subwords, false, false), true);
  addCommand("subword_right", L"", mv(MoveBy::SubwordEnds, true, false), true);
  addCommand("select_subword_left", L"", mv(MoveBy::Subwords, false, true), true);
  addCommand("select_subword_right", L"", mv(MoveBy::SubwordEnds, true, true), true);
  addCommand("page_up", L"", mv(MoveBy::Pages, false, false), true);
  addCommand("page_down", L"", mv(MoveBy::Pages, true, false), true);
  addCommand("select_page_up", L"", mv(MoveBy::Pages, false, true), true);
  addCommand("select_page_down", L"", mv(MoveBy::Pages, true, true), true);
  addCommand("line_begin", L"", mv(MoveBy::LineBegin, false, false), true);
  addCommand("line_end", L"", mv(MoveBy::LineEnd, true, false), true);
  addCommand("select_line_begin", L"", mv(MoveBy::LineBegin, false, true), true);
  addCommand("select_line_end", L"", mv(MoveBy::LineEnd, true, true), true);
  addCommand("doc_begin", L"", mv(MoveBy::DocBegin, false, false), true);
  addCommand("doc_end", L"", mv(MoveBy::DocEnd, true, false), true);
  addCommand("select_doc_begin", L"", mv(MoveBy::DocBegin, false, true), true);
  addCommand("select_doc_end", L"", mv(MoveBy::DocEnd, true, true), true);
  addCommand("move_to_bracket", L"Goto: Jump to Matching Bracket", mv(MoveBy::Brackets, true, false), true);
  addCommand("scroll_line_up", L"", [this] { scrollBy(-gfx_.lineHeight(), 0); });
  addCommand("scroll_line_down", L"", [this] { scrollBy(gfx_.lineHeight(), 0); });
  addCommand("show_at_center", L"View: Scroll to Selection", [this] { revealCaret(true); });

  // Selection ----------------------------------------------------------------
  addCommand("split_selection_into_lines", L"Selection: Split into Lines",
             E([](Editor& e) { e.splitSelectionIntoLines(); }), true);
  addCommand("add_cursor_above", L"Selection: Add Previous Line", E([](Editor& e) { e.addCursorLine(false); }), true);
  addCommand("add_cursor_below", L"Selection: Add Next Line", E([](Editor& e) { e.addCursorLine(true); }), true);
  addCommand("single_selection", L"Selection: Single Selection", E([](Editor& e) { e.singleSelection(); }), true);
  addCommand("cancel", L"", [this] {
    if (overlay_ != Overlay::None) {
      closeOverlay(false);
    } else if (findMode_ != FindMode::Hidden) {
      hideFind();
    } else if (Document* d = doc(); d && d->ed.selections().size() > 1) {
      edit([](Editor& e) { e.singleSelection(); });
    }
  });
  addCommand("expand_selection_line", L"Selection: Expand to Line", E([](Editor& e) { e.expandToLine(); }), true);
  addCommand("expand_selection_word", L"Selection: Expand to Word", E([](Editor& e) { e.expandToWord(); }), true);
  addCommand("expand_selection_brackets", L"Selection: Expand to Brackets",
             E([](Editor& e) { e.expandToBrackets(); }), true);
  addCommand("find_under_expand", L"Selection: Quick Add Next", E([](Editor& e) { e.findUnderExpand(); }), true);
  addCommand("find_under_expand_skip", L"Selection: Quick Skip Next",
             E([](Editor& e) { e.findUnderExpandSkip(); }), true);
  addCommand("find_all_under", L"Selection: Quick Find All", E([](Editor& e) { e.findAllUnder(); }), true);

  // Find ---------------------------------------------------------------------
  addCommand("show_find", L"Find: Find...", [this] { showFind(false); });
  addCommand("show_replace", L"Find: Replace...", [this] { showFind(true); });
  addCommand("incremental_find", L"Find: Incremental Find", [this] { showFind(false, true); });
  addCommand("find_next", L"Find: Find Next", [this] { findNext(true); });
  addCommand("find_prev", L"Find: Find Previous", [this] { findNext(false); });
  addCommand("find_under", L"Find: Quick Find", [this] { findUnder(true); });
  addCommand("find_under_prev", L"Find: Quick Find Previous", [this] { findUnder(false); });
  addCommand("use_selection_for_find", L"Find: Use Selection for Find", [this] { useSelectionForFind(); });
  addCommand("replace_next", L"Find: Replace Next", [this] { replaceNext(); });
  addCommand("replace_all", L"Find: Replace All", [this] { replaceAll(); });
  addCommand("find_all", L"Find: Find All", [this] { findAll(); });
  addCommand("find_in_files", L"Find: Find in Files...", [this] {
    std::wstring initial = findEdit_.text();
    if (Document* d = doc()) {
      const Selection& p = d->ed.selections().primary();
      if (!p.empty() && d->ed.buffer().lineOf(p.begin()) == d->ed.buffer().lineOf(p.end())) initial = d->ed.textOf(p);
    }
    prompt(L"Find in Files (uses the find panel's regex / case / whole-word toggles):", initial,
           [this](const std::wstring& q) {
             findEdit_.setText(q, true);
             findEdit_.remember();
             findInFiles(q);
           });
  });

  // Goto ---------------------------------------------------------------------
  addCommand("goto_anything", L"Goto: Goto Anything...", [this] { showOverlay(Overlay::Files); });
  addCommand("command_palette", L"", [this] { showOverlay(Overlay::Commands); });
  addCommand("goto_symbol", L"Goto: Goto Symbol...", [this] { showOverlay(Overlay::Symbols); });
  addCommand("goto_line", L"Goto: Goto Line...", [this] { showOverlay(Overlay::Line); });
  addCommand("next_tab", L"View: Next Tab", [this] {
    if (!docs_.empty()) activate((active_ + 1) % docs_.size());
  });
  addCommand("prev_tab", L"View: Previous Tab", [this] {
    if (!docs_.empty()) activate((active_ + docs_.size() - 1) % docs_.size());
  });
  for (int i = 1; i <= 8; ++i)
    addCommand("select_tab:" + std::to_string(i), L"", [this, i] { activate(static_cast<size_t>(i - 1)); });
  addCommand("select_last_tab", L"", [this] {
    if (!docs_.empty()) activate(docs_.size() - 1);
  });
  addCommand("switch_file", L"File: Switch Header/Implementation", [this] {
    Document* d = doc();
    if (!d || d->path.empty()) return;
    static const wchar_t* exts[] = {L"h", L"hpp", L"hh", L"hxx", L"c", L"cpp", L"cc", L"cxx", L"m", L"mm"};
    std::wstring p = d->path;
    size_t dot = p.find_last_of(L'.');
    if (dot == std::wstring::npos) return;
    std::wstring stem = p.substr(0, dot), ext = toLower(p.substr(dot + 1));
    bool header = ext[0] == L'h';
    for (const wchar_t* e : exts) {
      if ((e[0] == L'h') == header) continue;
      std::wstring cand = stem + L"." + e;
      if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
        openFile(cand);
        return;
      }
    }
    setStatus(L"No counterpart file found.");
  });

  // View ---------------------------------------------------------------------
  addCommand("toggle_sidebar", L"View: Toggle Side Bar", [this] {
    sidebarVisible_ = !sidebarVisible_;
    computeLayout();
    sessionDirty_ = true;
    invalidate();
  });
  addCommand("toggle_minimap", L"View: Toggle Minimap", [this] {
    minimapVisible_ = !minimapVisible_;
    computeLayout();
    sessionDirty_ = true;
    invalidate();
  });
  addCommand("toggle_fullscreen", L"View: Toggle Full Screen", [this] { toggleFullscreen(); });
  addCommand("toggle_distraction_free", L"View: Toggle Distraction Free Mode", [this] { toggleDistractionFree(); });
  addCommand("increase_font", L"View: Font Size Larger", [this] { changeFontSize(1); });
  addCommand("decrease_font", L"View: Font Size Smaller", [this] { changeFontSize(-1); });
  addCommand("reset_font", L"View: Font Size Reset", [this] {
    if (Document* pd = doc(); pd && pd->pdf) {
      zoomPdf(0);
      return;
    }
    fontSizeOverride_ = 0;
    applyFont();
    sessionDirty_ = true;
  });
  addCommand("theme_dark", L"UI: Select Dark Theme", [this] {
    settings_.theme = L"dark";
    applyTheme();
  });
  addCommand("theme_light", L"UI: Select Light Theme", [this] {
    settings_.theme = L"light";
    applyTheme();
  });

  for (const auto& lang : allLanguages()) {
    const Language* l = &lang;
    addCommand("set_syntax:" + wideToUtf8(lang.name), L"Set Syntax: " + lang.name, [this, l] {
      if (Document* d = doc()) {
        setLanguage(*d, *l, true);
        sessionDirty_ = true;
      }
    });
  }

  // Indentation, line endings, encodings --------------------------------------
  addCommand("indent_using_spaces", L"Indentation: Indent Using Spaces (Toggle)", [this] {
    if (Document* d = doc()) d->ed.config.translateTabs = !d->ed.config.translateTabs;
    invalidate();
  });
  for (int n = 1; n <= 8; ++n) {
    addCommand("tab_width:" + std::to_string(n), L"Indentation: Tab Width " + std::to_wstring(n), [this, n] {
      if (Document* d = doc()) d->ed.config.tabSize = n;
      computeLayout();
      invalidate();
    });
  }
  addCommand("detect_indentation", L"Indentation: Guess Settings From Buffer", [this] {
    if (Document* d = doc()) detectIndentation(*d);
    invalidate();
  });
  addCommand("convert_to_spaces", L"Indentation: Convert to Spaces",
             E([](Editor& e) { e.convertIndentation(true); }, false));
  addCommand("convert_to_tabs", L"Indentation: Convert to Tabs", E([](Editor& e) { e.convertIndentation(false); }, false));
  const std::pair<const char*, LineEnding> eols[] = {
      {"windows", LineEnding::CRLF}, {"unix", LineEnding::LF}, {"cr", LineEnding::CR}};
  for (const auto& [id, eol] : eols) {
    LineEnding e = eol;
    addCommand(std::string("line_ending:") + id, std::wstring(L"Line Endings: ") + lineEndingName(e), [this, e] {
      Document* d = doc();
      if (!d || d->eol == e || d->pdf || d->binary) return;
      d->eol = e;
      d->ed.markUnsaved();
      updateTitle();
      invalidate();
    });
  }
  for (const auto& choice : kEncodings) {
    Encoding enc = choice.enc;
    addCommand(std::string("save_with_encoding:") + choice.id,
               std::wstring(L"File: Save with Encoding: ") + encodingName(enc), [this, enc] {
                 Document* d = doc();
                 if (!d) return;
                 Encoding old = d->encoding;
                 d->encoding = enc;
                 if (!saveDocument(*d, false)) d->encoding = old;
               });
  }

  // Git ------------------------------------------------------------------------
  addCommand("toggle_git_mode", L"View: Toggle Git Mode", [this] { setGitMode(!gitMode_); });
  addCommand("toggle_terminal", L"View: Toggle Terminal Mode", [this] { setTerminalMode(!terminalMode_); });
  addCommand("terminal_new_shell", L"Terminal: Start a New Shell", [this] {
    term_->pty.stop();
    term_->exited = true;
    term_->started = false;
    setTerminalMode(true);
  });
  addCommand("terminal_clear", L"Terminal: Clear Scrollback", [this] {
    if (!term_->started || term_->exited) return;
    term_->screen.write("\x1b[3J");
    term_->scrolledBack = 0;
    invalidate();
  });
  addCommand("connect_ai_tool", L"Terminal: Connect an AI Tool to Verso (MCP)", [this] { connectAiTool(); });
  addCommand("mode_text", L"View: Text Mode", [this] { setEditorMode(false); });
  addCommand("mode_markdown", L"View: Markdown Mode", [this] { setEditorMode(true); });
  addCommand("toggle_markdown_mode", L"View: Toggle Markdown Mode",
             [this] { setEditorMode(!(mdMode_ && !gitMode_)); });
  addCommand("md_view_write", L"Markdown: Write View", [this] { setMdView(MdView::Write); });
  addCommand("md_view_split", L"Markdown: Split View (Editor + Preview)", [this] { setMdView(MdView::Split); });
  addCommand("md_view_read", L"Markdown: Read View", [this] { setMdView(MdView::Read); });
  addCommand("md_cycle_view", L"Markdown: Cycle Write / Split / Read", [this] {
    setMdView(mdMode_ && !gitMode_ ? static_cast<MdView>((static_cast<int>(mdView_) + 1) % 3) : mdView_);
  });
  addCommand("md_bold", L"Markdown: Bold", [this] { mdToggleWrap(L"**"); });
  addCommand("md_italic", L"Markdown: Italic", [this] { mdToggleWrap(L"*"); });
  addCommand("md_code", L"Markdown: Inline Code", [this] { mdToggleWrap(L"`"); });
  addCommand("md_strike", L"Markdown: Strikethrough", [this] { mdToggleWrap(L"~~"); });
  addCommand("md_link", L"Markdown: Insert Link", [this] { mdInsertLink(); });
  addCommand("md_back", L"Markdown: Back (follow link history)", [this] { navigateMarkdown(true); });
  addCommand("md_forward", L"Markdown: Forward (follow link history)", [this] { navigateMarkdown(false); });
  addCommand("toggle_word_wrap", L"View: Toggle Word Wrap", [this] {
    settings_.wordWrap = !settings_.wordWrap;
    sessionDirty_ = true;
    computeLayout();
    if (doc()) revealCaret();
    invalidate();
  });
  addCommand("git_open_repository", L"Git: Show Repository (Git Mode)", [this] { gitOpenRepository(); });
  addCommand("git_file_history", L"Git: File History", [this] {
    if (Document* d = doc()) gitFileTool(false, d->path);
  });
  addCommand("git_blame", L"Git: Blame File", [this] {
    if (Document* d = doc()) gitFileTool(true, d->path);
  });
  addCommand("git_refresh", L"Git: Refresh Status", [this] {
    for (auto& d : docs_) requestGitBaseline(*d);
    gitRefresh();
  });
  addCommand("next_modification", L"Git: Next Modification", [this] { gotoModification(true); });
  addCommand("prev_modification", L"Git: Previous Modification", [this] { gotoModification(false); });
  addCommand("revert_hunk", L"Git: Revert Hunk", [this] { revertHunk(); });

  // Preferences & help -------------------------------------------------------
  addCommand("open_settings", L"Preferences: Settings",
             [this] { openConfigFile(dataDir() + L"\\Preferences.json", Settings::defaultFileContents()); });
  addCommand("open_keymap", L"Preferences: Key Bindings",
             [this] { openConfigFile(dataDir() + L"\\Keymap.json", Keymap::defaultFileContents()); });
  addCommand("key_binding_reference", L"Help: Key Binding Reference",
             [this] { openScratch(keyBindingReference(), languageByName(L"Markdown")); });
  addCommand("third_party_licenses", L"Help: Third-Party Licenses", [this] {
    std::wstring text =
        L"Verso third-party notices\n"
        L"============================\n\n"
        L"Font: JetBrains Mono 2.304, Copyright 2020 The JetBrains Mono Project Authors\n"
        L"(https://github.com/JetBrains/JetBrainsMono). Bundled as \"JetBrains Mono Slashed\":\n"
        L"modified so the digit zero uses the font's slashed-zero glyph by default, and renamed.\n"
        L"Licensed under the SIL Open Font License, Version 1.1, reproduced below.\n\n"
        L"The C++ standard library is the Microsoft STL (Apache License 2.0 with LLVM Exception),\n"
        L"linked statically with the Microsoft C runtime under the Visual Studio license terms.\n"
        L"Git features run the separately installed git.exe (Git for Windows, GPL v2); it is not\n"
        L"included with Verso.\n\n";
    HMODULE module = GetModuleHandleW(nullptr);
    if (HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(205), MAKEINTRESOURCEW(10))) {
      if (HGLOBAL mem = LoadResource(module, res)) {
        const char* data = static_cast<const char*>(LockResource(mem));
        if (data) text += normalizeNewlines(utf8ToWide(std::string_view(data, SizeofResource(module, res))));
      }
    }
    openScratch(text, nullptr);
    if (Document* d = doc()) {
      d->ed.markSaved();
      updateTitle();
    }
  });
  addCommand("about", L"Help: About Verso", [this] {
    MessageBoxW(hwnd_,
                L"Verso " VERSO_VERSION_WSTRING L"\n\nA native text, Markdown and git editor with a built-in terminal.\n"
                L"Written in C++ with Direct2D and DirectWrite.\n\n"
                L"Copyright 2026 David Kendig\n"
                L"Licensed under the Apache License, Version 2.0.",
                L"About Verso", MB_ICONINFORMATION);
  });
}

// ---------------------------------------------------------------------------
// Menus

void App::buildMenu() {
  menuCommands_.clear();
  nextMenuId_ = 1000;
  HMENU bar = CreateMenu();
  auto item = [this](HMENU m, const wchar_t* label, const std::string& cmd) {
    UINT id = nextMenuId_++;
    menuCommands_[id] = cmd;
    std::wstring text = label;
    std::wstring sc = keymap_.shortcutFor(cmd);
    if (!sc.empty()) text += L"\t" + sc;
    AppendMenuW(m, MF_STRING, id, text.c_str());
  };
  auto sep = [](HMENU m) { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); };
  auto sub = [](HMENU parent, const wchar_t* label) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(m), label);
    return m;
  };

  HMENU file = sub(bar, L"&File");
  item(file, L"&New File", "new_file");
  item(file, L"&Open File...", "open_file");
  item(file, L"Open &Folder...", "open_folder");
  recentMenu_ = sub(file, L"Open &Recent");
  item(file, L"Reopen &Closed File", "reopen_last_file");
  sep(file);
  item(file, L"&Save", "save");
  item(file, L"Save &As...", "save_as");
  item(file, L"Save A&ll", "save_all");
  HMENU encMenu = sub(file, L"Save with &Encoding");
  for (const auto& c : kEncodings) item(encMenu, encodingName(c.enc), std::string("save_with_encoding:") + c.id);
  sep(file);
  item(file, L"Re&vert File", "revert_file");
  item(file, L"Rena&me File...", "rename_file");
  sep(file);
  item(file, L"Close Fil&e", "close_file");
  item(file, L"Close All F&iles", "close_all");
  sep(file);
  item(file, L"E&xit", "exit");

  HMENU editM = sub(bar, L"&Edit");
  item(editM, L"&Undo", "undo");
  item(editM, L"&Redo", "redo");
  sep(editM);
  item(editM, L"Cu&t", "cut");
  item(editM, L"&Copy", "copy");
  item(editM, L"&Paste", "paste");
  sep(editM);
  HMENU line = sub(editM, L"&Line");
  item(line, L"&Indent", "indent");
  item(line, L"&Unindent", "unindent");
  item(line, L"Swap Line U&p", "swap_line_up");
  item(line, L"Swap Line &Down", "swap_line_down");
  item(line, L"Duplicate &Line", "duplicate_line");
  item(line, L"D&elete Line", "delete_line");
  item(line, L"&Join Lines", "join_lines");
  HMENU comment = sub(editM, L"Co&mment");
  item(comment, L"Toggle &Comment", "toggle_comment");
  item(comment, L"Toggle &Block Comment", "toggle_block_comment");
  HMENU text = sub(editM, L"Te&xt");
  item(text, L"Insert Line &Before", "insert_line_before");
  item(text, L"Insert Line &After", "insert_line_after");
  item(text, L"Delete Word &Forward", "delete_word_forward");
  item(text, L"Delete Word Bac&kward", "delete_word_back");
  item(text, L"Delete to &End of Line", "delete_to_eol");
  item(text, L"Delete to Beginnin&g of Line", "delete_to_bol");
  item(text, L"&Transpose", "transpose");
  HMENU caseM = sub(editM, L"Convert C&ase");
  item(caseM, L"&Upper Case", "upper_case");
  item(caseM, L"&Lower Case", "lower_case");
  item(caseM, L"&Title Case", "title_case");
  item(caseM, L"&Swap Case", "swap_case");
  sep(editM);
  item(editM, L"&Sort Lines", "sort_lines");
  item(editM, L"Sort Lines (Case Sensiti&ve)", "sort_lines_case_sensitive");
  HMENU perm = sub(editM, L"Permute Li&nes");
  item(perm, L"&Reverse", "reverse_lines");
  item(perm, L"&Unique", "unique_lines");
  item(editM, L"Trim Trailing &White Space", "trim_trailing_whitespace");

  HMENU sel = sub(bar, L"&Selection");
  item(sel, L"&Split into Lines", "split_selection_into_lines");
  item(sel, L"Add &Previous Line", "add_cursor_above");
  item(sel, L"Add &Next Line", "add_cursor_below");
  item(sel, L"Single Se&lection", "single_selection");
  item(sel, L"Select &All", "select_all");
  sep(sel);
  item(sel, L"Expand Selection to L&ine", "expand_selection_line");
  item(sel, L"Expand Selection to &Word", "expand_selection_word");
  item(sel, L"Expand Selection to &Brackets", "expand_selection_brackets");
  sep(sel);
  item(sel, L"&Quick Add Next", "find_under_expand");
  item(sel, L"Quick S&kip Next", "find_under_expand_skip");
  item(sel, L"Quick &Find All", "find_all_under");

  HMENU find = sub(bar, L"F&ind");
  item(find, L"&Find...", "show_find");
  item(find, L"Find &Next", "find_next");
  item(find, L"Find &Previous", "find_prev");
  item(find, L"&Incremental Find", "incremental_find");
  sep(find);
  item(find, L"Find in Fil&es...", "find_in_files");
  sep(find);
  item(find, L"&Replace...", "show_replace");
  item(find, L"Replace Ne&xt", "replace_next");
  item(find, L"Replace &All", "replace_all");
  sep(find);
  item(find, L"&Quick Find", "find_under");
  item(find, L"Quick Find Pre&vious", "find_under_prev");
  item(find, L"Quick Find A&ll", "find_all_under");
  item(find, L"&Use Selection for Find", "use_selection_for_find");

  HMENU view = sub(bar, L"&View");
  item(view, L"Side &Bar", "toggle_sidebar");
  item(view, L"&Minimap", "toggle_minimap");
  sep(view);
  item(view, L"&Full Screen", "toggle_fullscreen");
  item(view, L"&Distraction Free", "toggle_distraction_free");
  sep(view);
  item(view, L"Te&xt Mode", "mode_text");
  item(view, L"Mar&kdown Mode", "mode_markdown");
  item(view, L"&Git Mode", "toggle_git_mode");
  item(view, L"Ter&minal Mode", "toggle_terminal");
  HMENU mdv = sub(view, L"Markdown Vie&w");
  item(mdv, L"&Write", "md_view_write");
  item(mdv, L"&Split", "md_view_split");
  item(mdv, L"&Read", "md_view_read");
  item(view, L"Word W&rap", "toggle_word_wrap");
  sep(view);
  HMENU syntax = sub(view, L"&Syntax");
  for (const auto& lang : allLanguages()) item(syntax, lang.name.c_str(), "set_syntax:" + wideToUtf8(lang.name));
  HMENU indent = sub(view, L"&Indentation");
  item(indent, L"Indent Using &Spaces", "indent_using_spaces");
  sep(indent);
  for (int n = 1; n <= 8; ++n) {
    std::wstring label = L"Tab Width: " + std::to_wstring(n);
    item(indent, label.c_str(), "tab_width:" + std::to_string(n));
  }
  sep(indent);
  item(indent, L"&Guess Settings From Buffer", "detect_indentation");
  item(indent, L"Convert Indentation to Spa&ces", "convert_to_spaces");
  item(indent, L"Convert Indentation to &Tabs", "convert_to_tabs");
  HMENU eol = sub(view, L"&Line Endings");
  item(eol, L"&Windows", "line_ending:windows");
  item(eol, L"&Unix", "line_ending:unix");
  item(eol, L"&Mac OS 9", "line_ending:cr");
  sep(view);
  HMENU font = sub(view, L"F&ont");
  item(font, L"&Larger", "increase_font");
  item(font, L"&Smaller", "decrease_font");
  item(font, L"&Reset", "reset_font");
  HMENU themeM = sub(view, L"&Theme");
  item(themeM, L"&Dark", "theme_dark");
  item(themeM, L"&Light", "theme_light");

  HMENU go = sub(bar, L"&Goto");
  item(go, L"Goto &Anything...", "goto_anything");
  item(go, L"Goto &Symbol...", "goto_symbol");
  item(go, L"Goto &Line...", "goto_line");
  sep(go);
  item(go, L"&Next Tab", "next_tab");
  item(go, L"&Previous Tab", "prev_tab");
  item(go, L"Switch &File (Header/Source)", "switch_file");
  sep(go);
  item(go, L"Jump to Matching &Bracket", "move_to_bracket");
  item(go, L"S&croll to Selection", "show_at_center");

  HMENU tools = sub(bar, L"&Tools");
  item(tools, L"&Command Palette...", "command_palette");
  sep(tools);
  item(tools, L"&Record Macro", "toggle_record_macro");
  item(tools, L"&Playback Macro", "run_macro");

  HMENU proj = sub(bar, L"&Project");
  item(proj, L"&Open Folder...", "open_folder");
  item(proj, L"&Add Folder to Project...", "add_folder");
  item(proj, L"&Close All Folders", "close_folders");

  HMENU prefs = sub(bar, L"P&references");
  item(prefs, L"&Settings", "open_settings");
  item(prefs, L"&Key Bindings", "open_keymap");

  HMENU help = sub(bar, L"&Help");
  item(help, L"&Key Binding Reference", "key_binding_reference");
  item(help, L"&Third-Party Licenses", "third_party_licenses");
  item(help, L"&About Verso", "about");

  HMENU old = menu_;
  menu_ = bar;
  if (!fullscreen_ && !gitMode_) SetMenu(hwnd_, menu_);
  if (old) DestroyMenu(old);
}

void App::onInitMenuPopup(HMENU menu) {
  if (menu == recentMenu_) {
    while (GetMenuItemCount(menu) > 0) DeleteMenu(menu, 0, MF_BYPOSITION);
    for (size_t i = 0; i < recentFiles_.size() && i < 20; ++i)
      AppendMenuW(menu, MF_STRING, 50000 + i, recentFiles_[i].c_str());
    if (!recentFiles_.empty() && !recentFolders_.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < recentFolders_.size() && i < 20; ++i)
      AppendMenuW(menu, MF_STRING, 51000 + i, recentFolders_[i].c_str());
    if (recentFiles_.empty() && recentFolders_.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"(empty)");
    return;
  }
  Document* d = doc();
  int count = GetMenuItemCount(menu);
  for (int i = 0; i < count; ++i) {
    UINT id = GetMenuItemID(menu, i);
    auto it = menuCommands_.find(id);
    if (it == menuCommands_.end()) continue;
    const std::string& c = it->second;
    bool checked = false;
    if (c == "toggle_sidebar") checked = sidebarVisible_;
    else if (c == "toggle_minimap") checked = minimapVisible_;
    else if (c == "toggle_fullscreen") checked = fullscreen_ && !distractionFree_;
    else if (c == "toggle_distraction_free") checked = distractionFree_;
    else if (c == "mode_text") checked = !gitMode_ && !mdMode_;
    else if (c == "mode_markdown") checked = !gitMode_ && mdMode_;
    else if (c == "toggle_git_mode") checked = gitMode_;
    else if (c == "toggle_terminal") checked = terminalMode_;
    else if (c == "md_view_write") checked = mdView_ == MdView::Write;
    else if (c == "md_view_split") checked = mdView_ == MdView::Split;
    else if (c == "md_view_read") checked = mdView_ == MdView::Read;
    else if (c == "toggle_word_wrap") checked = settings_.wordWrap;
    else if (c == "toggle_record_macro") checked = recording_;
    else if (c == "theme_dark") checked = theme_.dark;
    else if (c == "theme_light") checked = !theme_.dark;
    else if (d && c == "indent_using_spaces") checked = d->ed.config.translateTabs;
    else if (d && c.rfind("tab_width:", 0) == 0) checked = std::to_string(d->ed.config.tabSize) == c.substr(10);
    else if (d && c.rfind("set_syntax:", 0) == 0) checked = wideToUtf8(d->lang->name) == c.substr(11);
    else if (d && c.rfind("line_ending:", 0) == 0) checked = lineEndingId(d->eol) == c.substr(12);
    else if (d && c.rfind("save_with_encoding:", 0) == 0) checked = encodingId(d->encoding) == c.substr(19);
    CheckMenuItem(menu, id, MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
    if (c == "undo") EnableMenuItem(menu, id, MF_BYCOMMAND | (d && d->ed.canUndo() ? MF_ENABLED : MF_GRAYED));
    if (c == "redo") EnableMenuItem(menu, id, MF_BYCOMMAND | (d && d->ed.canRedo() ? MF_ENABLED : MF_GRAYED));
  }
}

UINT App::trackMenu(HMENU menu, POINT screen) {
  return static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, 0,
                                          hwnd_, nullptr));
}

void App::showContextMenu(POINT screen) {
  Document* d = doc();
  std::vector<std::string> cmds;
  HMENU m = CreatePopupMenu();
  auto add = [&](const wchar_t* label, const char* cmd) {
    cmds.push_back(cmd);
    std::wstring text = label;
    std::wstring sc = keymap_.shortcutFor(cmd);
    if (!sc.empty()) text += L"\t" + sc;
    AppendMenuW(m, MF_STRING, cmds.size(), text.c_str());
  };
  add(L"Cut", "cut");
  add(L"Copy", "copy");
  add(L"Paste", "paste");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  add(L"Select All", "select_all");
  add(L"Toggle Comment", "toggle_comment");
  if (d && !d->path.empty()) {
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    add(L"Copy File Path", "copy_file_path");
    add(L"Open Containing Folder", "open_containing_folder");
  }
  UINT r = trackMenu(m, screen);
  DestroyMenu(m);
  if (r >= 1 && r <= cmds.size()) runCommand(cmds[r - 1]);
}

void App::showStatusMenu(int kind, POINT screen) {
  Document* d = doc();
  if (!d) return;
  std::vector<std::string> cmds;
  HMENU m = CreatePopupMenu();
  auto add = [&](const std::wstring& label, const std::string& cmd, bool checked) {
    cmds.push_back(cmd);
    AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0), cmds.size(), label.c_str());
  };
  switch (kind) {
    case 0:
      add(L"Indent Using Spaces", "indent_using_spaces", d->ed.config.translateTabs);
      AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
      for (int n = 1; n <= 8; ++n)
        add(L"Tab Width: " + std::to_wstring(n), "tab_width:" + std::to_string(n), d->ed.config.tabSize == n);
      AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
      add(L"Guess Settings From Buffer", "detect_indentation", false);
      add(L"Convert Indentation to Spaces", "convert_to_spaces", false);
      add(L"Convert Indentation to Tabs", "convert_to_tabs", false);
      break;
    case 1:
      add(L"Windows", "line_ending:windows", d->eol == LineEnding::CRLF);
      add(L"Unix", "line_ending:unix", d->eol == LineEnding::LF);
      add(L"Mac OS 9", "line_ending:cr", d->eol == LineEnding::CR);
      break;
    case 2:
      for (const auto& c : kEncodings)
        add(std::wstring(L"Save with ") + encodingName(c.enc), std::string("save_with_encoding:") + c.id,
            d->encoding == c.enc);
      break;
    case 3:
      for (const auto& lang : allLanguages())
        add(lang.name, "set_syntax:" + wideToUtf8(lang.name), d->lang == &lang);
      break;
    default:
      break;
  }
  UINT r = trackMenu(m, screen);
  DestroyMenu(m);
  if (r >= 1 && r <= cmds.size()) runCommand(cmds[r - 1]);
}

// ---------------------------------------------------------------------------
// Clipboard, font size, macros

void App::doCopy(bool cut) {
  Document* d = doc();
  if (!d) return;
  ClipboardText c;
  if (cut) {
    edit([&](Editor& e) { c = e.cut(); });
  } else {
    c = d->ed.copy();
  }
  if (c.text.empty()) return;
  setClipboardText(hwnd_, c.text);
  lastCopied_ = c.text;
  lastCopiedWholeLines_ = c.wholeLines;
}

void App::doPaste() {
  std::wstring text = getClipboardText(hwnd_);
  if (text.empty()) return;
  bool whole = lastCopiedWholeLines_ && text == lastCopied_;
  edit([&](Editor& e) { e.paste(text, whole); });
}

void App::changeFontSize(float delta) {
  if (Document* pd = doc(); pd && pd->pdf) {  // zoom the page view instead
    zoomPdf(delta > 0 ? 1.1f : 1 / 1.1f);
    return;
  }
  float cur = fontSizeOverride_ > 0 ? fontSizeOverride_ : settings_.fontSize;
  fontSizeOverride_ = std::clamp(cur + delta, 6.0f, 48.0f);
  applyFont();
  revealCaret();
  sessionDirty_ = true;
  setStatus(L"Font size " + std::to_wstring(static_cast<int>(fontSizeOverride_ + 0.5f)), 1500);
}

void App::toggleMacroRecording() {
  if (!recording_) {
    recording_ = true;
    recordingMacro_.clear();
    setStatus(L"Recording macro... (Ctrl+Q to stop)", 600000);
  } else {
    recording_ = false;
    macro_ = recordingMacro_;
    setStatus(L"Macro recorded: " + std::to_wstring(macro_.size()) + L" steps");
  }
}

void App::playMacro() {
  if (macro_.empty() || recording_) return;
  playingMacro_ = true;
  for (const auto& step : macro_) {
    if (step.command.empty())
      typeText(step.text);
    else
      runCommand(step.command);
  }
  playingMacro_ = false;
}

std::wstring App::keyBindingReference() {
  std::wstring out = L"# Verso Key Bindings\n\n";
  out += L"Customize with Preferences > Key Bindings (Keymap.json in %APPDATA%\\Verso).\n\n";
  for (const auto& c : commands_) {
    std::wstring sc = keymap_.shortcutFor(c.id);
    if (sc.empty() && c.caption.empty()) continue;
    std::wstring name = c.caption.empty() ? utf8ToWide(c.id) : c.caption;
    out += L"- " + name;
    if (!sc.empty()) out += L"  `" + sc + L"`";
    out += L"  (command: " + utf8ToWide(c.id) + L")\n";
  }
  return out;
}

}  // namespace verso
