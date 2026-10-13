#pragma once

#include <string>
#include <vector>

#include "core/TextUtil.h"

namespace verso {

// The bundled JetBrains Mono build with a slashed zero (see res/fonts).
inline constexpr const wchar_t* kEmbeddedMonoFamilyName = L"JetBrains Mono Slashed";

// User preferences, loaded from a JSON file (comments and
// trailing commas allowed). Unknown keys are ignored.
struct Settings {
  std::wstring fontFace = kEmbeddedMonoFamilyName;
  float fontSize = 10.5f;
  float linePadding = 4.0f;
  std::wstring theme = L"dark";
  int tabSize = 4;
  bool translateTabs = false;
  bool detectIndentation = true;
  bool autoMatch = true;
  bool autoIndent = true;
  bool trimAutoWhitespace = true;
  bool drawMinimap = true;
  bool highlightLine = true;
  bool lineNumbers = true;
  bool drawIndentGuides = true;
  bool matchBrackets = true;
  bool matchSelection = true;
  bool drawWhiteSpaceInSelection = true;
  bool scrollPastEnd = true;
  bool hotExit = true;
  bool wordWrap = false;  // Markdown mode always wraps
  bool markdownRemoteImages = true;  // the Markdown preview downloads http(s) images
  std::wstring terminalShell;        // terminal mode's command line; empty = PowerShell
  bool mcpServer = true;             // lets AI tools (via verso-mcp.exe) control Verso
  bool trimTrailingWhitespaceOnSave = false;
  bool ensureNewlineAtEofOnSave = false;
  bool saveOnFocusLost = false;
  bool showEncoding = true;
  bool showLineEndings = true;
  bool sidebarShowOpenFiles = true;
  bool showGitStatus = true;  // side bar badges and the status bar branch
  bool miniDiff = true;       // gutter markers for lines changed since the git index
  bool caretBlink = true;
  float caretWidth = 2.0f;
  std::wstring defaultLineEnding = L"system";
  std::wstring wordSeparators{kDefaultWordSeparators};
  std::vector<int> rulers;
  std::vector<std::wstring> folderExcludePatterns{L".git", L".svn", L".hg", L"CVS", L".Trash", L".Trash-*",
                                                   L"node_modules", L"__pycache__"};
  std::vector<std::wstring> fileExcludePatterns{L"*.pyc", L"*.pyo", L"*.exe", L"*.dll", L"*.obj", L"*.o",
                                                 L"*.a",   L"*.lib", L"*.so",  L"*.dylib", L"*.ncb", L"*.sdf",
                                                 L"*.suo", L"*.pdb", L"*.idb", L".DS_Store", L"*.class", L"*.psd",
                                                 L"*.db",  L"*.verso-save-tmp"};

  // Returns false (keeping defaults for unparsed keys) when the file is
  // unreadable or invalid; `error` describes the problem.
  bool load(const std::wstring& path, std::wstring* error);
  static std::string defaultFileContents();
};

}  // namespace verso
