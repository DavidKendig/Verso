#include "app/Keymap.h"

#include <cctype>
#include <cwctype>

#include "core/Encoding.h"
#include "core/TextUtil.h"
#include "util/Json.h"

namespace verso {

namespace {

std::optional<UINT> keyNameToVk(std::string name) {
  for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (name.size() == 1) {
    char c = name[0];
    if (c >= 'a' && c <= 'z') return static_cast<UINT>('A' + (c - 'a'));
    if (c >= '0' && c <= '9') return static_cast<UINT>(c);
    SHORT r = VkKeyScanW(static_cast<wchar_t>(c));
    if (r != -1) return static_cast<UINT>(r & 0xFF);
    return std::nullopt;
  }
  if (name.size() >= 2 && name[0] == 'f' && std::isdigit(static_cast<unsigned char>(name[1]))) {
    int n = std::atoi(name.c_str() + 1);
    if (n >= 1 && n <= 24) return static_cast<UINT>(VK_F1 + n - 1);
  }
  struct Named {
    const char* name;
    UINT vk;
  };
  static const Named table[] = {
      {"up", VK_UP},           {"down", VK_DOWN},         {"left", VK_LEFT},          {"right", VK_RIGHT},
      {"home", VK_HOME},       {"end", VK_END},           {"pageup", VK_PRIOR},       {"pagedown", VK_NEXT},
      {"insert", VK_INSERT},   {"delete", VK_DELETE},     {"backspace", VK_BACK},     {"tab", VK_TAB},
      {"enter", VK_RETURN},    {"escape", VK_ESCAPE},     {"space", VK_SPACE},        {"pause", VK_PAUSE},
      {"break", VK_CANCEL},    {"plus", VK_OEM_PLUS},     {"equals", VK_OEM_PLUS},    {"minus", VK_OEM_MINUS},
      {"keypad_enter", VK_RETURN}, {"keypad_plus", VK_ADD}, {"keypad_minus", VK_SUBTRACT},
      {"keypad_multiply", VK_MULTIPLY}, {"keypad_divide", VK_DIVIDE}, {"keypad_period", VK_DECIMAL},
      {"backquote", VK_OEM_3},
  };
  for (const auto& t : table)
    if (name == t.name) return t.vk;
  if (name.rfind("keypad", 0) == 0 && name.size() == 7 && std::isdigit(static_cast<unsigned char>(name[6])))
    return static_cast<UINT>(VK_NUMPAD0 + (name[6] - '0'));
  return std::nullopt;
}

}  // namespace

std::optional<KeyCombo> Keymap::parseCombo(std::string_view spec) {
  KeyCombo k;
  while (!spec.empty() && spec.front() == ' ') spec.remove_prefix(1);
  while (!spec.empty() && spec.back() == ' ') spec.remove_suffix(1);
  std::string s(spec);
  // Split on '+', treating a trailing "++" as the plus key.
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+' && i > start) {
      parts.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  if (start < s.size()) parts.push_back(s.substr(start));
  if (parts.empty()) return std::nullopt;
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    std::string m = parts[i];
    for (auto& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (m == "ctrl" || m == "primary") k.ctrl = true;
    else if (m == "shift") k.shift = true;
    else if (m == "alt") k.alt = true;
    else return std::nullopt;
  }
  auto vk = keyNameToVk(parts.back());
  if (!vk) return std::nullopt;
  k.vk = *vk;
  return k;
}

std::optional<std::vector<KeyCombo>> Keymap::parseKeys(const std::vector<std::string>& specs) {
  std::vector<KeyCombo> out;
  for (const auto& s : specs) {
    auto k = parseCombo(s);
    if (!k) return std::nullopt;
    out.push_back(*k);
  }
  if (out.empty() || out.size() > 2) return std::nullopt;
  return out;
}

void Keymap::bind(std::initializer_list<const char*> keys, const char* command) {
  std::vector<std::string> specs(keys.begin(), keys.end());
  if (auto k = parseKeys(specs)) bindings_.push_back({*k, command});
}

void Keymap::loadDefaults() {
  bindings_.clear();
  // File
  bind({"ctrl+n"}, "new_file");
  bind({"ctrl+o"}, "open_file");
  bind({"ctrl+s"}, "save");
  bind({"ctrl+shift+s"}, "save_as");
  bind({"ctrl+w"}, "close_file");
  bind({"ctrl+f4"}, "close_file");
  bind({"ctrl+shift+t"}, "reopen_last_file");
  // View
  bind({"ctrl+k", "ctrl+b"}, "toggle_sidebar");
  bind({"f11"}, "toggle_fullscreen");
  bind({"shift+f11"}, "toggle_distraction_free");
  bind({"ctrl+="}, "increase_font");
  bind({"ctrl+shift+="}, "increase_font");
  bind({"ctrl+keypad_plus"}, "increase_font");
  bind({"ctrl+-"}, "decrease_font");
  bind({"ctrl+keypad_minus"}, "decrease_font");
  // Editing
  bind({"backspace"}, "left_delete");
  bind({"shift+backspace"}, "left_delete");
  bind({"delete"}, "right_delete");
  bind({"enter"}, "insert_newline");
  bind({"shift+enter"}, "insert_newline");
  bind({"ctrl+enter"}, "insert_line_after");
  bind({"ctrl+shift+enter"}, "insert_line_before");
  bind({"ctrl+z"}, "undo");
  bind({"ctrl+shift+z"}, "redo");
  bind({"ctrl+y"}, "redo");
  bind({"ctrl+x"}, "cut");
  bind({"shift+delete"}, "cut");
  bind({"ctrl+c"}, "copy");
  bind({"ctrl+insert"}, "copy");
  bind({"ctrl+v"}, "paste");
  bind({"shift+insert"}, "paste");
  bind({"ctrl+shift+v"}, "paste");
  bind({"tab"}, "tab");
  bind({"shift+tab"}, "unindent");
  bind({"ctrl+]"}, "indent");
  bind({"ctrl+["}, "unindent");
  bind({"ctrl+backspace"}, "delete_word_back");
  bind({"ctrl+delete"}, "delete_word_forward");
  bind({"ctrl+shift+backspace"}, "delete_to_bol");
  bind({"ctrl+shift+delete"}, "delete_to_eol");
  bind({"ctrl+k", "ctrl+k"}, "delete_to_eol");
  bind({"ctrl+k", "ctrl+backspace"}, "delete_to_bol");
  bind({"ctrl+/"}, "toggle_comment");
  bind({"ctrl+shift+/"}, "toggle_block_comment");
  bind({"ctrl+shift+up"}, "swap_line_up");
  bind({"ctrl+shift+down"}, "swap_line_down");
  bind({"ctrl+shift+d"}, "duplicate_line");
  bind({"ctrl+shift+k"}, "delete_line");
  bind({"ctrl+shift+j"}, "join_lines");
  bind({"ctrl+j"}, "join_lines");
  bind({"ctrl+t"}, "transpose");
  bind({"f9"}, "sort_lines");
  bind({"ctrl+f9"}, "sort_lines_case_sensitive");
  bind({"ctrl+k", "ctrl+u"}, "upper_case");
  bind({"ctrl+k", "ctrl+l"}, "lower_case");
  bind({"ctrl+q"}, "toggle_record_macro");
  bind({"ctrl+shift+q"}, "run_macro");
  // Movement
  bind({"left"}, "move_left");
  bind({"right"}, "move_right");
  bind({"up"}, "move_up");
  bind({"down"}, "move_down");
  bind({"shift+left"}, "select_left");
  bind({"shift+right"}, "select_right");
  bind({"shift+up"}, "select_up");
  bind({"shift+down"}, "select_down");
  bind({"ctrl+left"}, "word_left");
  bind({"ctrl+right"}, "word_right");
  bind({"ctrl+shift+left"}, "select_word_left");
  bind({"ctrl+shift+right"}, "select_word_right");
  bind({"alt+left"}, "subword_left");
  bind({"alt+right"}, "subword_right");
  bind({"alt+shift+left"}, "select_subword_left");
  bind({"alt+shift+right"}, "select_subword_right");
  bind({"pageup"}, "page_up");
  bind({"pagedown"}, "page_down");
  bind({"shift+pageup"}, "select_page_up");
  bind({"shift+pagedown"}, "select_page_down");
  bind({"home"}, "line_begin");
  bind({"end"}, "line_end");
  bind({"shift+home"}, "select_line_begin");
  bind({"shift+end"}, "select_line_end");
  bind({"ctrl+home"}, "doc_begin");
  bind({"ctrl+end"}, "doc_end");
  bind({"ctrl+shift+home"}, "select_doc_begin");
  bind({"ctrl+shift+end"}, "select_doc_end");
  bind({"ctrl+up"}, "scroll_line_up");
  bind({"ctrl+down"}, "scroll_line_down");
  bind({"ctrl+m"}, "move_to_bracket");
  bind({"ctrl+k", "ctrl+c"}, "show_at_center");
  // Selection
  bind({"ctrl+a"}, "select_all");
  bind({"ctrl+shift+l"}, "split_selection_into_lines");
  bind({"ctrl+alt+up"}, "add_cursor_above");
  bind({"ctrl+alt+down"}, "add_cursor_below");
  bind({"escape"}, "cancel");
  bind({"ctrl+l"}, "expand_selection_line");
  bind({"ctrl+shift+m"}, "expand_selection_brackets");
  bind({"ctrl+d"}, "find_under_expand");
  bind({"ctrl+k", "ctrl+d"}, "find_under_expand_skip");
  bind({"alt+f3"}, "find_all_under");
  // Find
  bind({"ctrl+f"}, "show_find");
  bind({"ctrl+h"}, "show_replace");
  bind({"ctrl+i"}, "incremental_find");
  bind({"f3"}, "find_next");
  bind({"shift+f3"}, "find_prev");
  bind({"ctrl+f3"}, "find_under");
  bind({"ctrl+shift+f3"}, "find_under_prev");
  bind({"ctrl+e"}, "use_selection_for_find");
  bind({"ctrl+shift+h"}, "replace_next");
  bind({"ctrl+shift+f"}, "find_in_files");
  // Git
  bind({"ctrl+shift+g"}, "toggle_git_mode");
  bind({"ctrl+backquote"}, "toggle_terminal");
  // Markdown
  bind({"ctrl+k", "ctrl+m"}, "toggle_markdown_mode");
  bind({"ctrl+k", "ctrl+r"}, "md_cycle_view");
  bind({"ctrl+."}, "next_modification");
  bind({"ctrl+,"}, "prev_modification");
  bind({"ctrl+k", "ctrl+z"}, "revert_hunk");
  // Goto
  bind({"ctrl+p"}, "goto_anything");
  bind({"ctrl+shift+p"}, "command_palette");
  bind({"ctrl+r"}, "goto_symbol");
  bind({"ctrl+g"}, "goto_line");
  bind({"ctrl+pagedown"}, "next_tab");
  bind({"ctrl+pageup"}, "prev_tab");
  bind({"ctrl+tab"}, "next_tab");
  bind({"ctrl+shift+tab"}, "prev_tab");
  for (int i = 1; i <= 8; ++i) {
    std::string key = "alt+" + std::to_string(i);
    std::string cmd = "select_tab:" + std::to_string(i);
    if (auto k = parseCombo(key)) bindings_.push_back({{*k}, cmd});
  }
  bind({"alt+9"}, "select_last_tab");
}

bool Keymap::loadUserFile(const std::wstring& path, std::wstring* error) {
  std::string bytes;
  if (!readFileBytes(path, bytes, nullptr)) return true;
  std::string err;
  auto v = json::parse(bytes, &err);
  if (!v || !v->isArray()) {
    if (error) *error = L"Keymap.json: " + utf8ToWide(err.empty() ? "expected an array" : err);
    return false;
  }
  for (const auto& b : v->array) {
    const json::Value* keys = b.get("keys");
    std::string command = b.get("command") ? b.get("command")->asString() : "";
    if (!keys || !keys->isArray() || command.empty()) continue;
    std::vector<std::string> specs;
    for (const auto& k : keys->array) specs.push_back(k.asString());
    if (auto parsed = parseKeys(specs)) bindings_.push_back({*parsed, command});
  }
  return true;
}

Keymap::Result Keymap::lookup(const std::optional<KeyCombo>& pending, const KeyCombo& combo,
                              std::string& command) const {
  if (pending) {
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it) {
      if (it->keys.size() == 2 && it->keys[0] == *pending && it->keys[1] == combo) {
        command = it->command;
        return Result::Command;
      }
    }
    return Result::None;
  }
  for (const auto& b : bindings_)
    if (b.keys.size() == 2 && b.keys[0] == combo) return Result::Prefix;
  for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it) {
    if (it->keys.size() == 1 && it->keys[0] == combo) {
      command = it->command;
      return Result::Command;
    }
  }
  return Result::None;
}

