#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "app/Gfx.h"
#include "app/MarkdownImages.h"
#include "app/Theme.h"
#include "core/Markdown.h"

namespace verso {

// Renders Markdown for Verso's MD mode (Split and Read views).
class MarkdownPreview {
 public:
  // Re-parses and re-lays out only when an input changed.
  // `fullWidth` lays text out across the whole area instead of a readable column.
  // `noteRoot` is the notes collection (vault) used to resolve wiki links and embeds.
  void update(Gfx& g, const Theme& theme, std::wstring_view text, uint64_t revision, float areaWidth,
              const std::wstring& baseDir, const std::wstring& noteRoot, float zoom, bool fullWidth);
  void paint(Gfx& g, const Theme& theme, const RectF& area, float scrollY, float mouseX, float mouseY);

  float maxScroll(const RectF& area) const;
  struct Link {
    std::wstring url;   // as written in the document
    bool wiki = false;  // a [[wiki link]]: url names a note ("Page#Heading")
  };
  // The link under (x, y); its url is empty when there is none.
  Link linkAt(float x, float y, const RectF& area, float scrollY) const;
  float yForLine(double line) const;  // scroll sync: document y of a source line
  double lineAtY(float y) const;      // inverse of yForLine
  // Document y of the heading with this anchor (GitHub style, case-insensitive), or -1.
  float yForAnchor(const std::wstring& anchor) const;

  MarkdownImages& images() { return images_; }
  // Preview scroll offset that puts a source line at the top, and the inverse.
  float scrollForLine(double line) const;
  double lineForScroll(float scroll) const;

 private:
  struct Box {
    float l = 0, t = 0, r = 0, b = 0;
  };
  struct LinkBox {
    Box box;
    Link link;
  };
  struct TextBlock {
    ComPtr<IDWriteTextLayout> layout;
    std::vector<Box> codeBoxes;  // inline code backgrounds, layout-relative
    std::vector<LinkBox> links;  // layout-relative
    float w = 0, h = 0;
    float baseline = 0;  // of the first line, which a tall inline image moves down
  };
  struct Cell {
    TextBlock text;
    float x = 0, y = 0, w = 0;  // item-relative cell box
    int align = 0;
  };
  struct Item {
    md::Block block;
    float x = 0, y = 0, w = 0, h = 0;  // document coordinates (x relative to the content column)
    TextBlock text;
    std::wstring marker;  // list bullet or number
    std::vector<Cell> cells;
    std::vector<float> colX, rowY;  // table grid lines (item-relative)
  };

  void layoutAll(Gfx& g, const Theme& theme);
  // `align` positions the text (and its link and code boxes) within `maxW`.
  TextBlock makeText(Gfx& g, const Theme& theme, const md::Inline& in, IDWriteTextFormat* fmt, float maxW,
                     bool allBold = false, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
  void layoutTable(Gfx& g, const Theme& theme, Item& item, float maxW);
  void layoutCode(Gfx& g, const Theme& theme, Item& item, float maxW);
  void paintText(Gfx& g, const Theme& theme, const TextBlock& t, float x, float y, Color base, float mouseX,
                 float mouseY);
  float contentLeft(const RectF& area) const;

  // Inputs of the current layout.
  std::wstring text_;
  uint64_t revision_ = UINT64_MAX;
  float areaWidth_ = -1, zoom_ = 0;
  bool fullWidth_ = false;
  bool dark_ = true;
  std::wstring baseDir_, noteRoot_;
  uint64_t generation_ = UINT64_MAX;

  float width_ = 0, height_ = 0;
  std::vector<Item> items_;
  std::vector<std::wstring> anchors_;  // heading anchors, in heading order
  ComPtr<IDWriteTextFormat> body_, heading_[6];
  MarkdownImages images_;
  uint64_t imagesRevision_ = 0;
};

}  // namespace verso
