#include "app/MarkdownImages.h"

#include <d2d1_3.h>
#include <shlwapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>

#include "app/MarkdownLinks.h"
#include "core/Encoding.h"
#include "core/TextUtil.h"

namespace verso {

namespace {

constexpr size_t kMaxImageBytes = 32u << 20;  // per image; larger sources are refused
constexpr int kWorkers = 3;                    // concurrent loads
constexpr size_t kKeepUnused = 48;             // cached images kept beyond the current layout
constexpr UINT kMaxDecodeSide = 4096;          // decoded pixels per side; drawing scales beyond

enum class SourceKind { File, Remote, Data };

// github.com/<owner>/<repo>/blob/<ref>/<path> is a web page; /raw/ serves the file.
std::wstring githubRaw(const std::wstring& url) {
  const std::wstring prefix = L"https://github.com/";
  if (!startsWithNoCase(url, prefix)) return url;
  size_t owner = url.find(L'/', prefix.size());
  size_t repo = owner == std::wstring::npos ? owner : url.find(L'/', owner + 1);
  if (repo == std::wstring::npos || url.compare(repo, 6, L"/blob/") != 0) return url;
  return url.substr(0, repo) + L"/raw/" + url.substr(repo + 6);
}

struct Resolved {
  SourceKind kind = SourceKind::File;
  std::wstring key;
  std::wstring error;
};

Resolved resolveSource(const std::wstring& raw, const std::wstring& baseDir) {
  Resolved r;
  std::wstring src(trim(raw));
  if (src.empty()) {
    r.error = L"The image has no source.";
    return r;
  }
  if (startsWithNoCase(src, L"data:")) {
    r.kind = SourceKind::Data;
    r.key = src;
    return r;
  }
  if (src.rfind(L"//", 0) == 0) src = L"https:" + src;
  if (startsWithNoCase(src, L"http://") || startsWithNoCase(src, L"https://")) {
    r.kind = SourceKind::Remote;
    r.key = githubRaw(src);
    return r;
  }
  if (!resolveMarkdownPath(src.substr(0, src.find(L'#')), baseDir, r.key, r.error) && r.error.empty())
    r.error = L"Unsupported image source: " + src;
  return r;
}

// --- Loading (worker threads) ------------------------------------------------

std::wstring winhttpError(DWORD code) {
  switch (code) {
    case ERROR_WINHTTP_TIMEOUT: return L"the request timed out";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return L"the server name could not be resolved";
    case ERROR_WINHTTP_CANNOT_CONNECT: return L"could not connect to the server";
    case ERROR_WINHTTP_SECURE_FAILURE: return L"the secure connection failed";
    case ERROR_WINHTTP_CONNECTION_ERROR: return L"the connection was reset";
    default: return L"network error " + std::to_wstring(code);
  }
}

struct WinHttpHandle {
  HINTERNET h = nullptr;
  ~WinHttpHandle() {
    if (h) WinHttpCloseHandle(h);
  }
};

bool fetchHttp(const std::wstring& url, std::string& out, std::wstring& err) {
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) {
    err = L"Malformed URL: " + url;
    return false;
  }
  std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
  std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
  path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
  if (path.empty()) path = L"/";
  WinHttpHandle session{WinHttpOpen(L"Verso/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.h)
    session.h = WinHttpOpen(L"Verso/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session.h) {
    err = L"Downloading is unavailable (" + winhttpError(GetLastError()) + L").";
    return false;
  }
  WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
  WinHttpHandle conn{WinHttpConnect(session.h, host.c_str(), uc.nPort, 0)};
  const wchar_t* accept[] = {L"image/*", L"*/*", nullptr};
  WinHttpHandle req{conn.h ? WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, accept,
                                                uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                           : nullptr};
  if (req.h) {
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(req.h, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
  }
  if (!req.h || !WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(req.h, nullptr)) {
    err = L"Download failed: " + winhttpError(GetLastError()) + L".";
    return false;
  }
  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &status, &size, WINHTTP_NO_HEADER_INDEX);
  if (status != 200) {
    err = L"Download failed: the server answered HTTP " + std::to_wstring(status) + L".";
    return false;
  }
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req.h, &avail)) {
      err = L"Download failed: " + winhttpError(GetLastError()) + L".";
      return false;
    }
    if (avail == 0) break;
    if (out.size() + avail > kMaxImageBytes) {
      err = L"The image is larger than 32 MB.";
      return false;
    }
    size_t old = out.size();
    out.resize(old + avail);
    DWORD got = 0;
    if (!WinHttpReadData(req.h, out.data() + old, avail, &got)) {
      err = L"Download failed: " + winhttpError(GetLastError()) + L".";
      return false;
    }
    out.resize(old + got);
  }
  return true;
}

