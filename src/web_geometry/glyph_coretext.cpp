// Native CoreText outlines and bitmap glyph rendering into the existing
// CanvasGlyphMask contract. Bitmap colour fonts become exact RGBA row spans.
#ifdef __APPLE__
// font_backend.h first: it defines BOOL/FALSE compat constants that would
// collide with CoreFoundation's macros if CoreText headers came in earlier.
#include "web_geometry/font_backend.h"
#include "web_geometry/mac_font_internal.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>

namespace ruyijs::web_geometry {
namespace {
struct OutlineState { CanvasTextPath* path; bool valid = true; };
void Outline(void* opaque, const CGPathElement* element) {
  auto& state = *static_cast<OutlineState*>(opaque);
  if (!state.valid || state.path->points.size() + 6 > 2 * 1024 * 1024) { state.valid = false; return; }
  const CGPoint* p = element->points;
  switch (element->type) {
    case kCGPathElementMoveToPoint: state.path->Move(float(p[0].x), float(p[0].y)); break;
    case kCGPathElementAddLineToPoint: state.path->Line(float(p[0].x), float(p[0].y)); break;
    case kCGPathElementAddQuadCurveToPoint:
      state.path->Quad(float(p[0].x), float(p[0].y), float(p[1].x), float(p[1].y)); break;
    case kCGPathElementAddCurveToPoint:
      state.path->Cubic(float(p[0].x), float(p[0].y), float(p[1].x), float(p[1].y), float(p[2].x), float(p[2].y)); break;
    case kCGPathElementCloseSubpath: state.path->Close(); break;
  }
}
CTFontRef RunFont(CTRunRef run) {
  return static_cast<CTFontRef>(CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName));
}
// CanvasGlyphRun.face borrows the IDWriteFontFace the way the shared shaper's
// runs do: AddRef/Release are no-ops and the collection owns the face. Each
// distinct CTFont a cascade list produces gets one shim here, so the pool is
// bounded by the fonts the process shapes with.
struct FacePool {
  std::vector<std::unique_ptr<IDWriteFontFace>> owned;
  IDWriteFontFace* Face(CTFontRef font) {
    for (auto& face : owned) if (face->CoreFont() == font) return face.get();
    CFRetain(font);
    owned.emplace_back(new IDWriteFontFace(font));
    return owned.back().get();
  }
};
IDWriteFontFace* PoolFace(CTFontRef font) {
  static FacePool pool;
  return pool.Face(font);
}
}  // namespace

void ReleaseCanvasGlyphRuns(std::vector<CanvasGlyphRun>* runs) {
  if (!runs) return;
  for (auto& run : *runs) {
    if (run.face) {
      // The face borrows from the FacePool (AddRef/Release are no-ops, as in
      // the shared shaper's runs), so this only clears the borrow.
      run.face->Release();
      run.face = nullptr;
    }
  }
}

bool SystemFonts::CanvasGlyphRuns(const std::u16string& text, const FontInput& input,
                                  std::vector<CanvasGlyphRun>* result, float* emsize_out) {
  if (!result || !emsize_out) return false;
  ReleaseCanvasGlyphRuns(result);
  result->clear();
  if (text.empty()) return true;
  if (text.size() > 65536 || !MacFontInputValid(input)) return false;
  if (!collection_) collection_ = std::make_unique<FontCollection>();
  // The em size the ScaledFont draws at, app-unit rounded like the Windows
  // pipeline (device pixels; the caller puts the canvas transform on top).
  const float emsize = float(MacFontSize(input));
  if (!std::isfinite(emsize) || emsize <= 0 || emsize > 16384) return false;
  *emsize_out = emsize;
  auto line = MacTextLine(*collection_, text, input);
  if (!line) return false;
  const double inverse = 1 / input.scale;
  const auto runs = CTLineGetGlyphRuns(line.get());
  for (CFIndex r = 0; r < CFArrayGetCount(runs); ++r) {
    auto run = static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, r));
    const CTFontRef font = RunFont(run);
    const CFIndex count = CTRunGetGlyphCount(run);
    std::vector<CGGlyph> glyphs(size_t(count), 0);
    std::vector<CGPoint> positions(size_t(count), CGPointZero);
    CTRunGetGlyphs(run, CFRangeMake(0, count), glyphs.data());
    CTRunGetPositions(run, CFRangeMake(0, count), positions.data());
    CanvasGlyphRun out;
    out.face = PoolFace(font);
    // CoreText synthesizes obliques and scales any bitmap strikes itself; the
    // macOS system CJK fonts are vector.
    out.syntheticItalic = false;
    out.cjkBitmapStrike = false;
    MacRef<CFDataRef> colr(CTFontCopyTable(font, kCTFontTableCOLR, kCTFontTableOptionNoOptions));
    out.colorFont = colr.get() != nullptr;
    // Coordinates in CSS pixels, quantized to app units like the Windows
    // pipeline; CoreText positions are y-up points.
    out.origin_x = input.x;
    out.origin_y = input.y;
    for (size_t i = 0; i < size_t(count); ++i) {
      out.glyphs.push_back(glyphs[i]);
      out.x.push_back(input.x + std::floor(positions[i].x * inverse * 60.0 + 0.5) / 60.0);
      out.y.push_back(input.y - std::floor(positions[i].y * inverse * 60.0 + 0.5) / 60.0);
    }
    result->push_back(std::move(out));
  }
  return true;
}

