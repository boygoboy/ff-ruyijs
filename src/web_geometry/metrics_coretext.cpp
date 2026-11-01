// SystemFonts metrics on macOS. Text is shaped by CoreText at the requested
// device scale, then converted to CSS pixels at the existing domain boundary.
#ifdef __APPLE__
#include "web_geometry/font_backend.h"
#include "web_geometry/mac_font_internal.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <string>

namespace ruyijs::web_geometry {
SystemFonts::SystemFonts() : collection_(std::make_unique<FontCollection>()) {}
SystemFonts::~SystemFonts() = default;
namespace {
std::u16string Normalize(const std::u16string& source) {
  std::u16string text;
  bool space = true;
  for (char16_t ch : source) {
    const bool current = ch == u' ' || ch == u'\n' || ch == u'\t' || ch == u'\r';
    if (!current || !space) text += current ? u' ' : ch;
    space = current;
  }
  return text;
}
double Width(CTLineRef line, double scale) {
  return std::round(CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr) * 60) / 60 / scale;
}
}  // namespace

bool SystemFonts::MeasureCanvas(const std::u16string& text, const FontInput& input,
                                CanvasTextMetrics* result) {
  if (!result) return false;
  *result = {};
  if (!MacFontInputValid(input)) return false;
  auto font = MacPrimaryFont(*collection_, input);
  auto line = MacTextLine(*collection_, text, input);
  if (!font || !line) return false;
  const double scale = input.scale;
  result->width = float(Width(line.get(), scale));
  result->font_ascent = std::round(CTFontGetAscent(font.get())) / scale;
  result->font_descent = std::round(CTFontGetDescent(font.get())) / scale;
  const double height = result->font_ascent + result->font_descent;
  if (height > 0) {
    result->em_ascent = MacFontSize(input) / scale * result->font_ascent / height;
    result->em_descent = MacFontSize(input) / scale - result->em_ascent;
  }
  // OpenType fallback baselines when there is no BASE-table override. These
  // match HarfBuzz's Roman/ideographic/hanging fallback formulas.
  result->hanging_baseline = result->font_ascent * .8;
  result->ideographic_baseline = -result->font_descent;
  const CGRect ink = CTLineGetBoundsWithOptions(line.get(), kCTLineBoundsUseGlyphPathBounds);
  result->ink = !CGRectIsNull(ink) && !CGRectIsEmpty(ink);
  if (result->ink) {
    result->ink_left = CGRectGetMinX(ink) / scale;
    result->ink_right = CGRectGetMaxX(ink) / scale;
    result->ink_top = -CGRectGetMaxY(ink) / scale;
    result->ink_bottom = -CGRectGetMinY(ink) / scale;
  }
  return true;
}