bool base64Decode(std::string_view in, std::string& out) {
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
  };
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=' || std::isspace(static_cast<unsigned char>(c))) continue;
    int v = value(c);
    if (v < 0) return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  return true;
}

bool decodeDataUri(const std::wstring& uri, std::string& out, std::wstring& err) {
  size_t comma = uri.find(L',');
  if (comma == std::wstring::npos) {
    err = L"Malformed data: URI.";
    return false;
  }
  std::wstring meta = toLower(uri.substr(5, comma - 5));
  std::string payload = wideToUtf8(std::wstring_view(uri).substr(comma + 1));
  bool b64 = meta.size() >= 7 && meta.compare(meta.size() - 7, 7, L";base64") == 0;
  if (b64) {
    if (!base64Decode(decodePercentBytes(payload), out)) {
      err = L"Malformed base64 in data: URI.";
      return false;
    }
  } else {
    out = decodePercentBytes(payload);
  }
  return true;
}

bool readLocal(const std::wstring& path, std::string& out, std::wstring& err, uint64_t& time) {
  uint64_t size = 0;
  if (!fileStat(path, &time, &size)) {
    err = L"File not found: " + path;
    return false;
  }
  if (size > kMaxImageBytes) {
    err = L"The image is larger than 32 MB.";
    return false;
  }
  return readFileBytes(path, out, &err);
}

bool rasterMagic(const std::string& b) {
  auto at = [&](size_t i) { return i < b.size() ? static_cast<unsigned char>(b[i]) : 0u; };
  return (at(0) == 0x89 && at(1) == 'P') || (at(0) == 0xFF && at(1) == 0xD8) || b.rfind("GIF8", 0) == 0 ||
         b.rfind("BM", 0) == 0 || b.rfind("RIFF", 0) == 0 || b.rfind("II*", 0) == 0 || b.rfind("MM", 0) == 0 ||
         (b.size() > 12 && b.compare(4, 4, "ftyp") == 0) || (at(0) == 0 && at(1) == 0 && at(2) == 1 && at(3) == 0);
}

