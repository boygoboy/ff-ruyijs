// Content timer precision (domain/execution_scheduling/timer_precision.h):
// the process secret (nsRFPService's sSecretMidpointSeed) and per-timeline
// seeds (RelativeTimeline::GetRandomTimelineSeed), both from the platform
// random generator as Firefox draws them.
#include "execution_scheduling/timer_precision.h"
#include <cstdint>
#include <cstdlib>
#include <mutex>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <sys/random.h>
#endif

namespace ruyijs::execution_scheduling {
namespace {
bool Random(void* out, size_t size) {
#ifdef _WIN32
  return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(size),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG));
#else
  return size <= 256 && getentropy(out, size) == 0;
#endif
}
}  // namespace
const domain::execution_scheduling::TimerSecret& ProcessTimerSecret() {
  // One secret per Context, as with a process per Context: a shared engine
  // runs each session on its own thread.
  thread_local domain::execution_scheduling::TimerSecret secret;
  thread_local bool ready = false;
  if (!ready) {
    if (!Random(secret.bytes, sizeof secret.bytes))
      for (auto& byte : secret.bytes) byte = uint8_t(std::rand());
    ready = true;
  }
  return secret;
}
int64_t NewTimelineSeed() {
  int64_t seed = 0;
  // RelativeTimeline keeps 0 as "not drawn yet"; it falls back to rand().
  if (!Random(&seed, sizeof seed) || seed == 0) seed = std::rand();
  return seed;
}
double ReduceContentTime(double ms, int64_t mixin) {
  using namespace domain::execution_scheduling;
  return ReduceTimePrecision(ProcessTimerSecret(), ms, TimeScale::MilliSeconds,
                             kTimerResolutionUSec, mixin);
}
}  // namespace ruyijs::execution_scheduling
