#include "web_geometry/font_backend.h"
#include "firefox_font_prefs.h"
#include "web_geometry/font_config.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <cctype>
#include <cmath>
#include <mutex>
#ifdef _WIN32
#pragma comment(lib, "user32.lib")  // SystemParametersInfoW (system-ui menu font)
#endif

namespace ruyijs::web_geometry {
namespace {
std::wstring Wide(const std::string& text) {
#ifdef _WIN32
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring result(size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size);
  return result;
#else
  std::wstring result;
  const auto* p = reinterpret_cast<const unsigned char*>(text.data());
  const auto* end = p + text.size();
  while (p < end) {
    uint32_t cp;
    if (*p < 0x80) { cp = *p++; }
    else if ((*p & 0xE0) == 0xC0 && p + 1 < end) {
      cp = (*p & 0x1F) << 6 | (p[1] & 0x3F); p += 2;
    } else if ((*p & 0xF0) == 0xE0 && p + 2 < end) {
      cp = (*p & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F); p += 3;
    } else if ((*p & 0xF8) == 0xF0 && p + 3 < end) {
      cp = (*p & 0x07) << 18 | (p[1] & 0x3F) << 12 | (p[2] & 0x3F) << 6 | (p[3] & 0x3F); p += 4;
    } else { cp = *p++; }
    result.push_back(wchar_t(cp));
  }
  return result;
#endif
}
std::mutex g_language_lock;
// The language of untagged content: nsLanguageAtomService::GetLocaleLanguage,
// the OS regional-preferences locale (not navigator.language / Accept-Language);
// an explicit override when a profile emulates another OS locale.
thread_local std::string g_language;
std::string Group(const std::string& language) {
  auto value = language;
  if (value.empty()) {
    std::lock_guard<std::mutex> guard(g_language_lock);
    value = g_language;
  }
  if (value.empty()) {
#ifdef _WIN32
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH))
      for (wchar_t ch : locale) { if (!ch) break; value += char(ch); }
#else
    value = "en-US";
#endif
  }
  for (char& ch : value) if (ch >= 'A' && ch <= 'Z') ch += 'a'-'A';
  if (value.starts_with("zh"))
    return value.find("tw") != value.npos || value.find("hant") != value.npos ? "zh-TW"
        : value.find("hk") != value.npos ? "zh-HK" : "zh-CN";
  for (const char* code : {"ja", "ko", "ar", "he", "th", "el"})
    if (value.starts_with(code)) return code;
  if (value.starts_with("ru") || value.starts_with("uk") || value.starts_with("bg")) return "x-cyrillic";
  return "x-western";
}
std::string Pref(const std::string& name) {
  for (const auto& entry : kFontPreferences) if (name == entry.key) return entry.value;
  return {};
}
// Splits a family list; `quoted` (when given) records which names were
// strings, since only an unquoted identifier can be a generic family.
void Split(const std::string& value, std::vector<std::wstring>* out,
           std::vector<bool>* quoted = nullptr) {
  size_t start = 0; char quote = 0;
  for (size_t i = 0; i <= value.size(); ++i) {
    char ch = i < value.size() ? value[i] : ',';
    if (quote) { if (ch == quote) quote = 0; }
    else if (ch == '\'' || ch == '"') quote = ch;
    else if (ch == ',') {
      auto item = value.substr(start, i-start); start = i+1;
      const auto begin = item.find_first_not_of(" \t\r\n\"'");
      const auto end = item.find_last_not_of(" \t\r\n\"'");
      if (begin == item.npos) continue;
      out->push_back(Wide(item.substr(begin, end-begin+1)));
      if (quoted) quoted->push_back(item.find_first_of("\"'") != item.npos);
    }
  }
}
// LookAndFeel's StyleSystemFont::Menu on Windows: the non-client menu font.
std::wstring MenuFont() {
#ifdef _WIN32
  NONCLIENTMETRICSW metrics{}; metrics.cbSize = sizeof(metrics);
  if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) return {};
  return metrics.lfMenuFont.lfFaceName;
#else
  return {};
