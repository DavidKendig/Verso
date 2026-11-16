#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <map>

#include "app/App.h"
#include "app/Clipboard.h"
#include "util/Json.h"

namespace verso {

namespace {

}  // namespace

Document* App::doc() { return active_ < docs_.size() ? docs_[active_].get() : nullptr; }

LineEnding App::defaultLineEnding() const {
  if (settings_.defaultLineEnding == L"unix") return LineEnding::LF;
  if (settings_.defaultLineEnding == L"windows") return LineEnding::CRLF;
#ifdef _WIN32
  return LineEnding::CRLF;  // "system"
#else
  return LineEnding::LF;
#endif
}

Document& App::newDocument(bool activateIt) {
  auto d = std::make_unique<Document>();
  d->id = nextDocId_++;
  d->eol = defaultLineEnding();
  Document& ref = *d;
  configureEditor(ref);
  setLanguage(ref, plainTextLanguage(), false);
  if (mdMode_)
    if (const Language* mdl = languageByName(L"Markdown")) setLanguage(ref, *mdl, false);
  size_t at = docs_.empty() ? 0 : std::min(active_ + 1, docs_.size());
  docs_.insert(docs_.begin() + static_cast<ptrdiff_t>(at), std::move(d));
  if (activateIt)
    activate(at);
  else if (at <= active_ && docs_.size() > 1)
    ++active_;
  sessionDirty_ = true;
  return ref;
}

void App::configureEditor(Document& d) {
  EditorConfig& c = d.ed.config;
  c.tabSize = settings_.tabSize;
  c.translateTabs = settings_.translateTabs;
  c.autoMatch = settings_.autoMatch;
  c.autoIndent = settings_.autoIndent;
  c.trimAutoWhitespace = settings_.trimAutoWhitespace;
  c.wordSeparators = settings_.wordSeparators;
}

void App::setLanguage(Document& d, const Language& lang, bool fromUser) {
  d.lang = &lang;
  d.syntaxFromUser = fromUser;
  d.syntax.reset(&lang);
  CommentStyle cs;
  cs.line = lang.lineComment;
  cs.blockStart = lang.blockStart;
  cs.blockEnd = lang.blockEnd;
  cs.indentAfterColon = lang.indentAfterColon;
  if (lang.name == L"Batch File") cs.line = L"REM";
  d.ed.comments = cs;
  invalidate();
}

void App::detectIndentation(Document& d) {
  if (!settings_.detectIndentation) return;
  const TextBuffer& b = d.ed.buffer();
  size_t tabs = 0, spaces = 0;
  std::map<size_t, size_t> deltas;
  size_t prevIndent = 0;
  bool prevSpaces = false;
  size_t limit = std::min<size_t>(b.lineCount(), 5000);
  for (size_t l = 0; l < limit; ++l) {
    std::wstring_view t = b.lineText(l);
    if (trim(t).empty()) continue;
    if (t[0] == L'\t') {
      ++tabs;
      prevSpaces = false;
      continue;
    }
    size_t n = 0;
    while (n < t.size() && t[n] == L' ') ++n;
    if (n >= 2 || (n > 0 && prevSpaces)) {
      ++spaces;
      if (n > prevIndent && prevSpaces) ++deltas[n - prevIndent];
      else if (n > prevIndent && prevIndent == 0) ++deltas[n];
    }
    prevIndent = n;
    prevSpaces = true;
  }
  if (tabs == 0 && spaces == 0) return;
  if (tabs > spaces) {
    d.ed.config.translateTabs = false;
    return;
  }
  d.ed.config.translateTabs = true;
  size_t best = 0, bestCount = 0;
  for (auto [delta, count] : deltas) {
    if (delta < 2 || delta > 8) continue;
    if (count > bestCount) {
      best = delta;
      bestCount = count;
    }
  }
  if (best) d.ed.config.tabSize = static_cast<int>(best);
}

uint64_t App::fileWriteTime(const std::wstring& path) {
  uint64_t t = 0;
  fileStat(path, &t, nullptr);
  return t;
}

Document* App::openFile(const std::wstring& rawPath, bool activateIt) {
  std::wstring path = fullPathOf(rawPath);
  for (size_t i = 0; i < docs_.size(); ++i) {
    if (samePath(docs_[i]->path, path)) {
      if (activateIt) activate(i);
      return docs_[i].get();
    }
  }
  DWORD attrs = GetFileAttributesW(path.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
    openFolder(path, false);
    return nullptr;
  }
  std::string bytes;
  std::wstring err;
  bool exists = attrs != INVALID_FILE_ATTRIBUTES;
  if (exists && !readFileBytes(path, bytes, &err)) {
    MessageBoxW(hwnd_, (L"Unable to open " + path + L"\n\n" + err).c_str(), L"Verso", MB_ICONERROR);
    return nullptr;
  }
  if (exists && isPdfData(bytes)) return openPdf(path, activateIt);
  DecodedText dt = decodeText(bytes, defaultLineEnding());

  Document* target = nullptr;
  if (Document* cur = doc(); cur && cur->path.empty() && !cur->ed.isDirty() && cur->ed.buffer().size() == 0)
    target = cur;
  Document& d = target ? *target : newDocument(false);
  d.path = path;
  d.encoding = dt.encoding;
  d.binary = dt.binary;
  d.eol = dt.lineEnding;
  configureEditor(d);
  std::wstring firstLine(dt.text.substr(0, std::min<size_t>(dt.text.find(L'\n'), 200)));
  d.ed.setText(std::move(dt.text));
  setLanguage(d, detectLanguage(path, firstLine), false);
  detectIndentation(d);
  d.diskWriteTime = fileWriteTime(path);
  d.diskMissing = !exists;
  d.scrollX = d.scrollY = 0;
  d.hasGitBaseline = false;
  d.gitHunks.clear();
  requestGitBaseline(d);
  gitRefresh();
  addRecent(recentFiles_, path);
  sessionDirty_ = true;
  if (activateIt) {
    for (size_t i = 0; i < docs_.size(); ++i)
      if (docs_[i].get() == &d) activate(i);
  }
  invalidate();
  return &d;
}

bool App::saveDocument(Document& d, bool saveAs) {
  if (d.pdf || d.binary) {
    setStatus(d.pdf ? L"PDF files open read-only; there is nothing to save."
                    : L"Binary files open read-only, so saving cannot change their bytes.",
              3000);
    return false;
  }
  std::wstring path = d.path;
  if (path.empty() || saveAs) {
    path = promptSaveFile(d);
    if (path.empty()) return false;
  }
  if (settings_.trimTrailingWhitespaceOnSave) d.ed.trimTrailingWhitespace();
  if (settings_.ensureNewlineAtEofOnSave) d.ed.ensureNewlineAtEof();
  afterEdit(d, false);

  if (!canEncode(d.ed.buffer().text(), d.encoding)) {
    std::wstring q = d.title() + L" contains characters that " + encodingName(d.encoding) +
                     L" cannot store; saving would replace them with \"?\".\n\nSave it as UTF-8 instead?";
    if (MessageBoxW(hwnd_, q.c_str(), L"Verso", MB_YESNO | MB_ICONWARNING) != IDYES) return false;
    d.encoding = Encoding::Utf8;
  }
  std::string bytes = encodeText(d.ed.buffer().text(), d.encoding, d.eol);
  std::wstring err;
  if (!writeFileAtomic(path, bytes, &err)) {
    MessageBoxW(hwnd_, (L"Unable to save " + path + L"\n\n" + err).c_str(), L"Verso", MB_ICONERROR);
    return false;
  }
  bool renamed = !samePath(path, d.path);
  d.path = path;
  d.ed.markSaved();
  d.diskWriteTime = fileWriteTime(path);
  d.diskMissing = false;
  if (renamed && !d.syntaxFromUser) {
    std::wstring first(d.ed.buffer().lineText(0).substr(0, 200));
    setLanguage(d, detectLanguage(path, first), false);
  }
  addRecent(recentFiles_, path);
  setStatus(L"Saved " + path);
  if (samePath(path, dataDir() + kPathSepStr + L"Preferences.json")) loadSettings(true);
  if (samePath(path, dataDir() + kPathSepStr + L"Keymap.json")) {
    loadKeymap(true);
    buildMenu();
  }
  if (renamed && !tree_.empty()) tree_.refresh();
  if (renamed) requestGitBaseline(d);
  gitRefresh();
  updateTitle();
  sessionDirty_ = true;
  invalidate();
  return true;
}

void App::saveAll() {
  for (auto& d : docs_)
    if (d->ed.isDirty() && !d->path.empty()) saveDocument(*d, false);
}

bool App::closeDocument(size_t index, bool promptUser) {
  if (index >= docs_.size()) return false;
  Document& d = *docs_[index];
  if (promptUser && d.ed.isDirty()) {
    activate(index);
    std::wstring q = L"Save changes to " + d.title() + L" before closing?";
    int r = MessageBoxW(hwnd_, q.c_str(), L"Verso", MB_YESNOCANCEL | MB_ICONWARNING);
    if (r == IDCANCEL) return false;
    if (r == IDYES && !saveDocument(d, false)) return false;
  }
  if (overlay_ != Overlay::None) closeOverlay(false);
  if (!d.path.empty()) {
    closedFiles_.push_back(d.path);
    if (closedFiles_.size() > 30) closedFiles_.erase(closedFiles_.begin());
  }
  docs_.erase(docs_.begin() + static_cast<ptrdiff_t>(index));
  if (docs_.empty())
    active_ = 0;
  else if (index < active_)
    --active_;
  else if (active_ >= docs_.size())
    active_ = docs_.size() - 1;
  computeLayout();
  updateTitle();
  resetBlink();
  sessionDirty_ = true;
  invalidate();
  return true;
}

void App::moveDocument(size_t from, size_t to) {
  if (from >= docs_.size() || to >= docs_.size() || from == to) return;
  Document* activeDoc = doc();
  auto moved = std::move(docs_[from]);
  docs_.erase(docs_.begin() + static_cast<ptrdiff_t>(from));
  docs_.insert(docs_.begin() + static_cast<ptrdiff_t>(to), std::move(moved));
  for (size_t i = 0; i < docs_.size(); ++i)
    if (docs_[i].get() == activeDoc) active_ = i;
  sessionDirty_ = true;
  invalidate();
}

Document* App::openFileInPlace(const std::wstring& rawPath) {
  std::wstring path = fullPathOf(rawPath);
  for (size_t i = 0; i < docs_.size(); ++i) {
    if (samePath(docs_[i]->path, path)) {
      activate(i);
      return docs_[i].get();
    }
  }
  Document* cur = doc();
  if (!cur || cur->ed.isDirty() || (cur->path.empty() && cur->ed.buffer().size() > 0)) {
    if (cur && cur->ed.isDirty())
      setStatus(cur->title() + L" has unsaved changes, so the link opened in a new tab.", 3000);
    return openFile(path, true);
  }
  const uint64_t oldId = cur->id;
  Document* opened = openFile(path, true);
  if (!opened || opened->id == oldId) return opened;  // reused an empty untitled tab
  // The new document takes the old one's place in the tab order.
  auto indexOf = [&](uint64_t id) {
    for (size_t i = 0; i < docs_.size(); ++i)
      if (docs_[i]->id == id) return i;
    return docs_.size();
  };
  size_t oldIndex = indexOf(oldId);
  if (oldIndex < docs_.size()) {
    closeDocument(oldIndex, false);
    moveDocument(indexOf(opened->id), std::min(oldIndex, docs_.size() - 1));
  }
  activate(indexOf(opened->id));
  return opened;
}

void App::closeAll() {
  while (!docs_.empty()) {
    if (!closeDocument(docs_.size() - 1, true)) return;
  }
}

void App::activate(size_t index) {
  if (index >= docs_.size()) return;
  if (overlay_ != Overlay::None && index != active_ && overlay_ != Overlay::Files) closeOverlay(false);
  active_ = index;
  computeLayout();
  updateTitle();
  resetBlink();
  sessionDirty_ = true;
  invalidate();
}

void App::reloadFromDisk(Document& d) {
  if (d.pdf) {  // reopen, keeping the reader's position and zoom
    auto fresh = std::make_unique<PdfView>(d.path, hwnd_, WM_APP_PDF_READY);
    fresh->scrollX = d.pdf->scrollX;
    fresh->scrollY = d.pdf->scrollY;
    fresh->zoom = d.pdf->zoom;
    d.pdf = std::move(fresh);
    d.diskWriteTime = fileWriteTime(d.path);
    invalidate();
    return;
  }
  std::string bytes;
  std::wstring err;
  if (!readFileBytes(d.path, bytes, &err)) {
    setStatus(L"Unable to reload: " + err);
    return;
  }
  DecodedText dt = decodeText(bytes, d.eol);
  d.encoding = dt.encoding;
  d.binary = dt.binary;
  if (dt.sawLineEnding) d.eol = dt.lineEnding;
  if (dt.text != d.ed.buffer().text()) d.ed.replaceAllText(std::move(dt.text));
  d.ed.markSaved();
  d.diskWriteTime = fileWriteTime(d.path);
  d.diskMissing = false;
  afterEdit(d, false);
}

void App::checkFilesOnDisk() {
  if (checkingFiles_) return;
  checkingFiles_ = true;
  for (size_t i = 0; i < docs_.size(); ++i) {
    Document& d = *docs_[i];
    if (d.path.empty()) continue;
    uint64_t t = fileWriteTime(d.path);
    if (t == 0) {
      if (!d.diskMissing && d.diskWriteTime != 0) {
        d.diskMissing = true;
        if (!d.pdf) d.ed.markUnsaved();
        setStatus(fileNameOf(d.path) + L" was deleted on disk.");
      }
      continue;
    }
    d.diskMissing = false;
    if (t == d.diskWriteTime) continue;
    d.diskWriteTime = t;
    if (!d.ed.isDirty()) {
      reloadFromDisk(d);
      continue;
    }
    activate(i);
    std::wstring q = d.path + L"\n\nThis file has been modified on disk. Reload it and lose your unsaved changes?";
    if (MessageBoxW(hwnd_, q.c_str(), L"Verso", MB_YESNO | MB_ICONQUESTION) == IDYES) reloadFromDisk(d);
  }
  checkingFiles_ = false;
  updateTitle();
  invalidate();
}

void App::openFolder(const std::wstring& folder, bool replace) {
  std::wstring f = fullPathOf(folder);
  tree_.rules = {settings_.folderExcludePatterns, settings_.fileExcludePatterns};
  if (replace)
    tree_.setFolders({f});
  else
    tree_.addFolder(f);
  sidebarVisible_ = true;
  addRecent(recentFolders_, f);
  sidebarScroll_ = 0;
  rebuildIndex();
  gitRefresh();
  if (terminalMode_) selectTerminal();
  computeLayout();
  sessionDirty_ = true;
  invalidate();
}

void App::rebuildIndex() {
  indexStarted_ = true;
  index_.start(tree_.folders(), tree_.rules, hwnd_, WM_APP_INDEX_READY);
}

void App::addRecent(std::vector<std::wstring>& list, const std::wstring& item) {
  list.erase(std::remove_if(list.begin(), list.end(), [&](const std::wstring& s) { return samePath(s, item); }),
             list.end());
  list.insert(list.begin(), item);
  if (list.size() > 20) list.resize(20);
}

#ifdef _WIN32  // Linux: linux/AppLinux.cpp (zenity dialogs, ~/.config/verso)
std::vector<std::wstring> App::promptOpenFiles() {
  std::vector<std::wstring> out;
  ComPtr<IFileOpenDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
  if (Document* d = doc(); d && !d->path.empty()) {
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(SHCreateItemFromParsingName(parentOf(d->path).c_str(), nullptr, IID_PPV_ARGS(&folder))))
      dlg->SetFolder(folder.Get());
  }
  if (FAILED(dlg->Show(hwnd_))) return out;
  ComPtr<IShellItemArray> items;
  if (FAILED(dlg->GetResults(&items))) return out;
  DWORD count = 0;
  items->GetCount(&count);
  for (DWORD i = 0; i < count; ++i) {
    ComPtr<IShellItem> item;
    PWSTR p = nullptr;
    if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
      out.emplace_back(p);
      CoTaskMemFree(p);
    }
  }
  return out;
}

