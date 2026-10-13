#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Owner-painted unified diff viewer with line selection and patch building for
// hunk / line level staging. Also used to show plain text (e.g. blame output).
class DiffView {
public:
    static bool Register(HINSTANCE inst);
    HWND Create(HWND parent, int id);
    HWND hwnd() const { return wnd_; }

    void SetDiff(const std::string& diff, bool keepScroll);
    void SetText(const std::wstring& text);
    void SetPlaceholder(const std::wstring& message);
    void UpdateMetrics();

    bool ScrollToFile(const std::string& path);
    int FindRow(std::wstring_view text, int startRow = 0) const;  // first visible row with this text
    void SetSelection(int anchorRow, int cursorRow);
    bool CursorInHunk() const;
    bool HasMultiLineSelection() const;
    std::wstring SelectedText() const;
    std::string CursorFilePath() const;

    // Builds a patch from the selected lines (or the cursor line). With
    // `wholeHunks` the selection is widened to the hunk(s) under it. `reverse` chooses how
    // unselected changes are folded so the result applies with `git apply -R`
    // (unstage / discard) instead of forward (stage).
    std::string BuildPatch(bool reverse, bool wholeHunks) const;

private:
    enum class Kind : uint8_t { Context, Add, Del, Hunk, FileHeader, Meta, NoNewline, Text };
    struct Line {
        Kind kind = Kind::Text;
        bool hidden = false;
        int oldNo = 0, newNo = 0;
        int file = -1;
        std::string raw;
        std::wstring text;
    };
    struct FileInfo {
        int header = 0, end = 0;
        std::string path, origPath;
        bool combined = false, binary = false, added = false, deleted = false;
        std::wstring modeChange;
    };

    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Paint(HDC dc, const RECT& rc);
    void Reset();
    void FinishParse();
    int VisibleRows() const;
    int RowAt(int y) const;
    int GutterWidth() const;
    void UpdateScroll();
    void ScrollTo(int top);
    void ScrollH(int x);
    void EnsureVisible(int row);
    void SetCursor(int row, bool extend);
    void SelectionLines(int& a, int& b) const;  // model line indices
    void WidenToHunks(int& a, int& b) const;

    HWND wnd_ = nullptr;
    std::vector<Line> lines_;
    std::vector<int> rows_;  // visible line indices
    std::vector<FileInfo> files_;
    std::wstring placeholder_;
    bool diffMode_ = false;
    int top_ = 0, hscroll_ = 0;
    int lineH_ = 16, charW_ = 8;
    size_t maxChars_ = 0;
    int anchor_ = -1, cursor_ = -1;
    bool dragging_ = false;
};