#endif
}
}
namespace {
thread_local std::atomic<double> g_device_pixel_ratio{1.0};
}
void SetDevicePixelRatio(double ratio) {
  if (std::isfinite(ratio) && ratio > 0) g_device_pixel_ratio = ratio;
}
bool TextRunOptimizeSpeed(const std::string& value, double cssFontSize) {
  std::string textRendering = value;
  for (auto& ch : textRendering) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  if (textRendering == "optimizespeed") return true;
  if (!textRendering.empty() && textRendering != "auto") return false;
  // nsPresContext::DevPixelsToFloatCSSPixels(auto_quality_min_font_size = 20).
  return cssFontSize < float(20.0 / g_device_pixel_ratio.load());
}
void SetBrowserLanguage(const std::string& language) {
  std::lock_guard<std::mutex> guard(g_language_lock);
  g_language = language;
}
std::string BrowserLanguage() {
  std::lock_guard<std::mutex> guard(g_language_lock);
  return g_language;
}
std::string DefaultGenericFamily() {
  const auto generic = Pref("font.default." + Group({}));
  return generic.empty() ? "serif" : generic;
}
std::vector<std::wstring> FontFamilies(const FontInput& input) {
  std::vector<std::wstring> declared, result;
  std::vector<bool> quoted;
  Split(input.family, &declared, &quoted);
  const auto group = Group(input.language);
  for (size_t i = 0; i < declared.size(); ++i) {
    auto name = declared[i];
    if (!quoted[i] && name == L"-moz-default") {
      const auto generic = DefaultGenericFamily();
      name.assign(generic.begin(), generic.end());
    }
    if (!quoted[i] && (name == L"serif" || name == L"sans-serif" || name == L"monospace" ||
        name == L"cursive" || name == L"fantasy" || name == L"system-ui")) {
      const std::string generic(name.begin(), name.end());
      if (const auto forced = GenericFamilyOverride(generic); !forced.empty()) {
        result.push_back(Wide(forced));
      } else {
        Split(Pref("font.name."+generic+"."+group), &result);
        Split(Pref("font.name-list."+generic+"."+group), &result);
      }
      if (name == L"system-ui")
        if (auto menu = MenuFont(); !menu.empty()) result.push_back(menu);
    } else result.push_back(name);
  }
  // gfxFontGroup::BuildFontList: without a generic in the list, the content
  // language's default generic is appended (system-ui does not count).
  bool generic = false;
  for (size_t i = 0; i < declared.size(); ++i) {
    const auto& name = declared[i];
    if (!quoted[i] && (name == L"serif" || name == L"sans-serif" || name == L"monospace" ||
        name == L"cursive" || name == L"fantasy" || name == L"-moz-default")) generic = true;
  }
  if (!generic) {
    auto fallback = Pref("font.default."+group);
    if (fallback.empty()) fallback = "serif";
    Split(Pref("font.name."+fallback+"."+group), &result);
    Split(Pref("font.name-list."+fallback+"."+group), &result);
  }
  return result;
}

// gfxPlatformFontList::GetFontPrefLangFor(char): the language group whose
// preferences a character falls back to ("" for Others, "cjk" for the CJK set,
// "emoji" handled by the caller).
std::string CharLangGroup(uint32_t ch) {
  struct Range { uint32_t first, last; const char* group; };
  static const Range kRanges[] = {
    {0x0000, 0x024F, "x-western"}, {0x0250, 0x02FF, "x-western"}, {0x1D00, 0x1DBF, "x-western"},
    {0x1E00, 0x1EFF, "x-western"}, {0x2C60, 0x2C7F, "x-western"}, {0xA720, 0xA7FF, "x-western"},
    {0xAB30, 0xAB6F, "x-western"},
    {0x0370, 0x03FF, "el"}, {0x1F00, 0x1FFF, "el"},
    {0x0400, 0x052F, "x-cyrillic"}, {0x2DE0, 0x2DFF, "x-cyrillic"}, {0xA640, 0xA69F, "x-cyrillic"},
    {0x1C80, 0x1C8F, "x-cyrillic"},
    {0x0530, 0x058F, "x-armn"}, {0x0590, 0x05FF, "he"},
    {0x0600, 0x06FF, "ar"}, {0x0750, 0x077F, "ar"}, {0x08A0, 0x08FF, "ar"}, {0xFB50, 0xFDFF, "ar"},
    {0xFE70, 0xFEFF, "ar"}, {0x1EE00, 0x1EEFF, "ar"},
    {0x0900, 0x097F, "x-devanagari"}, {0xA8E0, 0xA8FF, "x-devanagari"},
    {0x0980, 0x09FF, "x-beng"}, {0x0A00, 0x0A7F, "x-guru"}, {0x0A80, 0x0AFF, "x-gujr"},
    {0x0B00, 0x0B7F, "x-orya"}, {0x0B80, 0x0BFF, "x-tamil"}, {0x0C00, 0x0C7F, "x-telu"},
    {0x0C80, 0x0CFF, "x-knda"}, {0x0D00, 0x0D7F, "x-mlym"}, {0x0D80, 0x0DFF, "x-sinh"},
    {0x111E0, 0x111FF, "x-sinh"}, {0x0E00, 0x0E7F, "th"}, {0x0F00, 0x0FFF, "x-tibt"},
    {0x10A0, 0x10FF, "x-geor"}, {0x2D00, 0x2D2F, "x-geor"}, {0x1C90, 0x1CBF, "x-geor"},
    {0x1100, 0x11FF, "ko"}, {0x3130, 0x318F, "ko"}, {0xAC00, 0xD7AF, "ko"}, {0xA960, 0xA97F, "ko"},
    {0xD7B0, 0xD7FF, "ko"},
    {0x1200, 0x139F, "x-ethi"}, {0x2D80, 0x2DDF, "x-ethi"}, {0xAB00, 0xAB2F, "x-ethi"},
    {0x1400, 0x167F, "x-cans"}, {0x18B0, 0x18FF, "x-cans"}, {0x1780, 0x17FF, "x-khmr"},
    {0x19E0, 0x19FF, "x-khmr"},
    {0x2E80, 0x2FDF, "cjk"}, {0x2FF0, 0x30FF, "cjk"}, {0x3100, 0x312F, "cjk"}, {0x3190, 0x31BF, "cjk"},
    {0x31C0, 0x31FF, "cjk"}, {0x3200, 0x4DBF, "cjk"}, {0x4E00, 0x9FFF, "cjk"}, {0xF900, 0xFAFF, "cjk"},
    {0xFE10, 0xFE1F, "cjk"}, {0xFE30, 0xFE6F, "cjk"}, {0xFF00, 0xFFEF, "cjk"}, {0x16FE0, 0x16FFF, "cjk"},
    {0x1AFF0, 0x1B16F, "cjk"}, {0x20000, 0x3134F, "cjk"},
    {0x2200, 0x22FF, "x-math"}, {0x27C0, 0x27EF, "x-math"}, {0x2980, 0x2AFF, "x-math"},
    {0x1D400, 0x1D7FF, "x-math"},
  };
  for (const auto& range : kRanges)
    if (ch >= range.first && ch <= range.last) return range.group;
  return {};
}

