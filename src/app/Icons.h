#pragma once

#include <string>

#include "app/Gfx.h"

namespace verso {

// Side bar icon categories: folders, then files by kind (source, markup, text,
// CSS, image, binary, other).
enum class Icon { FolderClosed, FolderOpen, Source, Markup, Text, Css, Image, Binary, Default };

Icon iconForFile(const std::wstring& path);

// Draws a monochrome icon inside the 16x16 DIP box at (x, y).
void drawIcon(Gfx& g, Icon icon, float x, float y, Color c);

// Status bar button that toggles the side bar (a window with a left pane).
void drawSidebarToggle(Gfx& g, float x, float y, Color c, bool sidebarShown);

}  // namespace verso
