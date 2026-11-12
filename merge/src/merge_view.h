#pragma once
#include <windows.h>

#include <functional>
#include <memory>
#include <string>

#ifndef _WIN32
namespace verso {
class Gfx;
struct Theme;
struct RectF;
}  // namespace verso
#endif

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
#ifdef _WIN32
    bool translateAccelerator(MSG* msg);
#else
    // Linux: the git view is drawn by Verso inside its own window and gets its input
    // from the host (the Windows view is a child window that reads its own). Points are
    // in the coordinates of `paint`'s area. The input calls return false for what the
    // view leaves to the host: keys it has no use for (menu mnemonics) and the pointer
    // outside its area (the status bar), unless a drag or one of its dialogs holds it.
    enum class Pointer { Arrow, IBeam, ResizeH };
    void paint(verso::Gfx& g, const verso::Theme& theme, const verso::RectF& area);
    bool keyDown(unsigned vk, bool ctrl, bool shift, bool alt);
    void charTyped(wchar_t c);
    bool mouseButton(int button, bool down, float x, float y);
    bool mouseMove(float x, float y, Pointer* pointer);
    bool wheel(float notches, bool horizontal, float x, float y);
    bool modal() const;  // one of the view's dialogs is open
#endif
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
