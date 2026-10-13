cmake_minimum_required(VERSION 3.21)

# The version shown to users. CMake's numeric version (for file metadata) is the
# same with leading zeros dropped: 0.26.008 -> 0.26.8.
set(VERSO_VERSION "0.26.008")
string(REPLACE "." ";" _verso_parts "${VERSO_VERSION}")
foreach(_part MAJOR MINOR PATCH)
  list(POP_FRONT _verso_parts _value)
  string(REGEX REPLACE "^0+([0-9])" "\\1" VERSO_VERSION_${_part} "${_value}")
endforeach()
project(Verso VERSION ${VERSO_VERSION_MAJOR}.${VERSO_VERSION_MINOR}.${VERSO_VERSION_PATCH} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if(MSVC)
  add_compile_options(/permissive- /utf-8 /EHsc /Zc:__cplusplus)
endif()
if(WIN32)
  add_compile_definitions(UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

# Version header for code and resources (verso_version.h).
configure_file(res/version.h.in ${CMAKE_BINARY_DIR}/generated/verso_version.h @ONLY)

# --- Portable core: text model, editing, search, syntax, Markdown, terminal screen --
# Standard C++ only; builds and is tested on Windows, macOS and Linux.
add_library(verso_core STATIC
  src/core/TextUtil.cpp
  src/core/TextBuffer.cpp
  src/core/Selection.cpp
  src/core/Search.cpp
  src/core/Editor.cpp
  src/core/Encoding.cpp
  src/core/LineDiff.cpp
  src/core/Markdown.cpp
  src/core/WrapMap.cpp
  src/core/VtScreen.cpp
  src/syntax/Language.cpp
  src/syntax/Highlighter.cpp
  src/util/Json.cpp
  src/util/Fuzzy.cpp
)
target_include_directories(verso_core PUBLIC src ${CMAKE_BINARY_DIR}/generated)
if(MSVC)
  target_compile_options(verso_core PRIVATE /W4)
else()
  target_compile_options(verso_core PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_executable(verso_tests tests/core_tests.cpp)
target_link_libraries(verso_tests PRIVATE verso_core)
add_test(NAME editor_core COMMAND verso_tests)

# --- Front ends -----------------------------------------------------------------------
# The application front end (window, rendering, input, git mode, terminal, PDF and
# image views) uses Win32, Direct2D/DirectWrite, WIC, ConPTY and WinHTTP. A macOS or
# Linux front end would sit on the same portable core; see README > Platforms.
option(VERSO_BUILD_APP "Build the Verso application (Windows)" ON)

if(WIN32)
  # --- Git layer: process runner, git wrapper, helpers ---------------------------------
  add_library(verso_git STATIC
    merge/src/git_process.cpp
    merge/src/git.cpp
    merge/src/util.cpp
  )
  target_include_directories(verso_git PUBLIC merge/src)

  add_executable(verso_git_selftest
    merge/tests/selftest.cpp
    merge/src/graph.cpp
    merge/src/diff_view.cpp
    merge/src/theme.cpp
  )
  target_link_libraries(verso_git_selftest PRIVATE verso_git user32 gdi32 uxtheme dwmapi)
  if(MSVC)
    target_compile_options(verso_git_selftest PRIVATE /W4)
  endif()
  add_test(NAME verso_git COMMAND verso_git_selftest)

  if(VERSO_BUILD_APP)
    # --- Git mode: the git client as an embeddable view ---------------------------
    add_library(verso_gitview STATIC
      merge/src/main_window.cpp
      merge/src/commit_list.cpp
      merge/src/diff_view.cpp
      merge/src/dialogs.cpp
      merge/src/graph.cpp
      merge/src/theme.cpp
    )
    target_link_libraries(verso_gitview PUBLIC verso_git)
    if(MSVC)
      target_compile_options(verso_gitview PRIVATE /W4)
    endif()

    # --- Verso: the editor -----------------------------------------------------
    configure_file(res/Verso.manifest.in ${CMAKE_BINARY_DIR}/generated/Verso.manifest @ONLY)
    add_executable(Verso WIN32
      src/main.cpp
      src/app/App.cpp
      src/app/AppCommands.cpp
      src/app/AppFiles.cpp
      src/app/AppGit.cpp
      src/app/AppInput.cpp
      src/app/AppMarkdown.cpp
      src/app/AppMcp.cpp
      src/app/AppPdf.cpp
      src/app/AppPanels.cpp
      src/app/AppRender.cpp
      src/app/AppTerminal.cpp
      src/app/Clipboard.cpp
      src/app/DarkMenu.cpp
      src/app/FileTree.cpp
      src/app/Gfx.cpp
      src/app/GitService.cpp
      src/app/Icons.cpp
      src/app/Keymap.cpp
      src/app/LineEdit.cpp
      src/app/MarkdownImages.cpp
      src/app/MarkdownLinks.cpp
      src/app/MarkdownPreview.cpp
      src/app/McpServer.cpp
      src/app/PdfView.cpp
      src/app/Pty.cpp
      src/app/Settings.cpp
      res/Verso.rc
      ${CMAKE_BINARY_DIR}/generated/Verso.manifest
    )
    target_include_directories(Verso PRIVATE ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(Verso PRIVATE verso_core verso_gitview verso_git d2d1 dwrite imm32 shlwapi shell32 ole32
                          dwmapi comdlg32 user32 gdi32 gdiplus comctl32 uxtheme windowscodecs windowsapp winhttp)
    if(MSVC)
      target_compile_options(Verso PRIVATE /W4)
    endif()

    # --- verso-mcp: connects MCP clients (AI tools) to a running Verso ---------------
    add_executable(verso-mcp src/mcp/verso_mcp.cpp)
    if(MSVC)
      target_compile_options(verso-mcp PRIVATE /W4)
    endif()
    add_dependencies(Verso verso-mcp)

    # --- Install and package (cmake --install / cpack) ------------------------------
    install(TARGETS Verso verso-mcp RUNTIME DESTINATION .)
    install(FILES README.md LICENSE NOTICE DESTINATION .)
    install(FILES res/fonts/OFL.txt DESTINATION licenses RENAME JetBrainsMono-OFL.txt)
    set(CPACK_GENERATOR ZIP)
  endif()
else()
  message(STATUS "Verso ${VERSO_VERSION}: building the portable core and its tests. "
                 "The application front end is Windows-only for now (README > Platforms).")
endif()

set(CPACK_PACKAGE_NAME Verso)
set(CPACK_PACKAGE_VERSION ${VERSO_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Verso: a text, Markdown and git editor")
include(CPack)
