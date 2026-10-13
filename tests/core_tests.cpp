// Headless tests for the editing core, search, encoding, syntax and utilities.
#include <cstdio>
#include <string>

#include "core/Editor.h"
#include "core/Encoding.h"
#include "core/LineDiff.h"
#include "core/Markdown.h"
#include "core/VtScreen.h"
#include "core/WrapMap.h"
#include "syntax/Highlighter.h"
#include "util/Fuzzy.h"
#include "util/Json.h"

using namespace verso;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++g_checks;                                                            \
    if (!(cond)) {                                                         \
      ++g_failures;                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                      \
  } while (0)

#define CHECK_TEXT(ed, expected)                                                               \
  do {                                                                                         \
    ++g_checks;                                                                                \
    if ((ed).buffer().text() != (expected)) {                                                  \
      ++g_failures;                                                                            \
      std::printf("FAIL %s:%d: text was \"%s\"\n", __FILE__, __LINE__,                         \
                  wideToUtf8((ed).buffer().text()).c_str());                                   \
    }                                                                                          \
  } while (0)

static Editor make(const std::wstring& text, size_t caret = 0) {
  Editor ed;
  ed.setText(text);
  ed.setSelections({Selection::at(caret)}, 0);
  return ed;
}

static void testBuffer() {
  TextBuffer b(L"one\ntwo\nthree");
  CHECK(b.lineCount() == 3);
  CHECK(b.lineText(1) == L"two");
  b.insert(3, L"!\nnew");
  CHECK(b.text() == L"one!\nnew\ntwo\nthree");
  CHECK(b.lineCount() == 4);
  CHECK(b.lineText(1) == L"new");
  b.erase(4, 5);
  CHECK(b.text() == L"one!two\nthree");
  CHECK(b.lineCount() == 2);
  CHECK(b.lineOf(8) == 1);
  CHECK(b.lineStart(1) == 8);
}

static void testTypingAndUndo() {
  Editor ed = make(L"");
  ed.insertText(L"h");
  ed.insertText(L"i");
  ed.insertText(L" ");
  ed.insertText(L"x");
  CHECK_TEXT(ed, L"hi x");
  ed.undo();
  CHECK_TEXT(ed, L"hi");
  ed.undo();
  CHECK_TEXT(ed, L"");
  ed.redo();
  CHECK_TEXT(ed, L"hi");
  CHECK(ed.isDirty());
  ed.markSaved();
  CHECK(!ed.isDirty());
  ed.insertText(L"!");
  CHECK(ed.isDirty());
  ed.undo();
  CHECK(!ed.isDirty());
}

static void testAutoPair() {
  Editor ed = make(L"");
  ed.insertText(L"(");
  CHECK_TEXT(ed, L"()");
  CHECK(ed.selections().primary().caret == 1);
  ed.insertText(L")");
  CHECK_TEXT(ed, L"()");
  CHECK(ed.selections().primary().caret == 2);
  ed.setSelections({Selection::at(1)}, 0);
  ed.backspace();
  CHECK_TEXT(ed, L"");

  Editor q = make(L"don");
  q.setSelections({Selection::at(3)}, 0);
  q.insertText(L"'");
  CHECK_TEXT(q, L"don'");

  Editor w = make(L"word", 0);
  w.setSelections({Selection(0, 4)}, 0);
  w.insertText(L"[");
  CHECK_TEXT(w, L"[word]");
  CHECK(w.selections().primary().begin() == 1 && w.selections().primary().end() == 5);
}

static void testNewlineIndent() {
  Editor ed = make(L"{}", 1);
  ed.newline();
  CHECK_TEXT(ed, L"{\n\t\n}");
  CHECK(ed.selections().primary().caret == 3);

  Editor py = make(L"    if x:", 9);
  py.comments.indentAfterColon = true;
  py.config.translateTabs = true;
  py.newline();
  CHECK_TEXT(py, L"    if x:\n        ");

  Editor trim = make(L"a   ", 4);
  trim.newline();
  CHECK_TEXT(trim, L"a\n");
}

static void testFindUnderExpand() {
  Editor ed = make(L"foo food foo foo", 0);
  ed.findUnderExpand();
  CHECK(ed.selections().size() == 1 && ed.selections().primary().end() == 3);
  ed.findUnderExpand();
  CHECK(ed.selections().size() == 2);
  CHECK(ed.selections().primary().begin() == 9);  // whole word: skips "food"
  ed.findUnderExpand();
  CHECK(ed.selections().size() == 3);
  ed.insertText(L"x");
  CHECK_TEXT(ed, L"x food x x");

  Editor sub = make(L"ab abc ab", 0);
  sub.setSelections({Selection(0, 2)}, 0);  // manual selection: substring search
  sub.findUnderExpand();
  CHECK(sub.selections().size() == 2 && sub.selections().primary().begin() == 3);

  Editor all = make(L"x y x y x", 0);
  all.findAllUnder();
  CHECK(all.selections().size() == 3);
}

