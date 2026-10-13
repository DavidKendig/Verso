#include <windows.h>

#include <algorithm>
#include <cmath>

#include "app/App.h"

namespace verso {

bool App::editBlocked() {
  Document* d = doc();
  return markdownReadOnly() || (d && (d->pdf || d->binary));
}

Document* App::openPdf(const std::wstring& path, bool activateIt) {
  Document* target = nullptr;
  if (Document* cur = doc(); cur && cur->path.empty() && !cur->ed.isDirty() && cur->ed.buffer().size() == 0)
    target = cur;
  Document& d = target ? *target : newDocument(false);
  d.path = path;
  configureEditor(d);
  d.ed.setText(L"");
  setLanguage(d, plainTextLanguage(), false);
  d.pdf = std::make_unique<PdfView>(path, hwnd_, WM_APP_PDF_READY);
  d.diskWriteTime = fileWriteTime(path);
  d.diskMissing = false;
  d.scrollX = d.scrollY = 0;
  d.hasGitBaseline = false;
  d.gitHunks.clear();
  gitRefresh();
  addRecent(recentFiles_, path);
  sessionDirty_ = true;
  if (activateIt) {
    for (size_t i = 0; i < docs_.size(); ++i)
      if (docs_[i].get() == &d) activate(i);
  }
  invalidate();
  return &d;
}

void App::onPdfReady() {
  bool changed = false;
  for (auto& d : docs_)
    if (d->pdf && d->pdf->takeResults()) changed = true;
  if (changed) invalidate();
}

void App::paintPdf() {
  Document* d = doc();
  if (!d || !d->pdf || lay_.pdf.empty()) return;
  d->pdf->paint(gfx_, theme_, lay_.pdf);
}

bool App::pdfKey(UINT vk, bool shift) {
  Document* d = doc();
  if (!d || !d->pdf || lay_.pdf.empty() || overlay_ != Overlay::None || focus_ != Focus::Editor) return false;
  const float line = 60.0f, page = std::max(60.0f, lay_.pdf.h() * 0.9f);
  float dx = 0, dy = 0;
  switch (vk) {
    case VK_UP: dy = -line; break;
    case VK_DOWN: dy = line; break;
    case VK_LEFT: dx = -line; break;
    case VK_RIGHT: dx = line; break;
    case VK_PRIOR: dy = -page; break;
    case VK_NEXT: dy = page; break;
    case VK_SPACE: dy = shift ? -page : page; break;
    case VK_HOME: dy = -1e9f; break;
    case VK_END: dy = 1e9f; break;
    default: return false;
  }
  d->pdf->scrollBy(lay_.pdf, dx, dy);
  invalidate();
  return true;
}

void App::zoomPdf(float factor, float anchorY) {
  Document* d = doc();
  if (!d || !d->pdf || lay_.pdf.empty()) return;
  if (anchorY < 0) anchorY = lay_.pdf.h() / 2;
  d->pdf->setZoom(lay_.pdf, factor <= 0 ? 1.0f : d->pdf->zoom * factor, anchorY);
  setStatus(L"Zoom " + std::to_wstring(static_cast<int>(std::lround(d->pdf->zoom * 100))) + L"%", 1200);
  invalidate();
}

std::wstring App::pdfStatusText(Document& d) {
  switch (d.pdf->state()) {
    case PdfView::State::Loading: return L"Opening PDF…";
    case PdfView::State::Failed: return L"Unable to display this PDF";
    case PdfView::State::Ready: break;
  }
  if (d.pdf->pageCount() == 0) return L"No pages";
  return L"Page " + std::to_wstring(d.pdf->currentPage(lay_.pdf) + 1) + L" of " +
         std::to_wstring(d.pdf->pageCount());
}

}  // namespace verso