std::wstring App::promptSaveFile(const Document& d) {
  ComPtr<IFileSaveDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
  COMDLG_FILTERSPEC filter[] = {{L"All Files", L"*.*"}};
  dlg->SetFileTypes(1, filter);
  if (!d.path.empty()) {
    dlg->SetFileName(fileNameOf(d.path).c_str());
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(SHCreateItemFromParsingName(parentOf(d.path).c_str(), nullptr, IID_PPV_ARGS(&folder))))
      dlg->SetFolder(folder.Get());
  } else {
    std::wstring name = d.title() == L"untitled" ? L"untitled" : d.title();
    for (auto& c : name)
      if (std::wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    if (!d.lang->extensions.empty() && d.lang->mode != LexMode::Plain) name += L"." + d.lang->extensions.front();
    else name += L".txt";
    dlg->SetFileName(name.c_str());
    if (!tree_.empty()) {
      ComPtr<IShellItem> folder;
      if (SUCCEEDED(SHCreateItemFromParsingName(tree_.folders().front().c_str(), nullptr, IID_PPV_ARGS(&folder))))
        dlg->SetFolder(folder.Get());
    }
  }
  if (FAILED(dlg->Show(hwnd_))) return {};
  ComPtr<IShellItem> item;
  PWSTR p = nullptr;
  if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) return {};
  std::wstring out(p);
  CoTaskMemFree(p);
  return out;
}

