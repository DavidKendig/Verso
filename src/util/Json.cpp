#include "util/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/TextUtil.h"

namespace verso::json {

Value::Value(std::wstring_view s) : type(Type::String), string(wideToUtf8(s)) {}

const Value* Value::get(std::string_view key) const {
  if (type != Type::Object) return nullptr;
  for (const auto& m : object)
    if (m.key == key) return &m.value;
  return nullptr;
}

Value& Value::set(std::string_view key, Value v) {
  if (type != Type::Object) {
    *this = makeObject();
  }
  for (auto& m : object)
    if (m.key == key) {
      m.value = std::move(v);
      return m.value;
    }
  object.push_back({std::string(key), std::move(v)});
  return object.back().value;
}

Value& Value::push(Value v) {
  if (type != Type::Array) *this = makeArray();
  array.push_back(std::move(v));
  return array.back();
}

std::wstring Value::asWString(const std::wstring& def) const {
  return type == Type::String ? utf8ToWide(string) : def;
}

bool Value::getBool(std::string_view key, bool def) const {
  const Value* v = get(key);
  return v ? v->asBool(def) : def;
}

double Value::getNumber(std::string_view key, double def) const {
  const Value* v = get(key);
  return v ? v->asNumber(def) : def;
}

std::wstring Value::getWString(std::string_view key, const std::wstring& def) const {
  const Value* v = get(key);
  return v ? v->asWString(def) : def;
}

namespace {

class Parser {
 public:
  explicit Parser(std::string_view s) : s_(s) {}

  std::optional<Value> run(std::string* error) {
    skip();
    Value v;
    if (!value(v)) {
      if (error) *error = err_ + " at offset " + std::to_string(i_);
      return std::nullopt;
    }
    skip();
    if (i_ != s_.size()) {
      if (error) *error = "trailing characters at offset " + std::to_string(i_);
      return std::nullopt;
    }
    return v;
  }

 private:
  void skip() {
    for (;;) {
      while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
      if (i_ + 1 < s_.size() && s_[i_] == '/' && s_[i_ + 1] == '/') {
        while (i_ < s_.size() && s_[i_] != '\n') ++i_;
        continue;
      }
      if (i_ + 1 < s_.size() && s_[i_] == '/' && s_[i_ + 1] == '*') {
        size_t e = s_.find("*/", i_ + 2);
        i_ = e == std::string_view::npos ? s_.size() : e + 2;
        continue;
      }
      break;
    }
  }

  bool fail(const char* msg) {
    err_ = msg;
    return false;
  }