bool looksLikeSvg(const std::string& b) {
  if (rasterMagic(b)) return false;
  std::string head = b.substr(0, 4096);
  for (auto& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return head.find("<svg") != std::string::npos;
}

// --- SVG facts (parsed off the UI thread) -------------------------------------

struct SvgText {
  float x = 0, y = 0;
  std::wstring text;
  float size = 16;
  std::wstring family;
  int weight = 400;
  bool italic = false;
  Color color;
  int anchor = 0;  // 0 start, 1 middle, 2 end
  D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Identity();
};

struct SvgFacts {
  bool viewBox = false;
  float vbX = 0, vbY = 0, vbW = 0, vbH = 0;
  std::vector<SvgText> texts;
};

std::vector<float> numberList(std::wstring_view v) {
  std::vector<float> out;
  std::wstring s(v);
  const wchar_t* p = s.c_str();
  for (;;) {
    while (*p && (std::iswspace(*p) || *p == L',')) ++p;
    if (!*p) break;
    wchar_t* end = nullptr;
    double d = std::wcstod(p, &end);
    if (end == p) break;
    out.push_back(static_cast<float>(d));
    p = end;
  }
  return out;
}

// A length in user units ("11", "11px", "8pt", "1.2em", "50%").
float svgLength(const std::wstring& v, float em, float percentOf) {
  std::wstring s(trim(v));
  wchar_t* end = nullptr;
  double d = std::wcstod(s.c_str(), &end);
  if (end == s.c_str()) return -1;
  std::wstring unit = toLower(trim(end));
  if (unit.empty() || unit == L"px") return static_cast<float>(d);
  if (unit == L"pt") return static_cast<float>(d * 96 / 72);
  if (unit == L"em") return static_cast<float>(d) * em;
  if (unit == L"%") return percentOf > 0 ? static_cast<float>(d) * percentOf / 100 : -1;
  if (unit == L"in") return static_cast<float>(d * 96);
  if (unit == L"cm") return static_cast<float>(d * 96 / 2.54);
  if (unit == L"mm") return static_cast<float>(d * 96 / 25.4);
  return -1;
}

bool svgColor(const std::wstring& raw, Color& out) {
  std::wstring v = toLower(trim(raw));
  if (v.empty() || v == L"none" || v == L"transparent") return false;
  if (v[0] == L'#') {
    std::wstring h = v.substr(1);
    if (h.size() == 3) h = {h[0], h[0], h[1], h[1], h[2], h[2]};
    if (h.size() != 6) return true;
    out = Color::hex(static_cast<uint32_t>(std::wcstoul(h.c_str(), nullptr, 16)));
    return true;
  }
  if (v.rfind(L"rgb", 0) == 0) {
    auto n = numberList(v.substr(v.find(L'(') + 1));
    if (n.size() >= 3) out = Color{n[0] / 255, n[1] / 255, n[2] / 255, n.size() > 3 ? n[3] : 1.0f};
    return true;
  }
  static const std::pair<const wchar_t*, uint32_t> named[] = {
      {L"black", 0x000000}, {L"white", 0xFFFFFF},  {L"red", 0xFF0000},    {L"green", 0x008000},
      {L"blue", 0x0000FF},  {L"gray", 0x808080},   {L"grey", 0x808080},   {L"yellow", 0xFFFF00},
      {L"orange", 0xFFA500}, {L"silver", 0xC0C0C0}, {L"navy", 0x000080},  {L"purple", 0x800080},
      {L"teal", 0x008080},  {L"maroon", 0x800000}, {L"lime", 0x00FF00},   {L"aqua", 0x00FFFF}};
  for (const auto& [name, rgb] : named)
    if (v == name) {
      out = Color::hex(rgb);
      return true;
    }
  out = Color::hex(0x000000);  // currentColor, gradients and unknown names
  return true;
}

D2D1::Matrix3x2F svgTransform(std::wstring_view v) {
  D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Identity();
  size_t i = 0;
  while (i < v.size()) {
    while (i < v.size() && (std::iswspace(v[i]) || v[i] == L',')) ++i;
    size_t n = i;
    while (i < v.size() && std::iswalpha(v[i])) ++i;
    std::wstring name = toLower(v.substr(n, i - n));
    size_t open = v.find(L'(', i), close = v.find(L')', i);
    if (name.empty() || open == std::wstring_view::npos || close == std::wstring_view::npos) break;
    auto a = numberList(v.substr(open + 1, close - open - 1));
    i = close + 1;
    D2D1::Matrix3x2F t = D2D1::Matrix3x2F::Identity();
    if (name == L"matrix" && a.size() >= 6)
      t = D2D1::Matrix3x2F(a[0], a[1], a[2], a[3], a[4], a[5]);
    else if (name == L"translate" && !a.empty())
      t = D2D1::Matrix3x2F::Translation(a[0], a.size() > 1 ? a[1] : 0);
    else if (name == L"scale" && !a.empty())
      t = D2D1::Matrix3x2F::Scale(a[0], a.size() > 1 ? a[1] : a[0]);
    else if (name == L"rotate" && !a.empty())
      t = D2D1::Matrix3x2F::Rotation(a[0], D2D1::Point2F(a.size() > 2 ? a[1] : 0, a.size() > 2 ? a[2] : 0));
    else if (name == L"skewx" && !a.empty())
      t = D2D1::Matrix3x2F::Skew(a[0], 0);
    else if (name == L"skewy" && !a.empty())
      t = D2D1::Matrix3x2F::Skew(0, a[0]);
    // "A B" applies B first: p' = A(B(p)), which is p * B * A in Direct2D's row-vector form.
    m = t * m;
  }
  return m;
}

std::wstring xmlEntities(std::wstring_view s) {
  std::wstring out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != L'&') {
      out += s[i];
      continue;
    }
    size_t semi = s.find(L';', i);
    if (semi == std::wstring_view::npos || semi - i > 10) {
      out += s[i];
      continue;
    }
    std::wstring_view ent = s.substr(i + 1, semi - i - 1);
    if (ent == L"amp") out += L'&';
    else if (ent == L"lt") out += L'<';
    else if (ent == L"gt") out += L'>';
    else if (ent == L"quot") out += L'"';
    else if (ent == L"apos") out += L'\'';
    else if (ent == L"nbsp") out += L' ';
    else if (!ent.empty() && ent[0] == L'#') {
      std::wstring num(ent.substr(1));
      unsigned long cp = (!num.empty() && (num[0] == L'x' || num[0] == L'X')) ? std::wcstoul(num.c_str() + 1, nullptr, 16)
                                                                             : std::wcstoul(num.c_str(), nullptr, 10);
      if (cp >= 0x10000 && cp <= 0x10FFFF) {
        cp -= 0x10000;
        out += static_cast<wchar_t>(0xD800 + (cp >> 10));
        out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
      } else if (cp > 0 && cp < 0x10000) {
        out += static_cast<wchar_t>(cp);
      }
    } else {
      out.append(s.substr(i, semi - i + 1));
      i = semi;
      continue;
    }
    i = semi;
  }
  return out;
}

