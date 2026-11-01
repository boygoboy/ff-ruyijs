#ifdef _WIN32
// The glyph masks of one fillText. The run split is the shaping owner's (same
// fonts, same segments as the outline producer), and every glyph becomes the
// device-space mask Firefox hands to its mask blitter: the position is snapped
// to the nearest eighth of a pixel and the remainder becomes the DirectWrite
// glyph run offset, the way Skia fills a SkTextBlob position into a strike.
#include <cmath>
#include "web_geometry/script_runs.h"

#include "web_geometry/font_backend.h"

namespace ruyijs::web_geometry {
namespace {
uint32_t Codepoint(const std::u16string& text, size_t* at) {
  uint32_t code = text[(*at)++];
  if (code >= 0xd800 && code <= 0xdbff && *at < text.size() &&
      text[*at] >= 0xdc00 && text[*at] <= 0xdfff)
    code = 0x10000 + ((code - 0xd800) << 10) + text[(*at)++] - 0xdc00;
  return code;
}
bool Continuation(uint32_t code) {
  return code == 0x200d || (code >= 0xfe00 && code <= 0xfe0f) ||
      (code >= 0xe0100 && code <= 0xe01ef) ||
      hb_unicode_combining_class(hb_unicode_funcs_get_default(), code) != 0;
}
// SkPackedGlyphID packing: add the 1/8 rounding constant, floor for the device
// integer, and take the remaining quarter-pixel steps as the subpixel field.
void SnapPosition(float position, int* base, float* subpixel) {
  const float mapped = position + 0.125f;
  *base = int(std::floor(mapped));
  const float fraction = mapped - float(*base);
  *subpixel = float(int(fraction * 4.0f)) * 0.25f;
}
}  // namespace

bool SystemFonts::BuildCanvasGlyphMasks(const std::u16string& text, const FontInput& input,
                                        const float matrix[6], const uint8_t rgb[3],
                                        std::vector<CanvasGlyphMask>* result) {
  result->clear();
  if (text.empty()) return true;
  if (text.size() > 65536 || !std::isfinite(input.scale) || input.scale <= 0 ||
      !std::isfinite(input.size) || input.size <= 0 || !std::isfinite(input.x) ||
      !std::isfinite(input.y)) return false;
  if (!collection_) {
    auto collection = std::make_unique<FontCollection>();
    if (!collection->Initialize()) return false;
    collection_ = std::move(collection);
  }
  const float emsize = float(std::round(input.size * input.scale * 60.0) / 60.0);
  // A glyph mask is rasterised at the device size, which Skia takes from the
  // vertical extent of the text matrix.
  const float scale = std::sqrt(matrix[2] * matrix[2] + matrix[3] * matrix[3]);
  if (!std::isfinite(emsize) || emsize <= 0 || emsize > 16384 || !(scale > 0)) return false;
  const float linear[4] = {matrix[0] / scale, matrix[1] / scale, matrix[2] / scale,
                           matrix[3] / scale};
  int pen = 0;
  const std::vector<bool> cjk = CJKRunUnits(text);
  for (size_t start = 0; start < text.size();) {
    size_t end = start;
    SelectedFont font;
    const uint32_t first = Codepoint(text, &end);
    if (!collection_->Select(input, first, &font)) return false;
    while (end < text.size()) {
      if (cjk[end] != cjk[start]) break;
      size_t next = end;
      const uint32_t code = Codepoint(text, &next);
      if (!Continuation(code)) {
        SelectedFont candidate;
        if (!collection_->Select(input, code, &candidate)) return false;
        if (font.face.Get() != candidate.face.Get() ||
            font.syntheticItalic != candidate.syntheticItalic) break;
      }
      end = next;
    }
    FontInput local = input;
    local.x = local.y = 0;
    FontRun run;
    if (!ShapeFont(font, local, text.substr(start, end - start), &run)) return false;
    // gfxFont::DrawGlyphs for a synthetic-oblique run: the context matrix
    // becomes CTM.PreTranslate(p).PreMultiply(Matrix(1, 0, -skew, 1, 0, 0))
    // .PreTranslate(-p), p being the run origin on the baseline (float).
    float run_matrix[6] = {matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5]};
    if (font.syntheticItalic) {
      static const float kTanDefaultAngle = float(std::tan(14.0 * (2.0 * 3.14159265358979323846 / 360.0)));
      const float skew = kTanDefaultAngle;
      const float px = float(input.x + double(pen) / 60.0), py = float(input.y);
      float* m = run_matrix;
      m[4] += m[0] * px + m[2] * py;
      m[5] += m[1] * px + m[3] * py;
      const float c = -skew * m[0] + m[2], d = -skew * m[1] + m[3];
      m[2] = c;
      m[3] = d;
      m[4] += m[0] * -px + m[2] * -py;
      m[5] += m[1] * -px + m[3] * -py;
    }
    const float run_scale = std::sqrt(run_matrix[2] * run_matrix[2] + run_matrix[3] * run_matrix[3]);
    if (!(run_scale > 0)) return false;
    const float run_linear[4] = {run_matrix[0] / run_scale, run_matrix[1] / run_scale,
                                 run_matrix[2] / run_scale, run_matrix[3] / run_scale};
    int glyph_pen = pen;
    for (size_t i = 0; i < run.glyphs.size(); ++i) {
      // gfxDWriteFont::ShouldRoundXOffset is false for the natural measuring
      // mode Canvas text uses, so the shaper keeps gfxHarfBuzzShaper's plain
      // rounding to app units: no residual carry, one rounded advance per glyph.
      const int offset = int(std::floor(run.glyph_offsets_x[i] * 60.0f + 0.5f));
      const int advance = int(std::floor(run.glyph_advances[i] * 60.0f + 0.5f));
      const double x = input.x + double(glyph_pen + offset) / 60.0;
      const double y = input.y + double(std::floor(run.glyph_offsets_y[i] * 60.0f + 0.5f)) / 60.0;
      const float device_x = float(run_matrix[0] * x + run_matrix[2] * y + run_matrix[4]);
      const float device_y = float(run_matrix[1] * x + run_matrix[3] * y + run_matrix[5]);
      CanvasGlyphMask mask;
      // gfxFont::RenderColorGlyph: up to an adjusted size of 256 a COLR glyph is
      // a cached 2x snapshot the canvas resamples at the rounded glyph origin;
      // an upright run is all the snapshot path models, so a rotated or skewed
      // run (and a larger glyph) fills its layers directly as subpixel masks.
      const bool upright = run_linear[1] == 0.0f && run_linear[2] == 0.0f;
      const bool sampled = emsize <= 256.0f && upright;
      int color_x = 0, color_y = 0;
      float color_sx = 0, color_sy = 0;
      SnapPosition(device_x, &color_x, &color_sx);
      SnapPosition(device_y, &color_y, &color_sy);
      if (GlyphColorMask(font, sampled ? emsize : emsize * run_scale, sampled, run_linear, color_sx,
                         color_sy, run.glyphs[i], rgb, &mask)) {
        if (sampled) {
          mask.origin_x = x;
          mask.origin_y = y;
        } else {
          for (auto& layer : mask.layers) {
            layer.left += color_x;
            layer.top += color_y;
          }
        }
        if (!mask.layers.empty()) result->push_back(std::move(mask));
      } else {
        int base_x = 0, base_y = 0;
        float sub_x = 0, sub_y = 0;
        SnapPosition(device_x, &base_x, &sub_x);
        SnapPosition(device_y, &base_y, &sub_y);
        if (!GlyphMask(font, emsize * run_scale, run_linear, sub_x, sub_y, run.glyphs[i], rgb, &mask))
          return false;
        if (!mask.coverage.empty()) {
          mask.left += base_x;
          mask.top += base_y;
          result->push_back(std::move(mask));
        }
      }
      glyph_pen += advance;
    }
    pen += run.advance;
    start = end;
  }
  return true;
}
namespace {
// gfxDWriteFontEntry::IsCJKFont && gfxDWriteFont::HasBitmapStrikeForSize.
bool CJKBitmapStrike(IDWriteFontFace* face, uint32_t size) {
  auto table = [&](UINT32 tag, auto&& body) {
    const void* data = nullptr;
    UINT32 length = 0;
    void* context = nullptr;
    BOOL exists = FALSE;
    if (FAILED(face->TryGetFontTable(tag, &data, &length, &context, &exists))) return false;
    bool result = false;
    if (exists) result = body(static_cast<const uint8_t*>(data), length);
    face->ReleaseFontTable(context);
    return result;
  };
  auto be32 = [](const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  };
  const bool cjk = table(DWRITE_MAKE_OPENTYPE_TAG('O', 'S', '/', '2'),
                         [&](const uint8_t* p, UINT32 length) {
    return length >= 82 && (be32(p + 78) & (31u << 17)) != 0;
  });
  if (!cjk) return false;
  return table(DWRITE_MAKE_OPENTYPE_TAG('E', 'B', 'L', 'C'), [&](const uint8_t* p, UINT32 length) {
    if (length < 8 || be32(p) != 0x00020000) return false;
    const uint32_t count = be32(p + 4);
    if (count > 0xffff || length < 8 + count * 48) return false;
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* item = p + 8 + i * 48;
      if (item[44] == size && item[45] == size) {
        const unsigned start = unsigned(item[40]) << 8 | item[41];
        const unsigned end = unsigned(item[42]) << 8 | item[43];
        return uint16_t(end) >= uint16_t(start + 3);
      }
    }
    return false;
  });
}
}  // namespace

