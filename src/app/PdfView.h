#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "app/Gfx.h"
#include "app/Theme.h"

namespace verso {

inline constexpr UINT WM_APP_PDF_READY = WM_APP + 3;

// True when bytes start with a PDF file header ("%PDF-"). Readers also accept the
// header a little later, but text that merely mentions "%PDF-" must stay text.
bool isPdfData(std::string_view bytes);

// A read-only rendering of a PDF file using the Windows PDF engine (Windows.Data.Pdf).
// The document loads and pages rasterize on a worker thread; it posts `message` to
// `notify` when work finishes, and the UI thread then calls takeResults().
class PdfView {
 public:
  enum class State { Loading, Ready, Failed };

  PdfView(const std::wstring& path, HWND notify, UINT message);
  ~PdfView();
  PdfView(const PdfView&) = delete;
  PdfView& operator=(const PdfView&) = delete;

  State state() const { return state_; }
  const std::wstring& error() const { return error_; }
  int pageCount() const { return static_cast<int>(pages_.size()); }

  // UI thread: applies finished work. Returns true when the view changed.
  bool takeResults();

  void paint(Gfx& g, const Theme& theme, const RectF& area);

  // View state, owned by the UI thread.
  float scrollX = 0, scrollY = 0;
  float zoom = 1.0f;  // relative to fit-to-width

  void clampScroll(const RectF& area);
  void scrollBy(const RectF& area, float dx, float dy);
  // Changes zoom while keeping the point at viewport offset `anchorY` in place.
  void setZoom(const RectF& area, float newZoom, float anchorY);
  // 0-based page shown at the viewport's vertical center.
  int currentPage(const RectF& area);

 private:
  struct Page {
    float w = 612, h = 792;  // page size in DIPs (aspect corrected after rendering)
    float top = 0;           // document y of the page (layout)
    ComPtr<ID2D1Bitmap> bitmap;
    int bitmapW = 0;  // pixel width of `bitmap`, or of `pixels` awaiting upload
    int pixelsH = 0;
    std::vector<uint8_t> pixels;  // BGRA (premultiplied) from the worker
    bool failed = false;
  };
  struct Worker;

  void layout(const RectF& area);
  float scale(const RectF& area) const;
  float contentWidth(const RectF& area) const;

  std::shared_ptr<Worker> worker_;
  State state_ = State::Loading;
  std::wstring error_;
  std::vector<Page> pages_;
  float maxPageW_ = 612;

  // Layout cache.
  float layoutAreaW_ = -1, layoutZoom_ = 0;
  bool layoutDirty_ = true;
  float contentH_ = 0;
  uint64_t generation_ = UINT64_MAX;
  std::vector<std::pair<int, int>> lastRequest_;
};

}  // namespace verso