std::unordered_map<std::wstring, std::wstring> xmlAttributes(std::wstring_view tag) {
  std::unordered_map<std::wstring, std::wstring> attrs;
  size_t i = 0;
  while (i < tag.size() && !std::iswspace(tag[i]) && tag[i] != L'/') ++i;
  while (i < tag.size()) {
    while (i < tag.size() && (std::iswspace(tag[i]) || tag[i] == L'/')) ++i;
    size_t n = i;
    while (i < tag.size() && !std::iswspace(tag[i]) && tag[i] != L'=') ++i;
    if (i == n) break;
    std::wstring name = toLower(tag.substr(n, i - n));
    while (i < tag.size() && std::iswspace(tag[i])) ++i;
    if (i >= tag.size() || tag[i] != L'=') continue;
    ++i;
    while (i < tag.size() && std::iswspace(tag[i])) ++i;
    if (i < tag.size() && (tag[i] == L'"' || tag[i] == L'\'')) {
      wchar_t q = tag[i++];
      size_t e = tag.find(q, i);
      if (e == std::wstring_view::npos) e = tag.size();
      attrs[name] = xmlEntities(tag.substr(i, e - i));
      i = e + 1;
    }
  }
  // Inline CSS declarations override presentation attributes.
  if (auto it = attrs.find(L"style"); it != attrs.end()) {
    std::wstring style = it->second;
    size_t p = 0;
    while (p < style.size()) {
      size_t semi = style.find(L';', p);
      if (semi == std::wstring::npos) semi = style.size();
      std::wstring_view decl = std::wstring_view(style).substr(p, semi - p);
      size_t colon = decl.find(L':');
      if (colon != std::wstring_view::npos)
        attrs[toLower(trim(decl.substr(0, colon)))] = std::wstring(trim(decl.substr(colon + 1)));
      p = semi + 1;
    }
  }
  return attrs;
}

std::wstring firstFamily(const std::wstring& list) {
  std::wstring f(trim(list.substr(0, list.find(L','))));
  if (f.size() >= 2 && (f[0] == L'\'' || f[0] == L'"')) f = f.substr(1, f.size() - 2);
  std::wstring l = toLower(f);
  if (l == L"sans-serif" || l.empty() || l == L"system-ui" || l == L"-apple-system") return L"Segoe UI";
  if (l == L"serif") return L"Times New Roman";
  if (l == L"monospace") return L"Consolas";
  return f;
}

