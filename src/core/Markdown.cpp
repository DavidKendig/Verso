#include "core/Markdown.h"

#include "core/TextUtil.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <unordered_map>

namespace verso::md {

namespace {

constexpr size_t npos = std::wstring_view::npos;

bool isPunct(wchar_t c) { return c < 128 && std::iswpunct(c); }
bool isAlnum(wchar_t c) { return std::iswalnum(c) != 0 || c == L'_'; }

std::wstring_view trimView(std::wstring_view t);  // spaces and tabs; defined with the block helpers

// Characters of `t`'s leading white space that span at most `cols` columns (tabs to 4).
size_t charsForColumns(std::wstring_view t, size_t cols) {
  size_t i = 0, col = 0;
  while (i < t.size() && (t[i] == L' ' || t[i] == L'\t')) {
    size_t next = t[i] == L'\t' ? col + 4 - col % 4 : col + 1;
    if (next > cols) break;
    col = next;
    ++i;
  }
  return i;
}

// The "</name>" closing tag at or after `from`, ignoring case; npos when absent.
size_t findClosingTag(std::wstring_view s, size_t from, const std::wstring& name) {
  const std::wstring tag = L"</" + name + L">";
  for (size_t i = s.find(L"</", from); i != std::wstring_view::npos; i = s.find(L"</", i + 2))
    if (i + tag.size() <= s.size() &&
        std::equal(tag.begin(), tag.end(), s.begin() + static_cast<ptrdiff_t>(i),
                   [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); }))
      return i;
  return std::wstring_view::npos;
}

// Nested links, emphasis and HTML recurse; deeper nesting (only adversarial input
// reaches it) is shown as text instead of exhausting the stack.
constexpr int kMaxInlineDepth = 64;
thread_local int tInlineDepth = 0;

// Link reference definitions of the document being parsed (see parse()).
using RefMap = std::unordered_map<std::wstring, std::wstring>;
thread_local const RefMap* tRefs = nullptr;

// Reference labels match case-insensitively with runs of white space collapsed.
std::wstring normalizeLabel(std::wstring_view label) {
  std::wstring out;
  bool space = false;
  for (wchar_t c : label) {
    if (c == L' ' || c == L'\t' || c == L'\n') {
      space = !out.empty();
      continue;
    }
    if (space) out += L' ';
    space = false;
    out += static_cast<wchar_t>(std::towlower(c));
  }
  return out;
}

const std::wstring* lookupRef(std::wstring_view label) {
  if (!tRefs || label.empty()) return nullptr;
  auto it = tRefs->find(normalizeLabel(label));
  return it == tRefs->end() ? nullptr : &it->second;
}

void append(Inline& out, std::wstring_view s, uint8_t style, const std::wstring& url) {
  if (s.empty()) return;
  uint32_t start = static_cast<uint32_t>(out.text.size());
  out.text += s;
  if (!style) return;
  if (!out.spans.empty()) {
    Span& last = out.spans.back();
    if (last.style == style && !(style & Image) && last.url == url && last.start + last.len == start) {
      last.len += static_cast<uint32_t>(s.size());
      return;
    }
  }
  Span sp;
  sp.start = start;
  sp.len = static_cast<uint32_t>(s.size());
  sp.style = style;
  sp.url = url;
  out.spans.push_back(std::move(sp));
}

void appendImage(Inline& out, std::wstring_view alt, uint8_t style, const std::wstring& link, std::wstring src,
                 float width = 0, float height = 0) {
  append(out, alt.empty() ? std::wstring_view(L"image") : alt, static_cast<uint8_t>(style | Image), link);
  Span& sp = out.spans.back();
  sp.src = std::move(src);
  sp.width = width;
  sp.height = height;
}

// HTML attributes of a tag body (`img src="a.png" width=200`), names lowercased.
std::unordered_map<std::wstring, std::wstring> htmlAttributes(std::wstring_view s) {
  std::unordered_map<std::wstring, std::wstring> attrs;
  size_t i = 0;
  while (i < s.size() && !std::iswspace(s[i])) ++i;  // tag name
  while (i < s.size()) {
    while (i < s.size() && (std::iswspace(s[i]) || s[i] == L'/')) ++i;
    size_t n = i;
    while (i < s.size() && !std::iswspace(s[i]) && s[i] != L'=' && s[i] != L'/') ++i;
    if (i == n) break;
    std::wstring name = toLower(s.substr(n, i - n));
    while (i < s.size() && std::iswspace(s[i])) ++i;
    std::wstring value;
    if (i < s.size() && s[i] == L'=') {
      ++i;
      while (i < s.size() && std::iswspace(s[i])) ++i;
      if (i < s.size() && (s[i] == L'"' || s[i] == L'\'')) {
        wchar_t q = s[i++];
        size_t e = s.find(q, i);
        if (e == std::wstring_view::npos) e = s.size();
        value = s.substr(i, e - i);
        i = e + 1;
      } else {
        size_t v = i;
        while (i < s.size() && !std::iswspace(s[i])) ++i;
        value = s.substr(v, i - v);
      }
    }
    attrs[name] = value;
  }
  return attrs;
}

// An HTML length in CSS px ("200", "200px"); 0 for percentages and other units.
float htmlLength(const std::wstring& v) {
  wchar_t* end = nullptr;
  double n = std::wcstod(v.c_str(), &end);
  std::wstring unit = end ? toLower(end) : std::wstring();
  if (n <= 0 || (!unit.empty() && unit != L"px")) return 0;
  return static_cast<float>(n);
}

// Finds the `]` matching the `[` at `open`, honoring nesting and escapes.
size_t matchBracket(std::wstring_view s, size_t open) {
  int depth = 0;
  for (size_t i = open; i < s.size(); ++i) {
    if (s[i] == L'\\') {
      ++i;
      continue;
    }
    if (s[i] == L'`') {
      size_t e = s.find(L'`', i + 1);
      if (e != npos) i = e;
      continue;
    }
    if (s[i] == L'[') ++depth;
    if (s[i] == L']' && --depth == 0) return i;
  }
  return npos;
}

// Parses "(url "title")" at `open`; returns the index after ')' or npos.
size_t parseDestination(std::wstring_view s, size_t open, std::wstring& url) {
  if (open >= s.size() || s[open] != L'(') return npos;
  int depth = 0;
  size_t i = open;
  for (; i < s.size(); ++i) {
    if (s[i] == L'\\') {
      ++i;
      continue;
    }
    if (s[i] == L'(') ++depth;
    if (s[i] == L')' && --depth == 0) break;
  }
  if (i >= s.size()) return npos;
  std::wstring_view inner = s.substr(open + 1, i - open - 1);
  size_t b = 0;
  while (b < inner.size() && inner[b] == L' ') ++b;
  inner = inner.substr(b);
  if (!inner.empty() && inner[0] == L'<') {
    size_t e = inner.find(L'>');
    url = std::wstring(inner.substr(1, e == npos ? npos : e - 1));
  } else {
    size_t e = inner.find(L' ');
    url = std::wstring(inner.substr(0, e));
  }
  return i + 1;
}

void inlineInto(std::wstring_view s, uint8_t style, const std::wstring& url, Inline& out);

// Emphasis closer for a run of `k` characters `ch` opening at `i`.
size_t findCloser(std::wstring_view s, size_t from, wchar_t ch, size_t k) {
  for (size_t j = from; j + k <= s.size(); ++j) {
    if (s[j] == L'\\') {
      ++j;
      continue;
    }
    if (s[j] == L'`') {
      size_t e = s.find(L'`', j + 1);
      if (e != npos) j = e;
      continue;
    }
    if (s[j] != ch) continue;
    size_t run = 0;
    while (j + run < s.size() && s[j + run] == ch) ++run;
    if (run >= k && j > from && s[j - 1] != L' ') {
      if (ch == L'_' && j + k < s.size() && isAlnum(s[j + k])) {
        j += run - 1;
        continue;
      }
      return j + (run - k);  // use the innermost k characters of a longer run
    }
    j += run - 1;
  }
  return npos;
}

void inlineInto(std::wstring_view s, uint8_t style, const std::wstring& url, Inline& out) {
  if (tInlineDepth >= kMaxInlineDepth) {
    append(out, s, style, url);
    return;
  }
  struct DepthGuard {
    DepthGuard() { ++tInlineDepth; }
    ~DepthGuard() { --tInlineDepth; }
  } depthGuard;
  size_t i = 0, plain = 0;
  auto flush = [&](size_t upTo) {
    if (upTo > plain) append(out, s.substr(plain, upTo - plain), style, url);
  };
  while (i < s.size()) {
    wchar_t c = s[i];
    if (c == L'\\' && i + 1 < s.size() && isPunct(s[i + 1])) {
      flush(i);
      append(out, s.substr(i + 1, 1), style, url);
      i += 2;
      plain = i;
      continue;
    }
    if (c == L'\\' && i + 1 == s.size()) {  // trailing backslash: hard line break marker
      flush(i);
      i += 1;
      plain = i;
      continue;
    }
    if (c == L'`') {
      size_t k = 0;
      while (i + k < s.size() && s[i + k] == L'`') ++k;
      size_t j = i + k;
      size_t close = npos;
      while (j < s.size()) {
        size_t f = s.find(L'`', j);
        if (f == npos) break;
        size_t run = 0;
        while (f + run < s.size() && s[f + run] == L'`') ++run;
        if (run == k) {
          close = f;
          break;
        }
        j = f + run;
      }
      if (close != npos) {
        flush(i);
        std::wstring_view code = s.substr(i + k, close - i - k);
        if (code.size() >= 2 && code.front() == L' ' && code.back() == L' ') code = code.substr(1, code.size() - 2);
        append(out, code, static_cast<uint8_t>((style | Code) & ~(Bold | Italic)), url);
        i = close + k;
        plain = i;
        continue;
      }
      i += k;
      continue;
    }
    // Wiki links: [[Page]], [[Page|label]], [[Page#Heading]]; ![[file]] embeds an image.
    bool embed = c == L'!' && s.substr(i, 3) == L"![[";
    if (embed || (c == L'[' && i + 1 < s.size() && s[i + 1] == L'[')) {
      size_t open = i + (embed ? 3 : 2);
      size_t close = s.find(L"]]", open);
      std::wstring_view inner = close == npos ? std::wstring_view() : s.substr(open, close - open);
      if (close != npos && !inner.empty() && inner.find_first_of(L"[]\n") == npos) {
        size_t bar = inner.find(L'|');
        std::wstring_view target = inner.substr(0, bar);
        if (!target.empty() && target.back() == L'\\') target.remove_suffix(1);  // "\|" inside a table
        target = trimView(target);
        std::wstring_view label = bar == npos ? target : trimView(inner.substr(bar + 1));
        if (!target.empty()) {
          flush(i);
          if (embed) {
            // "![[image.png|200]]" sets the width, "|200x100" both dimensions.
            float w = 0, h = 0;
            if (bar != npos) {
              std::wstring dims(label);
              wchar_t* end = nullptr;
              w = static_cast<float>(std::wcstod(dims.c_str(), &end));
              if (end && (*end == L'x' || *end == L'X')) h = static_cast<float>(std::wcstod(end + 1, nullptr));
              if (w < 0) w = 0;
              if (h < 0) h = 0;
            }
            appendImage(out, target, static_cast<uint8_t>(style | WikiLink), url, std::wstring(target), w, h);
          } else {
            append(out, label.empty() ? target : label, static_cast<uint8_t>(style | Link | WikiLink),
                   std::wstring(target));
          }
          i = close + 2;
          plain = i;
          continue;
        }
      }
    }
    if ((c == L'!' && i + 1 < s.size() && s[i + 1] == L'[') || c == L'[') {
      size_t open = c == L'!' ? i + 1 : i;
      size_t close = matchBracket(s, open);
      std::wstring dest;
      size_t after = close == npos ? npos : parseDestination(s, close + 1, dest);
      if (close != npos && after == npos) {
        // Reference forms: [text][label], [text][] and [label].
        std::wstring_view text = s.substr(open + 1, close - open - 1);
        const std::wstring* ref = nullptr;
        size_t end = close + 1;
        if (close + 1 < s.size() && s[close + 1] == L'[') {
          size_t close2 = s.find(L']', close + 2);
          if (close2 != npos) {
            std::wstring_view label = s.substr(close + 2, close2 - close - 2);
            ref = lookupRef(label.empty() ? text : label);
            end = close2 + 1;
          }
        } else {
          ref = lookupRef(text);
        }
        if (ref) {
          dest = *ref;
          after = end;
        }
      }
      if (after != npos) {
        flush(i);
        std::wstring_view label = s.substr(open + 1, close - open - 1);
        if (c == L'!') {
          appendImage(out, label, style, url, dest);
        } else {
          inlineInto(label, static_cast<uint8_t>(style | Link), dest, out);
        }
        i = after;
        plain = i;
        continue;
      }
    }
    if (c == L'<') {
      size_t e = s.find(L'>', i);
      if (e != npos) {
        std::wstring_view inner = s.substr(i + 1, e - i - 1);
        bool autolink = inner.rfind(L"http://", 0) == 0 || inner.rfind(L"https://", 0) == 0 ||
                        inner.rfind(L"mailto:", 0) == 0;
        if (autolink && inner.find(L' ') == npos) {
          flush(i);
          append(out, inner, static_cast<uint8_t>(style | Link), std::wstring(inner));
          i = e + 1;
          plain = i;
          continue;
        }
        if (inner.rfind(L"!--", 0) == 0) {  // comment
          size_t end = s.find(L"-->", i + 4);
          flush(i);
          i = end == npos ? s.size() : end + 3;
          plain = i;
          continue;
        }
        bool closing = !inner.empty() && inner[0] == L'/';
        size_t nb = closing ? 1 : 0, ne = nb;
        while (ne < inner.size() && std::iswalnum(inner[ne])) ++ne;
        std::wstring name = toLower(inner.substr(nb, ne - nb));
        bool tagLike = !name.empty() && std::iswalpha(name[0]) &&
                       (ne == inner.size() || std::iswspace(inner[ne]) || inner[ne] == L'/');
        if (tagLike && !closing) {
          if (name == L"br") {
            flush(i);
            append(out, L"\n", style, url);
            i = e + 1;
            plain = i;
            continue;
          }
          if (name == L"img") {
            auto a = htmlAttributes(inner);
            flush(i);
            appendImage(out, a[L"alt"], style, url, a[L"src"], htmlLength(a[L"width"]), htmlLength(a[L"height"]));
            i = e + 1;
            plain = i;
            continue;
          }
          // Tags whose content is styled: <a href>, emphasis, code and HTML headings.
          uint8_t add = 0;
          std::wstring linkTo = url;
          if (name == L"a") {
            linkTo = htmlAttributes(inner)[L"href"];
            add = linkTo.empty() ? 0 : Link;
            if (!add) linkTo = url;
          } else if (name == L"b" || name == L"strong" ||
                     (name.size() == 2 && name[0] == L'h' && name[1] >= L'1' && name[1] <= L'6')) {
            add = Bold;
          } else if (name == L"i" || name == L"em") {
            add = Italic;
          } else if (name == L"s" || name == L"del" || name == L"strike") {
            add = Strike;
          } else if (name == L"code") {
            add = Code;
          }
          size_t cl = (add || name == L"a") ? findClosingTag(s, e + 1, name) : npos;
          if (cl != npos) {
            if (name[0] == L'h' && toLower(htmlAttributes(inner)[L"align"]) == L"center") out.centered = true;
            flush(i);
            std::wstring_view body = s.substr(e + 1, cl - e - 1);
            if (add & Code)
              append(out, body, static_cast<uint8_t>((style | Code) & ~(Bold | Italic)), linkTo);
            else
              inlineInto(body, static_cast<uint8_t>(style | add), linkTo, out);
            i = cl + name.size() + 3;
            plain = i;
            continue;
          }
        }
        if (tagLike) {
          // Layout tags carry no text of their own; drop them and keep their content.
          static const wchar_t* const kLayoutTags[] = {
              L"p",    L"div", L"center", L"span", L"picture", L"source", L"sup", L"sub",  L"details",
              L"summary", L"kbd", L"u",   L"small", L"big",    L"font",   L"a",   L"b",    L"strong",
              L"i",    L"em",  L"s",      L"del",  L"strike",  L"code",   L"h1",  L"h2",   L"h3",
              L"h4",   L"h5",  L"h6",     L"br",   L"img",     L"hr"};
          bool layoutTag = std::any_of(std::begin(kLayoutTags), std::end(kLayoutTags),
                                       [&](const wchar_t* t) { return name == t; });
          if (layoutTag) {
            if (!closing && (name == L"center" || toLower(htmlAttributes(inner)[L"align"]) == L"center"))
              out.centered = true;
            flush(i);
            i = e + 1;
            plain = i;
            continue;
          }
        }
      }
    }
    if ((c == L'h') && (s.substr(i, 7) == L"http://" || s.substr(i, 8) == L"https://") && !(style & Link) &&
        (i == 0 || !isAlnum(s[i - 1]))) {
      size_t e = i;
      while (e < s.size() && !std::iswspace(s[e]) && s[e] != L'<') ++e;
      while (e > i && std::wcschr(L".,;:!?)]'\"", s[e - 1])) --e;
      flush(i);
      std::wstring link(s.substr(i, e - i));
      append(out, link, static_cast<uint8_t>(style | Link), link);
      i = e;
      plain = i;
      continue;
    }
    if (c == L'~' && i + 1 < s.size() && s[i + 1] == L'~') {
      size_t close = s.find(L"~~", i + 2);
      if (close != npos && close > i + 2) {
        flush(i);
        inlineInto(s.substr(i + 2, close - i - 2), static_cast<uint8_t>(style | Strike), url, out);
        i = close + 2;
        plain = i;
        continue;
      }
    }
    if (c == L'*' || c == L'_') {
      size_t run = 0;
      while (i + run < s.size() && s[i + run] == c) ++run;
      bool leftFlanking = i + run < s.size() && !std::iswspace(s[i + run]);
      bool wordInside = c == L'_' && i > 0 && isAlnum(s[i - 1]);
      if (leftFlanking && !wordInside) {
        size_t k = std::min<size_t>(run, 3);
        for (; k >= 1; --k) {
          size_t close = findCloser(s, i + k, c, k);
          if (close == npos) continue;
          flush(i);
          const uint8_t add = k == 1   ? static_cast<uint8_t>(Italic)
                              : k == 2 ? static_cast<uint8_t>(Bold)
                                       : static_cast<uint8_t>(Bold | Italic);
          if (run > k) append(out, s.substr(i, run - k), style, url);
          inlineInto(s.substr(i + run, close - i - run), static_cast<uint8_t>(style | add), url, out);
          i = close + k;
          plain = i;
          break;
        }
        if (k >= 1) continue;
      }
      i += run;
      continue;
    }
    ++i;
  }
  flush(s.size());
}

// --- Block parsing ---------------------------------------------------------

struct Line {
  int quote = 0;
  std::wstring_view text;  // after quote markers
};

Line stripQuotes(std::wstring_view raw) {
  Line l;
  std::wstring_view t = raw;
  for (;;) {
    size_t sp = 0;
    while (sp < t.size() && sp < 3 && t[sp] == L' ') ++sp;
    if (sp < t.size() && t[sp] == L'>') {
      t = t.substr(sp + 1);
      if (!t.empty() && t[0] == L' ') t = t.substr(1);
      ++l.quote;
      continue;
    }
    break;
  }
  l.text = t;
  return l;
}

bool isBlank(std::wstring_view t) {
  return std::all_of(t.begin(), t.end(), [](wchar_t c) { return c == L' ' || c == L'\t'; });
}

size_t indentOf(std::wstring_view t) {
  size_t n = 0;
  for (wchar_t c : t) {
    if (c == L' ')
      ++n;
    else if (c == L'\t')
      n += 4 - n % 4;
    else
      break;
  }
  return n;
}

std::wstring_view trimView(std::wstring_view t) {
  size_t b = 0, e = t.size();
  while (b < e && (t[b] == L' ' || t[b] == L'\t')) ++b;
  while (e > b && (t[e - 1] == L' ' || t[e - 1] == L'\t')) --e;
  return t.substr(b, e - b);
}

bool isFence(std::wstring_view t, wchar_t& ch, size_t& len) {
  std::wstring_view s = t.substr(std::min<size_t>(indentOf(t), t.size()));
  if (indentOf(t) > 3 || s.size() < 3 || (s[0] != L'`' && s[0] != L'~')) return false;
  size_t n = 0;
  while (n < s.size() && s[n] == s[0]) ++n;
  if (n < 3) return false;
  ch = s[0];
  len = n;
  return true;
}

bool isRule(std::wstring_view t) {
  std::wstring_view s = trimView(t);
  if (s.size() < 3 || indentOf(t) > 3) return false;
  wchar_t c = s[0];
  if (c != L'-' && c != L'*' && c != L'_') return false;
  size_t count = 0;
  for (wchar_t x : s) {
    if (x == c)
      ++count;
    else if (x != L' ' && x != L'\t')
      return false;
  }
  return count >= 3;
}

int headingLevel(std::wstring_view t, std::wstring_view& content) {
  if (indentOf(t) > 3) return 0;
  std::wstring_view s = trimView(t);
  size_t n = 0;
  while (n < s.size() && s[n] == L'#') ++n;
  if (n == 0 || n > 6 || (n < s.size() && s[n] != L' ' && s[n] != L'\t')) return 0;
  std::wstring_view c = trimView(s.substr(n));
  // Strip an optional closing sequence of #s.
  size_t e = c.size();
  while (e > 0 && c[e - 1] == L'#') --e;
  if (e < c.size() && (e == 0 || c[e - 1] == L' ')) c = trimView(c.substr(0, e));
  content = c;
  return static_cast<int>(n);
}

struct ListMarker {
  bool ok = false;
  bool ordered = false;
  int number = 0;
  size_t indent = 0;
  std::wstring_view content;
};

ListMarker listMarker(std::wstring_view t) {
  ListMarker m;
  size_t ind = indentOf(t);
  size_t i = 0;
  while (i < t.size() && (t[i] == L' ' || t[i] == L'\t')) ++i;
  if (i >= t.size()) return m;
  size_t after;
  if (t[i] == L'-' || t[i] == L'*' || t[i] == L'+') {
    after = i + 1;
  } else if (std::iswdigit(t[i])) {
    size_t d = i;
    int num = 0;
    while (d < t.size() && std::iswdigit(t[d]) && d - i < 9) num = num * 10 + (t[d++] - L'0');
    if (d >= t.size() || (t[d] != L'.' && t[d] != L')')) return m;
    m.ordered = true;
    m.number = num;
    after = d + 1;
  } else {
    return m;
  }
  if (after < t.size() && t[after] != L' ' && t[after] != L'\t') return m;
  m.ok = true;
  m.indent = ind;
  m.content = trimView(t.substr(std::min(after, t.size())));
  return m;
}

std::vector<std::wstring_view> splitCells(std::wstring_view row) {
  std::wstring_view s = trimView(row);
  if (!s.empty() && s.front() == L'|') s = s.substr(1);
  if (!s.empty() && s.back() == L'|' && (s.size() < 2 || s[s.size() - 2] != L'\\')) s = s.substr(0, s.size() - 1);
  std::vector<std::wstring_view> cells;
  size_t start = 0;
  bool inCode = false, inWiki = false;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i < s.size() && s[i] == L'\\') {
      ++i;
      continue;
    }
    if (i < s.size() && s[i] == L'`') inCode = !inCode;
    // The "|" of [[Page|label]] does not separate cells.
    if (i + 1 < s.size() && s[i] == L'[' && s[i + 1] == L'[') inWiki = true;
    if (i + 1 < s.size() && s[i] == L']' && s[i + 1] == L']') inWiki = false;
    if (i == s.size() || (s[i] == L'|' && !inCode && !inWiki)) {
      cells.push_back(trimView(s.substr(start, i - start)));
      start = i + 1;
    }
  }
  return cells;
}

