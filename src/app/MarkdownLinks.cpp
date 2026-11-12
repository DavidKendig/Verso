#include "app/MarkdownLinks.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <functional>
#include <unordered_map>

#include "app/FileTree.h"
#include "core/TextUtil.h"

namespace verso {

namespace {

constexpr size_t kMaxTreeFiles = 50000;  // bounded walk of a notes collection
constexpr int kMaxTreeDepth = 12;
constexpr ULONGLONG kTreeTtlMs = 30000;  // how long a collection's file list is reused while drawing

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool isFile(const std::wstring& path) {
  DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// How close `path` is to `dir`: the length of their common folder prefix.
size_t closeness(const std::wstring& path, const std::wstring& dir) {
  std::wstring p = toLower(path), d = toLower(dir);
  size_t n = 0, lastSep = 0;
  while (n < p.size() && n < d.size() && p[n] == d[n]) {
    if (p[n] == kPathSep) lastSep = n;
    ++n;
  }
  return n == d.size() && n < p.size() && p[n] == kPathSep ? n : lastSep;
}

// Whether `path` ends with the relative reference `rel` at a folder boundary.
bool endsWithReference(const std::wstring& path, const std::wstring& rel) {
  if (!endsWithNoCase(path, rel)) return false;
  return path.size() == rel.size() || path[path.size() - rel.size() - 1] == kPathSep;
}

std::wstring leafOf(const std::wstring& path) {
  size_t slash = path.find_last_of(kPathSep);
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// The files of a notes collection, by lowercase file name.
struct NoteTree {
  std::wstring root;
  ULONGLONG builtAt = 0;
  std::vector<std::wstring> files;
  std::unordered_map<std::wstring, std::vector<size_t>> byName;
};

std::unordered_map<std::wstring, NoteTree>& treeCache() {
  static std::unordered_map<std::wstring, NoteTree> cache;  // UI thread only
  return cache;
}

const NoteTree& noteTree(const std::wstring& root) {
  NoteTree& t = treeCache()[toLower(root)];
  ULONGLONG now = GetTickCount64();
  if (t.builtAt != 0 && now - t.builtAt < kTreeTtlMs) return t;
  t = NoteTree{};
  t.root = root;
  std::function<void(const std::wstring&, int)> walk = [&](const std::wstring& dir, int depth) {
    if (depth > kMaxTreeDepth || t.files.size() >= kMaxTreeFiles) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + kPathSepStr + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
      std::wstring name = fd.cFileName;
      if (name == L"." || name == L"..") continue;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (name[0] == L'.' || name == L"node_modules" || (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
        walk(dir + kPathSepStr + name, depth + 1);
      } else {
        t.byName[toLower(name)].push_back(t.files.size());
        t.files.push_back(dir + kPathSepStr + name);
      }
    } while (t.files.size() < kMaxTreeFiles && FindNextFileW(h, &fd));
    FindClose(h);
  };
  walk(root, 0);
  t.builtAt = GetTickCount64();
  return t;
}

}  // namespace

std::string decodePercentBytes(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    int hi, lo;
    if (s[i] == '%' && i + 2 < s.size() && (hi = hexValue(s[i + 1])) >= 0 && (lo = hexValue(s[i + 2])) >= 0) {
      out += static_cast<char>(hi * 16 + lo);
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::wstring decodeUrlComponent(std::wstring_view s) { return utf8ToWide(decodePercentBytes(wideToUtf8(s))); }

std::wstring markdownRoot(const std::wstring& start) {
  std::wstring dir = start;
  while (!dir.empty()) {
    if (GetFileAttributesW((dir + kPathSepStr + L".obsidian").c_str()) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW((dir + kPathSepStr + L".git").c_str()) != INVALID_FILE_ATTRIBUTES)
      return dir;
    size_t slash = dir.find_last_of(kPathSep);
#ifdef _WIN32
    if (slash == std::wstring::npos || slash < 2) break;  // stop at the drive ("C:")
#else
    if (slash == std::wstring::npos || slash < 1) break;  // stop below "/"
#endif
    dir.resize(slash);
  }
  return {};
}

bool resolveMarkdownPath(const std::wstring& raw, const std::wstring& baseDir, std::wstring& out,
                         std::wstring& error) {
  std::wstring src(trim(raw));
  std::wstring path;
  bool fileUrl = false;
  if (startsWithNoCase(src, L"file:")) {
    fileUrl = true;
    std::wstring rest = src.substr(5);
#ifdef _WIN32
    if (rest.rfind(L"///", 0) == 0)
      path = rest.substr(3);
    else if (rest.rfind(L"//", 0) == 0)
      path = L"\\\\" + rest.substr(2);  // file://server/share
    else
      path = rest;
#else
    if (rest.rfind(L"//", 0) == 0) {  // file:///path, or file://host/path
      size_t slash = rest.find(L'/', 2);
      path = slash == std::wstring::npos ? L"/" : rest.substr(slash);
    } else {
      path = rest;
    }
#endif
  } else {
    size_t colon = src.find(L':');
    if (colon != std::wstring::npos && colon > 1 &&
        std::all_of(src.begin(), src.begin() + static_cast<ptrdiff_t>(colon), [](wchar_t c) { return std::iswalpha(c); })) {
      error = L"Unsupported link: " + src;
      return false;
    }
    path = src.substr(0, src.find(L'?'));
  }
  path = decodeUrlComponent(path);
  std::replace(path.begin(), path.end(), L'/', kPathSep);
#ifdef _WIN32
  (void)fileUrl;
  bool absolute = isAbsolutePath(path);
#else
  // A leading "/" in a Markdown link means the collection root (as on GitHub); only
  // file: URLs name absolute paths.
  bool absolute = fileUrl;
#endif
  if (!absolute) {
    if (baseDir.empty()) {
      error = L"Save the document to follow relative links and show relative images.";
      return false;
    }
    if (!path.empty() && path[0] == kPathSep) {  // "/docs/a.md": relative to the collection root, as on GitHub
      std::wstring root = markdownRoot(baseDir);
#ifdef _WIN32
      path = (root.empty() ? baseDir.substr(0, 2) : root) + path;
#else
      path = (root.empty() ? baseDir : root) + path;
#endif
    } else {
      path = baseDir + kPathSepStr + path;
    }
  }
  out = fullPathOf(path);
  return true;
}

WikiTarget parseWikiTarget(std::wstring_view target) {
  WikiTarget t;
  size_t hash = target.find(L'#');
  t.page = std::wstring(trim(target.substr(0, hash)));
  if (hash != std::wstring_view::npos) t.heading = std::wstring(trim(target.substr(hash + 1)));
  return t;
}

WikiResolution resolveWikiLink(const std::wstring& rawPage, const std::wstring& baseDir, const std::wstring& root) {
  WikiResolution r;
  std::wstring page(trim(rawPage));
  std::replace(page.begin(), page.end(), L'/', kPathSep);
  while (!page.empty() && page[0] == kPathSep) page.erase(0, 1);
  if (page.empty()) {
    r.error = L"The link names no page.";
    return r;
  }
  // "Page" means "Page.md"; a name that already has an extension may also be meant as written.
  bool isMd = endsWithNoCase(page, L".md");
  std::vector<std::wstring> names = isMd ? std::vector<std::wstring>{page}
                                         : std::vector<std::wstring>{page + L".md", page};
  auto found = [&](const std::wstring& p) {
    r.path = fullPathOf(p);
    r.exists = true;
    return r;
  };
  for (const auto& n : names) {
    if (!baseDir.empty() && isFile(baseDir + kPathSepStr + n)) return found(baseDir + kPathSepStr + n);
    if (!root.empty() && isFile(root + kPathSepStr + n)) return found(root + kPathSepStr + n);
  }
  // Anywhere in the collection: the match nearest the document wins.
  if (!root.empty()) {
    const NoteTree& tree = noteTree(root);
    for (const auto& n : names) {
      auto it = tree.byName.find(toLower(leafOf(n)));
      if (it == tree.byName.end()) continue;
      const std::wstring* pick = nullptr;
      size_t pickScore = 0;
      for (size_t i : it->second) {
        const std::wstring& c = tree.files[i];
        if (!endsWithReference(c, n)) continue;
        size_t score = closeness(c, baseDir);
        if (!pick || score > pickScore || (score == pickScore && c.size() < pick->size())) {
          pick = &c;
          pickScore = score;
        }
      }
      if (pick) return found(*pick);
    }
  }
  if (baseDir.empty()) {
    r.error = L"Save this document first, so the new note has a folder.";
    return r;
  }
  r.path = fullPathOf(baseDir + kPathSepStr + (isMd ? page : page + L".md"));
  return r;
}

WikiResolution resolveNoteLink(const std::wstring& ref, const std::wstring& baseDir, const std::wstring& root) {
  WikiResolution r;
  if (!resolveMarkdownPath(ref, baseDir, r.path, r.error)) return r;
  if (GetFileAttributesW(r.path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    r.exists = true;  // a file or a folder
    return r;
  }
  // Note apps write the shortest unique name, so look the name up across the collection.
  std::wstring trimmed(trim(ref));
  std::wstring name = decodeUrlComponent(trimmed.substr(0, trimmed.find(L'?')));
  std::replace(name.begin(), name.end(), L'/', kPathSep);
  name = leafOf(name);
  if (!name.empty() && !root.empty()) {
    WikiResolution w = resolveWikiLink(name, baseDir, root);
    if (w.exists && _wcsicmp(leafOf(w.path).c_str(), name.c_str()) == 0) return w;
  }
  return r;
}

void invalidateNoteTrees() { treeCache().clear(); }

}  // namespace verso
