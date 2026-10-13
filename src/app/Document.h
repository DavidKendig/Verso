#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "app/FileTree.h"
#include "app/PdfView.h"
#include "core/Editor.h"
#include "core/Encoding.h"
#include "core/LineDiff.h"
#include "core/WrapMap.h"
#include "syntax/Highlighter.h"

namespace verso {

// One open buffer: editing state plus its file identity and view state.
struct Document {
  uint64_t id = 0;
  Editor ed;
  std::wstring path;  // empty for untitled buffers
  Encoding encoding = Encoding::Utf8;
  LineEnding eol = LineEnding::CRLF;
  const Language* lang = &plainTextLanguage();
  bool syntaxFromUser = false;
  bool findResults = false;  // a "Find Results" buffer whose lines link to files
  // Binary data shown as text: read-only, since saving it back through a text
  // encoding could change bytes.
  bool binary = false;
  SyntaxCache syntax;
  uint64_t diskWriteTime = 0;  // last observed modification time
  bool diskMissing = false;
  float scrollX = 0, scrollY = 0;
  float previewScroll = 0;  // Markdown preview scroll offset
  // Split view: the editor position (and text revision) the preview was last matched
  // to; the preview follows the editor only when those change, so it can still scroll
  // past the point where the editor's own scrolling stops.
  float previewSyncY = -1;
  uint64_t previewSyncRev = UINT64_MAX;
  // Set for PDF files, which display rendered and read-only; the editor buffer stays empty.
  std::unique_ptr<PdfView> pdf;

  // Visual rows for word wrap at the current width (App::ensureWrap).
  WrapMap wrap;
  bool wrapBuilt = false, wrapOn = false;
  uint64_t wrapRevision = 0;
  size_t wrapCols = 0;
  int wrapTab = 0;

  // Git: the file's index version and the line hunks that differ from it.
  bool hasGitBaseline = false;
  std::wstring gitBaseline;
  std::vector<LineHunk> gitHunks;
  uint64_t gitHunksState = UINT64_MAX;

  // Cached longest line (code units) for horizontal scrolling.
  uint64_t longestState = UINT64_MAX;
  size_t longestLine = 0;

  std::wstring title() const {
    if (!path.empty()) return fileNameOf(path);
    if (findResults) return L"Find Results";
    std::wstring_view first = trim(ed.buffer().lineText(0));
    if (first.empty()) return L"untitled";
    std::wstring t(first.substr(0, 40));
    return t;
  }
};

}  // namespace verso