static void testLineOps() {
  Editor ed = make(L"a\nb\nc", 2);
  ed.swapLines(true);
  CHECK_TEXT(ed, L"b\na\nc");
  CHECK(ed.selections().primary().caret == 0);
  ed.swapLines(false);
  ed.swapLines(false);
  CHECK_TEXT(ed, L"a\nc\nb");
  CHECK(ed.selections().primary().caret == 4);

  Editor d = make(L"one\ntwo", 1);
  d.duplicate();
  CHECK_TEXT(d, L"one\none\ntwo");
  CHECK(d.selections().primary().caret == 5);
  d.deleteLine();
  CHECK_TEXT(d, L"one\ntwo");

  Editor j = make(L"a\n   b\nc", 0);
  j.joinLines();
  CHECK_TEXT(j, L"a b\nc");

  Editor ind = make(L"x\ny", 0);
  ind.selectAll();
  ind.indent();
  CHECK_TEXT(ind, L"\tx\n\ty");
  ind.unindent();
  CHECK_TEXT(ind, L"x\ny");

  Editor s = make(L"b\nA\nc", 0);
  s.permuteLines(LineOrder::Sort);
  CHECK_TEXT(s, L"A\nb\nc");
}

static void testComments() {
  Editor ed = make(L"a\n  b", 0);
  ed.comments.line = L"#";
  ed.selectAll();
  ed.toggleComment(false);
  CHECK_TEXT(ed, L"# a\n#   b");
  ed.toggleComment(false);
  CHECK_TEXT(ed, L"a\n  b");

  Editor css = make(L"color: red;", 0);
  css.comments.blockStart = L"/*";
  css.comments.blockEnd = L"*/";
  css.toggleComment(false);
  CHECK_TEXT(css, L"/* color: red; */");
  css.toggleComment(false);
  CHECK_TEXT(css, L"color: red;");
}

static void testMultiCursorAndClipboard() {
  Editor ed = make(L"aa\nbb\ncc", 0);
  ed.selectAll();
  ed.splitSelectionIntoLines();
  CHECK(ed.selections().size() == 3);
  ed.move(MoveBy::LineBegin, false, false);
  ed.insertText(L"-");
  CHECK_TEXT(ed, L"-aa\n-bb\n-cc");

  Editor c = make(L"line1\nline2", 2);
  ClipboardText clip = c.copy();
  CHECK(clip.wholeLines && clip.text == L"line1\n");
  c.setSelections({Selection::at(8)}, 0);
  c.paste(clip.text, clip.wholeLines);
  CHECK_TEXT(c, L"line1\nline1\nline2");

  Editor dist = make(L"x\ny", 0);
  dist.setSelections({Selection::at(1), Selection::at(3)}, 0);
  dist.paste(L"1\n2", false);
  CHECK_TEXT(dist, L"x1\ny2");

  Editor cut = make(L"a\nb\nc", 2);
  ClipboardText ct = cut.cut();
  CHECK(ct.text == L"b\n");
  CHECK_TEXT(cut, L"a\nc");

  Editor add = make(L"abc\ndef\nghi", 1);
  add.addCursorLine(true);
  add.addCursorLine(true);
  CHECK(add.selections().size() == 3);
  add.insertText(L"_");
  CHECK_TEXT(add, L"a_bc\nd_ef\ng_hi");
}

static void testMovement() {
  Editor ed = make(L"foo.bar  baz\n\tx", 0);
  ed.move(MoveBy::WordEnds, true, false);
  CHECK(ed.selections().primary().caret == 3);
  ed.move(MoveBy::WordEnds, true, false);
  CHECK(ed.selections().primary().caret == 4);
  ed.move(MoveBy::WordEnds, true, false);
  CHECK(ed.selections().primary().caret == 7);
  ed.move(MoveBy::Words, false, false);
  CHECK(ed.selections().primary().caret == 4);
  CHECK(ed.columnAt(14) == 4);  // "\tx": the x sits after a tab
  CHECK(ed.posAtColumn(1, 4) == 14);

  Editor sub = make(L"camelCaseWord", 0);
  sub.move(MoveBy::SubwordEnds, true, false);
  CHECK(sub.selections().primary().caret == 5);

  Editor home = make(L"    text", 6);
  home.move(MoveBy::LineBegin, false, false);
  CHECK(home.selections().primary().caret == 4);
  home.move(MoveBy::LineBegin, false, false);
  CHECK(home.selections().primary().caret == 0);

  Editor br = make(L"f(a, (b))", 1);
  br.move(MoveBy::Brackets, true, false);
  CHECK(br.selections().primary().caret == 9);
}