std::wstring Keymap::comboName(const KeyCombo& k) {
  std::wstring s;
  if (k.ctrl) s += L"Ctrl+";
  if (k.alt) s += L"Alt+";
  if (k.shift) s += L"Shift+";
  switch (k.vk) {
    case VK_UP: s += L"Up"; break;
    case VK_DOWN: s += L"Down"; break;
    case VK_LEFT: s += L"Left"; break;
    case VK_RIGHT: s += L"Right"; break;
    case VK_HOME: s += L"Home"; break;
    case VK_END: s += L"End"; break;
    case VK_PRIOR: s += L"PgUp"; break;
    case VK_NEXT: s += L"PgDn"; break;
    case VK_INSERT: s += L"Ins"; break;
    case VK_DELETE: s += L"Del"; break;
    case VK_BACK: s += L"Backspace"; break;
    case VK_TAB: s += L"Tab"; break;
    case VK_RETURN: s += L"Enter"; break;
    case VK_ESCAPE: s += L"Esc"; break;
    case VK_SPACE: s += L"Space"; break;
    case VK_ADD: s += L"Num+"; break;
    case VK_SUBTRACT: s += L"Num-"; break;
    default:
      if (k.vk >= VK_F1 && k.vk <= VK_F24) {
        s += L"F" + std::to_wstring(k.vk - VK_F1 + 1);
      } else {
        UINT ch = MapVirtualKeyW(k.vk, MAPVK_VK_TO_CHAR) & 0x7FFF;
        if (ch) s += static_cast<wchar_t>(std::towupper(static_cast<wint_t>(ch)));
        else s += L"?";
      }
  }
  return s;
}

std::wstring Keymap::shortcutFor(std::string_view command) const {
  for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it) {
    if (it->command != command) continue;
    std::wstring s = comboName(it->keys[0]);
    if (it->keys.size() > 1) s += L", " + comboName(it->keys[1]);
    return s;
  }
  return {};
}

std::string Keymap::defaultFileContents() {
  return R"(// Verso user key bindings. Entries here override the defaults.
// Example:
//   { "keys": ["ctrl+shift+r"], "command": "goto_symbol" },
//   { "keys": ["ctrl+k", "ctrl+t"], "command": "title_case" },
// Use the Command Palette (Ctrl+Shift+P) to discover command names; they are
// shown in the "Help: Key Binding Reference" document.
[
]
)";
}

}  // namespace verso
