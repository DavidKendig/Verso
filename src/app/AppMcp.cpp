// MCP tools: what AI tools (through verso-mcp.exe) can do in Verso.
#include <windows.h>

#include <algorithm>

#include "app/App.h"
#include "core/TextUtil.h"
#include "verso_version.h"

namespace verso {

namespace {

constexpr size_t kMaxReadChars = 200000;  // per read_document call; ask for a line range for more

// Tool definitions (MCP tools/list), as JSON Schema.
constexpr char kTools[] = R"JSON({"tools": [
 {"name": "get_state", "title": "Get Verso state",
  "description": "Verso's current mode (text, markdown, git or terminal), the active document with its caret, all open documents and the open project folders.",
  "inputSchema": {"type": "object", "properties": {}}},
 {"name": "list_documents", "title": "List open documents",
  "description": "The documents open in Verso's tabs, with path, language, line count and whether they have unsaved changes.",
  "inputSchema": {"type": "object", "properties": {}}},
 {"name": "open_file", "title": "Open a file",
  "description": "Opens a file in a Verso tab (or switches to it if open) and optionally moves the caret. Relative paths resolve against the first project folder. Verso stays in its current mode; use set_mode to show the editor.",
  "inputSchema": {"type": "object", "properties": {
    "path": {"type": "string", "description": "File path, absolute or relative to the project folder."},
    "line": {"type": "integer", "minimum": 1, "description": "Line to place the caret on (1-based)."},
    "column": {"type": "integer", "minimum": 1, "description": "Column on that line (1-based)."},
    "activate": {"type": "boolean", "description": "Make it the active tab (default true)."}},
   "required": ["path"]}},
 {"name": "read_document", "title": "Read a document",
  "description": "Text of an open document as it is in the editor, including unsaved changes. Defaults to the active document. Use start_line/end_line for large files.",
  "inputSchema": {"type": "object", "properties": {
    "path": {"type": "string", "description": "Document path; omitted means the active document. Opened if needed."},
    "start_line": {"type": "integer", "minimum": 1},
    "end_line": {"type": "integer", "minimum": 1}}}},
 {"name": "replace_text", "title": "Replace text",
  "description": "Replaces old_text with new_text in a document (undoable; unsaved until save_document). old_text must match exactly once unless replace_all is true.",
  "inputSchema": {"type": "object", "properties": {
    "path": {"type": "string", "description": "Document path; omitted means the active document."},
    "old_text": {"type": "string"},
    "new_text": {"type": "string"},
    "replace_all": {"type": "boolean", "description": "Replace every occurrence (default false)."}},
   "required": ["old_text", "new_text"]}},
 {"name": "insert_text", "title": "Insert text",
  "description": "Inserts text at a line and column (undoable; unsaved until save_document). Without a column, inserts at the end of the line; line 0 or past the end appends to the document.",
  "inputSchema": {"type": "object", "properties": {
    "path": {"type": "string"},
    "line": {"type": "integer", "minimum": 0},
    "column": {"type": "integer", "minimum": 1},
    "text": {"type": "string"}},
   "required": ["line", "text"]}},
 {"name": "set_text", "title": "Replace a document's text",
  "description": "Replaces the whole text of a document (undoable; unsaved until save_document).",
  "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "text": {"type": "string"}}, "required": ["text"]}},
 {"name": "save_document", "title": "Save a document",
  "description": "Saves a document to disk. An untitled document needs save_as.",
  "inputSchema": {"type": "object", "properties": {
    "path": {"type": "string", "description": "Document to save; omitted means the active document."},
    "save_as": {"type": "string", "description": "Save to this path instead."}}}},
 {"name": "close_document", "title": "Close a document",
  "description": "Closes a document's tab. Refuses when it has unsaved changes unless discard_changes is true.",
  "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}, "discard_changes": {"type": "boolean"}}}},
 {"name": "get_selection", "title": "Get the selection",
  "description": "The selections (carets) in a document, with their positions and selected text.",
  "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}}},
 {"name": "set_selection", "title": "Select text",
  "description": "Selects a range (or places the caret) in a document and scrolls to it.",
  "inputSchema": {"type": "object", "properties": {
    "path": {"type": "string"},
    "start_line": {"type": "integer", "minimum": 1}, "start_column": {"type": "integer", "minimum": 1},
    "end_line": {"type": "integer", "minimum": 1}, "end_column": {"type": "integer", "minimum": 1}},
   "required": ["start_line"]}},
 {"name": "find_text", "title": "Find text",
  "description": "Finds occurrences in a document; returns line, column and the line's text for each.",
  "inputSchema": {"type": "object", "properties": {
    "query": {"type": "string"}, "path": {"type": "string"},
    "regex": {"type": "boolean"}, "case_sensitive": {"type": "boolean"}, "whole_word": {"type": "boolean"},
    "max_results": {"type": "integer", "minimum": 1}},
   "required": ["query"]}},
 {"name": "set_mode", "title": "Switch Verso's mode",
  "description": "Switches Verso between its text editor, Markdown editor (with write, split or read view), git client and terminal.",
  "inputSchema": {"type": "object", "properties": {
    "mode": {"type": "string", "enum": ["text", "markdown", "git", "terminal"]},
    "markdown_view": {"type": "string", "enum": ["write", "split", "read"]}},
   "required": ["mode"]}},
 {"name": "list_commands", "title": "List Verso commands",
  "description": "Verso's commands (the Command Palette's), with ids for run_command.",
  "inputSchema": {"type": "object", "properties": {"filter": {"type": "string", "description": "Only commands whose id or caption contains this."}}}},
 {"name": "run_command", "title": "Run a Verso command",
  "description": "Runs a Verso command by id (see list_commands), as if chosen from the Command Palette.",
  "inputSchema": {"type": "object", "properties": {"command": {"type": "string"}}, "required": ["command"]}},
 {"name": "open_folder", "title": "Open a folder",
  "description": "Adds a folder to Verso's side bar (the project folders).",
  "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}},
 {"name": "show_message", "title": "Show a message",
  "description": "Shows a short message in Verso's status bar.",
  "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}}
]})JSON";

constexpr char kInstructions[] =
    "Verso is the text, Markdown and git editor the user is running. These tools act on its open "
    "documents and windows: open, read, find in and edit files in its tabs (edits are undoable and stay "
    "unsaved until save_document), switch its mode, and run its commands. Lines and columns are 1-based. "
    "Prefer read_document over reading the file from disk when the user may have unsaved changes.";

json::Value textResult(const std::string& text, bool isError = false) {
  json::Value item = json::Value::makeObject();
  item.set("type", "text");
  item.set("text", text);
  json::Value content = json::Value::makeArray();
  content.push(item);
  json::Value r = json::Value::makeObject();
  r.set("content", content);
  r.set("isError", isError);
  return r;
}

json::Value jsonResult(const json::Value& v) {
  json::Value r = textResult(json::serialize(v, 2));
  if (v.isObject()) r.set("structuredContent", v);
  return r;
}

json::Value error(const std::wstring& message) { return textResult(wideToUtf8(message), true); }

std::wstring wstr(const json::Value& args, const char* key) {
  const json::Value* v = args.get(key);
  return v && v->type == json::Value::Type::String ? v->asWString() : std::wstring();
}

long long integer(const json::Value& args, const char* key, long long def) {
  const json::Value* v = args.get(key);
  return v && v->type == json::Value::Type::Number ? static_cast<long long>(v->number) : def;
}

bool flag(const json::Value& args, const char* key, bool def) {
  const json::Value* v = args.get(key);
  return v && v->type == json::Value::Type::Bool ? v->boolean : def;
}

// 1-based (line, column) -> buffer offset, clamped to the document.
size_t offsetOf(const TextBuffer& b, long long line, long long column) {
  const size_t l = static_cast<size_t>(std::clamp<long long>(line, 1, static_cast<long long>(b.lineCount()))) - 1;
  const size_t len = b.lineEnd(l) - b.lineStart(l);
  return b.lineStart(l) + std::min(len, static_cast<size_t>(std::max<long long>(column, 1) - 1));
}

json::Value position(const TextBuffer& b, size_t pos) {
  const size_t l = b.lineOf(pos);
  json::Value p = json::Value::makeObject();
  p.set("line", static_cast<double>(l + 1));
  p.set("column", static_cast<double>(pos - b.lineStart(l) + 1));
  return p;
}

}  // namespace

json::Value App::mcpTools() const {
  auto tools = json::parse(kTools);
  return tools ? *tools : json::Value::makeObject();
}

void App::startMcp() {
  if (!settings_.mcpServer || mcp_.running()) return;
  std::wstring err;
  if (!mcp_.start(hwnd_, WM_APP_MCP, mcpTools(), kInstructions, &err)) setStatus(L"MCP server: " + err, 6000);
}

std::wstring App::mcpResolve(const std::wstring& path) const {
  if (path.empty()) return path;
  const bool absolute = (path.size() > 1 && path[1] == L':') || path.rfind(L"\\\\", 0) == 0;
  if (absolute) return fullPathOf(path);
  if (!tree_.folders().empty()) return fullPathOf(tree_.folders().front() + L"\\" + path);
  return fullPathOf(path);
}

Document* App::mcpDocument(const json::Value& args, std::wstring& err) {
  const std::wstring raw = wstr(args, "path");
  if (raw.empty()) {
    if (Document* d = doc()) return d;
    err = L"No document is open. Pass a path or use open_file.";
    return nullptr;
  }
  const std::wstring path = mcpResolve(raw);
  for (auto& d : docs_)
    if (samePath(d->path, path)) return d.get();
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    err = L"No such file: " + path;
    return nullptr;
  }
  Document* d = openFile(path, false);
  if (!d) err = L"Could not open " + path;
  return d;
}

json::Value App::mcpDocInfo(const Document& d) const {
  const TextBuffer& b = d.ed.buffer();
  json::Value v = json::Value::makeObject();
  v.set("path", d.path.empty() ? json::Value() : json::Value(std::wstring_view(d.path)));
  v.set("title", json::Value(std::wstring_view(d.title())));
  v.set("kind", d.pdf ? "pdf" : (d.binary ? "binary" : "text"));
  v.set("language", json::Value(std::wstring_view(d.lang->name)));
  v.set("dirty", d.ed.isDirty());
  v.set("lines", static_cast<double>(b.lineCount()));
  v.set("caret", position(b, d.ed.selections().primary().caret));
  v.set("active", !docs_.empty() && docs_[active_].get() == &d);
  return v;
}

json::Value App::mcpCall(const std::string& tool, const json::Value& args) {
  std::wstring err;
  auto mode = [&]() -> const char* {
    return gitMode_ ? "git" : terminalMode_ ? "terminal" : mdMode_ ? "markdown" : "text";
  };
  auto docs = [&]() {
    json::Value list = json::Value::makeArray();
    for (const auto& d : docs_) list.push(mcpDocInfo(*d));
    return list;
  };
  // Shared by the editing tools: documents that may not change are refused.
  auto editable = [&](Document* d) -> bool {
    if (d->pdf || d->binary) {
      err = d->title() + L" is read-only (PDF or binary).";
      return false;
    }
    return true;
  };
  auto finishEdit = [&](Document& d) {
    afterEdit(d, docs_[active_].get() == &d);
    invalidate();
  };

  if (tool == "get_state") {
    json::Value v = json::Value::makeObject();
    v.set("version", VERSO_VERSION_STRING);
    v.set("mode", mode());
    v.set("markdown_view", mdView_ == MdView::Write ? "write" : mdView_ == MdView::Split ? "split" : "read");
    v.set("active_document", doc() ? mcpDocInfo(*doc()) : json::Value());
    v.set("documents", docs());
    json::Value folders = json::Value::makeArray();
    for (const auto& f : tree_.folders()) folders.push(json::Value(std::wstring_view(f)));
    v.set("folders", folders);
    json::Value term = json::Value::makeObject();
    term.set("running", term_->started && !term_->exited);
    term.set("folder", json::Value(std::wstring_view(term_->folder)));
    term.set("title", json::Value(std::wstring_view(term_->screen.title())));
    v.set("terminal", term);
    return jsonResult(v);
  }
  if (tool == "list_documents") {
    json::Value v = json::Value::makeObject();
    v.set("documents", docs());
    return jsonResult(v);
  }
  if (tool == "open_file") {
    const std::wstring path = mcpResolve(wstr(args, "path"));
    if (path.empty()) return error(L"open_file needs a path.");
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return error(L"No such file: " + path);
    Document* d = openFile(path, flag(args, "activate", true));
    if (!d) return error(L"Could not open " + path);
    if (args.get("line")) {
      const TextBuffer& b = d->ed.buffer();
      const size_t pos = offsetOf(b, integer(args, "line", 1), integer(args, "column", 1));
      d->ed.setSelections({Selection::at(pos)}, 0);
      if (d == doc()) revealCaret(true);
    }
    invalidate();
    return jsonResult(mcpDocInfo(*d));
  }
  if (tool == "read_document") {
    Document* d = mcpDocument(args, err);
    if (!d) return error(err);
    if (d->pdf) return error(d->title() + L" is a PDF; Verso shows its pages but has no text for it.");
    const TextBuffer& b = d->ed.buffer();
    const long long total = static_cast<long long>(b.lineCount());
    const long long first = std::clamp<long long>(integer(args, "start_line", 1), 1, total);
    const long long last = std::clamp<long long>(integer(args, "end_line", total), first, total);
    const size_t from = b.lineStart(static_cast<size_t>(first - 1)), to = b.lineEnd(static_cast<size_t>(last - 1));
    std::wstring text = b.text().substr(from, to - from);
    const bool truncated = text.size() > kMaxReadChars;
    if (truncated) text.resize(kMaxReadChars);
    json::Value v = mcpDocInfo(*d);
    v.set("start_line", static_cast<double>(first));
    v.set("end_line", static_cast<double>(last));
    v.set("truncated", truncated);
    v.set("text", json::Value(std::wstring_view(text)));
    return jsonResult(v);
  }
  if (tool == "replace_text" || tool == "insert_text" || tool == "set_text") {
    Document* d = mcpDocument(args, err);
    if (!d || !editable(d)) return error(err);
    const TextBuffer& b = d->ed.buffer();
    const std::wstring& text = b.text();
    std::vector<Editor::Change> changes;
    if (tool == "replace_text") {
      const std::wstring oldText = wstr(args, "old_text"), newText = wstr(args, "new_text");
      if (oldText.empty()) return error(L"old_text must not be empty.");
      std::vector<size_t> hits;
      for (size_t p = text.find(oldText); p != std::wstring::npos; p = text.find(oldText, p + oldText.size()))
        hits.push_back(p);
      if (hits.empty()) return error(L"old_text was not found in " + d->title() + L".");
      if (hits.size() > 1 && !flag(args, "replace_all", false))
        return error(L"old_text occurs " + std::to_wstring(hits.size()) +
                     L" times; include more surrounding text or set replace_all.");
      for (size_t p : hits) changes.push_back({p, oldText.size(), newText});
    } else if (tool == "insert_text") {
      const long long line = integer(args, "line", 0);
      size_t pos = b.size();
      if (line >= 1 && line <= static_cast<long long>(b.lineCount()))
        pos = args.get("column") ? offsetOf(b, line, integer(args, "column", 1)) : b.lineEnd(static_cast<size_t>(line - 1));
      changes.push_back({pos, 0, wstr(args, "text")});
    } else {
      changes.push_back({0, b.size(), wstr(args, "text")});
    }
    const size_t caret = changes.back().pos + changes.back().text.size();
    d->ed.commit(std::move(changes), {Selection::at(caret)}, 0, EditKind::Other);
    finishEdit(*d);
    json::Value v = mcpDocInfo(*d);
    v.set("edited", true);
    return jsonResult(v);
  }
  if (tool == "save_document") {
    Document* d = mcpDocument(args, err);
    if (!d) return error(err);
    if (const std::wstring as = wstr(args, "save_as"); !as.empty()) {
      d->path = mcpResolve(as);
      d->ed.markUnsaved();
    } else if (d->path.empty()) {
      return error(L"This document is untitled; pass save_as with a path.");
    }
    if (!saveDocument(*d, false)) return error(L"Could not save " + d->title() + L" (see Verso's status bar).");
    updateTitle();
    invalidate();
    return jsonResult(mcpDocInfo(*d));
  }
  if (tool == "close_document") {
    Document* d = mcpDocument(args, err);
    if (!d) return error(err);
    if (d->ed.isDirty() && !flag(args, "discard_changes", false))
      return error(d->title() + L" has unsaved changes; save it or pass discard_changes.");
    for (size_t i = 0; i < docs_.size(); ++i)
      if (docs_[i].get() == d) closeDocument(i, false);
    return textResult("closed");
  }
  if (tool == "get_selection") {
    Document* d = mcpDocument(args, err);
    if (!d) return error(err);
    const TextBuffer& b = d->ed.buffer();
    json::Value list = json::Value::makeArray();
    for (const auto& s : d->ed.selections().all()) {
      json::Value v = json::Value::makeObject();
      v.set("start", position(b, s.begin()));
      v.set("end", position(b, s.end()));
      v.set("text", json::Value(std::wstring_view(b.text()).substr(s.begin(), s.length())));
      list.push(v);
    }
    json::Value v = json::Value::makeObject();
    v.set("path", d->path.empty() ? json::Value() : json::Value(std::wstring_view(d->path)));
    v.set("selections", list);
    return jsonResult(v);
  }
  if (tool == "set_selection") {
    Document* d = mcpDocument(args, err);
    if (!d) return error(err);
    const TextBuffer& b = d->ed.buffer();
    const size_t a = offsetOf(b, integer(args, "start_line", 1), integer(args, "start_column", 1));
    const size_t c = args.get("end_line") ? offsetOf(b, integer(args, "end_line", 1), integer(args, "end_column", 1)) : a;
    d->ed.setSelections({Selection(a, c)}, 0);
    for (size_t i = 0; i < docs_.size(); ++i)
      if (docs_[i].get() == d) activate(i);
    revealCaret(true);
    invalidate();
    return jsonResult(mcpDocInfo(*d));
  }
  if (tool == "find_text") {
    Document* d = mcpDocument(args, err);
    if (!d) return error(err);
    Searcher s;
    std::wstring compileError;
    if (!s.compile(wstr(args, "query"), {flag(args, "regex", false), flag(args, "case_sensitive", false),
                                         flag(args, "whole_word", false)},
                   &compileError, d->ed.config.wordSeparators))
      return error(L"Invalid search: " + compileError);
    const TextBuffer& b = d->ed.buffer();
    const size_t limit = static_cast<size_t>(std::clamp<long long>(integer(args, "max_results", 100), 1, 5000));
    json::Value list = json::Value::makeArray();
    for (const auto& m : s.all(b.text(), 0, std::wstring::npos, limit)) {
      json::Value v = position(b, m.pos);
      v.set("length", static_cast<double>(m.len));
      v.set("line_text", json::Value(b.lineText(b.lineOf(m.pos)).substr(0, 400)));
      list.push(v);
    }
    json::Value v = json::Value::makeObject();
    v.set("matches", list);
    return jsonResult(v);
  }
  if (tool == "set_mode") {
    const std::string m = args.get("mode") ? args.get("mode")->asString() : "";
    if (m == "terminal") {
      setTerminalMode(true);
    } else if (m == "git") {
      setTerminalMode(false);
      setGitMode(true);
    } else if (m == "text" || m == "markdown") {
      if (terminalMode_) setTerminalMode(false);
      setEditorMode(m == "markdown");
      const std::string view = args.get("markdown_view") ? args.get("markdown_view")->asString() : "";
      if (m == "markdown" && !view.empty())
        setMdView(view == "write" ? MdView::Write : view == "read" ? MdView::Read : MdView::Split);
    } else {
      return error(L"mode must be text, markdown, git or terminal.");
    }
    json::Value v = json::Value::makeObject();
    v.set("mode", mode());
    return jsonResult(v);
  }
  if (tool == "list_commands") {
    const std::wstring filter = toLower(wstr(args, "filter"));
    json::Value list = json::Value::makeArray();
    for (const auto& c : commands_) {
      if (c.caption.empty()) continue;
      if (!filter.empty() && toLower(utf8ToWide(c.id)).find(filter) == std::wstring::npos &&
          toLower(c.caption).find(filter) == std::wstring::npos)
        continue;
      json::Value v = json::Value::makeObject();
      v.set("id", c.id);
      v.set("caption", json::Value(std::wstring_view(c.caption)));
      list.push(v);
    }
    json::Value v = json::Value::makeObject();
    v.set("commands", list);
    return jsonResult(v);
  }
  if (tool == "run_command") {
    const std::string id = args.get("command") ? args.get("command")->asString() : "";
    if (!commandIndex_.count(id)) return error(L"Unknown command: " + utf8ToWide(id) + L" (see list_commands).");
    runCommand(id);
    invalidate();
    return textResult("ran " + id);
  }
  if (tool == "open_folder") {
    const std::wstring path = mcpResolve(wstr(args, "path"));
    const DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return error(L"No such folder: " + path);
    openFolder(path, false);
    return textResult("opened " + wideToUtf8(path));
  }
  if (tool == "show_message") {
    setStatus(wstr(args, "text"), 8000);
    invalidate();
    return textResult("shown");
  }
  return error(L"Unknown tool: " + utf8ToWide(tool));
}

}  // namespace verso