static void testSearch() {
  Searcher s;
  CHECK(s.compile(L"([a-z])(\\d)", {true, false, false}));
  Editor ed = make(L"a1 b2");
  CHECK(ed.replaceAll(s, L"$2$1") == 2);
  CHECK_TEXT(ed, L"1a 2b");

  Searcher w;
  w.compile(L"foo", {false, false, true});
  auto ms = w.all(L"Foo food foo");
  CHECK(ms.size() == 2);

  auto prev = w.prev(L"Foo food foo", 12, false);
  CHECK(prev && prev->pos == 9);

  Searcher bad;
  std::wstring err;
  CHECK(!bad.compile(L"(", {true, false, false}, &err));
  CHECK(!err.empty());

  Searcher ml;
  ml.compile(L"^b", {true, true, false});
  CHECK(ml.all(L"a\nb").size() == 1);
}

static void testEncoding() {
  std::string bytes = "a\r\nb\r\n\xC3\xA9";
  DecodedText d = decodeText(bytes, LineEnding::LF);
  CHECK(d.text == L"a\nb\n\u00e9");
  CHECK(d.lineEnding == LineEnding::CRLF);
  CHECK(d.encoding == Encoding::Utf8);
  CHECK(encodeText(d.text, d.encoding, d.lineEnding) == bytes);

  std::string u16 = encodeText(L"hi\n", Encoding::Utf16LE, LineEnding::LF);
  DecodedText d2 = decodeText(u16, LineEnding::CRLF);
  CHECK(d2.encoding == Encoding::Utf16LE && d2.text == L"hi\n" && d2.lineEnding == LineEnding::LF);

  DecodedText latin = decodeText("caf\xE9", LineEnding::CRLF);
  CHECK(latin.encoding == Encoding::Windows1252 && latin.text == L"caf\u00e9");

  // Binary detection: NUL bytes in 8-bit data, but not in UTF-16 (where they are normal).
  CHECK(!d.binary && !d2.binary && !latin.binary);
  CHECK(decodeText(std::string("MZ\x90\0\x03\0", 6), LineEnding::CRLF).binary);
  std::string lateNul(9000, 'a');
  lateNul[8500] = '\0';
  CHECK(!decodeText(lateNul, LineEnding::CRLF).binary);  // only the first 8000 bytes are examined
}

static void testJson() {
  auto v = json::parse(R"({
    // comment
    "font_size": 12, "name": "x\u00e9", "list": [1, 2,], /* block */ "flag": true,
  })");
  CHECK(v.has_value());
  if (v) {
    CHECK(v->getNumber("font_size", 0) == 12);
    CHECK(v->getWString("name") == L"x\u00e9");
    CHECK(v->get("list")->array.size() == 2);
    CHECK(v->getBool("flag", false));
    auto again = json::parse(json::serialize(*v));
    CHECK(again && again->getWString("name") == L"x\u00e9");
  }
  CHECK(!json::parse("{\"a\": }").has_value());
}

static void testFuzzy() {
  auto m = fuzzyMatch(L"fb", L"FooBar");
  CHECK(m && m->positions.size() == 2 && m->positions[0] == 0 && m->positions[1] == 3);
  CHECK(!fuzzyMatch(L"xyz", L"abc"));
  auto a = fuzzyMatchPath(L"edit", L"src/core/Editor.cpp");
  auto b = fuzzyMatchPath(L"edit", L"src/elsewhere/data/item.txt");
  CHECK(a && b && a->score > b->score);
}

