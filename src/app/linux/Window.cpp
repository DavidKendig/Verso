#include "app/linux/Window.h"

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

#include "cursor-shape-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace verso::platform {

namespace {
Window* gMain = nullptr;
}

Window* mainWindow() { return gMain; }

// ---------------------------------------------------------------------------------------
// Virtual keys: Windows VK codes from X keysyms of the unshifted key.
// ---------------------------------------------------------------------------------------
unsigned vkFromKeysym(xkb_keysym_t s) {
  if (s >= XKB_KEY_a && s <= XKB_KEY_z) return 'A' + (s - XKB_KEY_a);
  if (s >= XKB_KEY_A && s <= XKB_KEY_Z) return 'A' + (s - XKB_KEY_A);
  if (s >= XKB_KEY_0 && s <= XKB_KEY_9) return '0' + (s - XKB_KEY_0);
  if (s >= XKB_KEY_F1 && s <= XKB_KEY_F24) return 0x70 + (s - XKB_KEY_F1);
  if (s >= XKB_KEY_KP_0 && s <= XKB_KEY_KP_9) return 0x60 + (s - XKB_KEY_KP_0);
  switch (s) {
    case XKB_KEY_BackSpace: return 0x08;
    case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: return 0x09;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: return 0x0D;
    case XKB_KEY_Shift_L: case XKB_KEY_Shift_R: return 0x10;
    case XKB_KEY_Control_L: case XKB_KEY_Control_R: return 0x11;
    case XKB_KEY_Alt_L: case XKB_KEY_Alt_R: return 0x12;
    case XKB_KEY_ISO_Level3_Shift: return 0;  // AltGr composes characters; it is not Alt
    case XKB_KEY_Pause: return 0x13;
    case XKB_KEY_Caps_Lock: return 0x14;
    case XKB_KEY_Escape: return 0x1B;
    case XKB_KEY_space: case XKB_KEY_KP_Space: return 0x20;
    case XKB_KEY_Page_Up: case XKB_KEY_KP_Page_Up: return 0x21;
    case XKB_KEY_Page_Down: case XKB_KEY_KP_Page_Down: return 0x22;
    case XKB_KEY_End: case XKB_KEY_KP_End: return 0x23;
    case XKB_KEY_Home: case XKB_KEY_KP_Home: return 0x24;
    case XKB_KEY_Left: case XKB_KEY_KP_Left: return 0x25;
    case XKB_KEY_Up: case XKB_KEY_KP_Up: return 0x26;
    case XKB_KEY_Right: case XKB_KEY_KP_Right: return 0x27;
    case XKB_KEY_Down: case XKB_KEY_KP_Down: return 0x28;
    case XKB_KEY_Print: return 0x2C;
    case XKB_KEY_Insert: case XKB_KEY_KP_Insert: return 0x2D;
    case XKB_KEY_Delete: case XKB_KEY_KP_Delete: return 0x2E;
    case XKB_KEY_Super_L: return 0x5B;
    case XKB_KEY_Super_R: return 0x5C;
    case XKB_KEY_Menu: return 0x5D;
    case XKB_KEY_KP_Multiply: return 0x6A;
    case XKB_KEY_KP_Add: return 0x6B;
    case XKB_KEY_KP_Separator: return 0x6C;
    case XKB_KEY_KP_Subtract: return 0x6D;
    case XKB_KEY_KP_Decimal: return 0x6E;
    case XKB_KEY_KP_Divide: return 0x6F;
    case XKB_KEY_Num_Lock: return 0x90;
    case XKB_KEY_Scroll_Lock: return 0x91;
    case XKB_KEY_semicolon: return 0xBA;   // VK_OEM_1
    case XKB_KEY_equal: return 0xBB;       // VK_OEM_PLUS
    case XKB_KEY_comma: return 0xBC;       // VK_OEM_COMMA
    case XKB_KEY_minus: return 0xBD;       // VK_OEM_MINUS
    case XKB_KEY_period: return 0xBE;      // VK_OEM_PERIOD
    case XKB_KEY_slash: return 0xBF;       // VK_OEM_2
    case XKB_KEY_grave: return 0xC0;       // VK_OEM_3
    case XKB_KEY_bracketleft: return 0xDB; // VK_OEM_4
    case XKB_KEY_backslash: return 0xDC;   // VK_OEM_5
    case XKB_KEY_bracketright: return 0xDD;// VK_OEM_6
    case XKB_KEY_apostrophe: return 0xDE;  // VK_OEM_7
    default: return 0;
  }
}

