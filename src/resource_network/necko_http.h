#ifndef RUYIJS_NECKO_HTTP_H
#define RUYIJS_NECKO_HTTP_H
#include "resource_network/page_http.h"
namespace ruyijs::resource_network {
bool NeckoHttp(const std::string& method, const std::string& url, const HeaderPairs&,
               const std::string& body, HttpResult* result, uint32_t timeoutMs = 20000);
// Background start of helper sessions at Context start (necko_http.cpp), for
// the calling thread's transport partition.
void PrewarmNeckoSessions(size_t count) noexcept;
// A shared engine's helper budget across partitions; 0 keeps no global limit.
void SetNeckoHelperLimit(size_t limit) noexcept;
// Ends a closed session's helpers; one still in use goes when it is returned.
void ReleaseNeckoPartition(const std::string& key) noexcept;
}
#endif