static void testHighlighter() {
  const Language* py = languageByName(L"Python");
  CHECK(py != nullptr);
  std::vector<Token> toks;
  uint32_t state = 0;
  tokenizeLine(*py, L"def foo(): # hi", state, toks);
  CHECK(toks.size() >= 3);
  CHECK(toks[0].kind == TokenKind::Storage);
  CHECK(toks[2].kind == TokenKind::Function);
  CHECK(toks.back().kind == TokenKind::Comment);

  const Language* c = languageByName(L"C");
  TextBuffer buf(L"int a; /* start\nstill comment\nend */ int b;");
  SyntaxCache cache;
  cache.reset(c);
  const auto& l1 = cache.tokens(buf, 1);
  CHECK(l1.size() == 1 && l1[0].kind == TokenKind::Comment);
  const auto& l2 = cache.tokens(buf, 2);
  CHECK(l2[0].kind == TokenKind::Comment && l2.back().kind == TokenKind::Punctuation);

  {
    std::vector<Token> ct;
    uint32_t st = 0;
    tokenizeLine(*languageByName(L"C++"), L"wchar_t x = nullptr;", st, ct);
    CHECK(!ct.empty() && ct[0].kind == TokenKind::Type && ct[0].len == 7);
  }
  CHECK(detectLanguage(L"C:\\x\\main.cpp", L"").name == L"C++");
  CHECK(detectLanguage(L"script", L"#!/usr/bin/env python3").name == L"Python");
  CHECK(detectLanguage(L"CMakeLists.txt", L"").name == L"CMake");

  TextBuffer src(L"class A:\n    def run(self):\n        pass\n");
  SyntaxCache pc;
  pc.reset(py);
  auto syms = extractSymbols(src, pc);
  CHECK(syms.size() == 2 && syms[0].name == L"A" && syms[1].name == L"run");

  TextBuffer csrc(L"#include <x>\nint main(int argc) {\n  return 0;\n}\nvoid Foo::bar() {}\n");
  SyntaxCache cc;
  cc.reset(languageByName(L"C++"));
  auto csyms = extractSymbols(csrc, cc);
  CHECK(csyms.size() == 2 && csyms[0].name == L"main" && csyms[1].name == L"Foo::bar");
}

static std::vector<LineHunk> diffText(const std::wstring& a, const std::wstring& b) {
  return diffLines(splitLineViews(a), splitLineViews(b));
}

static void testLineDiff() {
  CHECK(diffText(L"a\nb\nc\n", L"a\nb\nc\n").empty());
  auto add = diffText(L"a\nc\n", L"a\nb\nc\n");
  CHECK(add.size() == 1 && add[0] == (LineHunk{1, 0, 1, 1}));
  auto del = diffText(L"a\nb\nc\n", L"a\nc\n");
  CHECK(del.size() == 1 && del[0] == (LineHunk{1, 1, 1, 0}));
  auto mod = diffText(L"a\nb\nc\n", L"a\nB\nc\n");
  CHECK(mod.size() == 1 && mod[0] == (LineHunk{1, 1, 1, 1}));
  // Two separate changes with shared lines in between.
  auto two = diffText(L"1\n2\n3\n4\n5\n6\n", L"1\nX\n3\n4\n6\nY\n");
  CHECK(two.size() == 3);
  if (two.size() == 3) {
    CHECK(two[0] == (LineHunk{1, 1, 1, 1}));
    CHECK(two[1] == (LineHunk{4, 1, 4, 0}));
    CHECK(two[2] == (LineHunk{6, 0, 5, 1}));
  }
  // Replacing everything and diffing against empty input.
  CHECK(diffText(L"", L"x\ny\n").size() == 1);
  CHECK((diffText(L"x\ny\n", L"") == std::vector<LineHunk>{LineHunk{0, 2, 0, 0}}));
  // Edit distance above the limit collapses into one replacement hunk.
  std::wstring big1, big2;
  for (int i = 0; i < 200; ++i) {
    big1 += L"a" + std::to_wstring(i) + L"\n";
    big2 += L"b" + std::to_wstring(i) + L"\n";
  }
  auto capped = diffLines(splitLineViews(big1), splitLineViews(big2), 10);
  CHECK(capped.size() == 1 && capped[0] == (LineHunk{0, 200, 0, 200}));
  // Interleaved edits keep the matched lines aligned.
  auto inter = diffText(L"a\nb\nc\nd\ne\n", L"a\nx\nc\ny\ne\n");
  CHECK(inter.size() == 2 && inter[0] == (LineHunk{1, 1, 1, 1}) && inter[1] == (LineHunk{3, 1, 3, 1}));
}

static bool hasSpan(const md::Inline& in, const std::wstring& text, uint8_t style) {
  for (const auto& s : in.spans)
    if (in.text.substr(s.start, s.len) == text && s.style == style) return true;
  return false;
}