std::wstring App::promptFolder() {
  ComPtr<IFileOpenDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  if (FAILED(dlg->Show(hwnd_))) return {};
  ComPtr<IShellItem> item;
  PWSTR p = nullptr;
  if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) return {};
  std::wstring out(p);
  CoTaskMemFree(p);
  return out;
}

std::wstring App::dataDir() {
  static std::wstring dir;
  if (!dir.empty()) return dir;
  PWSTR p = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) {
    std::wstring appData(p);
    CoTaskMemFree(p);
    dir = appData + L"\\Verso";
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
      // First run after the rename from TestText: copy its settings, key bindings and
      // session. The old folder stays as it was.
      CreateDirectoryW(dir.c_str(), nullptr);
      std::wstring old = appData + L"\\TestText";
      WIN32_FIND_DATAW fd;
      HANDLE h = FindFirstFileW((old + L"\\*").c_str(), &fd);
      if (h != INVALID_HANDLE_VALUE) {
        do {
          if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            CopyFileW((old + L"\\" + fd.cFileName).c_str(), (dir + L"\\" + fd.cFileName).c_str(), TRUE);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
      }
    }
  } else {
    dir = L".\\VersoData";
  }
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}
#endif  // _WIN32

void App::loadSettings(bool showErrors) {
  Settings s;
  std::wstring err;
  bool ok = s.load(dataDir() + kPathSepStr + L"Preferences.json", &err);
  if (!ok && showErrors) MessageBoxW(hwnd_, err.c_str(), L"Verso", MB_ICONWARNING);
  settings_ = s;
  tree_.rules = {settings_.folderExcludePatterns, settings_.fileExcludePatterns};
  preview_.images().setRemoteAllowed(settings_.markdownRemoteImages);
  for (auto& d : docs_) {
    int tab = d->ed.config.tabSize;
    bool spaces = d->ed.config.translateTabs;
    configureEditor(*d);
    if (settings_.detectIndentation) {
      d->ed.config.tabSize = tab;
      d->ed.config.translateTabs = spaces;
    }
  }
  if (hwnd_ && gfx_.editorFormat(FontStyle::Regular)) {
    applyFont();
    applyTheme();
    computeLayout();
  }
  invalidate();
}

