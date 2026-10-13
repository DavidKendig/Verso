#include "syntax/Language.h"

#include <algorithm>
#include <cwctype>

#include "core/TextUtil.h"

namespace verso {

namespace {

WordSet words(std::wstring_view list) {
  WordSet out;
  size_t i = 0;
  while (i < list.size()) {
    while (i < list.size() && list[i] == L' ') ++i;
    size_t b = i;
    while (i < list.size() && list[i] != L' ') ++i;
    if (i > b) out.emplace(list.substr(b, i - b));
  }
  return out;
}

std::vector<std::wstring> list(std::wstring_view s) {
  std::vector<std::wstring> out;
  for (const auto& w : words(s)) out.push_back(w);
  return out;
}

void merge(WordSet& into, const WordSet& from) { into.insert(from.begin(), from.end()); }

std::vector<Language> build() {
  std::vector<Language> L;

  {
    Language p;
    p.name = L"Plain Text";
    p.mode = LexMode::Plain;
    p.extensions = list(L"txt text log");
    L.push_back(std::move(p));
  }

  // C family -----------------------------------------------------------------
  const wchar_t* cKeywords =
      L"break case continue default do else for goto if return sizeof switch while _Alignas _Alignof "
      L"_Generic _Static_assert";
  const wchar_t* cStorage = L"auto const extern inline register restrict static struct typedef union enum volatile "
                            L"_Atomic _Noreturn _Thread_local";
  const wchar_t* cTypes =
      L"void char short int long float double signed unsigned bool _Bool size_t ssize_t ptrdiff_t intptr_t "
      L"uintptr_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t wchar_t FILE";
  {
    Language c;
    c.name = L"C";
    c.extensions = list(L"c h");
    c.lineComment = L"//";
    c.blockStart = L"/*";
    c.blockEnd = L"*/";
    c.hashPreprocessor = true;
    c.cFunctions = true;
    c.keywords = words(cKeywords);
    c.storage = words(cStorage);
    c.types = words(cTypes);
    c.constants = words(L"NULL true false EOF stdin stdout stderr");
    c.declType = words(L"struct union enum");
    L.push_back(c);

    Language cpp = c;
    cpp.name = L"C++";
    cpp.extensions = list(L"cpp cc cxx c++ hpp hh hxx h++ inl ipp ixx cppm tpp");
    merge(cpp.keywords,
          words(L"catch co_await co_return co_yield const_cast delete dynamic_cast new noexcept operator "
                L"reinterpret_cast requires static_assert static_cast throw try typeid using and or not xor "
                L"alignas alignof decltype"));
    merge(cpp.storage, words(L"class namespace template typename virtual explicit friend mutable private protected "
                             L"public constexpr consteval constinit override final export concept"));
    merge(cpp.types, words(L"char8_t char16_t char32_t nullptr_t std string wstring string_view vector map set "
                           L"unordered_map unordered_set unique_ptr shared_ptr weak_ptr optional variant array"));
    cpp.constants = words(L"nullptr true false NULL this");
    cpp.declType = words(L"class struct union enum namespace concept");
    cpp.capitalizedTypes = true;
    L.push_back(cpp);

    Language objc = c;
    objc.name = L"Objective-C";
    objc.extensions = list(L"m mm");
    merge(objc.keywords, words(L"@interface @implementation @end @property @synthesize @protocol @class @selector "
                               L"@autoreleasepool @try @catch @finally self super"));
    objc.identExtra = L"@";
    objc.constants = words(L"nil Nil YES NO NULL true false");
    objc.capitalizedTypes = true;
    L.push_back(objc);
  }
  {
    Language cs;
    cs.name = L"C#";
    cs.extensions = list(L"cs csx");
    cs.lineComment = L"//";
    cs.blockStart = L"/*";
    cs.blockEnd = L"*/";
    cs.hashPreprocessor = true;
    cs.cFunctions = true;
    cs.capitalizedTypes = true;
    cs.keywords = words(
        L"as base break case catch checked continue default delegate do else event explicit finally fixed for "
        L"foreach goto if implicit in is lock nameof new operator out params ref return sizeof stackalloc switch "
        L"throw try typeof unchecked using when where while with yield await get set init value");
    cs.storage = words(L"abstract async class const enum extern interface internal namespace override private "
                       L"protected public readonly record sealed static struct unsafe var virtual volatile partial");
    cs.types = words(L"bool byte char decimal double float int long object sbyte short string uint ulong ushort void "
                     L"dynamic nint nuint");
    cs.constants = words(L"true false null this");
    cs.declType = words(L"class struct interface enum record namespace");
    L.push_back(cs);
  }
  {
    Language j;
    j.name = L"Java";
    j.extensions = list(L"java");
    j.lineComment = L"//";
    j.blockStart = L"/*";
    j.blockEnd = L"*/";
    j.atAnnotations = true;
    j.capitalizedTypes = true;
    j.cFunctions = true;
    j.keywords = words(L"assert break case catch continue default do else extends finally for if implements import "
                       L"instanceof new package permits return super switch throw throws try while yield");
    j.storage = words(L"abstract class enum final interface native private protected public record sealed static "
                      L"strictfp synchronized transient var volatile");
    j.types = words(L"boolean byte char double float int long short void String Object");
    j.constants = words(L"true false null this");
    j.declType = words(L"class interface enum record");
    L.push_back(j);

    Language k;
    k.name = L"Kotlin";
    k.extensions = list(L"kt kts");
    k.lineComment = L"//";
    k.blockStart = L"/*";
    k.blockEnd = L"*/";
    k.atAnnotations = true;
    k.capitalizedTypes = true;
    k.tripleQuotes = true;
    k.keywords = words(L"as break by catch continue do else for fun if import in is object package return super "
                       L"throw try typealias when while where get set init constructor");
    k.storage = words(L"abstract annotation class companion const crossinline data enum external final infix inline "
                      L"inner interface internal lateinit noinline open operator out override private protected "
                      L"public reified sealed suspend tailrec val var vararg");
    k.constants = words(L"true false null this it");
    k.declFunction = words(L"fun");
    k.declType = words(L"class interface object");
    L.push_back(k);
  }
  {
    Language js;
    js.name = L"JavaScript";
    js.extensions = list(L"js mjs cjs jsx");
    js.shebangs = list(L"node");
    js.lineComment = L"//";
    js.blockStart = L"/*";
    js.blockEnd = L"*/";
    js.backtickStrings = true;
    js.identExtra = L"$";
    js.capitalizedTypes = true;
    js.keywords = words(L"async await break case catch continue debugger default delete do else export extends "
                        L"finally for from function if import in instanceof new of return super switch throw try "
                        L"typeof void while with yield as get set");
    js.storage = words(L"class const let var static");
    js.constants = words(L"true false null undefined NaN Infinity this");
    js.builtins = words(L"console window document globalThis Math JSON Object Array String Number Boolean Promise "
                        L"Map Set Symbol Error RegExp Date require module exports");
    js.declFunction = words(L"function");
    js.declType = words(L"class");
    L.push_back(js);

    Language ts = js;
    ts.name = L"TypeScript";
    ts.extensions = list(L"ts tsx mts cts");
    ts.shebangs.clear();
    merge(ts.keywords, words(L"declare implements infer is keyof namespace satisfies type unique asserts"));
    merge(ts.storage, words(L"abstract enum interface private protected public readonly override"));
    ts.types = words(L"any boolean never number object string symbol unknown void bigint");
    ts.declType = words(L"class interface type enum namespace");
    L.push_back(ts);

    Language json;
    json.name = L"JSON";
    json.extensions = list(L"json jsonc json5 babelrc eslintrc");
    json.lineComment = L"//";
    json.blockStart = L"/*";
    json.blockEnd = L"*/";
    json.stringQuotes = L"\"";
    json.jsonKeys = true;
    json.constants = words(L"true false null");
    L.push_back(json);
  }
  {
    Language py;
    py.name = L"Python";
    py.extensions = list(L"py pyw pyi pyx");
    py.filenames = list(L"sconstruct sconscript");
    py.shebangs = list(L"python python3 python2");
    py.lineComment = L"#";
    py.tripleQuotes = true;
    py.indentAfterColon = true;
    py.atAnnotations = true;
    py.capitalizedTypes = true;
    py.keywords = words(L"and as assert async await break continue del elif else except finally for from global if "
                        L"import in is lambda nonlocal not or pass raise return try while with yield match case");
    py.storage = words(L"class def");
    py.constants = words(L"True False None self cls NotImplemented Ellipsis");
    py.builtins = words(L"print len range open str int float list dict set frozenset tuple bool bytes type "
                        L"isinstance issubclass super enumerate zip map filter sorted reversed min max sum abs any all "
                        L"iter next getattr setattr hasattr delattr repr hash id input format object property "
                        L"staticmethod classmethod");
    py.declFunction = words(L"def");
    py.declType = words(L"class");
    L.push_back(py);
  }
  {
    Language rs;
    rs.name = L"Rust";
    rs.extensions = list(L"rs");
    rs.lineComment = L"//";
    rs.blockStart = L"/*";
    rs.blockEnd = L"*/";
    rs.stringQuotes = L"\"";
    rs.rustChars = true;
    rs.capitalizedTypes = true;
    rs.keywords = words(L"as async await break continue crate else for if in loop match move return self Self super "
                        L"use where while unsafe dyn ref");
    rs.storage = words(L"const enum extern fn impl let mod mut pub static struct trait type union");
    rs.types = words(L"i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str String Vec Option "
                     L"Result Box");
    rs.constants = words(L"true false None Some Ok Err");
    rs.declFunction = words(L"fn");
    rs.declType = words(L"struct enum trait type mod union");
    L.push_back(rs);
  }
  {
    Language go;
    go.name = L"Go";
    go.extensions = list(L"go");
    go.lineComment = L"//";
    go.blockStart = L"/*";
    go.blockEnd = L"*/";
    go.backtickStrings = true;
    go.keywords = words(L"break case chan continue default defer else fallthrough for go goto if import interface map "
                        L"package range return select switch");
    go.storage = words(L"const func struct type var");
    go.types = words(L"bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string "
                     L"uint uint8 uint16 uint32 uint64 uintptr any");
    go.constants = words(L"true false nil iota");
    go.builtins = words(L"append cap clear close complex copy delete imag len make max min new panic print println "
                        L"real recover");
    go.declFunction = words(L"func");
    go.declType = words(L"type");
    L.push_back(go);
  }
  {
    Language sw;
    sw.name = L"Swift";
    sw.extensions = list(L"swift");
    sw.lineComment = L"//";
    sw.blockStart = L"/*";
    sw.blockEnd = L"*/";
    sw.stringQuotes = L"\"";
    sw.tripleQuotes = true;
    sw.atAnnotations = true;
    sw.capitalizedTypes = true;
    sw.keywords = words(L"as async await break case catch continue default defer do else fallthrough for guard if "
                        L"import in is repeat rethrows return switch throw throws try where while some any");
    sw.storage = words(L"associatedtype class deinit enum extension fileprivate func init inout internal let open "
                       L"operator private protocol public static struct subscript typealias var mutating override "
                       L"final lazy weak");
    sw.constants = words(L"true false nil self Self super");
    sw.declFunction = words(L"func");
    sw.declType = words(L"class struct enum protocol extension");
    L.push_back(sw);
  }

  // Markup -------------------------------------------------------------------
  {
    Language html;
    html.name = L"HTML";
    html.mode = LexMode::Markup;
    html.extensions = list(L"html htm xhtml shtml vue svelte");
    html.blockStart = L"<!--";
    html.blockEnd = L"-->";
    L.push_back(html);

    Language xml = html;
    xml.name = L"XML";
    xml.extensions = list(L"xml xsd xsl xslt svg plist csproj vcxproj props targets resx xaml config manifest");
    L.push_back(xml);
  }
  {
    Language css;
    css.name = L"CSS";
    css.extensions = list(L"css");
    css.blockStart = L"/*";
    css.blockEnd = L"*/";
    css.identExtra = L"-";
    css.colonKeys = true;
    css.atAnnotations = true;
    css.constants = words(L"important inherit initial unset none auto transparent");
    L.push_back(css);

    Language scss = css;
    scss.name = L"SCSS";
    scss.extensions = list(L"scss sass less");
    scss.lineComment = L"//";
    scss.dollarVariables = true;
    L.push_back(scss);
  }
  {
    Language md;
    md.name = L"Markdown";
    md.mode = LexMode::Markdown;
    md.extensions = list(L"md markdown mdown mkd mdx");
    md.blockStart = L"<!--";
    md.blockEnd = L"-->";
    L.push_back(md);
  }

  // Scripting ----------------------------------------------------------------
  {
    Language sh;
    sh.name = L"Shell Script (Bash)";
    sh.extensions = list(L"sh bash zsh ksh fish");
    sh.filenames = list(L".bashrc .bash_profile .zshrc .profile");
    sh.shebangs = list(L"sh bash zsh ksh dash");
    sh.lineComment = L"#";
    sh.stringQuotes = L"\"'`";
    sh.dollarVariables = true;
    sh.keywords = words(L"if then else elif fi case esac for select while until do done in function time return exit "
                        L"break continue local export readonly declare typeset unset shift source alias eval exec "
                        L"set trap");
    sh.builtins = words(L"echo printf cd pwd read test cat grep sed awk mkdir rm cp mv ls chmod chown");
    sh.declFunction = words(L"function");
    L.push_back(sh);
  }
  {
    Language ps;
    ps.name = L"PowerShell";
    ps.extensions = list(L"ps1 psm1 psd1");
    ps.lineComment = L"#";
    ps.blockStart = L"<#";
    ps.blockEnd = L"#>";
    ps.caseInsensitive = true;
    ps.dollarVariables = true;
    ps.identExtra = L"-";
    ps.keywords = words(L"begin break catch class continue data do dynamicparam else elseif end exit filter finally "
                        L"for foreach from function if in param process return switch throw trap try until using "
                        L"while workflow -eq -ne -gt -ge -lt -le -like -notlike -match -notmatch -and -or -not");
    ps.constants = words(L"$true $false $null");
    ps.declFunction = words(L"function filter");
    ps.declType = words(L"class");
    L.push_back(ps);
  }
  {
    Language bat;
    bat.name = L"Batch File";
    bat.extensions = list(L"bat cmd");
    bat.lineComment = L"::";
    bat.lineComment2 = L"rem ";
    bat.caseInsensitive = true;
    bat.percentVariables = true;
    bat.stringQuotes = L"\"";
    bat.keywords = words(L"if else for in do goto call exit set setlocal endlocal echo off on not exist defined "
                         L"errorlevel shift pause start cd pushd popd equ neq lss leq gtr geq");
    L.push_back(bat);
  }
  {
    Language lua;
    lua.name = L"Lua";
    lua.extensions = list(L"lua");
    lua.lineComment = L"--";
    lua.blockStart = L"--[[";
    lua.blockEnd = L"]]";
    lua.keywords = words(L"and break do else elseif end for goto if in not or repeat return then until while");
    lua.storage = words(L"function local");
    lua.constants = words(L"true false nil self");
    lua.builtins = words(L"print pairs ipairs require table string math os io type tostring tonumber setmetatable "
                         L"getmetatable error pcall xpcall assert select");
    lua.declFunction = words(L"function");
    L.push_back(lua);
  }
  {
    Language rb;
    rb.name = L"Ruby";
    rb.extensions = list(L"rb rake gemspec ru");
    rb.filenames = list(L"rakefile gemfile");
    rb.shebangs = list(L"ruby");
    rb.lineComment = L"#";
    rb.atAnnotations = true;
    rb.capitalizedTypes = true;
    rb.keywords = words(L"alias and begin break case do else elsif end ensure for if in next not or redo rescue retry "
                        L"return super then undef unless until when while yield require require_relative include "
                        L"extend attr_accessor attr_reader attr_writer private protected public lambda proc");
    rb.storage = words(L"class def module");
    rb.constants = words(L"true false nil self");
    rb.builtins = words(L"puts print p raise");
    rb.declFunction = words(L"def");
    rb.declType = words(L"class module");
    L.push_back(rb);
  }
  {
    Language php;
    php.name = L"PHP";
    php.extensions = list(L"php phtml php3 php4 php5 phps");
    php.shebangs = list(L"php");
    php.lineComment = L"//";
    php.lineComment2 = L"#";
    php.blockStart = L"/*";
    php.blockEnd = L"*/";
    php.dollarVariables = true;
    php.capitalizedTypes = true;
    php.keywords = words(L"and array as break callable case catch clone continue declare default do echo else elseif "
                         L"empty enddeclare endfor endforeach endif endswitch endwhile extends finally fn for foreach "
                         L"global goto if implements include include_once instanceof insteadof isset list match new "
                         L"or print require require_once return switch throw try unset use while xor yield namespace");
    php.storage = words(L"abstract class const final function interface private protected public readonly static "
                        L"trait var");
    php.constants = words(L"true false null TRUE FALSE NULL");
    php.declFunction = words(L"function");
    php.declType = words(L"class interface trait");
    L.push_back(php);
  }
  {
    Language pl;
    pl.name = L"Perl";
    pl.extensions = list(L"pl pm t");
    pl.shebangs = list(L"perl");
    pl.lineComment = L"#";
    pl.dollarVariables = true;
    pl.keywords = words(L"if elsif else unless while until for foreach do last next redo return my our local use "
                        L"require package sub print die eq ne lt gt le ge and or not");
    pl.declFunction = words(L"sub");
    L.push_back(pl);
  }
  {
    Language sql;
    sql.name = L"SQL";
    sql.extensions = list(L"sql ddl dml");
    sql.lineComment = L"--";
    sql.blockStart = L"/*";
    sql.blockEnd = L"*/";
    sql.caseInsensitive = true;
    sql.keywords = words(
        L"select from where and or not insert into values update set delete create table drop alter add column index "
        L"view as join inner left right outer full cross on group by order having limit offset union all distinct "
        L"case when then else end is like in between exists primary key foreign references default unique check "
        L"constraint begin commit rollback transaction if returns return declare procedure function trigger "
        L"database schema use grant revoke with asc desc top over partition");
    sql.types = words(L"int integer bigint smallint tinyint decimal numeric float real double char varchar nvarchar "
                      L"text date time datetime timestamp boolean bool blob serial uuid json");
    sql.constants = words(L"true false null");
    sql.builtins = words(L"count sum avg min max coalesce cast convert now lower upper substring length round");
    L.push_back(sql);
  }
  {
    Language mk;
    mk.name = L"Makefile";
    mk.extensions = list(L"mk mak make");
    mk.filenames = list(L"makefile gnumakefile");
    mk.lineComment = L"#";
    mk.dollarVariables = true;
    mk.keywords = words(L"ifeq ifneq ifdef ifndef else endif include define endef export override");
    L.push_back(mk);
  }
  {
    Language cm;
    cm.name = L"CMake";
    cm.extensions = list(L"cmake");
    cm.filenames = list(L"cmakelists.txt");
    cm.lineComment = L"#";
    cm.caseInsensitive = true;
    cm.dollarVariables = true;
    cm.stringQuotes = L"\"";
    cm.keywords = words(L"if elseif else endif foreach endforeach while endwhile function endfunction macro endmacro "
                        L"return set unset option project add_executable add_library target_link_libraries "
                        L"target_include_directories target_compile_definitions target_compile_options "
                        L"target_sources find_package include message cmake_minimum_required add_subdirectory install "
                        L"add_test enable_testing list string file");
    cm.constants = words(L"on off true false yes no private public interface required");
    L.push_back(cm);
  }
  {
    Language dk;
    dk.name = L"Dockerfile";
    dk.extensions = list(L"dockerfile");
    dk.filenames = list(L"dockerfile containerfile");
    dk.lineComment = L"#";
    dk.caseInsensitive = true;
    dk.dollarVariables = true;
    dk.keywords = words(L"from run cmd label expose env add copy entrypoint volume user workdir arg onbuild "
                        L"stopsignal healthcheck shell as");
    L.push_back(dk);
  }
  {
    Language yml;
    yml.name = L"YAML";
    yml.extensions = list(L"yaml yml");
    yml.lineComment = L"#";
    yml.colonKeys = true;
    yml.identExtra = L"-";
    yml.constants = words(L"true false null yes no on off ~");
    L.push_back(yml);
  }
  {
    Language ini;
    ini.name = L"INI";
    ini.mode = LexMode::Ini;
    ini.extensions = list(L"ini cfg conf toml properties editorconfig gitconfig reg inf");
    ini.filenames = list(L".editorconfig .gitconfig .npmrc");
    ini.lineComment = L";";
    ini.lineComment2 = L"#";
    L.push_back(ini);
  }
  {
    Language diff;
    diff.name = L"Diff";
    diff.mode = LexMode::Diff;
    diff.extensions = list(L"diff patch");
    L.push_back(diff);
  }
  {
    Language gi;
    gi.name = L"Git Ignore";
    gi.mode = LexMode::Ini;
    gi.filenames = list(L".gitignore .gitattributes .dockerignore .npmignore");
    gi.lineComment = L"#";
    L.push_back(gi);
  }
  return L;
}

std::wstring lowerAscii(std::wstring_view s) {
  std::wstring out(s);
  for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
  return out;
}

}  // namespace

const std::vector<Language>& allLanguages() {
  static const std::vector<Language> langs = build();
  return langs;
}

const Language& plainTextLanguage() { return allLanguages().front(); }

const Language* languageByName(std::wstring_view name) {
  for (const auto& l : allLanguages())
    if (l.name == name) return &l;
  return nullptr;
}

const Language& detectLanguage(std::wstring_view path, std::wstring_view firstLine) {
  size_t slash = path.find_last_of(L"\\/");
  std::wstring file = lowerAscii(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
  for (const auto& l : allLanguages())
    for (const auto& f : l.filenames)
      if (f == file) return l;
  size_t dot = file.find_last_of(L'.');
  if (dot != std::wstring::npos) {
    std::wstring ext = file.substr(dot + 1);
    for (const auto& l : allLanguages())
      for (const auto& e : l.extensions)
        if (e == ext) return l;
  }
  if (startsWith(firstLine, L"#!")) {
    std::wstring line = lowerAscii(firstLine);
    for (const auto& l : allLanguages())
      for (const auto& s : l.shebangs) {
        size_t at = line.find(s);
        if (at == std::wstring::npos) continue;
        size_t end = at + s.size();
        bool boundary = (at == 0 || line[at - 1] == L'/' || line[at - 1] == L' ') &&
                        (end == line.size() || !std::iswalpha(line[end]));
        if (boundary) return l;
      }
  }
  if (startsWith(firstLine, L"<?xml")) return *languageByName(L"XML");
  if (startsWith(firstLine, L"<!DOCTYPE html") || startsWith(firstLine, L"<!doctype html"))
    return *languageByName(L"HTML");
  return plainTextLanguage();
}

}  // namespace verso