// Reads the facts the renderer needs: the intrinsic size, the viewBox and the text
// elements (with inherited style and transforms), which Direct2D does not draw.
bool parseSvg(const std::string& bytes, SvgFacts& facts, float& w, float& h, std::wstring& err) {
  std::string_view utf8(bytes);
  if (utf8.size() >= 3 && utf8.compare(0, 3, "\xEF\xBB\xBF") == 0) utf8.remove_prefix(3);  // BOM
  std::wstring x = utf8ToWide(utf8);
  struct Style {
    std::wstring name;
    bool hidden = false, fillNone = false;
    Color fill = Color::hex(0x000000);
    float fillOpacity = 1, opacity = 1, fontSize = 16;
    std::wstring family = L"Segoe UI";
    int weight = 400;
    bool italic = false;
    int anchor = 0;
    D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Identity();
  };
  static const wchar_t* const kHidden[] = {L"defs", L"clippath", L"mask", L"symbol", L"title", L"desc", L"style",
                                           L"script", L"lineargradient", L"radialgradient", L"pattern", L"marker",
                                           L"metadata", L"filter", L"foreignobject"};
  std::vector<Style> stack(1);
  bool root = false;
  float rootW = -1, rootH = -1;
  bool inText = false;
  size_t textDepth = 0;
  SvgText cur;
  std::wstring buf;

  auto finishText = [&] {
    inText = false;
    std::wstring t;
    bool space = false;
    for (wchar_t c : buf) {
      if (std::iswspace(c)) {
        space = !t.empty();
        continue;
      }
      if (space) t += L' ';
      space = false;
      t += c;
    }
    if (!t.empty()) {
      cur.text = std::move(t);
      facts.texts.push_back(cur);
    }
  };

  size_t i = 0;
  while (i < x.size()) {
    if (x[i] != L'<') {
      size_t e = x.find(L'<', i);
      if (e == std::wstring::npos) e = x.size();
      if (inText) buf += xmlEntities(std::wstring_view(x).substr(i, e - i));
      i = e;
      continue;
    }
    if (x.compare(i, 4, L"<!--") == 0) {
      size_t e = x.find(L"-->", i + 4);
      i = e == std::wstring::npos ? x.size() : e + 3;
      continue;
    }
    if (x.compare(i, 9, L"<![CDATA[") == 0) {
      size_t e = x.find(L"]]>", i + 9);
      if (e == std::wstring::npos) e = x.size();
      if (inText) buf.append(x, i + 9, e - i - 9);
      i = std::min(x.size(), e + 3);
      continue;
    }
    if (x.compare(i, 2, L"<?") == 0 || x.compare(i, 2, L"<!") == 0) {
      size_t e = x.find(L'>', i);
      i = e == std::wstring::npos ? x.size() : e + 1;
      continue;
    }
    // Tag end, skipping quoted attribute values.
    size_t e = i + 1;
    wchar_t quote = 0;
    for (; e < x.size(); ++e) {
      if (quote) {
        if (x[e] == quote) quote = 0;
      } else if (x[e] == L'"' || x[e] == L'\'') {
        quote = x[e];
      } else if (x[e] == L'>') {
        break;
      }
    }
    std::wstring_view tag = std::wstring_view(x).substr(i + 1, std::min(e, x.size()) - i - 1);
    i = e + 1;
    if (tag.empty()) continue;
    auto localName = [](std::wstring_view t) {
      size_t n = 0;
      while (n < t.size() && !std::iswspace(t[n]) && t[n] != L'/') ++n;
      std::wstring nm = toLower(t.substr(0, n));
      size_t colon = nm.find(L':');
      return colon == std::wstring::npos ? nm : nm.substr(colon + 1);
    };
    if (tag[0] == L'/') {
      std::wstring name = localName(trim(tag.substr(1)));
      while (stack.size() > 1) {
        if (inText && stack.size() == textDepth) finishText();
        bool match = stack.back().name == name;
        stack.pop_back();
        if (match) break;
      }
      continue;
    }
    bool selfClose = tag.back() == L'/';
    std::wstring name = localName(tag);
    auto a = xmlAttributes(tag);
    Style s = stack.back();
    s.name = name;
    if (std::any_of(std::begin(kHidden), std::end(kHidden), [&](const wchar_t* h2) { return name == h2; }) ||
        toLower(a[L"display"]) == L"none" || toLower(a[L"visibility"]) == L"hidden")
      s.hidden = true;
    if (auto it = a.find(L"fill"); it != a.end()) s.fillNone = !svgColor(it->second, s.fill);
    if (auto it = a.find(L"fill-opacity"); it != a.end()) s.fillOpacity = std::wcstof(it->second.c_str(), nullptr);
    if (auto it = a.find(L"opacity"); it != a.end()) s.opacity *= std::wcstof(it->second.c_str(), nullptr);
    if (auto it = a.find(L"font-size"); it != a.end()) {
      float fs = svgLength(it->second, s.fontSize, s.fontSize);
      if (fs > 0) s.fontSize = fs;
    }
    if (auto it = a.find(L"font-family"); it != a.end()) s.family = firstFamily(it->second);
    if (auto it = a.find(L"font-weight"); it != a.end()) {
      std::wstring fw = toLower(it->second);
      s.weight = fw == L"bold" || fw == L"bolder" ? 700 : fw == L"normal" ? 400 : fw == L"lighter" ? 300
                                                                                    : std::max(100, _wtoi(fw.c_str()));
    }
    if (auto it = a.find(L"font-style"); it != a.end()) s.italic = toLower(it->second) != L"normal";
    if (auto it = a.find(L"text-anchor"); it != a.end()) {
      std::wstring ta = toLower(it->second);
      s.anchor = ta == L"middle" ? 1 : ta == L"end" ? 2 : 0;
    }
    if (auto it = a.find(L"transform"); it != a.end()) s.m = svgTransform(it->second) * s.m;
    if (name == L"svg" && !root) {
      root = true;
      rootW = svgLength(a[L"width"], 16, 0);
      rootH = svgLength(a[L"height"], 16, 0);
      auto vb = numberList(a[L"viewbox"]);
      if (vb.size() >= 4 && vb[2] > 0 && vb[3] > 0) {
        facts.viewBox = true;
        facts.vbX = vb[0];
        facts.vbY = vb[1];
        facts.vbW = vb[2];
        facts.vbH = vb[3];
      }
    }
    if (name == L"text" && !inText && !s.hidden && !s.fillNone) {
      auto xs = numberList(a[L"x"]), ys = numberList(a[L"y"]);
      cur = SvgText{};
      cur.x = xs.empty() ? 0 : xs[0];
      cur.y = ys.empty() ? 0 : ys[0];
      cur.size = s.fontSize;
      cur.family = s.family;
      cur.weight = s.weight;
      cur.italic = s.italic;
      cur.color = s.fill.withAlpha(std::clamp(s.fill.a * s.fillOpacity * s.opacity, 0.0f, 1.0f));
      cur.anchor = s.anchor;
      cur.m = s.m;
      buf.clear();
      if (!selfClose) {
        inText = true;
        textDepth = stack.size() + 1;
      }
    }
    if (!selfClose) stack.push_back(std::move(s));
  }
  if (!root) {
    err = L"The file is not an SVG image.";
    return false;
  }
  if (rootW > 0 && rootH > 0) {
    w = rootW;
    h = rootH;
  } else if (facts.viewBox) {
    w = rootW > 0 ? rootW : (rootH > 0 ? rootH * facts.vbW / facts.vbH : facts.vbW);
    h = rootH > 0 ? rootH : w * facts.vbH / facts.vbW;
  } else {
    w = rootW > 0 ? rootW : 300;
    h = rootH > 0 ? rootH : 150;
  }
  return true;
}

