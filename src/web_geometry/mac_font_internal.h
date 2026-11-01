#pragma once
#ifdef __APPLE__
#include <CoreText/CoreText.h>
#include <CoreGraphics/CoreGraphics.h>
#include <cmath>
#include <utility>
#include "web_geometry/system_font.h"

namespace ruyijs::web_geometry {
template<class T> class MacRef {
 public:
  explicit MacRef(T value = nullptr) : value_(value) {}
  ~MacRef() { if (value_) CFRelease(value_); }
  MacRef(const MacRef&) = delete;
  MacRef& operator=(const MacRef&) = delete;
  MacRef(MacRef&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  MacRef& operator=(MacRef&& other) noexcept {
    if (this != &other) { if (value_) CFRelease(value_); value_ = std::exchange(other.value_, nullptr); }
    return *this;
  }
  T get() const { return value_; }
  explicit operator bool() const { return value_ != nullptr; }
 private:
  T value_;
};
// The CoreText helpers below take the shared FontCollection by reference for
// call symmetry with the shaper; the family-name list they actually read is
// the installed-family array MacFamilies() returns (read once per process).
CFArrayRef MacFamilies();
std::wstring MacWide(CFStringRef string);
MacRef<CFStringRef> MacString(const std::string& utf8);
MacRef<CTFontRef> MacPrimaryFont(const FontCollection&, const FontInput&);
MacRef<CTLineRef> MacTextLine(const FontCollection&, const std::u16string&, const FontInput&);
bool MacDrawGlyphMask(CTFontRef, CGGlyph, const CGAffineTransform&, const uint8_t rgb[3],
                      CanvasGlyphMask*, size_t* pixelBudget);
inline bool MacFontInputValid(const FontInput& input) {
  return std::isfinite(input.size) && input.size > 0 && std::isfinite(input.scale) &&
         input.scale > 0 && input.size * input.scale <= 16384 &&
         std::isfinite(input.x) && std::isfinite(input.y);
}
inline double MacFontSize(const FontInput& input) {
  return std::round(input.size * input.scale * 60) / 60;
}
}  // namespace ruyijs::web_geometry
#endif