// ---------------------------------------------------------------------------------------
struct Window::Impl {
  Window* self = nullptr;
  wl_display* dpy = nullptr;
  wl_registry* reg = nullptr;
  wl_compositor* comp = nullptr;
  wl_shm* shm = nullptr;
  xdg_wm_base* wm = nullptr;
  wl_seat* seat = nullptr;
  wl_pointer* ptr = nullptr;
  wl_keyboard* kbd = nullptr;
  wl_data_device_manager* ddm = nullptr;
  wl_data_device* ddev = nullptr;
  wl_data_source* source = nullptr;
  wl_data_offer* selection = nullptr;
  wl_data_offer* dndOffer = nullptr;
  std::map<wl_data_offer*, std::vector<std::string>> offers;
  std::string clipText;
  zxdg_decoration_manager_v1* decoMgr = nullptr;
  wp_cursor_shape_manager_v1* cursorMgr = nullptr;
  wp_cursor_shape_device_v1* cursorDev = nullptr;
  wl_surface* surf = nullptr;
  xdg_surface* xsurf = nullptr;
  xdg_toplevel* top = nullptr;
  wl_callback* frameCb = nullptr;
  xkb_context* xctx = nullptr;
  xkb_keymap* keymap = nullptr;
  xkb_state* xstate = nullptr;
  uint32_t serial = 0, enterSerial = 0, mods = 0;
  int w = 0, h = 0, pendingW = 0, pendingH = 0;
  int boundsW = 0, boundsH = 0;  // xdg_toplevel.configure_bounds: the usable area
  bool configured = false, dirty = true, running = true, focused = false, fullscreen = false;
  int exitCode = 0;
  float px = 0, py = 0;
  Cursor cursor = Cursor::Arrow;
  // key repeat
  int repeatFd = -1, repeatRate = 30, repeatDelay = 400;
  uint32_t repeatKey = 0;
  uint64_t keySeq = 0;                  // counts key presses and releases
  std::unordered_map<uint32_t, bool> held;  // keys down right now
  // buffers
  struct Buf {
    wl_buffer* b = nullptr;
    void* data = nullptr;
    size_t size = 0;
    int w = 0, h = 0;
    bool busy = false;
    cairo_surface_t* cs = nullptr;
  } bufs[2];
  // timers, watches, posts
  struct Timer {
    int fd;
    bool repeat;
    std::function<void()> fn;
  };
  std::map<uintptr_t, Timer> timers;
  std::map<int, std::function<void()>> watches;
  int postFd = -1;
  std::mutex postMu;
  std::vector<std::function<void()>> posted;

  void keyEvent(uint32_t code, bool pressed);
  void dispatchKey(uint32_t code, bool repeat);
  void applyCursor();
  void draw();
  void freeBuf(Buf& b);
};