void ReleaseCanvasGlyphRuns(std::vector<CanvasGlyphRun>* runs) {
  for (auto& run : *runs) if (run.face) { run.face->Release(); run.face = nullptr; }
  runs->clear();
}

bool SystemFonts::CanvasGlyphRuns(const std::u16string& text, const FontInput& input,
                                  std::vector<CanvasGlyphRun>* result, float* emsize_out) {
  for (auto& run : *result) if (run.face) run.face->Release();
  result->clear();
  if (text.empty()) return true;
  if (text.size() > 65536 || !std::isfinite(input.scale) || input.scale <= 0 ||
      !std::isfinite(input.size) || input.size <= 0 || !std::isfinite(input.x) ||
      !std::isfinite(input.y)) return false;
  if (!collection_) {
    auto collection = std::make_unique<FontCollection>();
    if (!collection->Initialize()) return false;
    collection_ = std::move(collection);
  }
  const float emsize = float(std::round(input.size * input.scale * 60.0) / 60.0);
  if (!std::isfinite(emsize) || emsize <= 0 || emsize > 16384) return false;
  *emsize_out = emsize;
  int pen = 0;
  const std::vector<bool> cjk = CJKRunUnits(text);
  for (size_t start = 0; start < text.size();) {
    size_t end = start;
    SelectedFont font;
    const uint32_t first = Codepoint(text, &end);
    if (!collection_->Select(input, first, &font)) return false;
    while (end < text.size()) {
      if (cjk[end] != cjk[start]) break;
      size_t next = end;
      const uint32_t code = Codepoint(text, &next);
      if (!Continuation(code)) {
        SelectedFont candidate;
        if (!collection_->Select(input, code, &candidate)) return false;
        if (font.face.Get() != candidate.face.Get() ||
            font.syntheticItalic != candidate.syntheticItalic) break;
      }
      end = next;
    }
    FontInput local = input;
    local.x = local.y = 0;
    FontRun run;
    if (!ShapeFont(font, local, text.substr(start, end - start), &run)) return false;
    CanvasGlyphRun out;
    out.face = font.face.Get();
    out.face->AddRef();
    out.syntheticItalic = font.syntheticItalic;
    out.cjkBitmapStrike = CJKBitmapStrike(out.face, uint32_t(std::lround(emsize)));
    {
      const void* data = nullptr;
      UINT32 length = 0;
      void* context = nullptr;
      BOOL exists = FALSE;
      if (SUCCEEDED(out.face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('C', 'O', 'L', 'R'), &data,
                                              &length, &context, &exists))) {
        out.colorFont = exists != FALSE;
        out.face->ReleaseFontTable(context);
      }
    }
    out.origin_x = input.x + double(pen) / 60.0;
    out.origin_y = input.y;
    int glyph_pen = pen;
    for (size_t i = 0; i < run.glyphs.size(); ++i) {
      const int offset = int(std::floor(run.glyph_offsets_x[i] * 60.0f + 0.5f));
      const int advance = int(std::floor(run.glyph_advances[i] * 60.0f + 0.5f));
      out.glyphs.push_back(run.glyphs[i]);
      out.x.push_back(input.x + double(glyph_pen + offset) / 60.0);
      out.y.push_back(input.y + double(std::floor(run.glyph_offsets_y[i] * 60.0f + 0.5f)) / 60.0);
      glyph_pen += advance;
    }
    result->push_back(std::move(out));
    pen += run.advance;
    start = end;
  }
  return true;
}
}  // namespace ruyijs::web_geometry

#endif  // _WIN32
