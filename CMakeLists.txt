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
option(VERSO_BUILD_APP "Build the Verso application (Windows, Linux)" ON)

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
elseif(UNIX AND NOT APPLE AND VERSO_BUILD_APP)
  # --- Linux front end (Wayland + Cairo/Pango) ---------------------------------------------
  # The app sources are shared with Windows. src/app/linux/ provides the window, drawing,
  # terminal, AI connection, images and PDF pages, plus the small part of the Win32 API the
  # shared code calls (Win32Compat), so the Windows code paths stay as they are.
  find_package(PkgConfig REQUIRED)
  find_package(Threads REQUIRED)
  pkg_check_modules(VERSO_LINUX REQUIRED IMPORTED_TARGET
    wayland-client xkbcommon cairo pangocairo fontconfig gio-2.0 gdk-pixbuf-2.0 librsvg-2.0 libcurl)
  pkg_get_variable(WAYLAND_PROTOCOLS wayland-protocols pkgdatadir)
  pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)
  set(_proto_dir ${CMAKE_BINARY_DIR}/protocols)
  file(MAKE_DIRECTORY ${_proto_dir})
  set(_proto_srcs)
  foreach(_spec
      "stable/xdg-shell/xdg-shell.xml|xdg-shell"
      "unstable/xdg-decoration/xdg-decoration-unstable-v1.xml|xdg-decoration-unstable-v1"
      "staging/cursor-shape/cursor-shape-v1.xml|cursor-shape-v1"
      "stable/tablet/tablet-v2.xml|tablet-v2")
    string(REPLACE "|" ";" _parts ${_spec})
    list(GET _parts 0 _xml)
    list(GET _parts 1 _name)
    add_custom_command(
      OUTPUT ${_proto_dir}/${_name}-client-protocol.h ${_proto_dir}/${_name}-protocol.c
      COMMAND ${WAYLAND_SCANNER} client-header ${WAYLAND_PROTOCOLS}/${_xml} ${_proto_dir}/${_name}-client-protocol.h
      COMMAND ${WAYLAND_SCANNER} private-code ${WAYLAND_PROTOCOLS}/${_xml} ${_proto_dir}/${_name}-protocol.c
      DEPENDS ${WAYLAND_PROTOCOLS}/${_xml})
    list(APPEND _proto_srcs ${_proto_dir}/${_name}-client-protocol.h ${_proto_dir}/${_name}-protocol.c)
  endforeach()
  enable_language(C)
  add_library(verso_protocols STATIC ${_proto_srcs})
  set_target_properties(verso_protocols PROPERTIES LINKER_LANGUAGE C)
  target_include_directories(verso_protocols PUBLIC ${_proto_dir})
  target_link_libraries(verso_protocols PUBLIC PkgConfig::VERSO_LINUX)

  # The bundled fonts become part of the program, as the .rc resources do on Windows.
  set(_font_cpp ${CMAKE_BINARY_DIR}/generated/EmbeddedFonts.cpp)
  file(GLOB _font_files ${CMAKE_SOURCE_DIR}/res/fonts/*.ttf)
  set(_font_body "#include <cstddef>\n#include <cstdint>\nnamespace verso::linux_fonts {\n")
  set(_font_index "")
  set(_n 0)
  foreach(_f ${_font_files})
    file(READ ${_f} _hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _hex "${_hex}")
    string(APPEND _font_body "static const uint8_t f${_n}[] = {${_hex}};\n")
    file(SIZE ${_f} _size)
    string(APPEND _font_index "{f${_n}, ${_size}},")
    math(EXPR _n "${_n} + 1")
  endforeach()
  file(READ ${CMAKE_SOURCE_DIR}/res/fonts/OFL.txt _ofl HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _ofl "${_ofl}")
  string(APPEND _font_body "struct Blob { const uint8_t* data; size_t size; };\n"
    "extern const Blob kFonts[] = {${_font_index}};\nextern const size_t kFontCount = ${_n};\n"
    "static const uint8_t ofl[] = {${_ofl}0x00};\nextern const char* const kOflText = reinterpret_cast<const char*>(ofl);\n}\n")
  file(WRITE ${_font_cpp}.in "${_font_body}")
  configure_file(${_font_cpp}.in ${_font_cpp} COPYONLY)

  add_library(verso_git STATIC
    merge/src/git_process.cpp
    merge/src/git.cpp
    merge/src/graph.cpp
    merge/src/util.cpp
  )
  target_include_directories(verso_git BEFORE PRIVATE src/app/linux/win32)
  target_include_directories(verso_git PUBLIC merge/src src)
  target_link_libraries(verso_git PUBLIC verso_core Threads::Threads)

  add_executable(verso
    src/app/linux/main.cpp
    src/app/linux/AppLinux.cpp
    src/app/linux/Window.cpp
    src/app/linux/Win32Compat.cpp
    src/app/linux/GfxCairo.cpp
    src/app/linux/Menus.cpp
    src/app/linux/Clipboard.cpp
    src/app/linux/Pty.cpp
    src/app/linux/GitView.cpp
    src/app/linux/GitWidgets.cpp
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
    src/app/FileTree.cpp
    src/app/GitService.cpp
    src/app/Icons.cpp
    src/app/Keymap.cpp
    src/app/LineEdit.cpp
    src/app/MarkdownImages.cpp
    src/app/MarkdownLinks.cpp
    src/app/MarkdownPreview.cpp
    src/app/McpServer.cpp
    src/app/PdfView.cpp
    src/app/Settings.cpp
    ${_font_cpp}
  )
  # src/app/linux/win32 holds the stand-ins for <windows.h> and friends (Win32Compat.h).
  target_include_directories(verso BEFORE PRIVATE src/app/linux/win32)
  target_include_directories(verso PRIVATE src ${CMAKE_BINARY_DIR}/generated)
  target_compile_options(verso PRIVATE -Wall -Wno-unused-parameter -Wno-missing-field-initializers)
  target_link_libraries(verso PRIVATE verso_core verso_git verso_protocols PkgConfig::VERSO_LINUX Threads::Threads util)

  # verso-mcp: connects MCP clients (AI tools) to a running Verso over a Unix socket.
  add_executable(verso-mcp src/mcp/verso_mcp.cpp)
  add_dependencies(verso verso-mcp)

  include(GNUInstallDirs)
  install(TARGETS verso verso-mcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES res/linux/verso.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  foreach(_size 16 32 48 64 128 256 512)
    install(FILES res/icons/verso-${_size}.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps
            RENAME verso.png)
  endforeach()
  install(FILES README.md LICENSE NOTICE res/fonts/OFL.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/verso)
else()
  message(STATUS "Verso ${VERSO_VERSION}: building the portable core and its tests. "
                 "The application front end runs on Windows and Linux (README > Platforms).")
endif()

set(CPACK_PACKAGE_NAME Verso)
set(CPACK_PACKAGE_VERSION ${VERSO_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Verso: a text, Markdown and git editor")
include(CPack)
