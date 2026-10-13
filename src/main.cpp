#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "app/App.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  // Background git status must never block on prompts or hold the index lock.
  SetEnvironmentVariableW(L"GIT_TERMINAL_PROMPT", L"0");
  SetEnvironmentVariableW(L"GIT_OPTIONAL_LOCKS", L"0");
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  LocalFree(argv);
  verso::App app;
  return app.run(instance, show, args);
}