void App::loadKeymap(bool showErrors) {
  keymap_.loadDefaults();
  std::wstring err;
  if (!keymap_.loadUserFile(dataDir() + kPathSepStr + L"Keymap.json", &err) && showErrors)
    MessageBoxW(hwnd_, err.c_str(), L"Verso", MB_ICONWARNING);
}

void App::applyTheme() {
  theme_ = settings_.theme == L"light" ? Theme::makeLight() : Theme::makeDark();
  if (hwnd_) applyDarkTitleBar();
  invalidate();
}

void App::applyFont() {
  float size = fontSizeOverride_ > 0 ? fontSizeOverride_ : settings_.fontSize;
  gfx_.setEditorFont(settings_.fontFace, size, settings_.linePadding);
  computeLayout();
  invalidate();
}

void App::openConfigFile(const std::wstring& path, const std::string& defaults) {
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::wstring err;
    std::string bytes = defaults;
    // Store with CRLF like other Windows text files.
    std::string crlf;
    for (char c : bytes) {
      if (c == '\n') crlf += '\r';
      crlf += c;
    }
    writeFileAtomic(path, crlf, &err);
  }
  openFile(path);
}

void App::openScratch(const std::wstring& content, const Language* lang) {
  Document& d = newDocument(true);
  d.ed.setText(content);
  if (lang) setLanguage(d, *lang, true);
  afterEdit(d, false);
}

