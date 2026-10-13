#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace verso {

// Resolution of link and image targets written in Markdown documents. Shared by
// the preview (images, link styling) and by link clicks so all read targets the
// same way.

// Percent-decoding over UTF-8 bytes, the encoding URLs use.
std::string decodePercentBytes(std::string_view s);
// Percent-decodes a URL component to text.
std::wstring decodeUrlComponent(std::wstring_view s);

// The root of the notes collection containing `dir`: the nearest folder that is an
// Obsidian vault (has .obsidian) or a git working tree (has .git), or "".
std::wstring markdownRoot(const std::wstring& dir);

// Resolves a local reference written in a Markdown document (a link or image
// target without its "#fragment") to a full path: relative to the document's
// folder, "/x" relative to the collection root (as on GitHub), or a file: URL.
// Fails with a message for URLs with other schemes or when a relative reference
// has no document folder.
bool resolveMarkdownPath(const std::wstring& ref, const std::wstring& baseDir, std::wstring& path,
                         std::wstring& error);

// A [[wiki link]] target: "Page", "Page#Heading" or "folder/Page.md".
struct WikiTarget {
  std::wstring page;     // as written, without the heading
  std::wstring heading;  // text after '#', or ""
};
WikiTarget parseWikiTarget(std::wstring_view target);

struct WikiResolution {
  std::wstring path;   // the file the link names, or where a new note goes
  bool exists = false;
  std::wstring error;  // set when no path could be chosen
};

// Finds the file a wiki link or embed names, as note apps do: "Page" means
// "Page.md" (a name with another extension is used as written). It looks next to
// the document, at `root`, and then for a file of that name anywhere under `root`,
// preferring the match nearest the document. When nothing matches, `path` is where
// a new note should be created, beside the document. `root` is the collection the
// document belongs to (see markdownRoot); its file list is cached briefly.
WikiResolution resolveWikiLink(const std::wstring& page, const std::wstring& baseDir, const std::wstring& root);

// Resolves a local Markdown link ("Gold%20Coast.md", "../npcs/a.md", "/x.md"),
// without its "#fragment", as note apps do: the written path first, then, when
// that file does not exist, a file of the same name anywhere in the collection.
// When neither exists, `path` is the written location (where a new note goes).
WikiResolution resolveNoteLink(const std::wstring& ref, const std::wstring& baseDir, const std::wstring& root);

// Forgets cached file lists, for example after creating a note.
void invalidateNoteTrees();

}  // namespace verso