static void testMarkdown() {
  auto in = md::parseInline(L"a **bold** and *it* `co*de*` ~~gone~~ [link](http://x.y) \\*lit\\*");
  CHECK(in.text == L"a bold and it co*de* gone link *lit*");
  CHECK(hasSpan(in, L"bold", md::Bold));
  CHECK(hasSpan(in, L"it", md::Italic));
  CHECK(hasSpan(in, L"co*de*", md::Code));
  CHECK(hasSpan(in, L"gone", md::Strike));
  CHECK(hasSpan(in, L"link", md::Link));
  auto nested = md::parseInline(L"***both*** and **outer *inner***");
  CHECK(nested.text == L"both and outer inner");
  CHECK(hasSpan(nested, L"both", md::Bold | md::Italic));
  CHECK(hasSpan(nested, L"inner", md::Bold | md::Italic));
  auto snake = md::parseInline(L"snake_case_name stays");
  CHECK(snake.text == L"snake_case_name stays" && snake.spans.empty());
  auto autolink = md::parseInline(L"see https://example.com/a.");
  CHECK(hasSpan(autolink, L"https://example.com/a", md::Link));

  std::wstring doc =
      L"# Title\n"
      L"\n"
      L"Some *text*\n"
      L"continues here.\n"
      L"\n"
      L"- one\n"
      L"  - nested\n"
      L"- [x] done\n"
      L"\n"
      L"1. first\n"
      L"2. second\n"
      L"\n"
      L"> quoted **line**\n"
      L"\n"
      L"```cpp\n"
      L"int x = 0;\n"
      L"```\n"
      L"\n"
      L"| A | B |\n"
      L"|:--|--:|\n"
      L"| 1 | 2 |\n"
      L"\n"
      L"---\n"
      L"Setext\n"
      L"======\n";
  auto blocks = md::parse(doc);
  CHECK(blocks.size() == 12);
  if (blocks.size() == 12) {
    CHECK(blocks[0].kind == md::BlockKind::Heading && blocks[0].level == 1 && blocks[0].content.text == L"Title");
    CHECK(blocks[1].kind == md::BlockKind::Paragraph && blocks[1].content.text == L"Some text continues here.");
    CHECK(blocks[1].line == 2 && blocks[1].lastLine == 3);
    CHECK(blocks[2].kind == md::BlockKind::ListItem && blocks[2].depth == 0 && !blocks[2].ordered);
    CHECK(blocks[3].kind == md::BlockKind::ListItem && blocks[3].depth == 1 && blocks[3].content.text == L"nested");
    CHECK(blocks[4].task == 1 && blocks[4].content.text == L"done");
    CHECK(blocks[5].ordered && blocks[5].number == 1 && blocks[6].number == 2);
    CHECK(blocks[7].quote == 1 && hasSpan(blocks[7].content, L"line", md::Bold));
    CHECK(blocks[8].kind == md::BlockKind::CodeBlock && blocks[8].lang == L"cpp" && blocks[8].code == L"int x = 0;");
    CHECK(blocks[9].kind == md::BlockKind::Table && blocks[9].rows.size() == 2 && blocks[9].align[1] == 2);
    CHECK(blocks[10].kind == md::BlockKind::Rule);
    CHECK(blocks[11].kind == md::BlockKind::Heading && blocks[11].level == 1 && blocks[11].content.text == L"Setext");
  }
  CHECK(md::slug(L"Hello, World!") == L"hello-world");
  auto hard = md::parse(L"line one  \nline two");
  CHECK(hard.size() == 1 && hard[0].content.text == L"line one\nline two");

  // Images: each has its own span carrying its source; linked images keep the link.
  auto imgs = md::parseInline(L"![a](x.png)![a](x.png) [![badge](b.svg)](https://ci) ");
  CHECK(imgs.spans.size() == 3);
  if (imgs.spans.size() == 3) {
    CHECK(imgs.spans[0].style == md::Image && imgs.spans[0].src == L"x.png" && imgs.spans[0].url.empty());
    CHECK(imgs.spans[1].src == L"x.png" && imgs.spans[1].start == 1);
    CHECK(imgs.spans[2].style == (md::Image | md::Link) && imgs.spans[2].src == L"b.svg" &&
          imgs.spans[2].url == L"https://ci");
  }
  // HTML images, links, emphasis, comments and centering layout tags.
  auto html = md::parse(
      L"<p align=\"center\">\n"
      L"  <img src=\"logo.png\" alt=\"Logo\" width=\"200\">\n"
      L"</p>\n"
      L"\n"
      L"<!-- note --> <a href=\"docs/guide.md\">Guide</a> and <b>bold</b><br>next\n");
  CHECK(html.size() == 2);
  if (html.size() == 2) {
    CHECK(html[0].content.centered && html[0].content.spans.size() == 1);
    if (!html[0].content.spans.empty()) {
      const md::Span& sp = html[0].content.spans[0];
      CHECK(sp.style == md::Image && sp.src == L"logo.png" && sp.width == 200 && sp.height == 0);
    }
    CHECK(html[1].content.text == L" Guide and bold\nnext");
    CHECK(hasSpan(html[1].content, L"Guide", md::Link) && hasSpan(html[1].content, L"bold", md::Bold));
  }
  auto htmlOnly = md::parse(L"<div align=\"center\">\n\n![x](y.png)\n\n</div>\n");
  CHECK(htmlOnly.size() == 1);  // the bare layout tags leave no paragraphs
  // Reference links and images, defined after their use and matched case-insensitively.
  auto refs = md::parse(
      L"See [the guide][Guide], [Guide][] and [guide], ![logo][img].\n"
      L"\n"
      L"[guide]: docs/guide.md \"Title\"\n"
      L"[IMG]: <assets/logo one.png>\n"
      L"\n"
      L"```\n"
      L"[notref]: in-code.md\n"
      L"```\n");
  CHECK(refs.size() == 2);
  if (!refs.empty()) {
    const auto& sp = refs[0].content.spans;
    CHECK(refs[0].content.text == L"See the guide, Guide and guide, logo.");
    CHECK(sp.size() == 4);
    if (sp.size() == 4) {
      CHECK(sp[0].url == L"docs/guide.md" && sp[1].url == L"docs/guide.md" && sp[2].url == L"docs/guide.md");
      CHECK(sp[3].style == md::Image && sp[3].src == L"assets/logo one.png");
    }
  }
  if (refs.size() == 2) CHECK(refs[1].kind == md::BlockKind::CodeBlock && refs[1].code == L"[notref]: in-code.md");
  CHECK(md::parseInline(L"[undefined] stays").text == L"[undefined] stays");
  // Wiki links and image embeds.
  auto wiki = md::parseInline(L"See [[Page Name]], [[notes/Other#Part|the part]] and ![[pic.png|120]].");
  CHECK(wiki.text == L"See Page Name, the part and pic.png.");
  CHECK(wiki.spans.size() == 3);
  if (wiki.spans.size() == 3) {
    CHECK(wiki.spans[0].style == (md::Link | md::WikiLink) && wiki.spans[0].url == L"Page Name");
    CHECK(wiki.spans[1].url == L"notes/Other#Part");
    CHECK(wiki.spans[2].style == (md::Image | md::WikiLink) && wiki.spans[2].src == L"pic.png" &&
          wiki.spans[2].width == 120);
  }
  auto wikiTable = md::parse(L"| A | B |\n|---|---|\n| [[x|y]] | 2 |\n");
  CHECK(wikiTable.size() == 1 && wikiTable[0].rows.size() == 2);
  if (wikiTable.size() == 1 && wikiTable[0].rows.size() == 2) {
    CHECK(wikiTable[0].rows[1][0].text == L"y" && wikiTable[0].rows[1][1].text == L"2");
  }
  CHECK(md::parseInline(L"`[[code]]` and [[]]").spans.size() == 1);  // code only; empty target stays text
  // Repeated headings get GitHub's numbered anchors.
  auto heads = md::anchors(md::parse(L"# Setup\n## Setup\n# Other Thing!\n## setup\n"));
  CHECK((heads == std::vector<std::wstring>{L"setup", L"setup-1", L"other-thing", L"setup-2"}));
}

