#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace verso::md {

// WikiLink marks a [[wiki link]] (also styled Link); its url is the target as written,
// such as "Page" or "Page#Heading", which names a note rather than a path. On an
// Image it marks an ![[embed]], whose src is likewise a name to look up.
enum Style : uint8_t { Bold = 1, Italic = 2, Code = 4, Link = 8, Strike = 16, Image = 32, WikiLink = 64 };

// A styled run inside Inline::text. Unstyled text has no span. An Image span's
// text is the image's alt text; each image has its own span.
struct Span {
  uint32_t start = 0;
  uint32_t len = 0;
  uint8_t style = 0;
  std::wstring url;             // link target (for an image: the enclosing link, if any)
  std::wstring src;             // image source as written (path, URL or data: URI)
  float width = 0, height = 0;  // image size from HTML attributes in CSS px; 0 = natural
};

// Inline content with Markdown markers removed.
struct Inline {
  std::wstring text;
  std::vector<Span> spans;
  bool centered = false;  // inside <p align="center">, <div align="center"> or <center>
};

enum class BlockKind { Paragraph, Heading, CodeBlock, ListItem, Rule, Table };

struct Block {
  BlockKind kind = BlockKind::Paragraph;
  int level = 0;        // heading level 1-6
  int depth = 0;        // list nesting depth (0 = top level)
  int quote = 0;        // blockquote depth
  bool ordered = false;
  int number = 0;       // ordered list item number
  int task = -1;        // -1 not a task, 0 open "[ ]", 1 done "[x]"
  Inline content;       // paragraph, heading and list item text
  std::wstring code;    // code block text ('\n' separated)
  std::wstring lang;    // code fence info string (first word)
  std::vector<std::vector<Inline>> rows;  // table: header row first
  std::vector<int> align;                 // table column alignment: 0 left, 1 center, 2 right
  size_t line = 0;      // first source line (0-based)
  size_t lastLine = 0;  // last source line
};

// CommonMark-flavored subset with GitHub extensions (tables, task lists,
// strikethrough, autolinks), reference links and the inline HTML common in
// READMEs: <img>, <a href>, <br>, emphasis tags, comments, and layout tags
// (<p>, <div>, <center>, ...) whose align="center" centers the paragraph. Other
// raw HTML is shown as text. Wiki links ([[Page]], [[Page|label]], [[Page#Heading]])
// and image embeds (![[image.png]], ![[image.png|200]]) follow note-app syntax.
std::vector<Block> parse(std::wstring_view text);
Inline parseInline(std::wstring_view s);
// GitHub-style heading anchor ("Hello, World!" -> "hello-world").
std::wstring slug(std::wstring_view heading);
// Anchors of the headings in `blocks`, in order, with GitHub's "-1", "-2", ...
// suffixes for repeated headings.
std::vector<std::wstring> anchors(const std::vector<Block>& blocks);

}  // namespace verso::md
