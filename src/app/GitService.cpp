#include "app/GitService.h"

#include "app/FileTree.h"

#include <algorithm>
#include <cwctype>

#include "core/Encoding.h"
#include "git.h"
#include "util.h"

namespace verso {

namespace {

bool isDirectory(const std::wstring& p) {
  DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool underRoot(const std::wstring& k, const std::wstring& rootKey) {
  return k == rootKey || (k.size() > rootKey.size() && k.compare(0, rootKey.size(), rootKey) == 0 &&
                          (k[rootKey.size()] == kPathSep || rootKey.back() == kPathSep));
}

}  // namespace

std::wstring GitService::key(const std::wstring& path) {
  std::wstring k = path;
#ifdef _WIN32  // Windows paths are case-insensitive and may use either slash
  for (auto& c : k) c = c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c));
#endif
  while (k.size() > 3 && k.back() == kPathSep) k.pop_back();
  return k;
}

GitService::~GitService() { stop(); }

void GitService::start(HWND notify, UINT msg) {
  notify_ = notify;
  msg_ = msg;
  exe_ = Git::Locate();
  if (exe_.empty() || worker_.joinable()) return;
  quit_ = false;
  worker_ = std::thread([this] { run(); });
}

void GitService::stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    quit_ = true;
    jobs_.clear();
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void GitService::refresh(std::vector<std::wstring> paths) {
  if (!available()) return;
  std::lock_guard<std::mutex> lock(mu_);
  // A queued refresh is replaced: only the latest set of paths matters.
  for (auto& j : jobs_) {
    if (!j.baseline) {
      j.paths = std::move(paths);
      cv_.notify_one();
      return;
    }
  }
  jobs_.push_back({false, 0, std::move(paths)});
  cv_.notify_one();
}

void GitService::requestBaseline(uint64_t docId, const std::wstring& path) {
  if (!available() || path.empty()) return;
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& j : jobs_)
    if (j.baseline && j.docId == docId && j.paths.front() == path) return;
  jobs_.push_back({true, docId, {path}});
  cv_.notify_one();
}

void GitService::run() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [&] { return quit_ || !jobs_.empty(); });
      if (quit_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    if (job.baseline) {
      GitBaseline b = loadBaseline(job.docId, job.paths.front());
      std::lock_guard<std::mutex> lock(mu_);
      pendingBaselines_.push_back(std::move(b));
    } else {
      std::vector<std::shared_ptr<GitRepoState>> result;
      std::unordered_set<std::wstring> seenDirs, seenRoots;
      for (const auto& p : job.paths) {
        std::wstring dir = isDirectory(p) ? p : parentOf(p);
        if (dir.empty() || !seenDirs.insert(key(dir)).second) continue;
        // Skip folders already covered by a scanned repository.
        bool covered = false;
        for (const auto& r : result)
          if (underRoot(key(dir), key(r->root))) covered = true;
        if (covered) continue;
        Git g;
        g.exe = exe_;
        std::wstring err;
        if (!g.Open(dir, err) || !seenRoots.insert(key(g.root)).second) continue;
        GitRepoState st;
        st.root = g.root;
        std::string branch = g.HeadBranch();
        if (!branch.empty()) {
          st.branch = Widen(branch);
        } else {
          std::string head = g.HeadHash();
          st.branch = head.empty() ? L"(no commits yet)" : L"detached at " + Widen(head.substr(0, 7));
        }
        st.operation = g.OperationState();
        ProcResult r =
            g.Run({L"status", L"--porcelain=v1", L"-z", L"--untracked-files=all", L"--ignored=matching"});
        if (r.ok()) {
          const std::wstring rootKey = key(st.root);
          auto parts = Split(r.out, '\0');
          for (size_t i = 0; i < parts.size(); ++i) {
            const std::string& e = parts[i];
            if (e.size() < 4) continue;
            char x = e[0], y = e[1];
            std::string rel = e.substr(3);
            if ((x == 'R' || x == 'C') && i + 1 < parts.size()) ++i;  // skip the original path
            std::wstring full = key(JoinRepoPath(st.root, rel));
            if (x == '!') {
              if (!rel.empty() && rel.back() == '/')
                st.ignoredDirs.push_back(full + kPathSepStr);
              else
                st.files[full] = GitBadge::Ignored;
              continue;
            }
            GitBadge b;
            if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D')) {
              b = GitBadge::Conflict;
              ++st.conflicts;
            } else if (x == '?') {
              b = GitBadge::Untracked;
              ++st.untracked;
            } else {
              if (x != ' ') ++st.staged;
              if (y != ' ') ++st.changed;
              if (y == 'M' || y == 'T') b = GitBadge::Modified;
              else if (y == 'D') b = GitBadge::Deleted;
              else if (x == 'A') b = GitBadge::Added;
              else if (x == 'R' || x == 'C') b = GitBadge::Renamed;
              else if (x == 'D') b = GitBadge::Deleted;
              else b = GitBadge::Staged;
            }
            st.files[full] = b;
            for (std::wstring d = parentOf(full); d.size() >= rootKey.size() && underRoot(d, rootKey);
                 d = parentOf(d)) {
              if (!st.changedDirs.insert(d).second) break;
            }
          }
        }
        result.push_back(std::make_shared<GitRepoState>(std::move(st)));
      }
      std::lock_guard<std::mutex> lock(mu_);
      pendingRepos_ = std::move(result);
    }
    if (notify_) PostMessageW(notify_, msg_, 0, 0);
  }
}

