#include "git.h"

#include <windows.h>

#include <fstream>

#include "util.h"

std::wstring Git::Locate() {
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, buf, nullptr)) return buf;
    const wchar_t* vars[] = {L"ProgramFiles", L"ProgramW6432", L"ProgramFiles(x86)", L"LOCALAPPDATA"};
    const wchar_t* tails[] = {L"\\Git\\cmd\\git.exe", L"\\Git\\cmd\\git.exe", L"\\Git\\cmd\\git.exe",
                              L"\\Programs\\Git\\cmd\\git.exe"};
    for (int i = 0; i < 4; ++i) {
        wchar_t base[MAX_PATH];
        if (!GetEnvironmentVariableW(vars[i], base, MAX_PATH)) continue;
        std::wstring p = std::wstring(base) + tails[i];
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return {};
}

ProcResult Git::Run(const std::vector<std::wstring>& args, const std::string* input, bool log) const {
    std::vector<std::wstring> full = {L"-c", L"core.quotepath=false", L"-c", L"color.ui=false"};
    full.insert(full.end(), args.begin(), args.end());
    ProcResult r = RunProcess(exe, full, root, input);
    if (logger && (log || !r.ok())) {
        std::wstring cmd = L"git";
        for (const auto& a : args) cmd += L" " + QuoteArg(a);
        logger(cmd, r);
    }
    return r;
}

bool Git::Open(const std::wstring& path, std::wstring& error) {
    std::wstring prevRoot = root;
    root = path;
    ProcResult r = Run({L"rev-parse", L"--show-toplevel", L"--absolute-git-dir"});
    if (!r.ok()) {
        error = Widen(Trim(r.err.empty() ? r.out : r.err));
        if (error.empty()) error = L"Not a git repository.";
        root = prevRoot;
        return false;
    }
    auto lines = Split(Trim(r.out), '\n');
    if (lines.size() < 2) {
        error = L"This folder has no working tree (bare repositories are not supported).";
        root = prevRoot;
        return false;
    }
    auto fix = [](std::string s) {
        std::wstring w = Widen(Trim(s));
        for (auto& c : w)
            if (c == L'/') c = L'\\';
        return w;
    };
    root = fix(lines[0]);
    gitDir = fix(lines[1]);
    return true;
}

static void ParseDecorations(const std::string& deco, std::vector<RefBadge>& out) {
    if (deco.empty()) return;
    for (std::string item : Split(deco, ',')) {
        item = Trim(item);
        RefBadge b;
        if (StartsWith(item, "HEAD -> ")) {
            item = item.substr(8);
            b.current = true;
        } else if (item == "HEAD") {
            b.kind = RefBadge::Head;
            b.name = L"HEAD";
            out.push_back(b);
            continue;
        }
        if (StartsWith(item, "tag: ")) item = item.substr(5);
        if (StartsWith(item, "refs/heads/")) {
            b.kind = RefBadge::Local;
            b.name = Widen(item.substr(11));
        } else if (StartsWith(item, "refs/remotes/")) {
            std::string n = item.substr(13);
            if (n.size() >= 5 && n.compare(n.size() - 5, 5, "/HEAD") == 0) continue;
            b.kind = RefBadge::Remote;
            b.name = Widen(n);
        } else if (StartsWith(item, "refs/tags/")) {
            b.kind = RefBadge::Tag;
            b.name = Widen(item.substr(10));
        } else {
            continue;
        }
        out.push_back(b);
    }
}

std::vector<Commit> Git::Log(int maxCount, const std::wstring& pathFilter, bool includeHead) const {
    std::vector<std::wstring> args = {L"log",
                                      L"--date-order",
                                      L"--decorate=full",
                                      L"--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%D%x1f%s%x1e",
                                      L"-n",
                                      std::to_wstring(maxCount),
                                      L"--branches",
                                      L"--remotes",
                                      L"--tags"};
    if (includeHead) args.push_back(L"HEAD");
    if (!pathFilter.empty()) {
        args.push_back(L"--");
        args.push_back(pathFilter);
    }
    std::vector<Commit> commits;
    ProcResult r = Run(args);
    if (!r.ok()) return commits;
    for (std::string rec : Split(r.out, '\x1e')) {
        size_t s = rec.find_first_not_of("\r\n");
        if (s == std::string::npos) continue;
        rec = rec.substr(s);
        auto f = Split(rec, '\x1f');
        if (f.size() < 7) continue;
        Commit c;
        c.hash = f[0];
        for (auto& p : Split(f[1], ' '))
            if (!p.empty()) c.parents.push_back(p);
        c.author = Widen(f[2]);
        c.email = Widen(f[3]);
        c.time = _atoi64(f[4].c_str());
        ParseDecorations(f[5], c.badges);
        c.subject = Widen(f[6]);
        commits.push_back(std::move(c));
    }
    return commits;
}