void App::sidebarContextMenu(TreeNode* node, POINT screen) {
  HMENU m = CreatePopupMenu();
  bool dir = node->dir;
  bool root = node->parent == nullptr;
  AppendMenuW(m, MF_STRING, 1, L"New File...");
  AppendMenuW(m, MF_STRING, 2, L"New Folder...");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  if (!root) {
    AppendMenuW(m, MF_STRING, 3, L"Rename...");
    AppendMenuW(m, MF_STRING, 4, L"Delete");
  }
  AppendMenuW(m, MF_STRING, 5, L"Copy Path");
  AppendMenuW(m, MF_STRING, 6, L"Open Containing Folder...");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, 7, L"Refresh Folders");
  if (root) AppendMenuW(m, MF_STRING, 8, L"Remove Folder from Project");
  if (git_.repoFor(node->path)) {
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 9, L"Open Git Repository...");
    if (!dir) {
      AppendMenuW(m, MF_STRING, 10, L"File History");
      AppendMenuW(m, MF_STRING, 11, L"Blame File");
    }
  }
  UINT cmd = trackMenu(m, screen);
  DestroyMenu(m);
  std::wstring base = dir ? node->path : parentOf(node->path);
  std::wstring path = node->path;
  switch (cmd) {
    case 1:
      prompt(L"File Name:", L"", [this, base](const std::wstring& name) {
        if (trim(name).empty()) return;
        std::wstring p = base + kPathSepStr + std::wstring(trim(name));
        std::wstring err;
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) writeFileAtomic(p, "", &err);
        tree_.refresh();
        openFile(p);
      });
      break;
    case 2:
      prompt(L"Folder Name:", L"", [this, base](const std::wstring& name) {
        if (trim(name).empty()) return;
        CreateDirectoryW((base + kPathSepStr + std::wstring(trim(name))).c_str(), nullptr);
        tree_.refresh();
        invalidate();
      });
      break;
    case 3:
      prompt(L"New Name:", fileNameOf(path), [this, path](const std::wstring& name) {
        if (trim(name).empty()) return;
        std::wstring target = parentOf(path) + kPathSepStr + std::wstring(trim(name));
        if (!MoveFileExW(path.c_str(), target.c_str(), 0)) {
          setStatus(L"Rename failed: " + lastErrorMessage(GetLastError()));
          return;
        }
        for (auto& d : docs_) {
          if (samePath(d->path, path)) {
            d->path = target;
            d->diskWriteTime = fileWriteTime(target);
          } else if (d->path.size() > path.size() && _wcsnicmp(d->path.c_str(), path.c_str(), path.size()) == 0 &&
                     d->path[path.size()] == kPathSep) {
            d->path = target + d->path.substr(path.size());
          }
        }
        tree_.refresh();
        updateTitle();
        sessionDirty_ = true;
        invalidate();
      });
      break;
    case 4: {
#ifdef _WIN32
      std::wstring q = L"Move " + path + L" to the Recycle Bin?";
#else
      std::wstring q = L"Move " + path + L" to the Trash?";
#endif
      if (MessageBoxW(hwnd_, q.c_str(), L"Verso", MB_OKCANCEL | MB_ICONWARNING) != IDOK) break;
      std::wstring from = path + L'\0';
      SHFILEOPSTRUCTW op{};
      op.hwnd = hwnd_;
      op.wFunc = FO_DELETE;
      op.pFrom = from.c_str();
      op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
      if (SHFileOperationW(&op) == 0) {
        tree_.refresh();
        checkFilesOnDisk();
      }
      break;
    }
    case 5:
      setClipboardText(hwnd_, path);
      break;
    case 6: {
      std::wstring arg = L"/select,\"" + path + L"\"";
      ShellExecuteW(hwnd_, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
      break;
    }
    case 7:
      tree_.refresh();
      rebuildIndex();
      break;
    case 9:
      if (const GitRepoState* r = git_.repoFor(path)) setGitMode(true, r->root);
      break;
    case 10:
    case 11:
      gitFileTool(cmd == 11, path);
      break;
    case 8:
      tree_.removeFolder(path);
      rebuildIndex();
      if (terminalMode_) selectTerminal();
      computeLayout();
      sessionDirty_ = true;
      break;
    default:
      break;
  }
  invalidate();
}

