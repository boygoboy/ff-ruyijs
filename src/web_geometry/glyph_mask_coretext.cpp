// Native CoreGraphics glyph rasterization. Colour bitmap fonts are represented
// by non-overlapping RGBA row spans without changing the canvas domain ABI.
#ifdef __APPLE__
#include "web_geometry/mac_font_internal.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace ruyijs::web_geometry {
namespace {
bool ColorSpans(const std::vector<uint8_t>& pixels, CanvasGlyphMask* out) {
  for (int y = 0; y < out->height; ++y) {
    for (int x = 0; x < out->width;) {
      const auto* pixel = pixels.data() + (size_t(y) * out->width + x) * 4;
      if (!pixel[3]) { ++x; continue; }
      int end = x + 1;
      while (end < out->width && std::memcmp(pixel, pixels.data() + (size_t(y) * out->width + end) * 4, 4) == 0) ++end;
      // Bound metadata as well as the bitmap allocation.
      if (out->layers.size() >= 262144) return false;
      CanvasGlyphMask::Layer layer;
      layer.left = out->left + x; layer.top = out->top + y;
      layer.width = end - x; layer.height = 1;
      layer.coverage.assign(size_t(layer.width), 255);
      layer.rgba[3] = pixel[3];
      for (unsigned c = 0; c < 3; ++c)
        layer.rgba[c] = uint8_t(std::min(255u, (unsigned(pixel[c]) * 255 + pixel[3] / 2) / pixel[3]));
      out->layers.push_back(std::move(layer));
      x = end;
    }
  }
  return true;
}
}  // namespace

bool MacDrawGlyphMask(CTFontRef font, CGGlyph glyph, const CGAffineTransform& transform,
                      const uint8_t rgb[3], CanvasGlyphMask* out, size_t* budget) {
  const CGRect box = CGRectApplyAffineTransform(
      CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, &glyph, nullptr, 1), transform);
  if (CGRectIsNull(box) || CGRectIsEmpty(box)) return true;
  const double left = std::floor(CGRectGetMinX(box)) - 2, top = std::floor(CGRectGetMinY(box)) - 2;
  const double width = std::ceil(CGRectGetMaxX(box)) + 2 - left;
  const double height = std::ceil(CGRectGetMaxY(box)) + 2 - top;
  constexpr double limit = double(std::numeric_limits<int>::max()) / 2;
  if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(width) || !std::isfinite(height) ||
      std::abs(left) > limit || std::abs(top) > limit || width <= 0 || height <= 0 ||
      width > 16384 || height > 16384 || width * height > 1024 * 1024 ||
      size_t(width * height) > *budget) return false;
  *budget -= size_t(width * height);
  out->left = int(left); out->top = int(top); out->width = int(width); out->height = int(height);
  std::vector<uint8_t> pixels(size_t(width * height) * 4, 0);
  MacRef<CGColorSpaceRef> colors(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
  MacRef<CGContextRef> context(CGBitmapContextCreate(pixels.data(), out->width, out->height, 8,
      size_t(out->width) * 4, colors.get(), kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big));
  if (!context) return false;
  CGContextSetRGBFillColor(context.get(), rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0, 1);
  CGContextSetShouldAntialias(context.get(), true);
  CGContextSetShouldSmoothFonts(context.get(), false);
  CGContextSetShouldSubpixelPositionFonts(context.get(), true);
  CGContextSetShouldSubpixelQuantizeFonts(context.get(), false);
  auto local = transform; local.tx -= left; local.ty -= top;
  CGContextConcatCTM(context.get(), local);
  const CGPoint position = CGPointZero;
  CTFontDrawGlyphs(font, &glyph, &position, 1, context.get());
  // CGBitmapContext stores scanlines from the top of its native y-up surface;
  // the mask ABI uses increasing canvas y. Reverse rows after drawing with
  // the device-space transform above so outlines and bitmaps agree.
  const size_t stride = size_t(out->width) * 4;
  for (int y = 0; y < out->height / 2; ++y)
    std::swap_ranges(pixels.begin() + size_t(y) * stride, pixels.begin() + size_t(y + 1) * stride,
                     pixels.begin() + size_t(out->height - 1 - y) * stride);
  if (CTFontGetSymbolicTraits(font) & kCTFontColorGlyphsTrait) return ColorSpans(pixels, out);
  out->coverage.resize(size_t(out->width) * out->height);
  for (size_t i = 0; i < out->coverage.size(); ++i) out->coverage[i] = pixels[i * 4 + 3];
  return true;
}
}  // namespace ruyijs::web_geometry
#endif
