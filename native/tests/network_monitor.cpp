#include "shell/network_monitor.h"
#include "shell/wifi.h"

#include <cstdint>
#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

} // namespace

int main()
{
    using namespace atrium;

    Check(LinkSpeedMbps(0) == 0.0, "zero link speed stays zero");
    Check(LinkSpeedMbps(1000000000ULL) == 1000.0, "1 Gbps reports 1000 Mbps");
    Check(LinkSpeedMbps(2500000000ULL) == 2500.0, "2.5 Gbps reports 2500 Mbps");
    Check(LinkSpeedMbps(~uint64_t{0}) == 0.0, "NDIS_LINK_SPEED_UNKNOWN reports zero instead of ~1.8e13 Mbps");

    constexpr GUID kGuid1{0x11111111, 0, 0, {1}}, kGuid2{0x22222222, 0, 0, {2}};
    auto m1 = MergeWifiNetworks({{L"HiddenNet", L"HiddenNet", 72, true, true, kGuid1}});
    Check(m1.size() == 1 && m1[0].ssid == L"HiddenNet" && m1[0].connected && IsEqualGUID(m1[0].interfaceGuid, kGuid1), "hidden SSID via profile");
    auto m2 = MergeWifiNetworks({{L"Neighbor", L"", 95, true, false, kGuid1},
                                 {L"Office", L"Office", 55, true, true, kGuid1},
                                 {L"Cafe", L"Cafe", 40, false, false, kGuid2},
                                 {L"Home5G", L"Home5G", 80, true, true, kGuid2}});
    Check(m2.size() == 4 && m2[0].ssid == L"Home5G" && m2[1].ssid == L"Office" && m2[2].ssid == L"Neighbor" && m2[3].ssid == L"Cafe" &&
              IsEqualGUID(m2[0].interfaceGuid, kGuid2) && IsEqualGUID(m2[1].interfaceGuid, kGuid1),
          "two connected interfaces and ordering");
    auto m3 = MergeWifiNetworks({{L"Corp", L"Corp-Old", 90, true, false, kGuid2}, {L"Corp", L"Corp-New", 65, true, true, kGuid1}});
    Check(m3.size() == 1 && m3[0].connected && m3[0].profile == L"Corp-New" && IsEqualGUID(m3[0].interfaceGuid, kGuid1) && m3[0].signal == 90,
          "connected profile/interface wins and keeps max signal");
    Check(ClassifyWifiListStatus(0, false, false) == WifiListStatus::NoAdapter && ClassifyWifiListStatus(2, false, true) == WifiListStatus::AccessDenied &&
              ClassifyWifiListStatus(2, true, true) == WifiListStatus::Ok && ClassifyWifiListStatus(1, false, false) == WifiListStatus::Error &&
              ClassifyWifiListStatus(0, false, false, ERROR_ACCESS_DENIED) == WifiListStatus::AccessDenied,
          "status classification");

    if (g_failures) {
        std::fprintf(stderr, "%d network monitor check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("network monitor checks passed");
    return 0;
}