// ---------------------------------------------------------------------------
// Session (hot exit)

void App::saveSession() {
  if (!ownsSession_) return;  // a second window (-n) leaves hot exit to the first
  json::Value root = json::Value::makeObject();
  root.set("version", 1);
  json::Value folders = json::Value::makeArray();
  for (const auto& f : tree_.folders()) folders.push(json::Value(std::wstring_view(f)));
  root.set("folders", folders);
  root.set("sidebar_visible", sidebarVisible_);
  root.set("sidebar_width", static_cast<double>(sidebarWidth_));
  root.set("minimap_visible", minimapVisible_);
  root.set("font_size", static_cast<double>(fontSizeOverride_));
  root.set("active", static_cast<int>(active_));
  root.set("git_mode", gitMode_);
  root.set("terminal_mode", terminalMode_);
  root.set("markdown_mode", mdMode_);
  root.set("markdown_view", static_cast<int>(mdView_));
  root.set("word_wrap", settings_.wordWrap);

#ifdef _WIN32  // on Linux (Wayland) the compositor places windows
  WINDOWPLACEMENT wp{sizeof(wp)};
  if (fullscreen_)
    wp = savedPlacement_;
  else
    GetWindowPlacement(hwnd_, &wp);
  json::Value win = json::Value::makeObject();
  win.set("x", static_cast<int>(wp.rcNormalPosition.left));
  win.set("y", static_cast<int>(wp.rcNormalPosition.top));
  win.set("w", static_cast<int>(wp.rcNormalPosition.right - wp.rcNormalPosition.left));
  win.set("h", static_cast<int>(wp.rcNormalPosition.bottom - wp.rcNormalPosition.top));
  win.set("maximized", wp.showCmd == SW_SHOWMAXIMIZED || (wp.flags & WPF_RESTORETOMAXIMIZED) != 0);
  root.set("window", win);
#endif

  auto list = [](const std::vector<std::wstring>& v) {
    json::Value a = json::Value::makeArray();
    for (const auto& s : v) a.push(json::Value(std::wstring_view(s)));
    return a;
  };
  root.set("recent_files", list(recentFiles_));
  root.set("recent_folders", list(recentFolders_));
  root.set("closed_files", list(closedFiles_));

  json::Value docs = json::Value::makeArray();
  for (const auto& dp : docs_) {
    const Document& d = *dp;
    bool dirty = d.ed.isDirty();
    if (d.path.empty() && d.ed.buffer().size() == 0) continue;
    if (d.path.empty() && !settings_.hotExit) continue;
    json::Value o = json::Value::makeObject();
    if (!d.path.empty()) o.set("path", json::Value(std::wstring_view(d.path)));
    if (settings_.hotExit && !d.pdf && !d.binary && (dirty || d.path.empty())) {
      o.set("content", json::Value(std::wstring_view(d.ed.buffer().text())));
      o.set("dirty", dirty);
    }
    o.set("encoding", encodingId(d.encoding));
    o.set("eol", lineEndingId(d.eol));
    // The file's time when last loaded or saved, so a restored unsaved buffer still
    // notices changes made on disk while Verso was closed. A string keeps all 64 bits.
    if (!d.path.empty()) o.set("disk_time", std::to_string(d.diskWriteTime));
    if (d.syntaxFromUser) o.set("syntax", json::Value(std::wstring_view(d.lang->name)));
    if (d.findResults) o.set("find_results", true);
    const Selection& s = d.ed.selections().primary();
    o.set("anchor", static_cast<double>(s.anchor));
    o.set("caret", static_cast<double>(s.caret));
    o.set("scroll_x", static_cast<double>(d.scrollX));
    o.set("scroll_y", static_cast<double>(d.scrollY));
    o.set("tab_size", d.ed.config.tabSize);
    o.set("translate_tabs", d.ed.config.translateTabs);
    docs.push(std::move(o));
  }
  root.set("docs", docs);

  std::wstring err;
  writeFileAtomic(dataDir() + kPathSepStr + L"Session.json", json::serialize(root, 1), &err);
  sessionDirty_ = false;
}