std::vector<FileEntry> Git::Status() const {
    std::vector<FileEntry> files;
    ProcResult r = Run({L"status", L"--porcelain=v1", L"-z", L"--untracked-files=all"});
    if (!r.ok()) return files;
    auto parts = Split(r.out, '\0');
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string& p = parts[i];
        if (p.size() < 4) continue;
        FileEntry e;
        e.x = p[0];
        e.y = p[1];
        e.path = p.substr(3);
        if ((e.x == 'R' || e.x == 'C') && i + 1 < parts.size()) e.origPath = parts[++i];
        e.untracked = e.x == '?';
        e.conflict = e.x == 'U' || e.y == 'U' || (e.x == 'A' && e.y == 'A') || (e.x == 'D' && e.y == 'D');
        if (e.x == '!') continue;
        files.push_back(std::move(e));
    }
    return files;
}

std::vector<RefInfo> Git::Refs() const {
    std::vector<RefInfo> refs;
    ProcResult r = Run({L"for-each-ref",
                        L"--format=%(refname)%1f%(objectname)%1f%(upstream:short)%1f%(upstream:track)%1f%(HEAD)%1f%(*objectname)",
                        L"refs/heads", L"refs/remotes", L"refs/tags"});
    if (!r.ok()) return refs;
    for (const auto& line : Split(r.out, '\n')) {
        auto f = Split(line, '\x1f');
        if (f.size() < 6) continue;
        RefInfo ref;
        ref.fullName = f[0];
        // For annotated tags, point at the tagged commit rather than the tag object.
        ref.target = f[5].empty() ? f[1] : f[5];
        ref.upstream = f[2];
        ref.current = f[4] == "*";
        const std::string& track = f[3];
        if (track.find("gone") != std::string::npos) ref.gone = true;
        if (auto a = track.find("ahead "); a != std::string::npos) ref.ahead = atoi(track.c_str() + a + 6);
        if (auto b = track.find("behind "); b != std::string::npos) ref.behind = atoi(track.c_str() + b + 7);
        if (StartsWith(ref.fullName, "refs/heads/")) {
            ref.kind = RefInfo::Local;
            ref.name = ref.fullName.substr(11);
        } else if (StartsWith(ref.fullName, "refs/remotes/")) {
            ref.kind = RefInfo::Remote;
            ref.name = ref.fullName.substr(13);
            if (ref.name.size() >= 5 && ref.name.compare(ref.name.size() - 5, 5, "/HEAD") == 0) continue;
        } else {
            ref.kind = RefInfo::Tag;
            ref.name = ref.fullName.substr(10);
        }
        refs.push_back(std::move(ref));
    }
    return refs;
}