namespace {
Window::Impl* I(void* d) { return static_cast<Window::Impl*>(d); }

void bufRelease(void* d, wl_buffer*) { static_cast<Window::Impl::Buf*>(d)->busy = false; }
const wl_buffer_listener kBuf = {bufRelease};

void wmPing(void*, xdg_wm_base* wm, uint32_t s) { xdg_wm_base_pong(wm, s); }
const xdg_wm_base_listener kWm = {wmPing};

void xsConfigure(void* d, xdg_surface* s, uint32_t serial) {
  auto* im = I(d);
  xdg_surface_ack_configure(s, serial);
  bool resized = false;
  if (im->pendingW > 0 && im->pendingH > 0 && (im->pendingW != im->w || im->pendingH != im->h)) {
    im->w = im->pendingW;
    im->h = im->pendingH;
    resized = true;
  } else if (!im->configured && im->boundsW > 0 && im->boundsH > 0 &&
             (im->w > im->boundsW || im->h > im->boundsH)) {
    // The compositor leaves the size to us: fit the requested size into the usable
    // area it announced (screen minus panels and docks).
    im->w = std::min(im->w, im->boundsW);
    im->h = std::min(im->h, im->boundsH);
  }
  bool first = !im->configured;
  im->configured = true;
  im->dirty = true;
  if ((resized || first) && im->self->onResize) im->self->onResize(im->w, im->h);
}
const xdg_surface_listener kXs = {xsConfigure};

void topConfigure(void* d, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
  I(d)->pendingW = w;
  I(d)->pendingH = h;
}
void topClose(void* d, xdg_toplevel*) {
  auto* im = I(d);
  if (im->self->onClose) im->self->onClose();
  else im->running = false;
}
void topBounds(void* d, xdg_toplevel*, int32_t w, int32_t h) {
  I(d)->boundsW = w;
  I(d)->boundsH = h;
}
void topCaps(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener kTop = {topConfigure, topClose, topBounds, topCaps};

void frameDone(void* d, wl_callback* cb, uint32_t) {
  wl_callback_destroy(cb);
  I(d)->frameCb = nullptr;
}
const wl_callback_listener kFrame = {frameDone};

// --- pointer --------------------------------------------------------------------------
void pEnter(void* d, wl_pointer*, uint32_t serial, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
  auto* im = I(d);
  im->enterSerial = serial;
  im->px = (float)wl_fixed_to_double(x);
  im->py = (float)wl_fixed_to_double(y);
  im->applyCursor();
  if (im->self->onMouseMove) im->self->onMouseMove(im->px, im->py, im->mods);
}
void pLeave(void* d, wl_pointer*, uint32_t, wl_surface*) {
  if (I(d)->self->onMouseLeave) I(d)->self->onMouseLeave();
}
void pMotion(void* d, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
  auto* im = I(d);
  im->px = (float)wl_fixed_to_double(x);
  im->py = (float)wl_fixed_to_double(y);
  if (im->self->onMouseMove) im->self->onMouseMove(im->px, im->py, im->mods);
}
void pButton(void* d, wl_pointer*, uint32_t serial, uint32_t, uint32_t button, uint32_t state) {
  auto* im = I(d);
  im->serial = serial;
  int b = -1;
  switch (button) {
    case BTN_LEFT: b = MouseLeft; break;
    case BTN_RIGHT: b = MouseRight; break;
    case BTN_MIDDLE: b = MouseMiddle; break;
    case BTN_SIDE: case BTN_BACK: b = MouseBack; break;
    case BTN_EXTRA: case BTN_FORWARD: b = MouseForward; break;
    default: return;
  }
  if (im->self->onMouseButton)
    im->self->onMouseButton(b, state == WL_POINTER_BUTTON_STATE_PRESSED, im->px, im->py, im->mods);
}
void pAxis(void* d, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t v) {
  auto* im = I(d);
  if (!im->self->onWheel) return;
  // ~10 units per wheel notch; Wayland's positive axis scrolls down, Windows' wheel up.
  float notches = (float)(-wl_fixed_to_double(v) / 10.0);
  im->self->onWheel(notches, axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL, im->px, im->py, im->mods);
}
void pFrame(void*, wl_pointer*) {}
void pAxisSource(void*, wl_pointer*, uint32_t) {}
void pAxisStop(void*, wl_pointer*, uint32_t, uint32_t) {}
void pAxisDiscrete(void*, wl_pointer*, uint32_t, int32_t) {}
void pAxis120(void*, wl_pointer*, uint32_t, int32_t) {}
void pAxisDir(void*, wl_pointer*, uint32_t, uint32_t) {}
const wl_pointer_listener kPtr = {pEnter, pLeave, pMotion, pButton, pAxis, pFrame,
                                  pAxisSource, pAxisStop, pAxisDiscrete, pAxis120, pAxisDir};

// --- keyboard -------------------------------------------------------------------------
void kKeymap(void* d, wl_keyboard*, uint32_t fmt, int32_t fd, uint32_t size) {
  auto* im = I(d);
  if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
  char* map = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
  close(fd);
  if (map == MAP_FAILED) return;
  xkb_keymap* km = xkb_keymap_new_from_string(im->xctx, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(map, size);
  if (!km) return;
  if (im->xstate) xkb_state_unref(im->xstate);
  if (im->keymap) xkb_keymap_unref(im->keymap);
  im->keymap = km;
  im->xstate = xkb_state_new(km);
}
void kEnter(void* d, wl_keyboard*, uint32_t serial, wl_surface*, wl_array*) {
  auto* im = I(d);
  im->serial = serial;
  im->focused = true;
  if (im->self->onFocus) im->self->onFocus(true);
}
void kLeave(void* d, wl_keyboard*, uint32_t, wl_surface*) {
  auto* im = I(d);
  im->repeatKey = 0;
  ++im->keySeq;
  im->held.clear();  // releases while another window has focus never reach us
  itimerspec off{};
  timerfd_settime(im->repeatFd, 0, &off, nullptr);
  im->focused = false;
  if (im->self->onFocus) im->self->onFocus(false);
}
void kKey(void* d, wl_keyboard*, uint32_t serial, uint32_t, uint32_t key, uint32_t state) {
  I(d)->serial = serial;
  I(d)->keyEvent(key + 8, state == WL_KEYBOARD_KEY_STATE_PRESSED);
}
void kMods(void* d, wl_keyboard*, uint32_t, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
  auto* im = I(d);
  if (!im->xstate) return;
  xkb_state_update_mask(im->xstate, dep, lat, lock, 0, 0, group);
  uint32_t m = 0;
  auto on = [&](const char* name) { return xkb_state_mod_name_is_active(im->xstate, name, XKB_STATE_MODS_EFFECTIVE) > 0; };
  if (on(XKB_MOD_NAME_SHIFT)) m |= ModShift;
  if (on(XKB_MOD_NAME_CTRL)) m |= ModCtrl;
  if (on(XKB_MOD_NAME_ALT)) m |= ModAlt;
  if (on(XKB_MOD_NAME_LOGO)) m |= ModSuper;
  im->mods = m;
}
void kRepeat(void* d, wl_keyboard*, int32_t rate, int32_t delay) {
  I(d)->repeatRate = rate;
  I(d)->repeatDelay = delay;
}
const wl_keyboard_listener kKbd = {kKeymap, kEnter, kLeave, kKey, kMods, kRepeat};

// --- clipboard and drag and drop -------------------------------------------------------
void offerOffer(void* d, wl_data_offer* o, const char* mime) { I(d)->offers[o].push_back(mime); }
void offerActions(void*, wl_data_offer*, uint32_t) {}
void offerAction(void*, wl_data_offer*, uint32_t) {}
const wl_data_offer_listener kOffer = {offerOffer, offerActions, offerAction};

std::string readOffer(Window::Impl* im, wl_data_offer* offer, const char* mime) {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) < 0) return {};
  wl_data_offer_receive(offer, mime, fds[1]);
  close(fds[1]);
  wl_display_flush(im->dpy);
  std::string out;
  char buf[8192];
  for (;;) {
    pollfd p{fds[0], POLLIN, 0};
    if (poll(&p, 1, 2000) <= 0) break;  // the owner never answered
    ssize_t n = read(fds[0], buf, sizeof buf);
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  close(fds[0]);
  return out;
}

bool offerHas(Window::Impl* im, wl_data_offer* o, const char* mime) {
  auto it = im->offers.find(o);
  if (it == im->offers.end()) return false;
  return std::find(it->second.begin(), it->second.end(), mime) != it->second.end();
}

std::vector<std::string> parseUriList(const std::string& list) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos < list.size()) {
    size_t nl = list.find('\n', pos);
    std::string line = list.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = nl == std::string::npos ? list.size() : nl + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line[0] == '#' || line.rfind("file://", 0) != 0) continue;
    std::string path;
    size_t i = 7;
    if (i < line.size() && line[i] != '/') i = line.find('/', i);  // file://host/path
    for (; i < line.size(); ++i) {
      if (line[i] == '%' && i + 2 < line.size()) {
        path += static_cast<char>(std::stoi(line.substr(i + 1, 2), nullptr, 16));
        i += 2;
      } else {
        path += line[i];
      }
    }
    out.push_back(path);
  }
  return out;
}

