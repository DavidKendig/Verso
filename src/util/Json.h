#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace verso::json {

struct Member;

// Minimal JSON value. Objects keep insertion order. The parser accepts
// comments and trailing commas, as many settings files do.
struct Value {
  enum class Type { Null, Bool, Number, String, Array, Object };

  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  std::string string;  // UTF-8
  std::vector<Value> array;
  std::vector<Member> object;

  Value() = default;
  Value(bool b) : type(Type::Bool), boolean(b) {}
  Value(int n) : type(Type::Number), number(n) {}
  Value(double n) : type(Type::Number), number(n) {}
  Value(const char* s) : type(Type::String), string(s) {}
  Value(std::string s) : type(Type::String), string(std::move(s)) {}
  Value(std::wstring_view s);
  static Value makeArray() { Value v; v.type = Type::Array; return v; }
  static Value makeObject() { Value v; v.type = Type::Object; return v; }

  bool isObject() const { return type == Type::Object; }
  bool isArray() const { return type == Type::Array; }
  const Value* get(std::string_view key) const;
  Value& set(std::string_view key, Value v);
  Value& push(Value v);

  bool asBool(bool def) const { return type == Type::Bool ? boolean : def; }
  double asNumber(double def) const { return type == Type::Number ? number : def; }
  std::string asString(const std::string& def = {}) const { return type == Type::String ? string : def; }
  std::wstring asWString(const std::wstring& def = {}) const;

  bool getBool(std::string_view key, bool def) const;
  double getNumber(std::string_view key, double def) const;
  std::wstring getWString(std::string_view key, const std::wstring& def = {}) const;
};

struct Member {
  std::string key;
  Value value;
};

std::optional<Value> parse(std::string_view text, std::string* error = nullptr);
std::string serialize(const Value& v, int indent = 2);

}  // namespace verso::json
