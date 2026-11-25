// Launch counts for Launchpad ordering and the recent-apps menu (most used first).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace atrium {

struct CatalogApp;

void RecordAppLaunch(const std::wstring& id);
// Flush debounced counts to disk (exit / end-session).
void FlushAppUsage();
// Higher = used more. Missing ids sort as 0.
uint64_t AppLaunchCount(const std::wstring& id);
void SortAppsByUsage(std::vector<CatalogApp>& apps);

} // namespace atrium