// The first frame of an encoded image, with its size; null when WIC cannot decode it.
// `bytes` must outlive the frame.
ComPtr<IWICBitmapFrameDecode> firstFrame(IWICImagingFactory* wic, const std::string& bytes, UINT& w, UINT& h) {
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  w = h = 0;
  if (!wic || FAILED(wic->CreateStream(stream.GetAddressOf())) ||
      FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())),
                                          static_cast<DWORD>(bytes.size()))) ||
      FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                          decoder.GetAddressOf())) ||
      FAILED(decoder->GetFrame(0, frame.GetAddressOf())) || FAILED(frame->GetSize(&w, &h)) || !w || !h)
    return nullptr;
  return frame;
}

bool rasterSize(IWICImagingFactory* wic, const std::string& bytes, float& w, float& h, std::wstring& err) {
  UINT fw = 0, fh = 0;
  if (!firstFrame(wic, bytes, fw, fh)) {
    err = L"The data is not an image format Windows can decode.";
    return false;
  }
  w = static_cast<float>(fw);
  h = static_cast<float>(fh);
  return true;
}

struct Job {
  std::wstring key;
  SourceKind kind;
};

struct Loaded {
  std::wstring key;
  bool ok = false;
  std::wstring error;
  std::string bytes;
  bool svg = false;
  float w = 0, h = 0;
  SvgFacts facts;
  uint64_t fileTime = 0;
};

Loaded load(const Job& job, IWICImagingFactory* wic) {
  Loaded r;
  r.key = job.key;
  std::string bytes;
  bool ok = false;
  switch (job.kind) {
    case SourceKind::File: ok = readLocal(job.key, bytes, r.error, r.fileTime); break;
    case SourceKind::Remote: ok = fetchHttp(job.key, bytes, r.error); break;
    case SourceKind::Data: ok = decodeDataUri(job.key, bytes, r.error); break;
  }
  if (ok) {
    r.svg = looksLikeSvg(bytes);
    ok = r.svg ? parseSvg(bytes, r.facts, r.w, r.h, r.error) : rasterSize(wic, bytes, r.w, r.h, r.error);
  }
  r.ok = ok && r.w > 0 && r.h > 0;
  if (r.ok) r.bytes = std::move(bytes);
  return r;
}

}  // namespace

// ---------------------------------------------------------------------------

struct MarkdownImages::Shared {
  std::mutex m;
  std::condition_variable cv;
  bool stop = false;
  std::deque<Job> jobs;
  std::vector<Loaded> done;
  HWND hwnd = nullptr;
  UINT message = 0;
  int threads = 0;

  static void worker(std::shared_ptr<Shared> self) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.GetAddressOf()));
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> l(self->m);
        self->cv.wait(l, [&] { return self->stop || !self->jobs.empty(); });
        if (self->stop) break;
        job = std::move(self->jobs.front());
        self->jobs.pop_front();
      }
      Loaded r = load(job, wic.Get());
      HWND hwnd;
      UINT message;
      {
        std::lock_guard<std::mutex> l(self->m);
        if (self->stop) break;
        self->done.push_back(std::move(r));
        hwnd = self->hwnd;
        message = self->message;
      }
      if (hwnd) PostMessageW(hwnd, message, 0, 0);
    }
    wic.Reset();
    if (SUCCEEDED(co)) CoUninitialize();
  }
};

struct MarkdownImages::Entry {
  SourceKind kind = SourceKind::File;
  State state = State::Loading;
  std::wstring error;
  bool pending = false;  // queued or loading
  uint64_t lastUsed = 0;
  uint64_t fileTime = 0;

  std::string bytes;  // the encoded image, kept to rebuild device resources
  bool svg = false;
  float w = 0, h = 0;
  SvgFacts facts;

  // Device resources for one render target generation.
  uint64_t generation = UINT64_MAX;
  bool deviceFailed = false;
  ComPtr<ID2D1Bitmap> bitmap;
  ComPtr<ID2D1SvgDocument> svgDoc;
  struct TextRun {
    ComPtr<IDWriteTextLayout> layout;
    float x = 0, y = 0;
    Color color;
    D2D1::Matrix3x2F m;
  };
  std::vector<TextRun> textRuns;
  bool textBuilt = false;
};

MarkdownImages::MarkdownImages() : shared_(std::make_shared<Shared>()) {}

MarkdownImages::~MarkdownImages() {
  {
    std::lock_guard<std::mutex> l(shared_->m);
    shared_->stop = true;
    shared_->jobs.clear();
    shared_->hwnd = nullptr;
  }
  shared_->cv.notify_all();
}

void MarkdownImages::setNotify(HWND hwnd, UINT message) {
  std::lock_guard<std::mutex> l(shared_->m);
  shared_->hwnd = hwnd;
  shared_->message = message;
}