GitBaseline GitService::loadBaseline(uint64_t docId, const std::wstring& path) {
  GitBaseline b;
  b.docId = docId;
  b.path = path;
  Git g;
  g.exe = exe_;
  std::wstring err;
  if (!g.Open(parentOf(path), err)) return b;
  std::wstring k = key(path), rootKey = key(g.root);
  if (!underRoot(k, rootKey) || k == rootKey) return b;
  // A root such as "C:\\" already ends with the separator.
  const bool rootHasSlash = !g.root.empty() && (g.root.back() == L'\\' || g.root.back() == L'/');
  std::wstring rel = path.substr(g.root.size() + (rootHasSlash ? 0 : 1));
  for (auto& c : rel)
    if (c == L'\\') c = L'/';
  ProcResult r = g.Run({L"show", L":" + rel});  // ":path" = the index (staged) version
  if (!r.ok() || r.out.size() > (16u << 20)) return b;
  b.tracked = true;
  b.text = decodeText(r.out, LineEnding::LF).text;
  return b;
}

bool GitService::collect(std::vector<GitBaseline>& baselines, bool* reposChanged) {
  std::lock_guard<std::mutex> lock(mu_);
  bool changed = false;
  if (pendingRepos_) {
    repos_ = std::move(*pendingRepos_);
    pendingRepos_.reset();
    changed = true;
  }
  if (reposChanged) *reposChanged = changed;
  baselines = std::move(pendingBaselines_);
  pendingBaselines_.clear();
  return changed || !baselines.empty();
}

const GitRepoState* GitService::repoFor(const std::wstring& path) const {
  if (path.empty()) return nullptr;
  std::wstring k = key(path);
  const GitRepoState* best = nullptr;
  for (const auto& r : repos_) {
    std::wstring rk = key(r->root);
    if (underRoot(k, rk) && (!best || rk.size() > best->root.size())) best = r.get();
  }
  return best;
}

GitBadge GitService::badgeFor(const std::wstring& path, bool isDir) const {
  const GitRepoState* repo = repoFor(path);
  if (!repo) return GitBadge::None;
  std::wstring k = key(path);
  for (const auto& d : repo->ignoredDirs)
    if (k.size() + 1 >= d.size() && (k + kPathSepStr).compare(0, d.size(), d) == 0) return GitBadge::Ignored;
  if (isDir) return repo->changedDirs.count(k) ? GitBadge::Modified : GitBadge::None;
  auto it = repo->files.find(k);
  return it == repo->files.end() ? GitBadge::None : it->second;
}

}  // namespace verso