static void testWrapMap() {
  TextBuffer b(L"short\nthe quick brown fox jumps\n  indented words wrap here\nnospaceshereatallxx");
  WrapMap m;
  m.build(b, 4, 10, false);
  CHECK(m.rowCount() == 4 && m.rowOf(b.lineStart(2)) == 2);
  m.build(b, 4, 10, true);
  // "the quick brown fox jumps" -> "the quick " | "brown fox " | "jumps"
  CHECK(m.row(1).line == 1 && b.view(m.row(1).start, m.row(1).end - m.row(1).start) == L"the quick ");
  CHECK(b.view(m.row(2).start, m.row(2).end - m.row(2).start) == L"brown fox ");
  CHECK(b.view(m.row(3).start, m.row(3).end - m.row(3).start) == L"jumps");
  CHECK(m.row(3).startCol == 20);
  // Continuation rows of an indented line hang at its indentation.
  size_t r = m.firstRowOfLine(2);
  CHECK(m.row(r).indent == 0 && m.row(r + 1).indent == 2);
  // A long word with no spaces breaks at the width.
  size_t lr = m.firstRowOfLine(3);
  CHECK(m.row(lr).end - m.row(lr).start == 10);
  CHECK(m.rowOf(m.row(2).start) == 2 && m.rowOf(m.row(2).start - 1) == 1);
  CHECK(m.rowCount() == m.firstRowOfLine(3) + 2);
}

