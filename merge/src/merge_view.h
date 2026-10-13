#pragma once
#include <windows.h>

#include <functional>
#include <memory>
#include <string>

// Notifications from the git view to the application that hosts it.
struct MergeViewCallbacks {
    // The open repository and branch, as "name (branch)", or "" when none is open.
    std::function<void(const std::wstring& subject)> onTitle;
    std::function<void(const std::wstring& path)> onOpenFile;   // "Open File" on an absolute path (required)
    std::function<void()> onSwitchMode;                          // user asked to return to the editor
};

// Verso's git client (git mode) as an embeddable child window. The host owns the
// top-level window, menu bar and message loop; it shows `menu()` while the
// view is active and routes menu commands and accelerators to it.
class MergeView {
public:
    // Returns nullptr (with `error` set) when git.exe cannot be found.
    static std::unique_ptr<MergeView> Create(HINSTANCE inst, HWND parent, MergeViewCallbacks callbacks,
                                             std::wstring* error);
    ~MergeView();

    HWND hwnd() const;
    HMENU menu() const;
    bool busy() const;

    // Host integration.
    bool translateAccelerator(MSG* msg);
    void onMenuCommand(WPARAM wp, LPARAM lp);
    void activate();  // opens the most recent repository if none is open, refreshes, focuses
    void refresh();   // re-reads the repository (after edits or external changes)
    void focus();

    // Repository actions. Paths may be any folder or file inside a working tree.
    // These return false while a git operation is running (see busy()), which keeps
    // the current repository until the operation completes.
    bool openRepository(const std::wstring& path);
    bool showFileHistory(const std::wstring& absolutePath);
    bool showBlame(const std::wstring& absolutePath);

    struct Impl;

private:
    explicit MergeView(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
