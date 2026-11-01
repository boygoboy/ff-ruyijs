#ifndef RUYIJS_RUNTIME_NATIVE_STACK_H
#define RUYIJS_RUNTIME_NATIVE_STACK_H
#include <cstddef>
#include <cstdint>
#include "js/TypeDecls.h"
#if defined(_MSC_VER)
#include <intrin.h>
#endif

// Where the frame of the function that expands it begins: the slot of its
// return address (MSVC ABI) or its frame pointer, a fixed offset from it.
// That function must be RUYIJS_NEVER_INLINE: inlined into a caller, it would
// report where the caller's frame begins, which differs from caller to caller.
#if defined(_MSC_VER)
#define RUYIJS_ENTRY_POSITION() reinterpret_cast<uintptr_t>(_AddressOfReturnAddress())
#define RUYIJS_NEVER_INLINE __declspec(noinline)
#else
#define RUYIJS_ENTRY_POSITION() reinterpret_cast<uintptr_t>(__builtin_frame_address(0))
#define RUYIJS_NEVER_INLINE __attribute__((noinline))
#endif

namespace ruyijs {
// The native stack left to scripts below `entry` (the stack grows down).
size_t NativeStackBelow(JSContext* cx, uintptr_t entry);
// Gives `cx` a quota that leaves exactly `available` bytes below `entry`, as
// the main thread of a single-session process has at the same point. A shared
// engine's session threads call it before any script runs, so recursion depth
// does not tell a session from a Context with a process of its own.
void MatchNativeStack(JSContext* cx, uintptr_t entry, size_t available);
}
#endif