bool App::restoreSession() {
  if (!ownsSession_) return false;
  std::string bytes;
  if (!readFileBytes(dataDir() + kPathSepStr + L"Session.json", bytes, nullptr)) return false;
  auto v = json::parse(bytes);
  if (!v || !v->isObject()) return false;

  auto readList = [&](const char* key, std::vector<std::wstring>& out) {
    if (const auto* a = v->get(key); a && a->isArray())
      for (const auto& x : a->array) out.push_back(x.asWString());
  };
  readList("recent_files", recentFiles_);
  readList("recent_folders", recentFolders_);
  readList("closed_files", closedFiles_);
  sidebarVisible_ = v->getBool("sidebar_visible", false);
  sidebarWidth_ = static_cast<float>(v->getNumber("sidebar_width", 240));
  minimapVisible_ = v->getBool("minimap_visible", true);
  fontSizeOverride_ = static_cast<float>(v->getNumber("font_size", 0));
  startInGitMode_ = v->getBool("git_mode", false);
  startInTerminalMode_ = v->getBool("terminal_mode", false);
  mdMode_ = v->getBool("markdown_mode", false);
  mdView_ = static_cast<MdView>(std::clamp(static_cast<int>(v->getNumber("markdown_view", 1)), 0, 2));
  settings_.wordWrap = v->getBool("word_wrap", settings_.wordWrap);
  if (fontSizeOverride_ > 0) applyFont();

  if (const auto* f = v->get("folders"); f && f->isArray()) {
    std::vector<std::wstring> folders;
    for (const auto& x : f->array) {
      std::wstring p = x.asWString();
      if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) folders.push_back(p);
    }
    tree_.rules = {settings_.folderExcludePatterns, settings_.fileExcludePatterns};
    tree_.setFolders(folders);
    if (!folders.empty()) rebuildIndex();
  }

  const size_t savedActive = static_cast<size_t>(v->getNumber("active", 0));
  size_t savedEntry = 0;
  const Document* activeDoc = nullptr;
  if (const auto* ds = v->get("docs"); ds && ds->isArray()) {
    for (const auto& o : ds->array) {
      const size_t entry = savedEntry++;
      active_ = docs_.empty() ? 0 : docs_.size() - 1;  // append in saved order
      std::wstring path = o.getWString("path");
      const json::Value* content = o.get("content");
      Document* d = nullptr;
      if (content && content->type == json::Value::Type::String) {
        Document& nd = newDocument(false);
        nd.path = path;
        nd.ed.setText(content->asWString());
        nd.encoding = encodingFromId(o.get("encoding") ? o.get("encoding")->asString() : "utf-8");
        nd.eol = lineEndingFromId(o.get("eol") ? o.get("eol")->asString() : "windows");
        std::wstring first(nd.ed.buffer().lineText(0).substr(0, 200));
        setLanguage(nd, path.empty() ? detectLanguage(L"", first) : detectLanguage(path, first), false);
        if (!path.empty()) {
          nd.diskWriteTime = fileWriteTime(path);
          if (const auto* t = o.get("disk_time"); t && t->type == json::Value::Type::String)
            nd.diskWriteTime = std::wcstoull(t->asWString().c_str(), nullptr, 10);
        }
        if (o.getBool("dirty", false)) nd.ed.markUnsaved();
        d = &nd;
      } else if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        d = openFile(path, false);
        if (d) {
          d->eol = lineEndingFromId(o.get("eol") ? o.get("eol")->asString() : lineEndingId(d->eol));
        }
      }
      if (!d) continue;
      if (entry == savedActive) activeDoc = d;
      d->findResults = o.getBool("find_results", false);
      if (d->findResults) d->ed.markSaved();
      if (const auto* syn = o.get("syntax")) {
        if (const Language* l = languageByName(syn->asWString())) setLanguage(*d, *l, true);
      }
      d->ed.config.tabSize = static_cast<int>(o.getNumber("tab_size", d->ed.config.tabSize));
      d->ed.config.translateTabs = o.getBool("translate_tabs", d->ed.config.translateTabs);
      size_t size = d->ed.buffer().size();
      size_t anchor = std::min(static_cast<size_t>(o.getNumber("anchor", 0)), size);
      size_t caret = std::min(static_cast<size_t>(o.getNumber("caret", 0)), size);
      d->ed.setSelections({Selection(anchor, caret)}, 0);
      d->scrollX = static_cast<float>(o.getNumber("scroll_x", 0));
      d->scrollY = static_cast<float>(o.getNumber("scroll_y", 0));
    }
  }
  // Saved entries that could not be restored shift the indexes, so find the document itself.
  active_ = docs_.empty() ? 0 : docs_.size() - 1;
  for (size_t i = 0; i < docs_.size(); ++i)
    if (docs_[i].get() == activeDoc) active_ = i;

#ifndef _WIN32
  computeLayout();
  return true;  // Wayland: the compositor sizes and places the window
#else
  if (const auto* w = v->get("window"); w && w->isObject()) {
    WINDOWPLACEMENT wp{sizeof(wp)};
    wp.rcNormalPosition.left = static_cast<LONG>(w->getNumber("x", 100));
    wp.rcNormalPosition.top = static_cast<LONG>(w->getNumber("y", 100));
    wp.rcNormalPosition.right = wp.rcNormalPosition.left + static_cast<LONG>(w->getNumber("w", 1280));
    wp.rcNormalPosition.bottom = wp.rcNormalPosition.top + static_cast<LONG>(w->getNumber("h", 820));
    wp.showCmd = w->getBool("maximized", false) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    // Keep the window reachable if the monitor layout changed.
    RECT r = wp.rcNormalPosition;
    if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL) == nullptr) {
      wp.rcNormalPosition = {100, 100, 1380, 920};
    }
    SetWindowPlacement(hwnd_, &wp);
    computeLayout();
    return true;
  }
  return false;
#endif
}

}  // namespace verso
