#include "runtime/native_stack.h"
#include "js/RootingAPI.h"
#include "js/Stack.h"

namespace ruyijs {
namespace {
uintptr_t Limit(JSContext* cx) {
  return JS::RootingContext::get(cx)->nativeStackLimit[JS::StackForSystemCode];
}
// A session thread has an 8 MiB stack (tools/generate.py); the quota never
// reaches past it.
constexpr JS::NativeStackSize kThreadStack = 8 * 1024 * 1024 - 256 * 1024;
}

size_t NativeStackBelow(JSContext* cx, uintptr_t entry) {
  const uintptr_t limit = Limit(cx);
  return entry > limit ? entry - limit : 0;
}

void MatchNativeStack(JSContext* cx, uintptr_t entry, size_t available) {
  // JS_SetNativeStackQuota measures from the thread's stack base, which is
  // not public: a known quota gives the base (up to the engine's constant
  // offset), and the second quota puts the limit `available` below `entry`.
  constexpr JS::NativeStackSize probe = 1024 * 1024;
  JS_SetNativeStackQuota(cx, probe);
  const uintptr_t target = entry - available;
  if (available >= entry || target >= Limit(cx) + probe) return;
  const JS::NativeStackSize quota = Limit(cx) + probe - target;
  if (quota <= kThreadStack) JS_SetNativeStackQuota(cx, quota);
}
}