std::vector<std::string> Git::Remotes() const {
    std::vector<std::string> out;
    ProcResult r = Run({L"remote"});
    for (auto& l : Split(r.out, '\n')) {
        std::string t = Trim(l);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

std::vector<StashInfo> Git::Stashes() const {
    std::vector<StashInfo> out;
    ProcResult r = Run({L"stash", L"list", L"--format=%gd%x1f%s"});
    if (!r.ok()) return out;
    for (auto& l : Split(r.out, '\n')) {
        auto f = Split(l, '\x1f');
        if (f.size() < 2) continue;
        out.push_back({f[0], Widen(f[1])});
    }
    return out;
}

std::string Git::HeadBranch() const {
    ProcResult r = Run({L"symbolic-ref", L"-q", L"--short", L"HEAD"});
    return r.ok() ? Trim(r.out) : std::string();
}

std::string Git::HeadHash() const {
    ProcResult r = Run({L"rev-parse", L"-q", L"--verify", L"HEAD"});
    return r.ok() ? Trim(r.out) : std::string();
}

std::wstring Git::OperationState() const {
    auto exists = [&](const wchar_t* name) {
        return GetFileAttributesW((gitDir + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    if (exists(L"rebase-merge") || exists(L"rebase-apply")) return L"REBASING";
    if (exists(L"MERGE_HEAD")) return L"MERGING";
    if (exists(L"CHERRY_PICK_HEAD")) return L"CHERRY-PICKING";
    if (exists(L"REVERT_HEAD")) return L"REVERTING";
    return L"";
}

std::vector<ChangedFile> Git::CommitFiles(const Commit& c) const {
    ProcResult r = c.parents.empty()
                       ? Run({L"diff-tree", L"--root", L"-r", L"-M", L"--name-status", L"-z",
                              L"--no-commit-id", Widen(c.hash)})
                       : Run({L"diff", L"-M", L"--name-status", L"-z", Widen(c.parents[0]), Widen(c.hash)});
    std::vector<ChangedFile> out;
    if (!r.ok()) return out;
    auto parts = Split(r.out, '\0');
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        ChangedFile f;
        f.status = parts[i];
        if (f.status.empty()) continue;
        if ((f.status[0] == 'R' || f.status[0] == 'C') && i + 2 < parts.size()) {
            f.origPath = parts[++i];
            f.path = parts[++i];
        } else {
            f.path = parts[++i];
        }
        out.push_back(std::move(f));
    }
    return out;
}

std::string Git::CommitDiff(const Commit& c) const {
    ProcResult r = c.parents.empty()
                       ? Run({L"show", L"--format=", L"--patch", L"-M", L"--no-ext-diff", Widen(c.hash)})
                       : Run({L"diff", L"-M", L"--no-ext-diff", Widen(c.parents[0]), Widen(c.hash)});
    return r.out;
}

std::wstring Git::CommitDetails(const std::string& hash) const {
    ProcResult r = Run({L"show", L"-s", L"--date=format-local:%Y-%m-%d %H:%M:%S",
                        L"--format=Commit:     %H%nParents:    %P%nAuthor:     %an <%ae>%nDate:       %ad%n"
                        L"Committer:  %cn <%ce>%n%n%B",
                        Widen(hash)});
    return ToCrlf(Widen(r.ok() ? r.out : r.err));
}

std::string Git::CommitMessage(const std::string& rev) const {
    ProcResult r = Run({L"log", L"-1", L"--format=%B", Widen(rev)});
    return r.ok() ? Trim(r.out) : std::string();
}

std::string Git::WorkingDiff(const std::vector<std::string>& paths, bool cached) const {
    std::vector<std::wstring> args = {L"diff", L"--no-ext-diff", L"-M"};
    if (cached) args.push_back(L"--cached");
    args.push_back(L"--");
    for (auto& p : paths) args.push_back(Widen(p));
    return Run(args).out;
}

// Builds a "new file" patch for an untracked file so it can be shown (and even
// partially staged with `git apply --cached`) like any other change.
std::string Git::NewFileDiff(const std::string& path) const {
    std::ifstream in(FullPath(path), std::ios::binary);
    std::string data;
    if (in) {
        in.seekg(0, std::ios::end);
        auto size = (std::streamoff)in.tellg();
        in.seekg(0);
        if (size > 4 * 1024 * 1024) {
            return "diff --git a/" + path + " b/" + path + "\nnew file mode 100644\nBinary files /dev/null and b/" +
                   path + " differ\n";
        }
        data.resize((size_t)size);
        in.read(data.data(), size);
    }
    std::string out = "diff --git a/" + path + " b/" + path + "\nnew file mode 100644\n";
    if (data.find('\0') != std::string::npos)
        return out + "Binary files /dev/null and b/" + path + " differ\n";
    if (data.empty()) return out;
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(data.substr(pos));
            pos = data.size();
        } else {
            lines.push_back(data.substr(pos, nl - pos));
            pos = nl + 1;
        }
    }
    out += "--- /dev/null\n+++ b/" + path + "\n@@ -0,0 +1," + std::to_string(lines.size()) + " @@\n";
    for (auto& l : lines) out += "+" + l + "\n";
    if (data.back() != '\n') out += "\\ No newline at end of file\n";
    return out;
}

std::wstring Git::FullPath(const std::string& rel) const { return JoinRepoPath(root, rel); }

std::string JoinNul(const std::vector<std::string>& items) {
    std::string s;
    for (auto& i : items) {
        s += i;
        s.push_back('\0');
    }
    return s;
}
