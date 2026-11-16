// Verso on Linux: the entry point (Windows: src/main.cpp).
#include <clocale>
#include <cstdlib>
#include <string>
#include <vector>

#include "app/App.h"
#include "core/TextUtil.h"

int main(int argc, char** argv) {
  // Character classification for wide strings; numbers keep the "C" format (JSON, settings).
  std::setlocale(LC_CTYPE, "");
  if (!std::setlocale(LC_CTYPE, "C.UTF-8")) std::setlocale(LC_CTYPE, "");
  // Background git status must never block on prompts or hold the index lock.
  setenv("GIT_TERMINAL_PROMPT", "0", 1);
  setenv("GIT_OPTIONAL_LOCKS", "0", 1);
  std::vector<std::wstring> args;
  for (int i = 1; i < argc; ++i) args.push_back(verso::utf8ToWide(argv[i]));
  verso::App app;
  return app.run(nullptr, 0, args);
}