  bool value(Value& out) {
    if (i_ >= s_.size()) return fail("unexpected end");
    char c = s_[i_];
    if (c == '{') return object(out);
    if (c == '[') return array(out);
    if (c == '"') {
      out.type = Value::Type::String;
      return str(out.string);
    }
    if (s_.substr(i_, 4) == "true") {
      i_ += 4;
      out = Value(true);
      return true;
    }
    if (s_.substr(i_, 5) == "false") {
      i_ += 5;
      out = Value(false);
      return true;
    }
    if (s_.substr(i_, 4) == "null") {
      i_ += 4;
      out = Value();
      return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      size_t b = i_;
      ++i_;
      while (i_ < s_.size() && (isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' || s_[i_] == 'e' ||
                                s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-'))
        ++i_;
      std::string num(s_.substr(b, i_ - b));
      out = Value(std::strtod(num.c_str(), nullptr));
      return true;
    }
    return fail("unexpected character");
  }

  // Nesting deeper than this is refused rather than overflowing the stack.
  static constexpr int kMaxDepth = 512;
  int depth_ = 0;
  struct DepthGuard {
    int& d;
    ~DepthGuard() { --d; }
  };

  bool object(Value& out) {
    DepthGuard guard{++depth_};
    if (depth_ > kMaxDepth) return fail("nesting too deep");
    out = Value::makeObject();
    ++i_;
    skip();
    while (i_ < s_.size() && s_[i_] != '}') {
      std::string key;
      if (s_[i_] != '"' || !str(key)) return fail("expected key");
      skip();
      if (i_ >= s_.size() || s_[i_] != ':') return fail("expected ':'");
      ++i_;
      skip();
      Value v;
      if (!value(v)) return false;
      out.set(key, std::move(v));
      skip();
      if (i_ < s_.size() && s_[i_] == ',') {
        ++i_;
        skip();
      } else if (i_ < s_.size() && s_[i_] != '}') {
        return fail("expected ',' or '}'");
      }
    }
    if (i_ >= s_.size()) return fail("unterminated object");
    ++i_;
    return true;
  }

  bool array(Value& out) {
    DepthGuard guard{++depth_};
    if (depth_ > kMaxDepth) return fail("nesting too deep");
    out = Value::makeArray();
    ++i_;
    skip();
    while (i_ < s_.size() && s_[i_] != ']') {
      Value v;
      if (!value(v)) return false;
      out.array.push_back(std::move(v));
      skip();
      if (i_ < s_.size() && s_[i_] == ',') {
        ++i_;
        skip();
      } else if (i_ < s_.size() && s_[i_] != ']') {
        return fail("expected ',' or ']'");
      }
    }
    if (i_ >= s_.size()) return fail("unterminated array");
    ++i_;
    return true;
  }

  static void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  bool hex4(unsigned& out) {
    if (i_ + 4 > s_.size()) return false;
    out = 0;
    for (int k = 0; k < 4; ++k) {
      char h = s_[i_++];
      out <<= 4;
      if (h >= '0' && h <= '9') out |= static_cast<unsigned>(h - '0');
      else if (h >= 'a' && h <= 'f') out |= static_cast<unsigned>(h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') out |= static_cast<unsigned>(h - 'A' + 10);
      else return false;
    }
    return true;
  }

  bool str(std::string& out) {
    ++i_;
    out.clear();
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i_ >= s_.size()) break;
      char e = s_[i_++];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          unsigned cp;
          if (!hex4(cp)) return fail("bad \\u escape");
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            // A high surrogate pairs only with a following low surrogate escape.
            size_t save = i_;
            unsigned lo = 0;
            if (i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
              i_ += 2;
              if (!hex4(lo)) return fail("bad \\u escape");
            }
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              i_ = save;
              cp = 0xFFFD;
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;  // a lone low surrogate
          }
          appendUtf8(out, cp);
          break;
        }
        default: out += e; break;
      }
    }
    return fail("unterminated string");
  }

  std::string_view s_;
  size_t i_ = 0;
  std::string err_;
};

void writeString(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

void write(std::string& out, const Value& v, int indent, int depth) {
  auto newline = [&](int d) {
    if (indent <= 0) return;
    out += '\n';
    out.append(static_cast<size_t>(d * indent), ' ');
  };
  switch (v.type) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.boolean ? "true" : "false"; break;
    case Value::Type::Number: {
      if (std::isfinite(v.number) && v.number == std::floor(v.number) && std::fabs(v.number) < 1e15) {
        out += std::to_string(static_cast<long long>(v.number));
      } else {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.17g", v.number);
        out += buf;
      }
      break;
    }
    case Value::Type::String: writeString(out, v.string); break;
    case Value::Type::Array:
      out += '[';
      for (size_t i = 0; i < v.array.size(); ++i) {
        if (i) out += ',';
        newline(depth + 1);
        write(out, v.array[i], indent, depth + 1);
      }
      if (!v.array.empty()) newline(depth);
      out += ']';
      break;
    case Value::Type::Object:
      out += '{';
      for (size_t i = 0; i < v.object.size(); ++i) {
        if (i) out += ',';
        newline(depth + 1);
        writeString(out, v.object[i].key);
        out += indent > 0 ? ": " : ":";
        write(out, v.object[i].value, indent, depth + 1);
      }
      if (!v.object.empty()) newline(depth);
      out += '}';
      break;
  }
}

}  // namespace

std::optional<Value> parse(std::string_view text, std::string* error) {
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text.remove_prefix(3);
  return Parser(text).run(error);
}

std::string serialize(const Value& v, int indent) {
  std::string out;
  write(out, v, indent, 0);
  return out;
}

}  // namespace verso::json
