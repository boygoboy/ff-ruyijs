// macOS font selection and shaping. CoreText supplies native shaping, fallback
// and colour-font support; configured family visibility is enforced on runs.
#ifdef __APPLE__
#include "web_geometry/mac_font_internal.h"
#include "web_geometry/font_config.h"
#include <mutex>

namespace ruyijs::web_geometry {
namespace {
std::string Language(const FontInput& input) {
  // The profile locale set for untagged content; empty lets CoreText use the
  // system resolution, as before.
  if (!input.language.empty()) return input.language;
  return BrowserLanguage();
}
bool Visible(CTFontRef font) {
  MacRef<CFStringRef> name(CTFontCopyFamilyName(font));
  return name && FontFamilyVisible(MacWide(name.get()));
}
std::vector<std::string> Families(const std::string& css) {
  std::vector<std::string> result;
  size_t start = 0;
  char quote = 0;
  for (size_t i = 0; i <= css.size(); ++i) {
    const char c = i < css.size() ? css[i] : ',';
    if (quote) { if (c == quote) quote = 0; }
    else if (c == '\'' || c == '"') quote = c;
    else if (c == ',') {
      const std::string item = css.substr(start, i - start);
      start = i + 1;
      const auto a = item.find_first_not_of(" \t\r\n\"'");
      const auto b = item.find_last_not_of(" \t\r\n\"'");
      if (a != item.npos) result.push_back(item.substr(a, b - a + 1));
    }
  }
  if (result.empty()) result.push_back("serif");
  return result;
}
CFArrayRef MacFamilies() {
  static CFArrayRef families = CTFontManagerCopyAvailableFontFamilyNames();
  return families;
}
MacRef<CTFontRef> FamilyFont(const FontCollection& collection, std::string family,
                            const FontInput& input) {
  if (family == "-moz-default") family = DefaultGenericFamily();
  const auto forced = GenericFamilyOverride(family);
  if (!forced.empty()) family = forced;
  else if (family == "serif") family = "Times";
  else if (family == "sans-serif") family = "Helvetica";
  else if (family == "monospace") family = "Menlo";
  else if (family == "cursive") family = "Apple Chancery";
  else if (family == "fantasy") family = "Papyrus";
  MacRef<CTFontRef> font;
  if (family == "system-ui") {
    const auto lang = MacString(Language(input));
    font = MacRef<CTFontRef>(CTFontCreateUIFontForLanguage(kCTFontUIFontSystem,
                                                         MacFontSize(input), lang.get()));
  } else {
    const auto name = MacString(family);
    bool installed = false;
    if (!name || !MacFamilies()) return font;
    for (CFIndex i = 0; i < CFArrayGetCount(MacFamilies()); ++i) {
      auto existing = static_cast<CFStringRef>(CFArrayGetValueAtIndex(MacFamilies(), i));
      if (CFStringCompare(existing, name.get(), kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
        installed = true; break;
      }
    }
    if (!installed) return font;
    font = MacRef<CTFontRef>(CTFontCreateWithName(name.get(), MacFontSize(input), nullptr));
  }
  if (!font || !Visible(font.get())) return MacRef<CTFontRef>();
  const CTFontSymbolicTraits traits = (input.italic ? kCTFontItalicTrait : 0) |
                                     (input.weight >= 600 ? kCTFontBoldTrait : 0);
  if (traits) {
    MacRef<CTFontRef> styled(CTFontCreateCopyWithSymbolicTraits(font.get(), 0, nullptr,
                                                             traits, traits));
    if (styled) font = std::move(styled);
  }
  return font;
}
}  // namespace

std::wstring MacWide(CFStringRef string) {
  if (!string) return {};
  const CFIndex units = CFStringGetLength(string);
  std::vector<UniChar> utf16(size_t(units), 0);
  CFStringGetCharacters(string, CFRangeMake(0, units), utf16.data());
  std::wstring out;
  for (size_t i = 0; i < utf16.size(); ++i) {
    uint32_t code = utf16[i];
    if (code >= 0xd800 && code <= 0xdbff && i + 1 < utf16.size() &&
        utf16[i + 1] >= 0xdc00 && utf16[i + 1] <= 0xdfff)
      code = 0x10000 + ((code - 0xd800) << 10) + utf16[++i] - 0xdc00;
    out.push_back(wchar_t(code));
  }
  return out;
}
MacRef<CFStringRef> MacString(const std::string& text) {
  return MacRef<CFStringRef>(CFStringCreateWithBytes(nullptr,
      reinterpret_cast<const UInt8*>(text.data()), CFIndex(text.size()), kCFStringEncodingUTF8, false));
}
MacRef<CTFontRef> MacPrimaryFont(const FontCollection& collection, const FontInput& input) {
  if (!MacFontInputValid(input)) return MacRef<CTFontRef>();
  for (const auto& family : Families(input.family)) {
    auto font = FamilyFont(collection, family, input);
    if (font) return font;
  }
  for (const char* family : {"serif", "sans-serif", "system-ui"}) {
    auto font = FamilyFont(collection, family, input);
    if (font) return font;
  }
  return MacRef<CTFontRef>();
}
MacRef<CTLineRef> MacTextLine(const FontCollection& collection, const std::u16string& text,
                            const FontInput& input) {
  if (text.size() > 65536) return MacRef<CTLineRef>();
  auto font = MacPrimaryFont(collection, input);
  if (!font) return MacRef<CTLineRef>();
  MacRef<CFMutableArrayRef> cascade(CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks));
  if (!cascade) return MacRef<CTLineRef>();
  for (const auto& family : Families(input.family)) {
    auto candidate = FamilyFont(collection, family, input);
    if (!candidate) continue;
    MacRef<CTFontDescriptorRef> descriptor(CTFontCopyFontDescriptor(candidate.get()));
    if (descriptor) CFArrayAppendValue(cascade.get(), descriptor.get());
  }
  if (CFArrayGetCount(cascade.get())) {
    const void* key = kCTFontCascadeListAttribute;
    const void* value = cascade.get();
    MacRef<CFDictionaryRef> attributes(CFDictionaryCreate(nullptr, &key, &value, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    MacRef<CTFontDescriptorRef> descriptor(CTFontDescriptorCreateWithAttributes(attributes.get()));
    MacRef<CTFontRef> specified(CTFontCreateCopyWithAttributes(font.get(), 0, nullptr, descriptor.get()));
    if (specified) font = std::move(specified);
  }
  MacRef<CFStringRef> string(CFStringCreateWithCharacters(nullptr,
      reinterpret_cast<const UniChar*>(text.data()), CFIndex(text.size())));
  MacRef<CFMutableAttributedStringRef> attributed(CFAttributedStringCreateMutable(nullptr, 0));
  if (!string || !attributed) return MacRef<CTLineRef>();
  CFAttributedStringReplaceString(attributed.get(), CFRangeMake(0, 0), string.get());
  const CFRange all = CFRangeMake(0, CFIndex(text.size()));
  CFAttributedStringSetAttribute(attributed.get(), all, kCTFontAttributeName, font.get());
  const auto language = MacString(Language(input));
  if (language) CFAttributedStringSetAttribute(attributed.get(), all, kCTLanguageAttributeName, language.get());
  if (std::isfinite(input.letterSpacing) && input.letterSpacing != 0) {
    // Letter-spaced text shapes without common ligatures (nsTextFrame's
    // spacing run), so every code point stays its own cluster for the
    // per-character geometry SystemFonts::Measure reports.
    const CFIndex none = 0;
    MacRef<CFNumberRef> ligature(CFNumberCreate(nullptr, kCFNumberCFIndexType, &none));
    if (ligature) CFAttributedStringSetAttribute(attributed.get(), all, kCTLigatureAttributeName, ligature.get());
  }
  MacRef<CTLineRef> line(CTLineCreateWithAttributedString(attributed.get()));
  if (!line) return line;
  const auto runs = CTLineGetGlyphRuns(line.get());
  for (CFIndex i = 0; i < CFArrayGetCount(runs); ++i) {
    auto run = static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, i));
    auto actual = static_cast<CTFontRef>(CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName));
    if (!actual || !Visible(actual)) return MacRef<CTLineRef>();
  }
  return line;
}
}  // namespace ruyijs::web_geometry
#endif
