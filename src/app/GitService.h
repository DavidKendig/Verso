#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace verso {

enum class GitBadge { None, Modified, Staged, Added, Renamed, Deleted, Untracked, Conflict, Ignored };

struct GitRepoState {
  std::wstring root;       // working tree root, Windows separators
  std::wstring branch;     // branch name, or "detached at <hash>"
  std::wstring operation;  // "", "MERGING", "REBASING", ...
  int changed = 0, staged = 0, untracked = 0, conflicts = 0;
  std::unordered_map<std::wstring, GitBadge> files;  // lowercase absolute path -> badge
  std::unordered_set<std::wstring> changedDirs;      // lowercase folders containing changes
  std::vector<std::wstring> ignoredDirs;             // lowercase, with trailing backslash
};

// Index ("staged") contents of a file, used as the base for gutter change markers.
struct GitBaseline {
  uint64_t docId = 0;
  std::wstring path;
  bool tracked = false;  // false: not in a repository or not in the index
  std::wstring text;     // '\n' separated
};

// Runs git on a background thread through the shared git layer (verso_git) and
// posts `msg` to `notify` whenever new results are ready.
class GitService {
 public:
  ~GitService();
  void start(HWND notify, UINT msg);
  void stop();
  bool available() const { return !exe_.empty(); }

  void refresh(std::vector<std::wstring> paths);  // folders or files; their repositories are scanned
  void requestBaseline(uint64_t docId, const std::wstring& path);

  // Main thread: picks up finished work. Returns true when anything changed.
  bool collect(std::vector<GitBaseline>& baselines);

  const GitRepoState* repoFor(const std::wstring& path) const;
  GitBadge badgeFor(const std::wstring& path, bool isDir) const;

  static std::wstring key(const std::wstring& path);

 private:
  struct Job {
    bool baseline = false;
    uint64_t docId = 0;
    std::vector<std::wstring> paths;
  };
  void run();
  GitBaseline loadBaseline(uint64_t docId, const std::wstring& path);

  std::wstring exe_;
  HWND notify_ = nullptr;
  UINT msg_ = 0;
  std::thread worker_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  bool quit_ = false;

  // Results handed from the worker to the main thread.
  std::optional<std::vector<std::shared_ptr<GitRepoState>>> pendingRepos_;
  std::vector<GitBaseline> pendingBaselines_;

  // Main-thread view.
  std::vector<std::shared_ptr<GitRepoState>> repos_;
};

}  // namespace verso