void MarkdownImages::setRemoteAllowed(bool allowed) {
  if (allowed == remoteAllowed_) return;
  remoteAllowed_ = allowed;
  ++revision_;
}

void MarkdownImages::enqueue(const std::wstring& key, Entry& e) {
  e.pending = true;
  {
    std::lock_guard<std::mutex> l(shared_->m);
    shared_->jobs.push_back({key, e.kind});
    if (shared_->threads < kWorkers) {
      ++shared_->threads;
      std::thread(&Shared::worker, shared_).detach();
    }
  }
  shared_->cv.notify_one();
}

void MarkdownImages::beginLayout() { ++layoutCounter_; }

MarkdownImages::Info MarkdownImages::request(const std::wstring& src, const std::wstring& baseDir,
                                             const std::wstring& root, bool wiki) {
  Info info;
  Resolved r;
  if (wiki && !startsWithNoCase(src, L"http://") && !startsWithNoCase(src, L"https://") && !startsWithNoCase(src, L"data:")) {
    WikiResolution w = resolveWikiLink(src, baseDir, root);
    if (!w.exists) {
      info.error = w.error.empty() ? L"No file named " + src + L" in this notes folder." : w.error;
      return info;
    }
    r.kind = SourceKind::File;
    r.key = w.path;
  } else {
    r = resolveSource(src, baseDir);
  }
  if (!r.error.empty()) {
    info.error = r.error;
    return info;
  }
  info.key = r.key;
  if (r.kind == SourceKind::Remote && !remoteAllowed_) {
    info.error = L"Remote images are turned off (\"markdown_remote_images\": false).";
    return info;
  }
  auto& slot = entries_[r.key];
  if (!slot) {
    slot = std::make_unique<Entry>();
    slot->kind = r.kind;
    enqueue(r.key, *slot);
  }
  Entry& e = *slot;
  e.lastUsed = layoutCounter_;
  if (e.kind == SourceKind::File && !e.pending) {
    // Reload a local image that changed (or appeared) on disk; the old one shows meanwhile.
    uint64_t t = 0;
    fileStat(r.key, &t, nullptr);
    if (t != e.fileTime) enqueue(r.key, e);
  }
  info.state = e.state;
  info.w = e.w;
  info.h = e.h;
  info.error = e.error;
  return info;
}

void MarkdownImages::endLayout() {
  if (entries_.size() <= kKeepUnused) return;
  std::vector<std::pair<uint64_t, std::wstring>> unused;
  for (const auto& [key, e] : entries_)
    if (e->lastUsed != layoutCounter_ && !e->pending) unused.emplace_back(e->lastUsed, key);
  std::sort(unused.begin(), unused.end());
  for (const auto& u : unused) {
    if (entries_.size() <= kKeepUnused) break;
    entries_.erase(u.second);
  }
}

bool MarkdownImages::takeResults() {
  std::vector<Loaded> done;
  {
    std::lock_guard<std::mutex> l(shared_->m);
    done.swap(shared_->done);
  }
  bool changed = false;
  for (auto& r : done) {
    auto it = entries_.find(r.key);
    if (it == entries_.end()) continue;
    Entry& e = *it->second;
    e.pending = false;
    e.fileTime = r.fileTime;
    if (r.ok) {
      e.state = State::Ready;
      e.error.clear();
      e.bytes = std::move(r.bytes);
      e.svg = r.svg;
      e.w = r.w;
      e.h = r.h;
      e.facts = std::move(r.facts);
    } else {
      e.state = State::Failed;
      e.error = r.error;
      e.bytes.clear();
    }
    e.generation = UINT64_MAX;  // rebuild device resources
    e.textRuns.clear();
    e.textBuilt = false;
    changed = true;
  }
  if (changed) ++revision_;
  return changed;
}

void MarkdownImages::draw(const std::wstring& key, const RectF& r) {
  if (!paint_) return;
  Gfx& g = *paint_;
  auto it = entries_.find(key);
  if (it == entries_.end() || it->second->state != State::Ready) {
    // Reserved space for an image that is still loading.
    g.fillRounded(r, 3, Color::hex(0x808080, 0.12f));
    g.strokeRounded(r, 3, Color::hex(0x808080, 0.35f), 1);
    return;
  }
  Entry& e = *it->second;
  if (e.generation != g.targetGeneration()) {
    e.bitmap.Reset();
    e.svgDoc.Reset();
    e.deviceFailed = false;
    e.generation = g.targetGeneration();
  }
  if (e.svg)
    drawSvg(g, e, r);
  else
    drawRaster(g, e, r);
}