bool isDelimiterRow(std::wstring_view t, std::vector<int>& align) {
  if (t.find(L'-') == npos) return false;
  auto cells = splitCells(t);
  if (cells.empty()) return false;
  align.clear();
  for (auto c : cells) {
    if (c.empty()) return false;
    bool left = c.front() == L':', right = c.back() == L':';
    std::wstring_view mid = c.substr(left ? 1 : 0, c.size() - (left ? 1 : 0) - (right ? 1 : 0));
    if (mid.empty() || mid.find_first_not_of(L'-') != npos) return false;
    align.push_back(left && right ? 1 : (right ? 2 : 0));
  }
  return true;
}

}  // namespace

Inline parseInline(std::wstring_view s) {
  Inline out;
  inlineInto(s, 0, std::wstring(), out);
  return out;
}

std::wstring slug(std::wstring_view heading) {
  std::wstring out;
  for (wchar_t c : heading) {
    if (std::iswalnum(c) || c == L'_' || c == L'-')
      out += static_cast<wchar_t>(std::towlower(c));
    else if (c == L' ')
      out += L'-';
  }
  return out;
}

std::vector<std::wstring> anchors(const std::vector<Block>& blocks) {
  std::vector<std::wstring> out;
  std::unordered_map<std::wstring, int> seen;
  for (const Block& b : blocks) {
    if (b.kind != BlockKind::Heading) continue;
    std::wstring s = slug(b.content.text);
    int n = seen[s]++;
    out.push_back(n == 0 ? s : s + L"-" + std::to_wstring(n));
  }
  return out;
}

