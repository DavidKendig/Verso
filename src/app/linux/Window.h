#pragma once
// Linux front end: one Wayland toplevel drawn with Cairo into shared-memory buffers.
//
// It plays the part of the Win32 window procedure for App: keyboard input arrives as
// Windows virtual-key codes (so Keymap and AppInput work unchanged) plus the typed
// characters, mouse input in window pixels, and timers, file-descriptor watches and
// work posted from background threads all run on the UI thread from one poll() loop.

#include <cairo.h>
#include <xkbcommon/xkbcommon.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace verso::platform {

enum Mod : uint32_t { ModShift = 1, ModCtrl = 2, ModAlt = 4, ModSuper = 8 };
enum class Cursor { Arrow, IBeam, Hand, ResizeH, ResizeV, Wait };
enum MouseButton { MouseLeft = 0, MouseRight = 1, MouseMiddle = 2, MouseBack = 3, MouseForward = 4 };

class Window {
 public:
  Window();
  ~Window();
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  // --- events (all on the UI thread) ---------------------------------------------------
  std::function<void(cairo_t* cr, int w, int h)> onPaint;
  std::function<void(int w, int h)> onResize;
  // vk: Windows virtual-key code (0 when the key has none); text: what the key types.
  std::function<void(unsigned vk, uint32_t mods, bool repeat)> onKeyDown;
  std::function<void(unsigned vk, uint32_t mods)> onKeyUp;
  std::function<void(char32_t c)> onChar;
  std::function<void(int button, bool down, float x, float y, uint32_t mods)> onMouseButton;
  std::function<void(float x, float y, uint32_t mods)> onMouseMove;
  std::function<void()> onMouseLeave;
  // Wheel in notches (120 units on Windows = 1.0 here); positive dy scrolls content up.
  std::function<void(float notches, bool horizontal, float x, float y, uint32_t mods)> onWheel;
  std::function<void(bool active)> onFocus;
  std::function<void()> onClose;  // the user closed the window
  std::function<void(const std::vector<std::string>& paths)> onDropFiles;

  // --- window --------------------------------------------------------------------------
  bool create(const std::string& title, const std::string& appId, int w, int h);
  void setTitle(const std::string& title);
  void setFullscreen(bool on);
  void setCursor(Cursor c);
  void invalidate();  // repaint on the next frame
  int width() const;
  int height() const;
  uint32_t mods() const;
  bool active() const;

  // --- clipboard -------------------------------------------------------------------------
  void setClipboard(const std::string& utf8);
  bool hasClipboardText() const;
  // The current clipboard text (blocks briefly while the owner sends it).
  std::string clipboardText();

  // --- loop, timers, watches, cross-thread posts -------------------------------------
  int run();
  // One pass of the loop (paint, wait up to timeoutMs, dispatch); false once the window is
  // gone. Modal popups and dialogs run their own loop with it, like a Win32 modal loop.
  bool step(int timeoutMs = -1);
  bool running() const;
  void quit(int code = 0);
  // Repeating (or one-shot) timer; ids are the caller's, like SetTimer.
  void setTimer(uintptr_t id, unsigned ms, bool repeat, std::function<void()> fn);
  void killTimer(uintptr_t id);
  // Calls fn whenever fd is readable (or hung up) until removeWatch.
  void addWatch(int fd, std::function<void()> fn);
  void removeWatch(int fd);
  // Thread-safe: runs fn on the UI thread soon (replaces PostMessage(WM_APP...)).
  void post(std::function<void()> fn);

  struct Impl;

 private:
  std::unique_ptr<Impl> d_;
};

// The UI thread's window (App creates exactly one), for helpers that need to post work.
Window* mainWindow();

// Windows virtual-key code for an X keysym (US layout semantics, like Windows VKs).
unsigned vkFromKeysym(xkb_keysym_t sym);

}  // namespace verso::platform