void MarkdownImages::drawRaster(Gfx& g, Entry& e, const RectF& r) {
  ID2D1RenderTarget* rt = g.target();
  if (!e.bitmap && !e.deviceFailed) {
    e.deviceFailed = true;
    if (!wic_) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.GetAddressOf()));
    ComPtr<IWICFormatConverter> conv;
    UINT fw = 0, fh = 0;
    if (ComPtr<IWICBitmapFrameDecode> frame = firstFrame(wic_.Get(), e.bytes, fw, fh)) {
      ComPtr<IWICBitmapSource> source = frame;
      UINT cap = std::min<UINT>(rt->GetMaximumBitmapSize(), kMaxDecodeSide);
      if (fw > cap || fh > cap) {
        double s = std::min(static_cast<double>(cap) / fw, static_cast<double>(cap) / fh);
        ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(wic_->CreateBitmapScaler(scaler.GetAddressOf())) &&
            SUCCEEDED(scaler->Initialize(frame.Get(), std::max<UINT>(1, static_cast<UINT>(fw * s)),
                                         std::max<UINT>(1, static_cast<UINT>(fh * s)), WICBitmapInterpolationModeFant)))
          source = scaler;
      }
      if (SUCCEEDED(wic_->CreateFormatConverter(conv.GetAddressOf())) &&
          SUCCEEDED(conv->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeMedianCut)) &&
          SUCCEEDED(rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, e.bitmap.GetAddressOf())))
        e.deviceFailed = false;
    }
  }
  if (!e.bitmap) {
    g.strokeRounded(r, 3, Color::hex(0x808080, 0.5f), 1);
    return;
  }
  D2D1_RECT_F dest = D2D1::RectF(r.l, r.t, r.r, r.b);
  ComPtr<ID2D1DeviceContext> dc;
  if (SUCCEEDED(rt->QueryInterface(IID_PPV_ARGS(dc.GetAddressOf()))))
    dc->DrawBitmap(e.bitmap.Get(), &dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
  else
    rt->DrawBitmap(e.bitmap.Get(), dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void MarkdownImages::drawSvg(Gfx& g, Entry& e, const RectF& r) {
  ComPtr<ID2D1DeviceContext5> dc;
  if (FAILED(g.target()->QueryInterface(IID_PPV_ARGS(dc.GetAddressOf())))) {
    g.strokeRounded(r, 3, Color::hex(0x808080, 0.5f), 1);  // SVG needs Windows 10 1703 or later
    return;
  }
  if (!e.svgDoc && !e.deviceFailed) {
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(e.bytes.data()), static_cast<UINT>(e.bytes.size())));
    if (!stream || FAILED(dc->CreateSvgDocument(stream.Get(), D2D1::SizeF(e.w, e.h), e.svgDoc.GetAddressOf())))
      e.deviceFailed = true;
  }
  if (!e.textBuilt) {
    e.textBuilt = true;
    for (const SvgText& t : e.facts.texts) {
      auto fmt = g.createFormat(t.family, std::max(0.5f, t.size), static_cast<DWRITE_FONT_WEIGHT>(t.weight),
                                t.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL);
      if (!fmt) continue;
      fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      auto layout = g.layout(t.text, fmt.Get(), 100000, 100000);
      if (!layout) continue;
      DWRITE_TEXT_METRICS m{};
      layout->GetMetrics(&m);
      DWRITE_LINE_METRICS line{};
      UINT32 lines = 0;
      layout->GetLineMetrics(&line, 1, &lines);
      float dx = t.anchor == 1 ? -m.width / 2 : (t.anchor == 2 ? -m.width : 0);
      e.textRuns.push_back({layout, t.x + dx, t.y - line.baseline, t.color, t.m});
    }
  }
  D2D1_MATRIX_3X2_F saved;
  dc->GetTransform(&saved);
  D2D1::Matrix3x2F place = D2D1::Matrix3x2F::Scale(r.w() / e.w, r.h() / e.h) *
                           D2D1::Matrix3x2F::Translation(r.l, r.t) * *D2D1::Matrix3x2F::ReinterpretBaseType(&saved);
  // viewBox to viewport (preserveAspectRatio="xMidYMid meet"), for the text overlay.
  D2D1::Matrix3x2F view = D2D1::Matrix3x2F::Identity();
  if (e.facts.viewBox) {
    float s = std::min(e.w / e.facts.vbW, e.h / e.facts.vbH);
    view = D2D1::Matrix3x2F::Scale(s, s) *
           D2D1::Matrix3x2F::Translation((e.w - e.facts.vbW * s) / 2 - e.facts.vbX * s,
                                         (e.h - e.facts.vbH * s) / 2 - e.facts.vbY * s);
  }
  g.pushClip(r);
  dc->SetTransform(place);
  if (e.svgDoc) dc->DrawSvgDocument(e.svgDoc.Get());
  for (const auto& t : e.textRuns) {
    dc->SetTransform(t.m * view * place);
    g.drawLayout(t.layout.Get(), t.x, t.y, t.color);
  }
  dc->SetTransform(saved);
  g.popClip();
}

}  // namespace verso
