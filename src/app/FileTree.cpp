#include "app/FileTree.h"

#include <shlwapi.h>

#include <algorithm>
#include <functional>

namespace verso {

std::wstring fileNameOf(const std::wstring& path) {
  size_t s = path.find_last_of(L"\\/");
  return s == std::wstring::npos ? path : path.substr(s + 1);
}

bool samePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

std::wstring fullPathOf(const std::wstring& path) {
  DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (n == 0) return path;
  std::wstring out(n, L'\0');
  n = GetFullPathNameW(path.c_str(), n, out.data(), nullptr);
  if (n == 0 || n >= out.size()) return path;
  out.resize(n);
  return out;
}

std::wstring parentOf(const std::wstring& path) {
  size_t s = path.find_last_of(L"\\/");
  return s == std::wstring::npos ? std::wstring() : path.substr(0, s);
}

bool ExcludeRules::excluded(const std::wstring& name, bool dir) const {
  for (const auto& p : dir ? folders : files)
    if (PathMatchSpecW(name.c_str(), p.c_str())) return true;
  return false;
}

namespace {

struct DirEntry {
  std::wstring name;
  bool dir;
};

std::vector<DirEntry> listDir(const std::wstring& path, const ExcludeRules& rules) {
  std::vector<DirEntry> out;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                              FIND_FIRST_EX_LARGE_FETCH);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (dir && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;  // avoid junction loops
    if (rules.excluded(name, dir)) continue;
    out.push_back({name, dir});
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
    if (a.dir != b.dir) return a.dir;
    return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  return out;
}

std::wstring normalizeFolder(std::wstring f) {
  while (f.size() > 3 && (f.back() == L'\\' || f.back() == L'/')) f.pop_back();
  return f;
}

}  // namespace

void FileTree::setFolders(const std::vector<std::wstring>& folders) {
  roots_.clear();
  for (const auto& f : folders) addFolder(f);
}

void FileTree::addFolder(const std::wstring& folder) {
  std::wstring f = normalizeFolder(folder);
  for (const auto& r : roots_)
    if (_wcsicmp(r->path.c_str(), f.c_str()) == 0) return;
  auto n = std::make_unique<TreeNode>();
  n->name = fileNameOf(f).empty() ? f : fileNameOf(f);
  n->path = f;
  n->dir = true;
  n->expanded = true;
  load(*n);
  roots_.push_back(std::move(n));
}

void FileTree::removeFolder(const std::wstring& folder) {
  roots_.erase(std::remove_if(roots_.begin(), roots_.end(),
                              [&](const auto& r) { return _wcsicmp(r->path.c_str(), folder.c_str()) == 0; }),
               roots_.end());
}

std::vector<std::wstring> FileTree::folders() const {
  std::vector<std::wstring> out;
  for (const auto& r : roots_) out.push_back(r->path);
  return out;
}

void FileTree::load(TreeNode& n) {
  n.children.clear();
  for (const auto& e : listDir(n.path, rules)) {
    auto c = std::make_unique<TreeNode>();
    c->name = e.name;
    c->path = n.path + L"\\" + e.name;
    c->dir = e.dir;
    c->parent = &n;
    n.children.push_back(std::move(c));
  }
  n.loaded = true;
}

void FileTree::reload(TreeNode& n) {
  if (!n.loaded) return;
  std::vector<std::unique_ptr<TreeNode>> old = std::move(n.children);
  load(n);
  for (auto& c : n.children) {
    for (auto& o : old) {
      if (o && o->dir && c->dir && o->name == c->name) {
        c->expanded = o->expanded;
        if (o->loaded) {
          c->children = std::move(o->children);
          for (auto& g : c->children) g->parent = c.get();
          c->loaded = true;
          reload(*c);
        }
        break;
      }
    }
  }
}

void FileTree::refresh() {
  for (auto& r : roots_) reload(*r);
}

std::vector<TreeRow> FileTree::rows() const {
  std::vector<TreeRow> out;
  std::function<void(TreeNode*, int)> walk = [&](TreeNode* n, int depth) {
    out.push_back({n, depth});
    if (n->dir && n->expanded)
      for (auto& c : n->children) walk(c.get(), depth + 1);
  };
  for (const auto& r : roots_) walk(r.get(), 0);
  return out;
}

void FileTree::toggle(TreeNode* n) {
  if (!n || !n->dir) return;
  n->expanded = !n->expanded;
  if (n->expanded && !n->loaded) load(*n);
}

TreeNode* FileTree::find(const std::wstring& path) const {
  std::function<TreeNode*(TreeNode*)> walk = [&](TreeNode* n) -> TreeNode* {
    if (_wcsicmp(n->path.c_str(), path.c_str()) == 0) return n;
    for (auto& c : n->children)
      if (TreeNode* f = walk(c.get())) return f;
    return nullptr;
  };
  for (const auto& r : roots_)
    if (TreeNode* f = walk(r.get())) return f;
  return nullptr;
}

void FileTree::reveal(const std::wstring& path) {
  for (const auto& r : roots_) {
    size_t n = r->path.size();
    if (path.size() <= n || _wcsnicmp(path.c_str(), r->path.c_str(), n) != 0 || path[n] != L'\\') continue;
    TreeNode* cur = r.get();
    cur->expanded = true;
    size_t pos = n + 1;
    while (cur) {
      size_t next = path.find(L'\\', pos);
      if (next == std::wstring::npos) return;
      std::wstring part = path.substr(pos, next - pos);
      TreeNode* child = nullptr;
      if (!cur->loaded) load(*cur);
      for (auto& c : cur->children)
        if (c->dir && _wcsicmp(c->name.c_str(), part.c_str()) == 0) child = c.get();
      if (!child) return;
      child->expanded = true;
      if (!child->loaded) load(*child);
      cur = child;
      pos = next + 1;
    }
  }
}

void FileIndex::start(const std::vector<std::wstring>& roots, const ExcludeRules& rules, HWND notify, UINT msg) {
  stop();
  cancel_ = false;
  ready_ = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    entries_.clear();
  }
  worker_ = std::thread([this, roots, rules, notify, msg] {
    constexpr size_t kMaxFiles = 200000;
    std::vector<Entry> found;
    for (const auto& root : roots) {
      std::wstring rootName = fileNameOf(root);
      std::vector<std::pair<std::wstring, std::wstring>> stack{{root, rootName}};
      while (!stack.empty() && !cancel_ && found.size() < kMaxFiles) {
        auto [dir, rel] = stack.back();
        stack.pop_back();
        auto entries = listDir(dir, rules);
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
          std::wstring full = dir + L"\\" + it->name;
          std::wstring display = rel + L"/" + it->name;
          if (it->dir)
            stack.emplace_back(full, display);
          else
            found.push_back({full, display});
        }
      }
    }
    std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) { return a.display < b.display; });
    {
      std::lock_guard<std::mutex> lock(mu_);
      entries_ = std::move(found);
    }
    ready_ = true;
    if (!cancel_ && notify) PostMessageW(notify, msg, 0, 0);
  });
}

void FileIndex::stop() {
  cancel_ = true;
  if (worker_.joinable()) worker_.join();
  std::lock_guard<std::mutex> lock(mu_);
  entries_.clear();  // a stopped index lists nothing (Close All Folders)
  ready_ = false;
}

std::vector<FileIndex::Entry> FileIndex::snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  return entries_;
}

}  // namespace verso