namespace {

// "[label]: destination" (up to three spaces of indent; an optional title is ignored).
bool referenceDefinition(std::wstring_view t, std::wstring& label, std::wstring& dest) {
  if (indentOf(t) > 3) return false;
  std::wstring_view s = trimView(t);
  if (s.size() < 4 || s[0] != L'[' || s[1] == L'^') return false;  // "[^1]:" is a footnote
  size_t close = s.find(L"]:");
  if (close == npos || close < 2) return false;
  std::wstring_view rest = trimView(s.substr(close + 2));
  if (rest.empty()) return false;
  if (rest[0] == L'<') {
    size_t e = rest.find(L'>');
    if (e == npos) return false;
    dest = std::wstring(rest.substr(1, e - 1));
  } else {
    size_t e = 0;
    while (e < rest.size() && rest[e] != L' ' && rest[e] != L'\t') ++e;
    dest = std::wstring(rest.substr(0, e));
  }
  label = normalizeLabel(s.substr(1, close - 1));
  return !label.empty();
}

}  // namespace

std::vector<Block> parse(std::wstring_view text) {
  std::vector<std::wstring_view> lines;
  {
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
      if (i == text.size() || text[i] == L'\n') {
        std::wstring_view l = text.substr(start, i - start);
        if (!l.empty() && l.back() == L'\r') l.remove_suffix(1);
        lines.push_back(l);
        start = i + 1;
      }
    }
    if (!lines.empty() && lines.back().empty() && !text.empty() && text.back() == L'\n') lines.pop_back();
  }

  // Collect link reference definitions first: they may follow their uses.
  RefMap refs;
  std::vector<bool> isDefinition(lines.size(), false);
  {
    wchar_t fenceCh = 0;
    size_t fenceLen = 0;
    bool prevBlank = true;
    for (size_t n = 0; n < lines.size(); ++n) {
      std::wstring_view t = stripQuotes(lines[n]).text;
      wchar_t ch;
      size_t len;
      if (isFence(t, ch, len)) {
        if (!fenceCh) {
          fenceCh = ch;
          fenceLen = len;
        } else if (ch == fenceCh && len >= fenceLen) {
          fenceCh = 0;
        }
        prevBlank = false;
        continue;
      }
      if (fenceCh) continue;
      std::wstring label, dest;
      // A definition cannot interrupt a paragraph, but may follow another definition.
      if ((prevBlank || (n > 0 && isDefinition[n - 1])) && referenceDefinition(t, label, dest)) {
        refs.emplace(label, dest);  // the first definition of a label wins
        isDefinition[n] = true;
      }
      prevBlank = isBlank(t);
    }
  }
  struct RefScope {
    const RefMap* saved;
    explicit RefScope(const RefMap* r) : saved(tRefs) { tRefs = r; }
    ~RefScope() { tRefs = saved; }
  } refScope(&refs);

  std::vector<Block> blocks;
  std::wstring para;  // pending paragraph / list item source
  Block pending;
  bool open = false;  // a paragraph or list item is accumulating lines

  auto flush = [&] {
    if (!open) return;
    pending.content = parseInline(para);
    // HTML layout tags alone (such as `<p align="center">`) leave nothing to show.
    bool empty = pending.kind == BlockKind::Paragraph && pending.content.spans.empty() &&
                 trimView(pending.content.text).empty();
    if (!empty) blocks.push_back(std::move(pending));
    pending = Block{};
    para.clear();
    open = false;
  };
  auto addLine = [&](std::wstring_view t, size_t lineNo) {
    std::wstring_view s = trimView(t);
    bool hard = t.size() >= 2 && t.substr(t.size() - 2) == L"  ";
    if (!para.empty()) para += para.back() == L'\n' ? L"" : L" ";
    para += s;
    if (hard || (!s.empty() && s.back() == L'\\')) {
      if (!para.empty() && para.back() == L'\\') para.pop_back();
      para += L'\n';
    }
    pending.lastLine = lineNo;
  };

  for (size_t n = 0; n < lines.size(); ++n) {
    Line ln = stripQuotes(lines[n]);
    std::wstring_view t = ln.text;

    if (isBlank(t) || isDefinition[n]) {
      flush();
      continue;
    }
    if (open && ln.quote != pending.quote && pending.kind != BlockKind::ListItem) flush();

    // Fenced code block.
    wchar_t fch;
    size_t flen;
    if (isFence(t, fch, flen)) {
      flush();
      Block b;
      b.kind = BlockKind::CodeBlock;
      b.quote = ln.quote;
      b.line = n;
      std::wstring_view info = trimView(t.substr(indentOf(t) + flen));
      b.lang = std::wstring(info.substr(0, info.find_first_of(L" \t{")));
      size_t fenceIndent = indentOf(t);
      size_t m = n + 1;
      for (; m < lines.size(); ++m) {
        std::wstring_view body = stripQuotes(lines[m]).text;
        wchar_t c2;
        size_t l2;
        if (isFence(body, c2, l2) && c2 == fch && l2 >= flen && isBlank(body.substr(indentOf(body) + l2))) break;
        if (!b.code.empty() || m > n + 1) b.code += L'\n';
        b.code += body.substr(charsForColumns(body, fenceIndent));
      }
      b.lastLine = std::min(m, lines.size() - 1);
      blocks.push_back(std::move(b));
      n = m;
      continue;
    }

    // Setext headings underline the open paragraph.
    if (open && pending.kind == BlockKind::Paragraph && indentOf(t) <= 3) {
      std::wstring_view s = trimView(t);
      bool eq = !s.empty() && s.find_first_not_of(L'=') == npos;
      bool dash = !s.empty() && s.find_first_not_of(L'-') == npos;
      if (eq || dash) {
        pending.kind = BlockKind::Heading;
        pending.level = eq ? 1 : 2;
        pending.lastLine = n;
        flush();
        continue;
      }
    }

    std::wstring_view headingText;
    if (int level = headingLevel(t, headingText)) {
      flush();
      Block b;
      b.kind = BlockKind::Heading;
      b.level = level;
      b.quote = ln.quote;
      b.line = b.lastLine = n;
      b.content = parseInline(headingText);
      blocks.push_back(std::move(b));
      continue;
    }

    if (isRule(t)) {
      flush();
      Block b;
      b.kind = BlockKind::Rule;
      b.quote = ln.quote;
      b.line = b.lastLine = n;
      blocks.push_back(std::move(b));
      continue;
    }

    // Tables: a header row followed by a delimiter row.
    std::vector<int> align;
    if (t.find(L'|') != npos && n + 1 < lines.size() && isDelimiterRow(stripQuotes(lines[n + 1]).text, align)) {
      flush();
      Block b;
      b.kind = BlockKind::Table;
      b.quote = ln.quote;
      b.line = n;
      b.align = align;
      auto addRow = [&](std::wstring_view row) {
        std::vector<Inline> cells;
        for (auto c : splitCells(row)) cells.push_back(parseInline(c));
        cells.resize(align.size());
        b.rows.push_back(std::move(cells));
      };
      addRow(t);
      size_t m = n + 2;
      for (; m < lines.size(); ++m) {
        std::wstring_view r = stripQuotes(lines[m]).text;
        if (isBlank(r) || r.find(L'|') == npos) break;
        addRow(r);
      }
      b.lastLine = m - 1;
      blocks.push_back(std::move(b));
      n = m - 1;
      continue;
    }

    ListMarker lm = listMarker(t);
    if (lm.ok) {
      flush();
      // Nesting depth from the indentation relative to earlier items.
      int depth = 0;
      for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
        if (it->kind != BlockKind::ListItem || it->quote != ln.quote) break;
        // Items store their marker indent in `level`.
        if (static_cast<size_t>(it->level) < lm.indent) {
          depth = it->depth + 1;
          break;
        }
        if (static_cast<size_t>(it->level) == lm.indent) {
          depth = it->depth;
          break;
        }
      }
      pending = Block{};
      pending.kind = BlockKind::ListItem;
      pending.quote = ln.quote;
      pending.depth = depth;
      pending.level = static_cast<int>(lm.indent);
      pending.ordered = lm.ordered;
      pending.number = lm.number;
      pending.line = n;
      std::wstring_view content = lm.content;
      if (content.size() >= 3 && content[0] == L'[' && content[2] == L']' &&
          (content[1] == L' ' || content[1] == L'x' || content[1] == L'X') &&
          (content.size() == 3 || content[3] == L' ')) {
        pending.task = content[1] == L' ' ? 0 : 1;
        content = trimView(content.substr(3));
      }
      open = true;
      addLine(content, n);
      continue;
    }

    // Indented code (not inside a paragraph or list).
    if (!open && indentOf(t) >= 4) {
      Block b;
      b.kind = BlockKind::CodeBlock;
      b.quote = ln.quote;
      b.line = n;
      size_t m = n;
      for (; m < lines.size(); ++m) {
        std::wstring_view body = stripQuotes(lines[m]).text;
        if (!isBlank(body) && indentOf(body) < 4) break;
        if (m > n) b.code += L'\n';
        size_t strip = 0, col = 0;
        while (strip < body.size() && col < 4) {
          col += body[strip] == L'\t' ? 4 - col % 4 : 1;
          ++strip;
        }
        b.code += body.substr(strip);
      }
      while (!b.code.empty() && b.code.back() == L'\n') b.code.pop_back();
      b.lastLine = m - 1;
      blocks.push_back(std::move(b));
      n = m - 1;
      continue;
    }

    // Paragraph text (or a lazy continuation of the open list item).
    if (!open) {
      pending = Block{};
      pending.kind = BlockKind::Paragraph;
      pending.quote = ln.quote;
      pending.line = n;
      open = true;
    }
    addLine(t, n);
  }
  flush();
  return blocks;
}

}  // namespace verso::md
