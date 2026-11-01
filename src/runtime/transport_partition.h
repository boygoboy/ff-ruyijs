#ifndef RUYIJS_RUNTIME_TRANSPORT_PARTITION_H
#define RUYIJS_RUNTIME_TRANSPORT_PARTITION_H
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ruyijs {
// The network identity of one Context session (docs/engine.md). Necko helper
// processes are leased per partition, so a session never reuses another
// partition's connections or TLS session cache, and a partition's helper and
// worker processes get its proxy and Firefox TLS release in their environment.
// A single-session process runs in the default partition: key "", no
// overrides, the inherited process environment exactly as before.
struct TransportPartition {
  std::string key;
  // Values that replace the inherited ones (RUYIJS_HTTP_PROXY, NECKO_FIREFOX)
  // in the partition's child processes.
  std::vector<std::pair<std::string, std::string>> environment;
};
using PartitionRef = std::shared_ptr<const TransportPartition>;
// The calling thread's partition. A session thread sets its own once; a
// transport thread a session starts adopts the starter's (PartitionScope).
PartitionRef CurrentPartition();
void SetCurrentPartition(PartitionRef partition);
class PartitionScope final {
 public:
  explicit PartitionScope(PartitionRef partition) : previous_(CurrentPartition()) {
    SetCurrentPartition(std::move(partition));
  }
  ~PartitionScope() { SetCurrentPartition(std::move(previous_)); }
  PartitionScope(const PartitionScope&) = delete;
  PartitionScope& operator=(const PartitionScope&) = delete;
 private:
  PartitionRef previous_;
};
// The process environment with the partition's overrides applied: a Windows
// CreateProcessW block (CREATE_UNICODE_ENVIRONMENT) or POSIX "NAME=value"
// strings. Callers keep the inherited environment when Overrides() is false.
inline bool Overrides(const TransportPartition& partition) { return !partition.environment.empty(); }
#ifdef _WIN32
std::wstring EnvironmentBlock(const TransportPartition& partition);
#else
std::vector<std::string> EnvironmentStrings(const TransportPartition& partition);
#endif
}
#endif
