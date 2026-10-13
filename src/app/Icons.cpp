#include "app/Icons.h"

#include <cwctype>

#include "syntax/Language.h"

namespace verso {

namespace {

std::wstring extensionOf(const std::wstring& path) {
  size_t slash = path.find_last_of(L"\\/");
  size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
  std::wstring ext = path.substr(dot + 1);
  for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
  return ext;
}

bool inList(const std::wstring& ext, std::initializer_list<const wchar_t*> list) {
  for (const wchar_t* e : list)
    if (ext == e) return true;
  return false;
}

D2D1_POINT_2F P(float x, float y) { return D2D1::Point2F(x, y); }

}  // namespace

Icon iconForFile(const std::wstring& path) {
  std::wstring ext = extensionOf(path);
  if (inList(ext, {L"png", L"jpg", L"jpeg", L"gif", L"bmp", L"ico", L"webp", L"tif", L"tiff", L"tga", L"psd", L"heic",
                   L"avif"}))
    return Icon::Image;
  if (inList(ext, {L"exe", L"dll", L"obj", L"o", L"lib", L"a", L"so", L"dylib", L"pdb", L"bin", L"dat", L"zip",
                   L"7z", L"rar", L"gz", L"tgz", L"xz", L"bz2", L"tar", L"jar", L"class", L"pyc", L"pyo", L"wasm",
                   L"iso", L"msi", L"cab", L"pdf", L"doc", L"docx", L"xls", L"xlsx", L"ppt", L"pptx", L"ttf",
                   L"otf", L"woff", L"woff2", L"mp3", L"mp4", L"wav", L"ogg", L"mkv", L"avi", L"mov", L"db",
                   L"sqlite", L"idb", L"ilk", L"exp", L"res", L"pch", L"ipch"}))
    return Icon::Binary;
  const Language& lang = detectLanguage(path, L"");
  switch (lang.mode) {
    case LexMode::Markup:
    case LexMode::Markdown:
      return Icon::Markup;
    case LexMode::Plain:
      return inList(ext, {L"txt", L"text", L"log"}) || path.empty() ? Icon::Text : Icon::Default;
    default:
      break;
  }
  if (lang.name == L"CSS" || lang.name == L"SCSS") return Icon::Css;
  if (lang.name == L"Git Ignore") return Icon::Text;
  return Icon::Source;
}

void drawIcon(Gfx& g, Icon icon, float x, float y, Color c) {
  const float w = 1.4f;
  switch (icon) {
    case Icon::FolderClosed: {
      D2D1_POINT_2F pts[] = {P(x + 1, y + 3), P(x + 6, y + 3), P(x + 7.5f, y + 4.5f), P(x + 15, y + 4.5f),
                             P(x + 15, y + 13.5f), P(x + 1, y + 13.5f)};
      g.path(pts, 6, c, w, true, true);
      break;
    }
    case Icon::FolderOpen: {
      D2D1_POINT_2F back[] = {P(x + 1.5f, y + 12), P(x + 1.5f, y + 3), P(x + 6, y + 3), P(x + 7.5f, y + 4.5f),
                              P(x + 13, y + 4.5f), P(x + 13, y + 6.5f)};
      g.path(back, 6, c, w, false, false);
      D2D1_POINT_2F flap[] = {P(x + 3.5f, y + 7), P(x + 15.5f, y + 7), P(x + 13, y + 13.5f), P(x + 1, y + 13.5f)};
      g.path(flap, 4, c, w, true, true);
      break;
    }
    case Icon::Default: {
      D2D1_POINT_2F doc[] = {P(x + 3, y + 1.5f), P(x + 9.5f, y + 1.5f), P(x + 13, y + 5), P(x + 13, y + 14.5f),
                             P(x + 3, y + 14.5f)};
      g.path(doc, 5, c, w, true, false);
      D2D1_POINT_2F fold[] = {P(x + 9.5f, y + 1.5f), P(x + 9.5f, y + 5), P(x + 13, y + 5)};
      g.path(fold, 3, c, w, false, false);
      break;
    }
    case Icon::Text: {
      g.line(x + 2, y + 3, x + 7, y + 3, c, w);
      g.line(x + 4.5f, y + 3, x + 4.5f, y + 8, c, w);
      g.line(x + 9, y + 3, x + 14, y + 3, c, w);
      g.line(x + 9, y + 6.5f, x + 14, y + 6.5f, c, w);
      g.line(x + 2, y + 10, x + 14, y + 10, c, w);
      g.line(x + 2, y + 13.5f, x + 11, y + 13.5f, c, w);
      break;
    }
    case Icon::Markup: {
      D2D1_POINT_2F left[] = {P(x + 6, y + 4), P(x + 2.5f, y + 8), P(x + 6, y + 12)};
      D2D1_POINT_2F right[] = {P(x + 10, y + 4), P(x + 13.5f, y + 8), P(x + 10, y + 12)};
      g.path(left, 3, c, 1.6f, false, false);
      g.path(right, 3, c, 1.6f, false, false);
      break;
    }
    case Icon::Source: {
      g.line(x + 2.5f, y + 13.5f, x + 7, y + 2.5f, c, 1.5f);
      const float cx = x + 11.5f, cy = y + 6, r = 2.8f;
      g.line(cx, cy - r, cx, cy + r, c, 1.4f);
      g.line(cx - r * 0.866f, cy - r * 0.5f, cx + r * 0.866f, cy + r * 0.5f, c, 1.4f);
      g.line(cx - r * 0.866f, cy + r * 0.5f, cx + r * 0.866f, cy - r * 0.5f, c, 1.4f);
      break;
    }
    case Icon::Css: {
      D2D1_POINT_2F l[] = {P(x + 6.5f, y + 2), P(x + 5.2f, y + 2), P(x + 4.6f, y + 2.6f), P(x + 4.6f, y + 6.6f),
                           P(x + 3, y + 8), P(x + 4.6f, y + 9.4f), P(x + 4.6f, y + 13.4f), P(x + 5.2f, y + 14),
                           P(x + 6.5f, y + 14)};
      D2D1_POINT_2F r[] = {P(x + 9.5f, y + 2), P(x + 10.8f, y + 2), P(x + 11.4f, y + 2.6f), P(x + 11.4f, y + 6.6f),
                           P(x + 13, y + 8), P(x + 11.4f, y + 9.4f), P(x + 11.4f, y + 13.4f), P(x + 10.8f, y + 14),
                           P(x + 9.5f, y + 14)};
      g.path(l, 9, c, 1.4f, false, false);
      g.path(r, 9, c, 1.4f, false, false);
      break;
    }
    case Icon::Image: {
      g.strokeRounded({x + 1, y + 2, x + 15, y + 14}, 1.5f, c, w);
      D2D1_POINT_2F hills[] = {P(x + 3, y + 12), P(x + 6.5f, y + 7.5f), P(x + 9, y + 10), P(x + 10.5f, y + 8.5f),
                               P(x + 13, y + 12)};
      g.path(hills, 5, c, w, true, true);
      g.fillEllipse(x + 11, y + 5.2f, 1.3f, 1.3f, c);
      break;
    }
    case Icon::Binary: {
      g.text(L"10", x + 3, y - 0.5f, c, g.iconFormat());
      g.text(L"01", x + 3, y + 7, c, g.iconFormat());
      break;
    }
  }
}

void drawSidebarToggle(Gfx& g, float x, float y, Color c, bool sidebarShown) {
  g.strokeRounded({x + 0.5f, y + 1, x + 15.5f, y + 13}, 2, c, 1.3f);
  if (sidebarShown)
    g.fill({x + 2, y + 2.5f, x + 6.5f, y + 11.5f}, c);
  else
    g.line(x + 6.5f, y + 1.5f, x + 6.5f, y + 12.5f, c, 1.3f);
}

}  // namespace verso