// gfxFontGroup::WhichPrefFontSupportsChar: the families to try, in order. A
// CJK character tries the content language (if CJK) then zh-CN, zh-HK, zh-TW,
// ja, ko (accept-languages and the app locale lead in Firefox; zh-CN here);
// every list ends with x-unicode. Each language uses the page's generic when
// the font list named one, else its own default generic.
std::vector<std::wstring> PrefFallbackFamilies(uint32_t ch, const FontInput& input) {
  std::vector<std::string> langs;
  const auto charGroup = CharLangGroup(ch);
  auto add = [&](const std::string& lang) {
    if (!lang.empty() && std::find(langs.begin(), langs.end(), lang) == langs.end()) langs.push_back(lang);
  };
  if (charGroup == "cjk" || charGroup == "ko") {
    const auto page = Group(input.language);
    if (page == "zh-CN" || page == "zh-TW" || page == "zh-HK" || page == "ja" || page == "ko") add(page);
    if (charGroup == "ko") add("ko");
    for (const char* lang : {"zh-CN", "zh-HK", "zh-TW", "ja", "ko"}) add(lang);
  } else {
    add(charGroup);
  }
  add("x-unicode");
  // mFallbackGeneric: the first generic named in the list (not system-ui).
  std::string pageGeneric;
  {
    std::vector<std::wstring> declared;
    std::vector<bool> quoted;
    Split(input.family, &declared, &quoted);
    for (size_t i = 0; i < declared.size() && pageGeneric.empty(); ++i) {
      const auto& name = declared[i];
      if (quoted[i]) continue;
      if (name == L"serif" || name == L"sans-serif" || name == L"monospace" || name == L"cursive" ||
          name == L"fantasy") pageGeneric.assign(name.begin(), name.end());
    }
  }
  std::vector<std::wstring> result;
  for (const auto& lang : langs) {
    auto generic = pageGeneric;
    if (generic.empty()) {
      generic = Pref("font.default." + lang);
      if (generic.empty()) generic = "serif";
    }
    Split(Pref("font.name." + generic + "." + lang), &result);
    Split(Pref("font.name-list." + generic + "." + lang), &result);
  }
  return result;
}
std::vector<std::wstring> CommonFallbackFamilies(uint32_t ch) {
  std::vector<std::wstring> list;
  const hb_script_t script = hb_unicode_script(hb_unicode_funcs_get_default(), ch);
  auto add = [&](std::initializer_list<const wchar_t*> names) { for (auto* n : names) list.push_back(n); };
  switch (script) {
    case HB_SCRIPT_HAN: case HB_SCRIPT_BOPOMOFO:
      add({L"SimSun"}); if (ch > 0xFFFF) add({L"SimSun-ExtB"}); break;
    case HB_SCRIPT_HIRAGANA: case HB_SCRIPT_KATAKANA: add({L"Yu Gothic", L"MS PGothic"}); break;
    case HB_SCRIPT_HANGUL: add({L"Malgun Gothic"}); break;
    case HB_SCRIPT_YI: add({L"Microsoft Yi Baiti"}); break;
    case HB_SCRIPT_MONGOLIAN: add({L"Mongolian Baiti"}); break;
    case HB_SCRIPT_TIBETAN: add({L"Microsoft Himalaya"}); break;
    case HB_SCRIPT_PHAGS_PA: add({L"Microsoft PhagsPa"}); break;
    case HB_SCRIPT_SYRIAC: add({L"Estrangelo Edessa"}); break;
    case HB_SCRIPT_THAANA: add({L"MV Boli"}); break;
    case HB_SCRIPT_BENGALI: add({L"Vrinda", L"Nirmala UI"}); break;
    case HB_SCRIPT_DEVANAGARI: add({L"Kokila", L"Nirmala UI"}); break;
    case HB_SCRIPT_GUJARATI: add({L"Shruti", L"Nirmala UI"}); break;
    case HB_SCRIPT_GURMUKHI: add({L"Raavi", L"Nirmala UI"}); break;
    case HB_SCRIPT_KANNADA: add({L"Tunga", L"Nirmala UI"}); break;
    case HB_SCRIPT_MALAYALAM: add({L"Kartika", L"Nirmala UI"}); break;
    case HB_SCRIPT_ORIYA: add({L"Kalinga", L"Nirmala UI"}); break;
    case HB_SCRIPT_TAMIL: add({L"Latha", L"Nirmala UI"}); break;
    case HB_SCRIPT_TELUGU: add({L"Gautami", L"Nirmala UI"}); break;
    case HB_SCRIPT_SINHALA: add({L"Iskoola Pota", L"Nirmala UI"}); break;
    case HB_SCRIPT_CHAKMA: case HB_SCRIPT_MEETEI_MAYEK: case HB_SCRIPT_OL_CHIKI:
    case HB_SCRIPT_SORA_SOMPENG: add({L"Nirmala UI"}); break;
    case HB_SCRIPT_MYANMAR: add({L"Myanmar Text"}); break;
    case HB_SCRIPT_KHMER: add({L"Khmer UI"}); break;
    case HB_SCRIPT_LAO: add({L"Lao UI"}); break;
    case HB_SCRIPT_THAI: add({L"Tahoma", L"Leelawadee UI"}); break;
    case HB_SCRIPT_TAI_LE: add({L"Microsoft Tai Le"}); break;
    case HB_SCRIPT_BUGINESE: add({L"Leelawadee UI"}); break;
    case HB_SCRIPT_NEW_TAI_LUE: add({L"Microsoft New Tai Lue"}); break;
    case HB_SCRIPT_JAVANESE: add({L"Javanese Text"}); break;
    case HB_SCRIPT_GEORGIAN: case HB_SCRIPT_LISU: add({L"Segoe UI"}); break;
    case HB_SCRIPT_ETHIOPIC: add({L"Nyala", L"Ebrima"}); break;
    case HB_SCRIPT_ADLAM: case HB_SCRIPT_NKO: case HB_SCRIPT_OSMANYA: case HB_SCRIPT_TIFINAGH:
    case HB_SCRIPT_VAI: add({L"Ebrima"}); break;
    case HB_SCRIPT_CANADIAN_SYLLABICS: add({L"Euphemia"}); break;
    case HB_SCRIPT_CHEROKEE: case HB_SCRIPT_OSAGE: add({L"Gadugi"}); break;
    case HB_SCRIPT_BRAILLE: case HB_SCRIPT_DESERET: add({L"Segoe UI Symbol"}); break;
    default: break;
  }
  // Arial is the default system fallback; symbols and punctuation also try
  // the Segoe UI families and Cambria Math.
  add({L"Arial"});
  const uint32_t block = ch >> 8;
  const auto category = hb_unicode_general_category(hb_unicode_funcs_get_default(), ch);
  const bool symbol = category >= HB_UNICODE_GENERAL_CATEGORY_CURRENCY_SYMBOL &&
                      category <= HB_UNICODE_GENERAL_CATEGORY_OTHER_SYMBOL;
  const bool punctuation = category >= HB_UNICODE_GENERAL_CATEGORY_CONNECT_PUNCTUATION &&
                           category <= HB_UNICODE_GENERAL_CATEGORY_OPEN_PUNCTUATION;
  if (script == HB_SCRIPT_COMMON || (block >= 0x20 && block <= 0x2b) || block == 0x2e ||
      symbol || punctuation)
    add({L"Segoe UI", L"Segoe UI Symbol", L"Cambria Math"});
  add({L"Arial Unicode MS", L"Segoe UI Emoji", L"Twemoji Mozilla"});
  return list;
}
std::vector<std::wstring> EmojiFallbackFamilies() {
  std::vector<std::wstring> result;
  Split(Pref("font.name-list.emoji"), &result);
  return result;
}
}
