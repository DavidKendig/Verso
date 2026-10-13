#pragma once

#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace verso {

struct ExcludeRules {
  std::vector<std::wstring> folders;
  std::vector<std::wstring> files;
  bool excluded(const std::wstring& name, bool dir) const;
};

struct TreeNode {
  std::wstring name;
  std::wstring path;
  bool dir = false;
  bool expanded = false;
  bool loaded = false;
  TreeNode* parent = nullptr;
  std::vector<std::unique_ptr<TreeNode>> children;
};

struct TreeRow {
  TreeNode* node;
  int depth;
};

// Side bar folder tree; directories are read lazily when expanded.
class FileTree {
 public:
  void setFolders(const std::vector<std::wstring>& folders);
  void addFolder(const std::wstring& folder);
  void removeFolder(const std::wstring& folder);
  std::vector<std::wstring> folders() const;
  bool empty() const { return roots_.empty(); }
  std::vector<TreeRow> rows() const;
  void toggle(TreeNode* n);
  void refresh();
  TreeNode* find(const std::wstring& path) const;
  void reveal(const std::wstring& path);  // expands ancestors of a file
  ExcludeRules rules;

 private:
  void load(TreeNode& n);
  void reload(TreeNode& n);
  std::vector<std::unique_ptr<TreeNode>> roots_;
};

// Background recursive file listing for Goto Anything.
class FileIndex {
 public:
  struct Entry {
    std::wstring path;     // absolute
    std::wstring display;  // relative to its root folder, prefixed with the folder name
  };

  ~FileIndex() { stop(); }
  void start(const std::vector<std::wstring>& roots, const ExcludeRules& rules, HWND notify, UINT msg);
  void stop();
  std::vector<Entry> snapshot() const;
  bool ready() const { return ready_; }

 private:
  mutable std::mutex mu_;
  std::vector<Entry> entries_;
  std::thread worker_;
  std::atomic<bool> cancel_{false};
  std::atomic<bool> ready_{false};
};

std::wstring fileNameOf(const std::wstring& path);
std::wstring parentOf(const std::wstring& path);
// Windows paths compare case-insensitively.
bool samePath(const std::wstring& a, const std::wstring& b);
// The absolute, normalized form of `path` (any length), or `path` when it has none.
std::wstring fullPathOf(const std::wstring& path);

}  // namespace verso