bool SystemFonts::Measure(const std::u16string& source, const FontInput& input,
                          ruyijs::domain::geometry::SvgTextMeasurement* result) {
  if (!result) return false;
  *result = {};
  if (source.size() > 65536 || !std::isfinite(input.scale) || input.scale <= 0) return false;
  const auto text = Normalize(source);
  if (text.empty() || input.size == 0) return true;
  auto line = MacTextLine(*collection_, text, input);
  if (!line) return false;
  CGFloat ascent = 0, descent = 0;
  const double width = CTLineGetTypographicBounds(line.get(), &ascent, &descent, nullptr) / input.scale;
  ascent /= input.scale; descent /= input.scale;
  const size_t visible = text.size() - (text.back() == u' ' ? 1 : 0);
  // CSS letter-spacing in CSS pixels, app-unit rounded like the Windows
  // pipeline (ShapeFont's spacing = round(letterSpacing * scale * 60)). Each
  // code point is its own cluster because MacTextLine shapes spaced text
  // without ligatures; spacing follows every cluster (nsTextFrame
  // PropertyProvider::GetSpacing), including the trailing one.
  const double spacing = std::isfinite(input.letterSpacing) && input.letterSpacing != 0
      ? std::floor(input.letterSpacing * input.scale * 60 + .5) / 60 / input.scale : 0;
  // The cumulative cluster count before each UTF-16 unit, when spacing is on.
  std::vector<unsigned> clusters;
  if (spacing != 0) {
    clusters.resize(text.size() + 1);
    unsigned count = 0;
    for (size_t i = 0; i < text.size(); ++i) {
      clusters[i] = count;
      if (!(text[i] >= 0xdc00 && text[i] <= 0xdfff && i && text[i - 1] >= 0xd800 && text[i - 1] <= 0xdbff)) ++count;
    }
    clusters[text.size()] = count;
  }
  const double trailing = CTLineGetTrailingWhitespaceWidth(line.get()) / input.scale;
  // The measured length keeps every cluster's spacing but a trailing space's
  // own (the Windows pipeline subtracts the last run advance).
  const double totalSpacing = spacing == 0 ? 0
      : spacing * (clusters[text.size()] - (text.back() == u' ' ? 1u : 0u));
  result->length = float(std::round((width - trailing + totalSpacing) * 60) / 60);
  const std::array<double, 4> box = {input.x, input.y - ascent,
                                     width + (spacing == 0 ? 0 : spacing * clusters[text.size()]),
                                     ascent + descent};
  result->box = {float(box[0]), float(box[1]), float(box[2]), float(box[3])};
  result->transformed = input.transform.ApplyUnrounded(box);
  result->characters.resize(visible);
  for (size_t i = 0; i < visible; ++i) {
    if (text[i] >= 0xdc00 && text[i] <= 0xdfff && i && text[i - 1] >= 0xd800 && text[i - 1] <= 0xdbff) continue;
    const size_t units = text[i] >= 0xd800 && text[i] <= 0xdbff && i + 1 < text.size() &&
                          text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff ? 2 : 1;
    const double shift = spacing == 0 ? 0 : spacing * clusters[i];
    const double a = CTLineGetOffsetForStringIndex(line.get(), CFIndex(i), nullptr) / input.scale + shift;
    const double b = CTLineGetOffsetForStringIndex(line.get(), CFIndex(i + units), nullptr) / input.scale
        + shift + spacing;
    result->characters[i] = {float(input.x + std::min(a, b)), float(input.y - ascent),
                             float(std::abs(b - a)), float(ascent + descent)};
  }
  return true;
}

bool SystemFonts::LineMetrics(const FontInput& input, double* content, double* normal) {
  if (!content || !normal) return false;
  *content = *normal = 0;
  if (!std::isfinite(input.size) || input.size <= 0) return true;
  FontInput unscaled = input;
  unscaled.scale = 1;
  auto font = MacPrimaryFont(*collection_, unscaled);
  if (!font) return false;
  *content = std::round(CTFontGetAscent(font.get())) + std::round(CTFontGetDescent(font.get()));
  const double leading = std::ceil(CTFontGetLeading(font.get()));
  *normal = std::max(*content + leading, std::round(input.size * 60 * 1.2) / 60);
  return true;
}

bool SystemFonts::SvgLengthMetrics(const FontInput& input, float* ex, float* ch, float* ic, float* cap) {
  if (!ex || !ch || !ic || !cap) return false;
  *ex = *ch = *ic = *cap = 0;
  if (input.size <= 0) return true;
  FontInput unscaled = input;
  unscaled.scale = 1;
  auto font = MacPrimaryFont(*collection_, unscaled);
  if (!font) return false;
  *ex = float(std::round(CTFontGetXHeight(font.get()) * 60) / 60);
  *cap = float(std::round(CTFontGetCapHeight(font.get()) * 60) / 60);
  const UniChar codes[] = {0x30, 0x6c34};
  for (unsigned i = 0; i < 2; ++i) {
    CGGlyph glyph = 0;
    CGSize advance{};
    double width = i ? input.size : input.size * .5;
    if (CTFontGetGlyphsForCharacters(font.get(), codes + i, &glyph, 1) && glyph) {
      CTFontGetAdvancesForGlyphs(font.get(), kCTFontOrientationHorizontal, &glyph, &advance, 1);
      width = advance.width;
    }
    *(i ? ic : ch) = float(std::round(width * 60) / 60);
  }
  if (*ex <= 0) *ex = float(input.size * .5);
  if (*cap <= 0) *cap = float(input.size);
  return true;
}
}  // namespace ruyijs::web_geometry
#endif