// Regressions found by code review; each case lost or corrupted text before its fix.
static void testRegressions() {
  // Overlapping changes from multi-caret commands no longer delete text.
  Editor j = make(L"a\n  \nb", 0);
  j.selectAll();
  j.joinLines();
  CHECK_TEXT(j, L"a b");
  Editor t = make(L"abcd", 0);
  t.setSelections({Selection::at(1), Selection::at(2)}, 0);
  t.transpose();
  CHECK(t.buffer().size() == 4);  // letters swapped, none lost
  Editor css = make(L"/* x */", 1);
  css.comments.blockStart = L"/*";
  css.comments.blockEnd = L"*/";
  css.setSelections({Selection::at(1), Selection::at(4)}, 0);
  css.toggleComment(false);
  CHECK_TEXT(css, L"x");

  // Regexes see past the searched range; zero-length matches replace.
  Searcher look;
  look.compile(L"(fo)o(?=bar)", {true, true, false});
  Editor la = make(L"foobar");
  CHECK(la.replaceAll(look, L"$1") == 1);
  CHECK_TEXT(la, L"fobar");
  Editor ln = make(L"foobar");
  ln.setSelections({Selection(0, 3)}, 0);
  ln.replaceNext(look, L"$1", true);
  CHECK_TEXT(ln, L"fobar");
  Searcher word;
  word.compile(L"foo\\b", {true, true, false});
  CHECK(!word.prev(L"foobar", 3, false).has_value());
  Searcher caret;
  caret.compile(L"^", {true, true, false});
  Editor pre = make(L"a\nb");
  CHECK(pre.replaceAll(caret, L"// ") == 2);
  CHECK_TEXT(pre, L"// a\n// b");
  CHECK(!caret.next(L"a\nb", 0, false).has_value());  // Find Next skips empty matches

  // JSON: deep nesting fails cleanly; unpaired surrogates become U+FFFD.
  CHECK(!json::parse(std::string(100000, '[')).has_value());
  auto sur = json::parse(R"(["\uD83DA", "😀", "\uDC00"])");
  CHECK(sur && sur->array.size() == 3);
  if (sur && sur->array.size() == 3) {
    CHECK(sur->array[0].asWString() == L"�A");
    CHECK(sur->array[1].asWString() == L"\U0001F600");
    CHECK(sur->array[2].asWString() == L"�");
  }

  // Encodings: characters a code page cannot store are detected before saving.
  CHECK(canEncode(L"café", Encoding::Windows1252));
  CHECK(!canEncode(L"日本", Encoding::Windows1252));
  CHECK(canEncode(L"日本", Encoding::Utf8));
  std::string oddUtf16("\xFF\xFE" "a\0b", 5);
  CHECK(decodeText(oddUtf16, LineEnding::CRLF).binary);

  // Markdown: tabs in indented fences, deep nesting, case-insensitive HTML tags.
  auto fence = md::parse(L"  ```\n  \tindented\n\tfmt.Println()\n  ```\n");
  CHECK(fence.size() == 1 && fence[0].code == L"\tindented\n\tfmt.Println()");
  std::wstring deep(5000, L'[');
  deep += L"x";
  for (int i = 0; i < 5000; ++i) deep += L"](u)";
  CHECK(!md::parseInline(deep).text.empty());  // no stack overflow
  auto tag = md::parseInline(L"<B>bold</b> and <span>x</span>");
  CHECK(tag.text == L"bold and x" && hasSpan(tag, L"bold", md::Bold));

  // Wrapped rows never put the caret inside a surrogate pair.
  std::wstring emoji;
  for (int i = 0; i < 12; ++i) emoji += L"\U0001F600";
  TextBuffer eb(emoji);
  WrapMap wm;
  wm.build(eb, 4, 5, true);
  CHECK(wm.rowCount() > 1);
  if (wm.rowCount() > 1) {
    size_t p = wm.caretOnRow(0, wm.row(0).end);
    wchar_t at = eb.at(p);
    CHECK(!(at >= 0xDC00 && at <= 0xDFFF) && p < wm.row(0).end);
  }

  // PowerShell: $true is a constant and -eq a keyword.
  if (const Language* ps = languageByName(L"PowerShell")) {
    std::vector<Token> toks;
    uint32_t state = 0;
    tokenizeLine(*ps, L"$true -eq 1", state, toks);
    CHECK(!toks.empty() && toks[0].kind == TokenKind::Constant);
    bool eq = false;
    for (const auto& tk : toks) eq = eq || (tk.start == 6 && tk.kind == TokenKind::Keyword);
    CHECK(eq);
  }
}

static std::wstring vtRow(const VtScreen& s, int row) {
  return s.text(s.scrollbackSize() + static_cast<size_t>(row), 0, s.scrollbackSize() + static_cast<size_t>(row),
                s.cols());
}

