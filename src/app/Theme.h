#pragma once

#include "app/Gfx.h"
#include "syntax/Language.h"

namespace verso {

struct Theme {
  bool dark = true;
  Color bg, gutterFg, gutterFgActive, lineHighlight, selection, selectionBorder, caret;
  Color findMatch, findMatchBorder, wordMatchBorder, bracket, guide, ruler, whitespace;
  Color minimapView, scrollThumb, scrollThumbHot;
  Color chrome, tabActive, tabHover, tabText, tabTextActive, tabBorder, accent;
  Color sidebarBg, sidebarText, sidebarTextDim, sidebarSelected, sidebarHover;
  Color statusBg, statusText;
  Color panelBg, inputBg, inputBorder, inputBorderFocus, inputText, inputPlaceholder, inputSelection;
  Color buttonBg, buttonHover, buttonOn, buttonText;
  Color overlayBg, overlayBorder, overlaySelected, overlayText, overlayDetail, overlayMatch, shadow, error;
  Color gitAdded = Color::hex(0x7FB95F), gitModified = Color::hex(0xE0A84E), gitDeleted = Color::hex(0xE06C75);
  Color tokens[static_cast<int>(TokenKind::Count)];
  bool italicComments = true;

  Color token(TokenKind k) const { return tokens[static_cast<int>(k)]; }

  static Theme makeDark() {
    Theme t;
    t.dark = true;
    t.bg = Color::hex(0x23272E);
    t.gutterFg = Color::hex(0x5B6370);
    t.gutterFgActive = Color::hex(0xC2C8D2);
    t.lineHighlight = Color::hex(0x2C313A);
    t.selection = Color::hex(0x3B4A60);
    t.selectionBorder = Color::hex(0x4D5F7A);
    t.caret = Color::hex(0xF5C065);
    t.findMatch = Color::hex(0xF5C065, 0.18f);
    t.findMatchBorder = Color::hex(0xF5C065, 0.85f);
    t.wordMatchBorder = Color::hex(0xA9B4C4, 0.55f);
    t.bracket = Color::hex(0xF5C065);
    t.guide = Color::hex(0xFFFFFF, 0.12f);
    t.ruler = Color::hex(0xFFFFFF, 0.08f);
    t.whitespace = Color::hex(0xFFFFFF, 0.25f);
    t.minimapView = Color::hex(0xFFFFFF, 0.07f);
    t.scrollThumb = Color::hex(0xFFFFFF, 0.13f);
    t.scrollThumbHot = Color::hex(0xFFFFFF, 0.25f);
    t.chrome = Color::hex(0x1A1D22);
    t.tabActive = Color::hex(0x23272E);
    t.tabHover = Color::hex(0x20242A);
    t.tabText = Color::hex(0x8A929E);
    t.tabTextActive = Color::hex(0xE3E7ED);
    t.tabBorder = Color::hex(0x121417);
    t.accent = Color::hex(0x5FB3B3);
    t.sidebarBg = Color::hex(0x1D2025);
    t.sidebarText = Color::hex(0xB4BBC6);
    t.sidebarTextDim = Color::hex(0x6E7682);
    t.sidebarSelected = Color::hex(0x2F3540);
    t.sidebarHover = Color::hex(0x262A31);
    t.statusBg = Color::hex(0x1A1D22);
    t.statusText = Color::hex(0x949BA6);
    t.panelBg = Color::hex(0x1D2025);
    t.inputBg = Color::hex(0x16181C);
    t.inputBorder = Color::hex(0x31363F);
    t.inputBorderFocus = Color::hex(0x5FB3B3);
    t.inputText = Color::hex(0xDDE2E8);
    t.inputPlaceholder = Color::hex(0x5D6570);
    t.inputSelection = Color::hex(0x3B4A60);
    t.buttonBg = Color::hex(0x2A2F37);
    t.buttonHover = Color::hex(0x343A44);
    t.buttonOn = Color::hex(0x3E6B6B);
    t.buttonText = Color::hex(0xC9CFD8);
    t.overlayBg = Color::hex(0x272B33);
    t.overlayBorder = Color::hex(0x3A404B);
    t.overlaySelected = Color::hex(0x343B47);
    t.overlayText = Color::hex(0xD8DDE4);
    t.overlayDetail = Color::hex(0x7C8592);
    t.overlayMatch = Color::hex(0x6CC9C9);
    t.shadow = Color::hex(0x000000, 0.35f);
    t.error = Color::hex(0xE06C75);

    auto set = [&](TokenKind k, uint32_t rgb) { t.tokens[static_cast<int>(k)] = Color::hex(rgb); };
    set(TokenKind::Text, 0xD8DEE6);
    set(TokenKind::Comment, 0x7D8796);
    set(TokenKind::String, 0x9BC97A);
    set(TokenKind::Number, 0xF29E62);
    set(TokenKind::Keyword, 0xC795D8);
    set(TokenKind::Storage, 0xC795D8);
    set(TokenKind::Type, 0x6ECBD0);
    set(TokenKind::Function, 0x72A7E6);
    set(TokenKind::Constant, 0xF29E62);
    set(TokenKind::Operator, 0xEE7E6B);
    set(TokenKind::Punctuation, 0xA7B0BD);
    set(TokenKind::Preprocessor, 0xC795D8);
    set(TokenKind::Tag, 0xEE6D73);
    set(TokenKind::Attribute, 0xF2B865);
    set(TokenKind::Heading, 0x72A7E6);
    set(TokenKind::Emphasis, 0xF2B865);
    set(TokenKind::Link, 0x6ECBD0);
    set(TokenKind::Variable, 0xEE6D73);
    set(TokenKind::Inserted, 0x9BC97A);
    set(TokenKind::Deleted, 0xEE6D73);
    return t;
  }

