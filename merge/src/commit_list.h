#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "git.h"
#include "graph.h"

// Owner-painted, virtualised commit list with graph lanes and ref badges.
// Item ids: -1 = "working directory" pseudo row, >= 0 = index into commits.
class CommitList {
public:
    static constexpr int kNone = -2;
    static constexpr int kWorkdir = -1;
    static constexpr WORD kSelChange = 1;  // WM_COMMAND notification code

    static bool Register(HINSTANCE inst);
    HWND Create(HWND parent, int id);
    HWND hwnd() const { return wnd_; }

    void SetModel(const std::vector<Commit>* commits, const std::vector<GraphRow>* graph, std::vector<int> rows,
                  bool showGraph, bool workdirRow, const std::string& headHash);
    void SetWorkdirText(const std::wstring& text, bool dirty);
    void SetBanner(const std::wstring& text);  // shown above the list (e.g. file history)
    void UpdateMetrics();

    int Selected() const { return selected_; }
    void Select(int item, bool notify);
    int ItemAtScreenPoint(POINT pt) const;

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Paint(HDC dc, const RECT& rc);
    void PaintGraph(HDC dc, int row, const RECT& rowRc, int item);
    int RowCount() const { return (workdir_ ? 1 : 0) + (int)rows_.size(); }
    int ItemOfRow(int row) const;
    int RowOfItem(int item) const;
    int BannerHeight() const { return banner_.empty() ? 0 : rowH_; }
    int VisibleRows() const;
    void EnsureVisible(int row);
    void ScrollTo(int top);
    void UpdateScroll();
    void SelectRow(int row, bool notify);

    HWND wnd_ = nullptr;
    int id_ = 0;
    const std::vector<Commit>* commits_ = nullptr;
    const std::vector<GraphRow>* graph_ = nullptr;
    std::vector<int> rows_;
    bool showGraph_ = true;
    bool workdir_ = true;
    bool workdirDirty_ = false;
    std::wstring workdirText_;
    std::wstring banner_;
    std::string headHash_;
    int selected_ = kNone;
    int top_ = 0;
    int rowH_ = 24;
    int laneW_ = 14;
};
