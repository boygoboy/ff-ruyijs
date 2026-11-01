#ifdef _WIN32
#include "web_geometry/font_backend.h"
#include <dwrite_3.h>
#include <algorithm>
#include <cmath>

namespace ruyijs::web_geometry {
SystemFonts::SystemFonts() = default;
SystemFonts::~SystemFonts() = default;
namespace {
std::u16string Normalize(const std::u16string& text) {
  std::u16string result;
  bool space = true;
  for (char16_t ch : text) {
    const bool current = ch == u' ' || ch == u'\n' || ch == u'\t' || ch == u'\r';
    if (!current || !space) result += current ? u' ' : ch;
    space = current;
  }
  return result;
}
uint32_t Codepoint(const std::u16string& text, size_t* at) {
  uint32_t code = text[(*at)++];
  if (code >= 0xd800 && code <= 0xdbff && *at < text.size() &&
      text[*at] >= 0xdc00 && text[*at] <= 0xdfff)
    code = 0x10000 + ((code-0xd800)<<10) + text[(*at)++]-0xdc00;
  return code;
}
bool Continuation(uint32_t code) {
  return code == 0x200d || (code >= 0xfe00 && code <= 0xfe0f) ||
      (code >= 0xe0100 && code <= 0xe01ef) ||
      hb_unicode_combining_class(hb_unicode_funcs_get_default(), code) != 0;
}
}
namespace {
// font.blacklist.underline_offset (modules/libpref/init/all.js).
bool BadUnderlineFamily(IDWriteFontFace* face) {
  if (!face) return false;
  static const wchar_t* kFamilies[] = {
      L"FangSong", L"Gulim", L"GulimChe", L"MingLiU", L"MingLiU-ExtB", L"MingLiU_HKSCS",
      L"MingLiU-HKSCS-ExtB", L"MS Gothic", L"MS Mincho", L"MS PGothic", L"MS PMincho",
      L"MS UI Gothic", L"PMingLiU", L"PMingLiU-ExtB", L"SimHei", L"SimSun", L"SimSun-ExtB",
      L"Hei", L"Kai", L"Apple LiGothic", L"Apple LiSung", L"Osaka"};
  ComPtr<IDWriteFontFace3> face3;
  if (FAILED(face->QueryInterface(IID_PPV_ARGS(&face3)))) return false;
  ComPtr<IDWriteLocalizedStrings> names;
  if (FAILED(face3->GetFamilyNames(&names))) return false;
  for (UINT32 i = 0; i < names->GetCount(); ++i) {
    UINT32 length = 0;
    if (FAILED(names->GetStringLength(i, &length))) continue;
    std::wstring name(length + 1, L'\0');
    if (FAILED(names->GetString(i, name.data(), length + 1))) continue;
    name.resize(length);
    for (const wchar_t* bad : kFamilies)
      if (_wcsicmp(name.c_str(), bad) == 0) return true;
  }
  return false;
}
}  // namespace

double FrameMaxDescent(const DWRITE_FONT_METRICS& metrics, double conv, double maxAscent,
                       double maxDescent, IDWriteFontFace* face, double size) {
  double underlineSize = std::max(1.0, metrics.underlineThickness * conv);
  double underlineOffset = std::min(metrics.underlinePosition * conv, -1.0);
  if (maxAscent < 1.0) {
    underlineSize = 0;
    underlineOffset = 0;
  } else if (size > 0 && BadUnderlineFamily(face)) {
    // gfxFont::SanitizeMetrics for a bad-underline CJK family (not a system font).
    const double maxHeight = maxAscent + maxDescent;
    const double emAscent = maxHeight > 0 ? size * maxAscent / maxHeight : 0;
    const double emDescent = size - emAscent;
    const double internalLeading = std::max(maxHeight - size, 0.0);
    const double externalLeading = std::ceil(metrics.lineGap * conv);
    underlineOffset = std::min(underlineOffset, -2.0);
    if (internalLeading + externalLeading > underlineSize)
      underlineOffset = std::min(underlineOffset, -emDescent);
    else
      underlineOffset = std::min(underlineOffset, underlineSize - emDescent);
  } else if (underlineSize - underlineOffset > maxDescent) {
    if (underlineSize > maxDescent) underlineSize = std::max(maxDescent, 1.0);
    underlineOffset = underlineSize - maxDescent;
  }
  const double offset = std::floor(-underlineOffset + 0.5);
  const double minDescent = offset + std::floor(underlineSize + 0.5);
  return std::floor(std::max(minDescent, maxDescent) + 0.5);
}
bool SystemFonts::LineMetrics(const FontInput& input, double* content, double* normal) {
  *content = *normal = 0;
  if (!std::isfinite(input.size) || input.size <= 0) return true;
  if (!collection_) {
    auto collection = std::make_unique<FontCollection>();
    if (!collection->Initialize()) return false;
    collection_ = std::move(collection);
  }
  SelectedFont font;
  if (!collection_->Select(input, 0x20, &font)) return false;
  DWRITE_FONT_METRICS metrics{}; font.face->GetMetrics(&metrics);
  if (!metrics.designUnitsPerEm) return false;
  const double size = float(std::round(input.size * 60) / 60);
  const float factor = float(size / metrics.designUnitsPerEm);
  const double maxAscent = std::round(metrics.ascent * factor);
  const double maxDescent = std::round(metrics.descent * factor);
  const double height = maxAscent + maxDescent;
  // nsFontMetrics rounds each metric to whole pixels; GetNormalLineHeight uses
  // 1.2em only when the font has neither internal nor external leading.
  const double em = std::floor(size + .5);
  const double internal = std::floor(std::max(height - size, 0.0) + .5);
  const double external = std::floor(std::ceil(metrics.lineGap * factor) + .5);
  // An inline box is MaxAscent + MaxDescent (nsLineLayout / nsTextFrame),
  // the descent widened to hold the underline.
  *content = maxAscent + FrameMaxDescent(metrics, double(factor), maxAscent, maxDescent,
                                         font.face.Get(), size);
  *normal = internal == 0 && external == 0 ? std::floor(float(em * 60) * 1.2f + .5f) / 60
                                           : em + internal + external;
  return true;
}
bool SystemFonts::Measure(const std::u16string& source, const FontInput& input,
                          ruyijs::domain::geometry::SvgTextMeasurement* result) {
  *result = {};
  if (source.size() > 65536 || !std::isfinite(input.scale) || input.scale <= 0) return false;
  const auto text = Normalize(source);
  if (text.empty() || input.size == 0) return true;
  if (!collection_) {
    auto collection = std::make_unique<FontCollection>();
    if (!collection->Initialize()) return false;
    collection_ = std::move(collection);
  }
  const size_t visible = text.size() - (text.back() == u' ' ? 1 : 0);
  result->characters.resize(visible);
  ruyijs::domain::geometry::SvgBounds bounds, transformed;
  int total = 0, trailing = 0;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = start;
    const auto first = Codepoint(text, &end);
    SelectedFont font;
    bool missing = false;
    if (!collection_->Select(input, first, &font, &missing) &&
        (!missing || !collection_->Select(input, 0x20, &font))) return false;
    while (!missing && end < text.size()) {
      size_t next = end;
      const auto code = Codepoint(text, &next);
      SelectedFont candidate;
      if (!Continuation(code)) {
        bool absent = false;
        if (!collection_->Select(input, code, &candidate, &absent)) {
          if (absent) break;
          return false;
        }
        if (font.face.Get() != candidate.face.Get() || font.syntheticItalic != candidate.syntheticItalic) break;
      }
      end = next;
    }
    FontInput local = input;
    local.x += total / 60.0 / input.scale;
    FontRun run;
    if (missing ? !MissingFontRun(font, local, first, end-start, &run)
                : !ShapeFont(font, local, text.substr(start, end-start), &run)) return false;
    bounds.Include(run.box);
    transformed.Include(input.transform.ApplyUnrounded(run.unrounded));
    int advance = 0;
    std::vector<int> offsets(run.advances.size());
    for (size_t i = 0; i < offsets.size(); ++i) { offsets[i] = advance; advance += run.advances[i]; }
    const float inverse = 1 / input.scale;
    for (size_t i = start; i < std::min(end, visible); ++i) {
      const int cluster = run.clusters[i-start];
      if (cluster < 0) continue;
      result->characters[i] = {float(input.x + (total+offsets[cluster])/60.0*inverse),
        float(input.y-run.ascent/60*inverse), float(run.advances[cluster]/60.0*inverse),
        float((run.ascent+run.descent)/60*inverse)};
    }
    if (end == text.size() && visible < text.size()) trailing = run.advances.back();
    total += run.advance; start = end;
  }
  result->box = bounds.box;
  result->transformed = transformed.box;
  result->length = float((total-trailing)/60.0/input.scale);
  return true;
}
}

#endif  // _WIN32