  static Theme makeLight() {
    Theme t;
    t.dark = false;
    t.bg = Color::hex(0xFBFBFA);
    t.gutterFg = Color::hex(0xA8ADB4);
    t.gutterFgActive = Color::hex(0x3C424A);
    t.lineHighlight = Color::hex(0xF0F1F2);
    t.selection = Color::hex(0xD5E2F4);
    t.selectionBorder = Color::hex(0xB8CBE6);
    t.caret = Color::hex(0x1F6FEB);
    t.findMatch = Color::hex(0xF0B429, 0.25f);
    t.findMatchBorder = Color::hex(0xD18F00);
    t.wordMatchBorder = Color::hex(0x7A8696, 0.6f);
    t.bracket = Color::hex(0xD18F00);
    t.guide = Color::hex(0x000000, 0.12f);
    t.ruler = Color::hex(0x000000, 0.09f);
    t.whitespace = Color::hex(0x000000, 0.25f);
    t.minimapView = Color::hex(0x000000, 0.06f);
    t.scrollThumb = Color::hex(0x000000, 0.15f);
    t.scrollThumbHot = Color::hex(0x000000, 0.28f);
    t.chrome = Color::hex(0xE7E8EA);
    t.tabActive = Color::hex(0xFBFBFA);
    t.tabHover = Color::hex(0xEFF0F1);
    t.tabText = Color::hex(0x6B717A);
    t.tabTextActive = Color::hex(0x202428);
    t.tabBorder = Color::hex(0xD4D6D9);
    t.accent = Color::hex(0x2B8A8A);
    t.sidebarBg = Color::hex(0xEFF0F1);
    t.sidebarText = Color::hex(0x30353C);
    t.sidebarTextDim = Color::hex(0x7C828B);
    t.sidebarSelected = Color::hex(0xD9DEE6);
    t.sidebarHover = Color::hex(0xE4E6E9);
    t.statusBg = Color::hex(0xE7E8EA);
    t.statusText = Color::hex(0x555B63);
    t.panelBg = Color::hex(0xEDEEF0);
    t.inputBg = Color::hex(0xFFFFFF);
    t.inputBorder = Color::hex(0xC9CCD1);
    t.inputBorderFocus = Color::hex(0x2B8A8A);
    t.inputText = Color::hex(0x202428);
    t.inputPlaceholder = Color::hex(0x9AA0A8);
    t.inputSelection = Color::hex(0xCFE0F5);
    t.buttonBg = Color::hex(0xE0E2E5);
    t.buttonHover = Color::hex(0xD3D6DA);
    t.buttonOn = Color::hex(0x9FD0D0);
    t.buttonText = Color::hex(0x30353C);
    t.overlayBg = Color::hex(0xFFFFFF);
    t.overlayBorder = Color::hex(0xC9CCD1);
    t.overlaySelected = Color::hex(0xE3EAF4);
    t.overlayText = Color::hex(0x202428);
    t.overlayDetail = Color::hex(0x7C828B);
    t.overlayMatch = Color::hex(0x1F7A7A);
    t.shadow = Color::hex(0x000000, 0.15f);
    t.error = Color::hex(0xC0392B);

    auto set = [&](TokenKind k, uint32_t rgb) { t.tokens[static_cast<int>(k)] = Color::hex(rgb); };
    set(TokenKind::Text, 0x2B3036);
    set(TokenKind::Comment, 0x8A929C);
    set(TokenKind::String, 0x3E8A2F);
    set(TokenKind::Number, 0xC0581E);
    set(TokenKind::Keyword, 0x9A3FB0);
    set(TokenKind::Storage, 0x9A3FB0);
    set(TokenKind::Type, 0x13808A);
    set(TokenKind::Function, 0x2463B5);
    set(TokenKind::Constant, 0xC0581E);
    set(TokenKind::Operator, 0xC23B2B);
    set(TokenKind::Punctuation, 0x5A626C);
    set(TokenKind::Preprocessor, 0x9A3FB0);
    set(TokenKind::Tag, 0xC2303B);
    set(TokenKind::Attribute, 0xA86B00);
    set(TokenKind::Heading, 0x2463B5);
    set(TokenKind::Emphasis, 0xA86B00);
    set(TokenKind::Link, 0x13808A);
    set(TokenKind::Variable, 0xC2303B);
    set(TokenKind::Inserted, 0x3E8A2F);
    set(TokenKind::Deleted, 0xC2303B);
    return t;
  }
};

}  // namespace verso
