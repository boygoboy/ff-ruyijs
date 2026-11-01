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
// Two functions called from the same frame report the same position.
#if defined(_MSC_VER)
#define RUYIJS_ENTRY_POSITION() reinterpret_cast<uintptr_t>(_AddressOfReturnAddress())
#else
#define RUYIJS_ENTRY_POSITION() reinterpret_cast<uintptr_t>(__builtin_frame_address(0))
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
