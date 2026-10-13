#include "app/Settings.h"

#include "core/Encoding.h"
#include "util/Json.h"

namespace verso {

bool Settings::load(const std::wstring& path, std::wstring* error) {
  std::string bytes;
  if (!readFileBytes(path, bytes, nullptr)) return true;  // no user file: defaults
  std::string err;
  auto v = json::parse(bytes, &err);
  if (!v || !v->isObject()) {
    if (error) *error = L"Preferences.json: " + utf8ToWide(err.empty() ? "expected an object" : err);
    return false;
  }
  fontFace = v->getWString("font_face", fontFace);
  fontSize = static_cast<float>(v->getNumber("font_size", fontSize));
  linePadding = static_cast<float>(v->getNumber("line_padding", linePadding));
  theme = v->getWString("theme", theme);
  tabSize = static_cast<int>(v->getNumber("tab_size", tabSize));
  translateTabs = v->getBool("translate_tabs_to_spaces", translateTabs);
  detectIndentation = v->getBool("detect_indentation", detectIndentation);
  autoMatch = v->getBool("auto_match_enabled", autoMatch);
  autoIndent = v->getBool("auto_indent", autoIndent);
  trimAutoWhitespace = v->getBool("trim_automatic_white_space", trimAutoWhitespace);
  drawMinimap = v->getBool("draw_minimap", drawMinimap);
  highlightLine = v->getBool("highlight_line", highlightLine);
  lineNumbers = v->getBool("line_numbers", lineNumbers);
  drawIndentGuides = v->getBool("draw_indent_guides", drawIndentGuides);
  matchBrackets = v->getBool("match_brackets", matchBrackets);
  matchSelection = v->getBool("match_selection", matchSelection);
  drawWhiteSpaceInSelection = v->getBool("draw_white_space_in_selection", drawWhiteSpaceInSelection);
  scrollPastEnd = v->getBool("scroll_past_end", scrollPastEnd);
  hotExit = v->getBool("hot_exit", hotExit);
  wordWrap = v->getBool("word_wrap", wordWrap);
  markdownRemoteImages = v->getBool("markdown_remote_images", markdownRemoteImages);
  terminalShell = v->getWString("terminal_shell", terminalShell);
  mcpServer = v->getBool("mcp_server", mcpServer);
  trimTrailingWhitespaceOnSave = v->getBool("trim_trailing_white_space_on_save", trimTrailingWhitespaceOnSave);
  ensureNewlineAtEofOnSave = v->getBool("ensure_newline_at_eof_on_save", ensureNewlineAtEofOnSave);
  saveOnFocusLost = v->getBool("save_on_focus_lost", saveOnFocusLost);
  showEncoding = v->getBool("show_encoding", showEncoding);
  showLineEndings = v->getBool("show_line_endings", showLineEndings);
  sidebarShowOpenFiles = v->getBool("sidebar_show_open_files", sidebarShowOpenFiles);
  showGitStatus = v->getBool("show_git_status", showGitStatus);
  miniDiff = v->getBool("mini_diff", miniDiff);
  caretBlink = v->getBool("caret_blink", caretBlink);
  caretWidth = static_cast<float>(v->getNumber("caret_width", caretWidth));
  defaultLineEnding = v->getWString("default_line_ending", defaultLineEnding);
  wordSeparators = v->getWString("word_separators", wordSeparators);
  if (const auto* r = v->get("rulers"); r && r->isArray()) {
    rulers.clear();
    for (const auto& x : r->array) rulers.push_back(static_cast<int>(x.asNumber(0)));
  }
  auto readList = [&](const char* key, std::vector<std::wstring>& out) {
    if (const auto* a = v->get(key); a && a->isArray()) {
      out.clear();
      for (const auto& x : a->array) out.push_back(x.asWString());
    }
  };
  readList("folder_exclude_patterns", folderExcludePatterns);
  readList("file_exclude_patterns", fileExcludePatterns);

  if (fontSize < 4) fontSize = 4;
  if (fontSize > 72) fontSize = 72;
  if (tabSize < 1) tabSize = 1;
  if (tabSize > 16) tabSize = 16;
  return true;
}

std::string Settings::defaultFileContents() {
  return R"(// Verso preferences. Changes apply when this file is saved.
// The file accepts comments and trailing commas.
{
	// "JetBrains Mono Slashed" is bundled with Verso (JetBrains Mono with a
	// slashed zero). Any installed font family name works here too.
	"font_face": "JetBrains Mono Slashed",
	"font_size": 10.5,
	"line_padding": 4,

	// "dark" or "light"
	"theme": "dark",

	"tab_size": 4,
	"translate_tabs_to_spaces": false,
	"detect_indentation": true,
	"auto_indent": true,
	"auto_match_enabled": true,
	"trim_automatic_white_space": true,

	"draw_minimap": true,
	"highlight_line": true,
	"line_numbers": true,
	"draw_indent_guides": true,
	"match_brackets": true,
	"match_selection": true,
	"draw_white_space_in_selection": true,
	"scroll_past_end": true,
	// Soft-wrap long lines in Text mode (Markdown mode always wraps).
	"word_wrap": false,
	// Download http(s) images shown in the Markdown preview. Turn off to keep
	// previews from contacting image hosts.
	"markdown_remote_images": true,
	// Terminal mode's shell command line. Empty uses PowerShell 7 (pwsh) when
	// installed, otherwise Windows PowerShell. Example: "cmd.exe".
	"terminal_shell": "",
	// Let AI tools control Verso through MCP (verso-mcp.exe). Only programs run by
	// your Windows user can connect. Restart Verso after changing this.
	"mcp_server": true,
	"rulers": [],
	"caret_blink": true,
	"caret_width": 2,

	// Restore unsaved changes and open files on restart without prompting.
	"hot_exit": true,
	"trim_trailing_white_space_on_save": false,
	"ensure_newline_at_eof_on_save": false,
	"save_on_focus_lost": false,

	// "system", "windows" or "unix"
	"default_line_ending": "system",
	"show_encoding": true,
	"show_line_endings": true,

	// List open tabs in an "OPEN FILES" section at the top of the side bar.
	"sidebar_show_open_files": true,

	// Git: side bar status badges, the branch in the status bar, and gutter
	// markers for lines changed since the git index (staged version).
	"show_git_status": true,
	"mini_diff": true,

	"word_separators": "./\\()\"'-:,.;<>~!@#$%^&*|+=[]{}`~?",
	"folder_exclude_patterns": [".git", ".svn", ".hg", "CVS", ".Trash", ".Trash-*", "node_modules", "__pycache__"],
	"file_exclude_patterns": ["*.pyc", "*.pyo", "*.exe", "*.dll", "*.obj", "*.o", "*.a", "*.lib", "*.so", "*.dylib", "*.ncb", "*.sdf", "*.suo", "*.pdb", "*.idb", ".DS_Store", "*.class", "*.psd", "*.db"],
}
)";
}

}  // namespace verso