static void testVtScreen() {
  VtScreen s(10, 3, 100);
  s.write("hello\r\nworld");
  CHECK(vtRow(s, 0) == L"hello" && vtRow(s, 1) == L"world");
  CHECK(s.cursorX() == 5 && s.cursorY() == 1);

  // Colors and attributes.
  s.write("\x1b[1;31mR\x1b[38;5;200mP\x1b[48;2;1;2;3mT\x1b[0mn");
  const VtLine& l = s.line(s.scrollbackSize() + 1);
  CHECK(l.cells[5].ch == U'R' && (l.cells[5].attrs & VtBold) && l.cells[5].fg.kind == VtColor::Palette &&
        l.cells[5].fg.index == 1);
  CHECK(l.cells[6].fg.kind == VtColor::Palette && l.cells[6].fg.index == 200);
  CHECK(l.cells[7].bg.kind == VtColor::Rgb && l.cells[7].bg.rgb == 0x010203);
  CHECK(l.cells[8].attrs == 0 && l.cells[8].fg.kind == VtColor::Default);

  // Cursor positioning and erasing.
  s.write("\x1b[1;3H\x1b[K");
  CHECK(vtRow(s, 0) == L"he" && s.cursorX() == 2 && s.cursorY() == 0);
  s.write("\x1b[2J\x1b[H");
  CHECK(vtRow(s, 0).empty() && vtRow(s, 1).empty());

  // Scrolling moves lines into history; automatic wraps are remembered.
  VtScreen t(5, 2, 100);
  t.write("1\r\n2\r\n3\r\nabcdefgh");
  CHECK(t.scrollbackSize() == 3 && t.linesScrolled() == 3);
  CHECK(t.text(0, 0, 0, 5) == L"1" && t.text(1, 0, 1, 5) == L"2");
  CHECK(t.line(t.lineCount() - 2).wrapped);
  CHECK(t.text(t.lineCount() - 2, 0, t.lineCount() - 1, 5) == L"abcdefgh");

  // The alternate screen has no history and restores the primary screen.
  VtScreen a(8, 2, 100);
  a.write("main\x1b[?1049h\x1b[Halt");
  CHECK(a.altScreen() && vtRow(a, 0) == L"alt" && a.scrollbackSize() == 0);
  a.write("\x1b[?1049l");
  CHECK(!a.altScreen() && vtRow(a, 0) == L"main" && a.cursorX() == 4);

  // Wide characters take two cells; UTF-8 may be split across writes.
  VtScreen w(6, 2, 10);
  std::string han = "\xE4\xB8\xAD";  // U+4E2D
  w.write(han.substr(0, 1));
  w.write(han.substr(1) + "x");
  const VtLine& wl = w.line(0);
  CHECK(wl.cells[0].ch == 0x4E2D && wl.cells[0].width == 2 && wl.cells[1].width == 0 && wl.cells[2].ch == U'x');
  CHECK(vtRow(w, 0) == L"中x" && w.cursorX() == 3);

  // Queries are answered; titles and modes are tracked.
  VtScreen q(20, 5, 10);
  q.write("\x1b[3;4H\x1b[6n\x1b]0;My Title\x07\x1b[?25l\x1b[?1h\x1b[?2004h");
  CHECK(q.takeReplies() == "\x1b[3;4R");
  CHECK(q.title() == L"My Title" && !q.cursorVisible() && q.appCursorKeys() && q.bracketedPaste());

  // Scroll regions, insert/delete lines and characters.
  VtScreen r(4, 4, 10);
  r.write("a\r\nb\r\nc\r\nd\x1b[2;3r\x1b[2;1H\x1b[L");
  CHECK(vtRow(r, 0) == L"a" && vtRow(r, 1).empty() && vtRow(r, 2) == L"b" && vtRow(r, 3) == L"d");
  r.write("\x1b[r\x1b[4;1Hxyz\x1b[4;2H\x1b[P");
  CHECK(vtRow(r, 3) == L"xz");

  // Resizing keeps the cursor line visible.
  VtScreen z(10, 4, 10);
  z.write("1\r\n2\r\n3\r\n4");
  z.resize(5, 2);
  CHECK(z.rows() == 2 && z.cols() == 5 && vtRow(z, 1) == L"4" && z.cursorY() == 1);
}

int main() {
  testVtScreen();
  testRegressions();
  testLineDiff();
  testWrapMap();
  testMarkdown();
  testBuffer();
  testTypingAndUndo();
  testAutoPair();
  testNewlineIndent();
  testFindUnderExpand();
  testLineOps();
  testComments();
  testMultiCursorAndClipboard();
  testMovement();
  testSearch();
  testEncoding();
  testJson();
  testFuzzy();
  testHighlighter();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
