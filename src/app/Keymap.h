#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace verso {

struct KeyCombo {
  UINT vk = 0;
  bool ctrl = false, shift = false, alt = false;
  bool operator==(const KeyCombo& o) const { return vk == o.vk && ctrl == o.ctrl && shift == o.shift && alt == o.alt; }
};

struct Binding {
  std::vector<KeyCombo> keys;  // one combo, or two for a chord ("ctrl+k, ctrl+b")
  std::string command;
};

class Keymap {
 public:
  enum class Result { None, Prefix, Command };

  void loadDefaults();
  // Appends user bindings (JSON: [{"keys": [...], "command": "..."}]).
  bool loadUserFile(const std::wstring& path, std::wstring* error);
  Result lookup(const std::optional<KeyCombo>& pending, const KeyCombo& combo, std::string& command) const;
  std::wstring shortcutFor(std::string_view command) const;

  static std::optional<KeyCombo> parseCombo(std::string_view spec);
  static std::optional<std::vector<KeyCombo>> parseKeys(const std::vector<std::string>& specs);
  static std::wstring comboName(const KeyCombo& k);
  static std::string defaultFileContents();

 private:
  void bind(std::initializer_list<const char*> keys, const char* command);
  std::vector<Binding> bindings_;  // later entries take precedence
};

}  // namespace verso