void ddOffer(void* d, wl_data_device*, wl_data_offer* o) {
  I(d)->offers[o] = {};
  wl_data_offer_add_listener(o, &kOffer, d);
}
void ddEnter(void* d, wl_data_device*, uint32_t serial, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer* o) {
  auto* im = I(d);
  im->dndOffer = o;
  if (o && offerHas(im, o, "text/uri-list")) {
    wl_data_offer_accept(o, serial, "text/uri-list");
    wl_data_offer_set_actions(o, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
  } else if (o) {
    wl_data_offer_accept(o, serial, nullptr);
  }
}
void ddLeave(void* d, wl_data_device*) {
  auto* im = I(d);
  if (im->dndOffer) {
    im->offers.erase(im->dndOffer);
    wl_data_offer_destroy(im->dndOffer);
    im->dndOffer = nullptr;
  }
}
void ddMotion(void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {}
void ddDrop(void* d, wl_data_device*) {
  auto* im = I(d);
  wl_data_offer* o = im->dndOffer;
  im->dndOffer = nullptr;
  if (!o) return;
  if (offerHas(im, o, "text/uri-list")) {
    auto paths = parseUriList(readOffer(im, o, "text/uri-list"));
    wl_data_offer_finish(o);
    if (!paths.empty() && im->self->onDropFiles) im->self->onDropFiles(paths);
  }
  im->offers.erase(o);
  wl_data_offer_destroy(o);
}
void ddSelection(void* d, wl_data_device*, wl_data_offer* o) {
  auto* im = I(d);
  if (im->selection && im->selection != o) {
    im->offers.erase(im->selection);
    wl_data_offer_destroy(im->selection);
  }
  im->selection = o;
}
const wl_data_device_listener kDd = {ddOffer, ddEnter, ddLeave, ddMotion, ddDrop, ddSelection};

void srcTarget(void*, wl_data_source*, const char*) {}
void srcSend(void* d, wl_data_source*, const char*, int32_t fd) {
  const std::string& t = I(d)->clipText;
  size_t off = 0;
  while (off < t.size()) {
    ssize_t n = write(fd, t.data() + off, t.size() - off);
    if (n <= 0) break;
    off += static_cast<size_t>(n);
  }
  close(fd);
}
void srcCancelled(void* d, wl_data_source* s) {
  auto* im = I(d);
  if (im->source == s) im->source = nullptr;
  wl_data_source_destroy(s);
}
void srcDndDrop(void*, wl_data_source*) {}
void srcDndFinished(void*, wl_data_source*) {}
void srcAction(void*, wl_data_source*, uint32_t) {}
const wl_data_source_listener kSrc = {srcTarget, srcSend, srcCancelled, srcDndDrop, srcDndFinished, srcAction};

void seatCaps(void* d, wl_seat* s, uint32_t caps) {
  auto* im = I(d);
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !im->ptr) {
    im->ptr = wl_seat_get_pointer(s);
    wl_pointer_add_listener(im->ptr, &kPtr, d);
    if (im->cursorMgr) im->cursorDev = wp_cursor_shape_manager_v1_get_pointer(im->cursorMgr, im->ptr);
  }
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !im->kbd) {
    im->kbd = wl_seat_get_keyboard(s);
    wl_keyboard_add_listener(im->kbd, &kKbd, d);
  }
}
void seatName(void*, wl_seat*, const char*) {}
const wl_seat_listener kSeat = {seatCaps, seatName};