bool SystemFonts::BuildCanvasTextPath(const std::u16string& text, const FontInput& input,
                                      CanvasTextPath* result) {
  if (!result) return false;
  *result = {};
  if (text.empty()) return true;
  auto line = MacTextLine(*collection_, text, input);
  if (!line) return false;
  OutlineState state{result};
  const auto runs = CTLineGetGlyphRuns(line.get());
  for (CFIndex r = 0; r < CFArrayGetCount(runs); ++r) {
    auto run = static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, r));
    const CTFontRef font = RunFont(run);
    const CFIndex count = CTRunGetGlyphCount(run);
    std::vector<CGGlyph> glyphs(size_t(count), 0);
    std::vector<CGPoint> positions(size_t(count), CGPointZero);
    CTRunGetGlyphs(run, CFRangeMake(0, count), glyphs.data());
    CTRunGetPositions(run, CFRangeMake(0, count), positions.data());
    for (size_t i = 0; i < glyphs.size(); ++i) {
      const double inverse = 1 / input.scale;
      const auto transform = CGAffineTransformMake(inverse, 0, 0, -inverse,
          input.x + positions[i].x * inverse, input.y - positions[i].y * inverse);
      MacRef<CGPathRef> path(CTFontCreatePathForGlyph(font, glyphs[i], &transform));
      if (!path) {
        // Colour bitmap glyphs have no vector outline. Report the unsupported
        // conversion explicitly; fillText uses native colour masks below.
        const CGRect box = CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, &glyphs[i], nullptr, 1);
        if (!CGRectIsEmpty(box)) return false;
        continue;
      }
      CGPathApply(path.get(), &state, Outline);
      if (!state.valid) return false;
    }
  }
  return true;
}

bool SystemFonts::BuildCanvasGlyphMasks(const std::u16string& text, const FontInput& input,
                                        const float matrix[6], const uint8_t rgb[3],
                                        std::vector<CanvasGlyphMask>* result) {
  if (!result || !matrix || !rgb) return false;
  result->clear();
  if (text.empty()) return true;
  for (unsigned i = 0; i < 6; ++i) if (!std::isfinite(matrix[i])) return false;
  auto line = MacTextLine(*collection_, text, input);
  if (!line) return false;
  size_t budget = 16 * 1024 * 1024;
  const auto runs = CTLineGetGlyphRuns(line.get());
  for (CFIndex r = 0; r < CFArrayGetCount(runs); ++r) {
    auto run = static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, r));
    const CTFontRef font = RunFont(run);
    const CFIndex count = CTRunGetGlyphCount(run);
    std::vector<CGGlyph> glyphs(size_t(count), 0);
    std::vector<CGPoint> positions(size_t(count), CGPointZero);
    CTRunGetGlyphs(run, CFRangeMake(0, count), glyphs.data());
    CTRunGetPositions(run, CFRangeMake(0, count), positions.data());
    for (size_t i = 0; i < glyphs.size(); ++i) {
      const double inverse = 1 / input.scale;
      const double x = input.x + positions[i].x * inverse, y = input.y - positions[i].y * inverse;
      const auto transform = CGAffineTransformMake(matrix[0] * inverse, matrix[1] * inverse,
          -matrix[2] * inverse, -matrix[3] * inverse,
          matrix[0] * x + matrix[2] * y + matrix[4], matrix[1] * x + matrix[3] * y + matrix[5]);
      CanvasGlyphMask mask;
      if (!MacDrawGlyphMask(font, glyphs[i], transform, rgb, &mask, &budget)) return false;
      if (!mask.coverage.empty() || !mask.layers.empty()) result->push_back(std::move(mask));
    }
  }
  return true;
}
}  // namespace ruyijs::web_geometry
#endif
