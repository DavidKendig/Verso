#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "app/Pty.h"
#include "core/VtScreen.h"

namespace verso {

// Terminal mode's session: the shell running in a pseudo console, its screen, and
// the view state (scrollback position, mouse selection).
struct TerminalView {
  VtScreen screen{120, 30};
  Pty pty;
  bool started = false, exited = false;
  DWORD exitCode = 0;
  std::wstring shell;   // the command line that was started
  std::wstring folder;  // the folder it belongs to (and started in)

  size_t scrolledBack = 0;      // lines above the live screen being viewed
  uint64_t seenScrolled = 0;    // screen.linesScrolled() when last applied

  // Selection in history coordinates (line index into the screen's history, column).
  bool selecting = false, hasSelection = false;
  size_t selAnchorLine = 0, selLine = 0;
  int selAnchorCol = 0, selCol = 0;

  wchar_t pendingHigh = 0;  // first half of a surrogate pair typed with WM_CHAR
};

}  // namespace verso
