// Self-test for the git layer, commit graph and hunk/line staging patches.
// Builds a throwaway repository in %TEMP%, exercises the same code paths the
// UI uses, and checks the results with plain git commands.
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "../src/diff_view.h"
#include "../src/git.h"
#include "../src/graph.h"
#include "../src/theme.h"
#include "../src/util.h"

static int g_failures = 0;

static void Check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

static void WriteFile(const std::wstring& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << data;
}

static std::string Must(Git& g, std::vector<std::wstring> args, const std::string* in = nullptr) {
    ProcResult r = g.Run(args, in);
    if (!r.ok()) {
        printf("git command failed: %s\n", r.err.c_str());
        exit(2);
    }
    return r.out;
}

static bool Apply(Git& g, const std::string& patch, bool cached, bool reverse) {
    std::vector<std::wstring> args = {L"apply", L"--recount", L"--whitespace=nowarn"};
    if (cached) args.push_back(L"--cached");
    if (reverse) args.push_back(L"-R");
    args.push_back(L"-");
    ProcResult r = g.Run(args, &patch);
    if (!r.ok()) printf("    git apply error: %s\n    patch was:\n%s\n", r.err.c_str(), patch.c_str());
    return r.ok();
}

int main() {
    SetEnvironmentVariableW(L"GIT_TERMINAL_PROMPT", L"0");
    theme::Init(96);

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring root = std::wstring(tmp) + L"verso-git-selftest-" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(root.c_str(), nullptr);

    Git g;
    g.exe = Git::Locate();
    g.root = root;
    printf("git: %ls\nrepo: %ls\n", g.exe.c_str(), root.c_str());

    Must(g, {L"init", L"-q", L"-b", L"main"});
    Must(g, {L"config", L"user.name", L"Self Test"});
    Must(g, {L"config", L"user.email", L"selftest@example.com"});
    Must(g, {L"config", L"core.autocrlf", L"false"});
    std::string ten;
    for (int i = 1; i <= 10; ++i) ten += "line" + std::to_string(i) + "\n";
    WriteFile(root + L"\\a.txt", ten);
    Must(g, {L"add", L"-A"});
    Must(g, {L"commit", L"-qm", L"Initial"});
    Must(g, {L"checkout", L"-qb", L"feature"});
    WriteFile(root + L"\\f.txt", "feature\n");
    Must(g, {L"add", L"-A"});
    Must(g, {L"commit", L"-qm", L"Feature work"});
    Must(g, {L"checkout", L"-q", L"main"});
    WriteFile(root + L"\\m.txt", "main\n");
    Must(g, {L"add", L"-A"});
    Must(g, {L"commit", L"-qm", L"Main work"});
    Must(g, {L"merge", L"-q", L"--no-edit", L"feature"});
    Must(g, {L"tag", L"-a", L"v1", L"-m", L"v1"});

    printf("\nRepository reads\n");
    std::wstring err;
    Check(g.Open(root, err), "Open repository");
    auto commits = g.Log(100, L"", true);
    Check(commits.size() == 4, "Log returns 4 commits");
    Check(!commits.empty() && commits[0].parents.size() == 2, "Newest commit is a merge with 2 parents");
    bool headBadge = false, tagBadge = false;
    for (auto& b : commits.empty() ? std::vector<RefBadge>{} : commits[0].badges) {
        if (b.kind == RefBadge::Local && b.name == L"main" && b.current) headBadge = true;
        if (b.kind == RefBadge::Tag && b.name == L"v1") tagBadge = true;
    }
    Check(headBadge, "Merge commit decorated with current branch 'main'");
    Check(tagBadge, "Merge commit decorated with tag 'v1'");
    auto graph = BuildGraph(commits);
    Check(graph.size() == 4 && graph[0].merge && graph[0].width == 2, "Graph: merge row spans two lanes");
    Check(graph.size() == 4 && graph[3].col == 0 && graph[3].bottom.empty(), "Graph: root commit closes its lane");
    auto refs = g.Refs();
    bool tagPeeled = false;
    for (auto& r : refs)
        if (r.kind == RefInfo::Tag && r.name == "v1") tagPeeled = !commits.empty() && r.target == commits[0].hash;
    Check(tagPeeled, "Annotated tag resolves to its commit");
    Check(g.HeadBranch() == "main", "HeadBranch == main");

    printf("\nLine and hunk staging\n");
    std::string changed = ten;
    changed.replace(changed.find("line1\n"), 6, "line1 X\n");
    changed.replace(changed.find("line5\n"), 6, "line5 X\n");
    changed.replace(changed.find("line10\n"), 7, "line10 X\n");
    WriteFile(root + L"\\a.txt", changed);
    auto status = g.Status();
    Check(status.size() == 1 && status[0].y == 'M', "Status shows a.txt modified");

    HWND host = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 800, 600, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    DiffView::Register(GetModuleHandleW(nullptr));
    DiffView dv;
    dv.Create(host, 1);

    dv.SetDiff(g.WorkingDiff({"a.txt"}, false), false);
    int del5 = dv.FindRow(L"line5"), add5 = dv.FindRow(L"line5 X");
    Check(del5 > 0 && add5 == del5 + 1, "Diff view parsed line5 change");
    dv.SetSelection(del5, add5);
    Check(Apply(g, dv.BuildPatch(false, false), true, false), "Stage selected lines (line5 only)");
    std::string cached = Must(g, {L"diff", L"--cached", L"--", L"a.txt"});
    Check(cached.find("+line5 X") != std::string::npos && cached.find("+line1 X") == std::string::npos &&
              cached.find("+line10 X") == std::string::npos,
          "Index contains only the line5 change");

    dv.SetDiff(g.WorkingDiff({"a.txt"}, true), false);
    int staged5 = dv.FindRow(L"line5 X");
    dv.SetSelection(staged5, staged5);
    Check(Apply(g, dv.BuildPatch(true, true), true, true), "Unstage hunk");
    Check(Must(g, {L"diff", L"--cached"}).empty(), "Index is clean again");

    dv.SetDiff(g.WorkingDiff({"a.txt"}, false), false);
    int add10 = dv.FindRow(L"line10 X");
    dv.SetSelection(add10, add10);
    Check(Apply(g, dv.BuildPatch(false, true), true, false), "Stage whole hunk");
    cached = Must(g, {L"diff", L"--cached", L"--", L"a.txt"});
    Check(cached.find("+line1 X") != std::string::npos && cached.find("+line10 X") != std::string::npos,
          "Whole hunk staged");
    Must(g, {L"reset", L"-q"});

    dv.SetDiff(g.WorkingDiff({"a.txt"}, false), false);
    int del1 = dv.FindRow(L"line1"), add1 = dv.FindRow(L"line1 X");
    dv.SetSelection(del1, add1);
    Check(Apply(g, dv.BuildPatch(true, false), false, true), "Discard selected lines (line1)");
    std::ifstream ain(root + L"\\a.txt", std::ios::binary);
    std::string after((std::istreambuf_iterator<char>(ain)), std::istreambuf_iterator<char>());
    ain.close();
    Check(after.find("line1\n") == 0 && after.find("line5 X") != std::string::npos,
          "Working tree: line1 restored, line5 change kept");
    Must(g, {L"checkout", L"--", L"a.txt"});

    printf("\nUntracked and CRLF files\n");
    WriteFile(root + L"\\notes.txt", "first\nsecond\nthird");
    dv.SetDiff(g.NewFileDiff("notes.txt"), false);
    int first = dv.FindRow(L"first");
    dv.SetSelection(first, first);
    Check(Apply(g, dv.BuildPatch(false, false), true, false), "Stage one line of an untracked file");
    Check(Must(g, {L"show", L":notes.txt"}) == "first\n", "Index holds only the selected line");
    Must(g, {L"rm", L"-q", L"--cached", L"-f", L"notes.txt"});

    WriteFile(root + L"\\crlf.txt", "one\r\ntwo\r\n");
    dv.SetDiff(g.NewFileDiff("crlf.txt"), false);
    int one = dv.FindRow(L"one");
    dv.SetSelection(one, one);
    Check(Apply(g, dv.BuildPatch(false, true), true, false), "Stage CRLF file via hunk");
    Check(Must(g, {L"show", L":crlf.txt"}) == "one\r\ntwo\r\n", "CRLF line endings preserved in index");

    printf("\nCommit with message from stdin\n");
    std::string msg = "Unicode \xE2\x9C\x93 commit\n\nBody line\n";
    Must(g, {L"commit", L"-q", L"-F", L"-"}, &msg);
    Check(g.CommitMessage("HEAD") == "Unicode \xE2\x9C\x93 commit\n\nBody line", "Commit message round-trips UTF-8");
    auto files = g.CommitFiles(g.Log(1, L"", true)[0]);
    Check(files.size() == 1 && files[0].path == "crlf.txt" && files[0].status == "A", "CommitFiles lists crlf.txt");

    DestroyWindow(host);
    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL TESTS PASSED", g_failures,
           g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
