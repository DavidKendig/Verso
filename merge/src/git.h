#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "git_process.h"

struct RefBadge {
    enum Kind { Head, Local, Remote, Tag } kind = Local;
    std::wstring name;
    bool current = false;  // the branch HEAD points at
};

struct Commit {
    std::string hash;
    std::vector<std::string> parents;
    std::wstring author, email, subject;
    int64_t time = 0;
    std::vector<RefBadge> badges;
};

struct FileEntry {
    char x = ' ', y = ' ';  // porcelain v1 index / worktree status
    std::string path, origPath;
    bool conflict = false;
    bool untracked = false;
};

struct RefInfo {
    enum Kind { Local, Remote, Tag } kind = Local;
    std::string fullName;  // refs/heads/main
    std::string name;      // main, origin/main, v1.0
    std::string target;    // commit hash
    std::string upstream;  // origin/main
    int ahead = 0, behind = 0;
    bool gone = false;
    bool current = false;
};

struct StashInfo {
    std::string ref;  // stash@{0}
    std::wstring message;
};

struct ChangedFile {
    std::string status;  // M, A, D, R100 ...
    std::string path, origPath;
};

// Thin wrapper around the git command line. All repository reads and writes go
// through git.exe so behaviour matches the user's git configuration exactly.
class Git {
public:
    using Logger = std::function<void(const std::wstring& command, const ProcResult& result)>;

    std::wstring exe;
    std::wstring root;    // working tree root (Windows separators)
    std::wstring gitDir;  // absolute .git directory
    Logger logger;

    static std::wstring Locate();

    ProcResult Run(const std::vector<std::wstring>& args, const std::string* input = nullptr,
                   bool log = false) const;

    bool Open(const std::wstring& path, std::wstring& error);

    std::vector<Commit> Log(int maxCount, const std::wstring& pathFilter, bool includeHead) const;
    std::vector<FileEntry> Status() const;
    std::vector<RefInfo> Refs() const;
    std::vector<std::string> Remotes() const;
    std::vector<StashInfo> Stashes() const;
    std::string HeadBranch() const;  // empty when detached or unborn
    std::string HeadHash() const;    // empty when unborn
    std::wstring OperationState() const;  // "", "MERGING", "REBASING", ...

    std::vector<ChangedFile> CommitFiles(const Commit& c) const;
    std::string CommitDiff(const Commit& c) const;
    std::wstring CommitDetails(const std::string& hash) const;
    std::string CommitMessage(const std::string& rev) const;
    std::string WorkingDiff(const std::vector<std::string>& paths, bool cached) const;
    std::string NewFileDiff(const std::string& path) const;

    std::wstring FullPath(const std::string& rel) const;
};

std::string JoinNul(const std::vector<std::string>& items);
