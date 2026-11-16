#pragma once

#include <windows.h>
#ifdef _WIN32
#include <wincodec.h>
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/Gfx.h"

namespace verso {

inline constexpr UINT WM_APP_MD_IMAGES = WM_APP + 4;

// Images shown by the Markdown preview. This class owns turning an image reference
// into a source (a local file, an http(s) URL or a data: URI), loading the bytes off
// the UI thread, and drawing raster images (WIC) and SVG (Direct2D, with SVG <text>
// drawn by DirectWrite because Direct2D's SVG renderer omits text).
class MarkdownImages {
 public:
  enum class State { Loading, Ready, Failed };
  struct Info {
    std::wstring key;  // resolved source; identifies the image for draw()
    State state = State::Failed;
    float w = 0, h = 0;  // intrinsic size in CSS px (Ready only)
    std::wstring error;
  };

  MarkdownImages();
  ~MarkdownImages();
  MarkdownImages(const MarkdownImages&) = delete;
  MarkdownImages& operator=(const MarkdownImages&) = delete;

  // Finished loads are announced by posting `message` to `hwnd`.
  void setNotify(HWND hwnd, UINT message);
  void setRemoteAllowed(bool allowed);

  // Resolves `src` (as written in the document) against the document's folder and
  // starts loading it when needed. An ![[embed]] (`wiki`) names a file to find
  // anywhere in the notes collection at `root`. Call between beginLayout() and endLayout().
  Info request(const std::wstring& src, const std::wstring& baseDir, const std::wstring& root, bool wiki);
  void beginLayout();
  void endLayout();  // releases images the latest layout no longer uses (keeping a few)

  // UI thread: applies finished loads. Returns true when an image changed state.
  bool takeResults();
  // Increments whenever images change in a way that needs a new layout.
  uint64_t revision() const { return revision_; }

  // Drawing happens from inside DirectWrite layouts, so the target is set per paint.
  void beginPaint(Gfx* g) { paint_ = g; }
  void endPaint() { paint_ = nullptr; }
  void draw(const std::wstring& key, const RectF& r);

 private:
  struct Entry;
  struct Shared;

  void enqueue(const std::wstring& key, Entry& e);
  void drawRaster(Gfx& g, Entry& e, const RectF& r);
  void drawSvg(Gfx& g, Entry& e, const RectF& r);

  std::shared_ptr<Shared> shared_;
  std::unordered_map<std::wstring, std::unique_ptr<Entry>> entries_;
  bool remoteAllowed_ = true;
  uint64_t revision_ = 1;
  uint64_t layoutCounter_ = 0;
  Gfx* paint_ = nullptr;
#ifdef _WIN32
  ComPtr<IWICImagingFactory> wic_;
#endif
};

}  // namespace verso