void regGlobal(void* d, wl_registry* r, uint32_t name, const char* iface, uint32_t ver) {
  auto* im = I(d);
  std::string i = iface;
  auto vmin = [](uint32_t a, uint32_t b) { return a < b ? a : b; };
  if (i == wl_compositor_interface.name)
    im->comp = static_cast<wl_compositor*>(wl_registry_bind(r, name, &wl_compositor_interface, vmin(ver, 4)));
  else if (i == wl_shm_interface.name)
    im->shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
  else if (i == xdg_wm_base_interface.name) {
    im->wm = static_cast<xdg_wm_base*>(wl_registry_bind(r, name, &xdg_wm_base_interface, vmin(ver, 5)));
    xdg_wm_base_add_listener(im->wm, &kWm, d);
  } else if (i == wl_seat_interface.name && !im->seat) {
    im->seat = static_cast<wl_seat*>(wl_registry_bind(r, name, &wl_seat_interface, vmin(ver, 7)));
    wl_seat_add_listener(im->seat, &kSeat, d);
  } else if (i == wl_data_device_manager_interface.name)
    im->ddm = static_cast<wl_data_device_manager*>(
        wl_registry_bind(r, name, &wl_data_device_manager_interface, vmin(ver, 3)));
  else if (i == zxdg_decoration_manager_v1_interface.name)
    im->decoMgr = static_cast<zxdg_decoration_manager_v1*>(
        wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1));
  else if (i == wp_cursor_shape_manager_v1_interface.name)
    im->cursorMgr = static_cast<wp_cursor_shape_manager_v1*>(
        wl_registry_bind(r, name, &wp_cursor_shape_manager_v1_interface, 1));
}
void regRemove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kReg = {regGlobal, regRemove};
}  // namespace

