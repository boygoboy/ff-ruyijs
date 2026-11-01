#include "runtime/transport_partition.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
extern char** environ;
#endif

namespace ruyijs {
namespace {
thread_local PartitionRef current;
}
PartitionRef CurrentPartition() {
  if (current) return current;
  static const PartitionRef process = std::make_shared<const TransportPartition>();
  return process;
}
void SetCurrentPartition(PartitionRef partition) { current = std::move(partition); }

#ifdef _WIN32
namespace {
std::wstring Wide(const std::string& text) {
  if (text.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring wide(size_t(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), count);
  return wide;
}
bool SameName(const std::wstring& entry, const std::wstring& name) {
  // Windows variable names compare without case; "=C:" style entries never match.
  return entry.size() > name.size() && entry[name.size()] == L'=' &&
         CompareStringOrdinal(entry.data(), int(name.size()), name.data(), int(name.size()), TRUE) ==
             CSTR_EQUAL;
}
}
std::wstring EnvironmentBlock(const TransportPartition& partition) {
  std::vector<std::wstring> entries;
  if (wchar_t* strings = GetEnvironmentStringsW()) {
    for (const wchar_t* entry = strings; *entry; entry += wcslen(entry) + 1) entries.emplace_back(entry);
    FreeEnvironmentStringsW(strings);
  }
  for (const auto& [name, value] : partition.environment) {
    const std::wstring wideName = Wide(name), line = wideName + L"=" + Wide(value);
    bool replaced = false;
    for (auto& entry : entries)
      if (SameName(entry, wideName)) { entry = line; replaced = true; }
    if (!replaced) entries.push_back(line);
  }
  std::wstring block;
  for (const auto& entry : entries) { block += entry; block.push_back(L'\0'); }
  block.push_back(L'\0');
  return block;
}
#else
std::vector<std::string> EnvironmentStrings(const TransportPartition& partition) {
  std::vector<std::string> entries;
  for (char** entry = environ; entry && *entry; ++entry) entries.emplace_back(*entry);
  for (const auto& [name, value] : partition.environment) {
    const std::string prefix = name + "=", line = prefix + value;
    bool replaced = false;
    for (auto& entry : entries)
      if (entry.compare(0, prefix.size(), prefix) == 0) { entry = line; replaced = true; }
    if (!replaced) entries.push_back(line);
  }
  return entries;
}
#endif
}