// ---------------------------------------------------------------------------------------
void Window::Impl::applyCursor() {
  if (!cursorDev || !enterSerial) return;
  wp_cursor_shape_device_v1_shape s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
  switch (cursor) {
    case Cursor::IBeam: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT; break;
    case Cursor::Hand: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER; break;
    case Cursor::ResizeH: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE; break;
    case Cursor::ResizeV: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE; break;
    case Cursor::Wait: s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT; break;
    default: break;
  }
  wp_cursor_shape_device_v1_set_shape(cursorDev, enterSerial, s);
}

void Window::Impl::dispatchKey(uint32_t code, bool repeat) {
  if (!xstate || !keymap) return;
  // The VK comes from the key's unshifted symbol, as on Windows.
  const xkb_keysym_t* syms = nullptr;
  xkb_layout_index_t layout = xkb_state_key_get_layout(xstate, code);
  int n = xkb_keymap_key_get_syms_by_level(keymap, code, layout, 0, &syms);
  unsigned vk = n > 0 ? vkFromKeysym(syms[0]) : 0;
  if (!vk) vk = vkFromKeysym(xkb_state_key_get_one_sym(xstate, code));
  if (self->onKeyDown) self->onKeyDown(vk, mods, repeat);
  // Like WM_CHAR: Enter, Tab, Backspace, Escape and Ctrl+letter arrive as control codes.
  uint32_t cp = xkb_state_key_get_utf32(xstate, code);
  if (cp && self->onChar) self->onChar(static_cast<char32_t>(cp));
}

void Window::Impl::keyEvent(uint32_t code, bool pressed) {
  itimerspec ts{};
  const uint64_t seq = ++keySeq;
  if (pressed) {
    held[code] = true;
    // A new key ends the previous key's repeat before its handler runs.
    repeatKey = 0;
    timerfd_settime(repeatFd, 0, &ts, nullptr);
    dispatchKey(code, false);
    // The handler may have run a modal loop (a file dialog, a menu) that saw this key's
    // release or other keys: repeat only a key that is still the latest and still down.
    if (keySeq != seq || !held[code]) return;
    if (keymap && xkb_keymap_key_repeats(keymap, code) && repeatRate > 0) {
      repeatKey = code;
      ts.it_value.tv_sec = repeatDelay / 1000;
      ts.it_value.tv_nsec = static_cast<long>(repeatDelay % 1000) * 1000000L;
      ts.it_interval.tv_nsec = 1000000000L / repeatRate;
    } else {
      repeatKey = 0;
    }
    timerfd_settime(repeatFd, 0, &ts, nullptr);
    return;
  }
  held.erase(code);
  if (code == repeatKey) {
    repeatKey = 0;
    timerfd_settime(repeatFd, 0, &ts, nullptr);
  }
  if (self->onKeyUp && xstate) {
    const xkb_keysym_t* syms = nullptr;
    int n = xkb_keymap_key_get_syms_by_level(keymap, code, xkb_state_key_get_layout(xstate, code), 0, &syms);
    if (n > 0) self->onKeyUp(vkFromKeysym(syms[0]), mods);
  }
}

void Window::Impl::freeBuf(Buf& b) {
  if (b.cs) cairo_surface_destroy(b.cs);
  if (b.b) wl_buffer_destroy(b.b);
  if (b.data) munmap(b.data, b.size);
  b = Buf{};
}

void Window::Impl::draw() {
  if (!configured || !dirty || frameCb || w <= 0 || h <= 0) return;
  Buf* b = nullptr;
  for (auto& c : bufs)
    if (!c.busy) {
      b = &c;
      break;
    }
  if (!b) return;
  if (b->w != w || b->h != h) {
    freeBuf(*b);
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, w);
    b->size = static_cast<size_t>(stride) * h;
    int fd = memfd_create("verso-buffer", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(b->size)) < 0) {
      if (fd >= 0) close(fd);
      return;
    }
    b->data = mmap(nullptr, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (b->data == MAP_FAILED) {
      close(fd);
      b->data = nullptr;
      return;
    }
    wl_shm_pool* pool = wl_shm_create_pool(shm, fd, static_cast<int32_t>(b->size));
    b->b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b->b, &kBuf, b);
    b->cs = cairo_image_surface_create_for_data(static_cast<unsigned char*>(b->data), CAIRO_FORMAT_ARGB32, w, h, stride);
    b->w = w;
    b->h = h;
  }
  dirty = false;
  cairo_t* cr = cairo_create(b->cs);
  if (self->onPaint) self->onPaint(cr, w, h);
  cairo_destroy(cr);
  cairo_surface_flush(b->cs);
  frameCb = wl_surface_frame(surf);
  wl_callback_add_listener(frameCb, &kFrame, this);
  wl_surface_attach(surf, b->b, 0, 0);
  wl_surface_damage_buffer(surf, 0, 0, w, h);
  wl_surface_commit(surf);
  b->busy = true;
}

// ---------------------------------------------------------------------------------------
Window::Window() : d_(std::make_unique<Impl>()) {
  d_->self = this;
  if (!gMain) gMain = this;
}

Window::~Window() {
  for (auto& [id, t] : d_->timers) close(t.fd);
  if (d_->repeatFd >= 0) close(d_->repeatFd);
  if (d_->postFd >= 0) close(d_->postFd);
  for (auto& b : d_->bufs) d_->freeBuf(b);
  if (d_->dpy) wl_display_disconnect(d_->dpy);
  if (gMain == this) gMain = nullptr;
}

bool Window::create(const std::string& title, const std::string& appId, int w, int h) {
  auto* im = d_.get();
  im->dpy = wl_display_connect(nullptr);
  if (!im->dpy) return false;
  im->xctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  im->repeatFd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  im->postFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  im->reg = wl_display_get_registry(im->dpy);
  wl_registry_add_listener(im->reg, &kReg, im);
  wl_display_roundtrip(im->dpy);
  wl_display_roundtrip(im->dpy);
  if (!im->comp || !im->shm || !im->wm) return false;
  if (im->ddm && im->seat) {
    im->ddev = wl_data_device_manager_get_data_device(im->ddm, im->seat);
    wl_data_device_add_listener(im->ddev, &kDd, im);
  }
  im->w = w;
  im->h = h;
  im->surf = wl_compositor_create_surface(im->comp);
  im->xsurf = xdg_wm_base_get_xdg_surface(im->wm, im->surf);
  xdg_surface_add_listener(im->xsurf, &kXs, im);
  im->top = xdg_surface_get_toplevel(im->xsurf);
  xdg_toplevel_add_listener(im->top, &kTop, im);
  xdg_toplevel_set_title(im->top, title.c_str());
  xdg_toplevel_set_app_id(im->top, appId.c_str());
  xdg_toplevel_set_min_size(im->top, 480, 300);
  if (im->decoMgr) {
    auto* deco = zxdg_decoration_manager_v1_get_toplevel_decoration(im->decoMgr, im->top);
    zxdg_toplevel_decoration_v1_set_mode(deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
  }
  wl_surface_commit(im->surf);
  wl_display_roundtrip(im->dpy);
  return true;
}

void Window::setTitle(const std::string& t) {
  if (d_->top) xdg_toplevel_set_title(d_->top, t.c_str());
}

void Window::setFullscreen(bool on) {
  if (!d_->top || on == d_->fullscreen) return;
  d_->fullscreen = on;
  if (on) xdg_toplevel_set_fullscreen(d_->top, nullptr);
  else xdg_toplevel_unset_fullscreen(d_->top);
}

void Window::setCursor(Cursor c) {
  if (c == d_->cursor) return;
  d_->cursor = c;
  d_->applyCursor();
}

void Window::invalidate() { d_->dirty = true; }
int Window::width() const { return d_->w; }
int Window::height() const { return d_->h; }
uint32_t Window::mods() const { return d_->mods; }
bool Window::active() const { return d_->focused; }

void Window::setClipboard(const std::string& utf8) {
  auto* im = d_.get();
  if (!im->ddm || !im->ddev) return;
  im->clipText = utf8;
  if (im->source) wl_data_source_destroy(im->source);
  im->source = wl_data_device_manager_create_data_source(im->ddm);
  wl_data_source_add_listener(im->source, &kSrc, im);
  for (const char* m : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT"})
    wl_data_source_offer(im->source, m);
  wl_data_device_set_selection(im->ddev, im->source, im->serial);
}

bool Window::hasClipboardText() const {
  auto* im = d_.get();
  if (im->source) return true;
  if (!im->selection) return false;
  for (const char* m : {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT"})
    if (offerHas(im, im->selection, m)) return true;
  return false;
}

std::string Window::clipboardText() {
  auto* im = d_.get();
  if (im->source) return im->clipText;  // we own the clipboard
  if (!im->selection) return {};
  for (const char* m : {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT"})
    if (offerHas(im, im->selection, m)) return readOffer(im, im->selection, m);
  return {};
}

void Window::quit(int code) {
  d_->exitCode = code;
  d_->running = false;
}

void Window::setTimer(uintptr_t id, unsigned ms, bool repeat, std::function<void()> fn) {
  killTimer(id);
  int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  if (fd < 0) return;
  itimerspec ts{};
  ts.it_value.tv_sec = ms / 1000;
  ts.it_value.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
  if (ms == 0) ts.it_value.tv_nsec = 1;
  if (repeat) ts.it_interval = ts.it_value;
  timerfd_settime(fd, 0, &ts, nullptr);
  d_->timers[id] = {fd, repeat, std::move(fn)};
}

void Window::killTimer(uintptr_t id) {
  auto it = d_->timers.find(id);
  if (it == d_->timers.end()) return;
  close(it->second.fd);
  d_->timers.erase(it);
}

void Window::addWatch(int fd, std::function<void()> fn) { d_->watches[fd] = std::move(fn); }
void Window::removeWatch(int fd) { d_->watches.erase(fd); }

void Window::post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(d_->postMu);
    d_->posted.push_back(std::move(fn));
  }
  uint64_t one = 1;
  if (d_->postFd >= 0) (void)!write(d_->postFd, &one, sizeof one);
}

bool Window::running() const { return d_->running; }

int Window::run() {
  while (d_->running && step(-1)) {
  }
  return d_->exitCode;
}

bool Window::step(int timeoutMs) {
  auto* im = d_.get();
  {
    im->draw();
    while (wl_display_prepare_read(im->dpy) != 0) wl_display_dispatch_pending(im->dpy);
    wl_display_flush(im->dpy);
    std::vector<pollfd> fds;
    fds.push_back({wl_display_get_fd(im->dpy), POLLIN, 0});
    fds.push_back({im->repeatFd, POLLIN, 0});
    fds.push_back({im->postFd, POLLIN, 0});
    std::vector<uintptr_t> timerIds;
    for (auto& [id, t] : im->timers) {
      fds.push_back({t.fd, POLLIN, 0});
      timerIds.push_back(id);
    }
    size_t watchBase = fds.size();
    std::vector<int> watchFds;
    for (auto& [fd, fn] : im->watches) {
      fds.push_back({fd, POLLIN, 0});
      watchFds.push_back(fd);
    }
    // Repaint promptly when something is pending but the frame callback is missing.
    int timeout = (im->dirty && !im->frameCb) ? 0 : timeoutMs;
    if (poll(fds.data(), fds.size(), timeout) < 0 && errno != EINTR) {
      wl_display_cancel_read(im->dpy);
      im->running = false;
      return false;
    }
    if (fds[0].revents & POLLIN) {
      if (wl_display_read_events(im->dpy) < 0) {
        im->running = false;
        return false;
      }
    } else {
      wl_display_cancel_read(im->dpy);
    }
    if ((fds[0].revents & (POLLERR | POLLHUP)) || wl_display_dispatch_pending(im->dpy) < 0) {
      im->running = false;  // the compositor went away
      return false;
    }
    if (fds[1].revents & POLLIN) {
      uint64_t n;
      if (read(im->repeatFd, &n, sizeof n) > 0 && im->repeatKey) im->dispatchKey(im->repeatKey, true);
    }
    if (fds[2].revents & POLLIN) {
      uint64_t n;
      (void)!read(im->postFd, &n, sizeof n);
      std::vector<std::function<void()>> work;
      {
        std::lock_guard<std::mutex> lock(im->postMu);
        work.swap(im->posted);
      }
      for (auto& fn : work) fn();
    }
    for (size_t k = 0; k < timerIds.size(); ++k) {
      if (!(fds[3 + k].revents & POLLIN)) continue;
      auto it = im->timers.find(timerIds[k]);
      if (it == im->timers.end()) continue;
      uint64_t n;
      (void)!read(it->second.fd, &n, sizeof n);
      auto fn = it->second.fn;  // may kill or reset its own timer
      if (!it->second.repeat) killTimer(timerIds[k]);
      fn();
    }
    for (size_t k = 0; k < watchFds.size(); ++k) {
      if (!(fds[watchBase + k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      auto it = im->watches.find(watchFds[k]);
      if (it == im->watches.end()) continue;
      auto fn = it->second;  // may remove itself
      fn();
    }
  }
  return im->running;
}

}  // namespace verso::platform
