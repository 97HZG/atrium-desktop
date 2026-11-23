#include "shell/quick_toggles.h"

#include <windows.h>
#include <powrprof.h>
#include <powersetting.h>
#include <roapi.h>
#include <windows.networking.connectivity.h>
#include <windows.networking.networkoperators.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

#include <chrono>
#include <mutex>

namespace atrium {

namespace {

using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Networking::Connectivity;
using namespace ABI::Windows::Networking::NetworkOperators;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;

constexpr wchar_t kNightLightKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount\\Current\\"
    L"default$windows.data.bluelightreduction.bluelightreductionstate\\windows.data.bluelightreduction.bluelightreductionstate";
constexpr wchar_t kPersonalizeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr size_t kTimestampOffset = 10;
constexpr DWORD kDefaultBatteryThreshold = 20;

std::mutex g_energyMutex;
std::optional<DWORD> g_prevDcThreshold;

size_t VarintEnd(const std::vector<uint8_t>& data, size_t start)
{
    size_t i = start;
    while (i < data.size() && (data[i] & 0x80u) != 0)
        ++i;
    return i < data.size() ? i + 1 : data.size();
}

uint64_t DecodeVarint(const std::vector<uint8_t>& data, size_t start, size_t end)
{
    uint64_t value = 0;
    for (size_t i = end; i > start; --i)
        value = (value << 7) | (data[i - 1] & 0x7Fu);
    return value;
}

std::vector<uint8_t> EncodeVarint(uint64_t value)
{
    std::vector<uint8_t> out;
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7Fu);
        value >>= 7;
        if (value != 0)
            byte |= 0x80u;
        out.push_back(byte);
    } while (value != 0);
    return out;
}

std::optional<DWORD> ReadRegDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(root, subKey, valueName, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return value;
}

std::optional<std::vector<uint8_t>> ReadNightLightRaw()
{
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kNightLightKey, L"Data", RRF_RT_REG_BINARY, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0)
        return std::nullopt;
    std::vector<uint8_t> data(size);
    if (RegGetValueW(HKEY_CURRENT_USER, kNightLightKey, L"Data", RRF_RT_REG_BINARY, nullptr, data.data(), &size) != ERROR_SUCCESS)
        return std::nullopt;
    data.resize(size);
    return data;
}

ComPtr<INetworkOperatorTetheringManager> OpenTetheringManager()
{
    ComPtr<INetworkInformationStatics> netInfo;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Networking_Connectivity_NetworkInformation).Get(), IID_PPV_ARGS(&netInfo))))
        return nullptr;
    ComPtr<IConnectionProfile> profile;
    if (FAILED(netInfo->GetInternetConnectionProfile(&profile)) || !profile)
        return nullptr;

    ComPtr<INetworkOperatorTetheringManagerStatics2> statics;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Networking_NetworkOperators_NetworkOperatorTetheringManager).Get(),
                                      IID_PPV_ARGS(&statics))))
        return nullptr;

    TetheringCapability cap = TetheringCapability_DisabledByHardwareLimitation;
    if (FAILED(statics->GetTetheringCapabilityFromConnectionProfile(profile.Get(), &cap)) || cap != TetheringCapability_Enabled)
        return nullptr;

    ComPtr<INetworkOperatorTetheringManager> mgr;
    if (FAILED(statics->CreateFromConnectionProfile(profile.Get(), &mgr)))
        return nullptr;
    return mgr;
}

bool AwaitTetheringOp(IAsyncOperation<NetworkOperatorTetheringOperationResult*>* op)
{
    if (!op)
        return false;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done)
        return false;
    auto handler = Callback<IAsyncOperationCompletedHandler<NetworkOperatorTetheringOperationResult*>>(
        [done](IAsyncOperation<NetworkOperatorTetheringOperationResult*>*, AsyncStatus) -> HRESULT {
            SetEvent(done);
            return S_OK;
        });
    if (FAILED(op->put_Completed(handler.Get()))) {
        CloseHandle(done);
        return false;
    }
    WaitForSingleObject(done, 15000);
    CloseHandle(done);

    ComPtr<INetworkOperatorTetheringOperationResult> result;
    if (FAILED(op->GetResults(&result)) || !result)
        return false;
    TetheringOperationStatus status = TetheringOperationStatus_Unknown;
    return SUCCEEDED(result->get_Status(&status)) && status == TetheringOperationStatus_Success;
}

} // namespace

std::optional<bool> IsNightLightOn(const std::vector<uint8_t>& data)
{
    if (data.size() <= kTimestampOffset)
        return std::nullopt;
    const size_t p = VarintEnd(data, kTimestampOffset);
    if (p + 10 > data.size())
        return std::nullopt;
    if (data[p] != 0x2A || data[p + 1] != 0x2B || data[p + 2] != 0x0E)
        return std::nullopt;
    return data[p + 8] == 0x10 && data[p + 9] == 0x00;
}

std::vector<uint8_t> ToggleNightLightData(const std::vector<uint8_t>& data, bool on, uint64_t timestampSec)
{
    const auto wasOn = IsNightLightOn(data);
    if (!wasOn.has_value())
        return {};
    const size_t tsEnd = VarintEnd(data, kTimestampOffset);
    const uint64_t oldTs = DecodeVarint(data, kTimestampOffset, tsEnd);
    const std::vector<uint8_t> tsBytes = EncodeVarint(timestampSec > oldTs ? timestampSec : oldTs + 1);

    std::vector<uint8_t> out;
    out.reserve(data.size() + 4);
    out.insert(out.end(), data.begin(), data.begin() + kTimestampOffset);
    out.insert(out.end(), tsBytes.begin(), tsBytes.end());
    out.insert(out.end(), data.begin() + static_cast<ptrdiff_t>(tsEnd), data.begin() + static_cast<ptrdiff_t>(tsEnd + 8));

    const size_t lenIdx = kTimestampOffset + tsBytes.size() + 3;
    if (*wasOn == on) {
        out.insert(out.end(), data.begin() + static_cast<ptrdiff_t>(tsEnd + 8), data.end());
    } else if (on) {
        out[lenIdx] = static_cast<uint8_t>(out[lenIdx] + 2);
        out.push_back(0x10);
        out.push_back(0x00);
        out.insert(out.end(), data.begin() + static_cast<ptrdiff_t>(tsEnd + 8), data.end());
    } else {
        out[lenIdx] = static_cast<uint8_t>(out[lenIdx] - 2);
        out.insert(out.end(), data.begin() + static_cast<ptrdiff_t>(tsEnd + 10), data.end());
    }
    return out;
}

std::optional<bool> ReadNightLight()
{
    const auto data = ReadNightLightRaw();
    if (!data.has_value())
        return std::nullopt;
    return IsNightLightOn(*data);
}

bool SetNightLight(bool on)
{
    const auto data = ReadNightLightRaw();
    if (!data.has_value())
        return false;
    const auto nowSec = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const std::vector<uint8_t> next = ToggleNightLightData(*data, on, nowSec);
    if (next.empty())
        return false;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kNightLightKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    const LSTATUS rc = RegSetValueExW(key, L"Data", 0, REG_BINARY, next.data(), static_cast<DWORD>(next.size()));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

std::optional<bool> ReadEnergySaver()
{
    SYSTEM_POWER_STATUS sps{};
    if (!GetSystemPowerStatus(&sps))
        return std::nullopt;
    return (sps.SystemStatusFlag & 1u) != 0;
}

bool SetEnergySaver(bool on)
{
    std::scoped_lock lock(g_energyMutex);
    GUID* active = nullptr;
    if (PowerGetActiveScheme(nullptr, &active) != ERROR_SUCCESS || !active)
        return false;

    bool ok = false;
    DWORD target = kDefaultBatteryThreshold;
    if (on) {
        DWORD cur = 0;
        if (PowerReadDCValueIndex(nullptr, active, &GUID_ENERGY_SAVER_SUBGROUP, &GUID_ENERGY_SAVER_BATTERY_THRESHOLD, &cur) == ERROR_SUCCESS &&
            !g_prevDcThreshold.has_value() && cur < 100) {
            g_prevDcThreshold = cur;
        }
        target = 100;
    } else {
        target = g_prevDcThreshold.value_or(kDefaultBatteryThreshold);
        g_prevDcThreshold.reset();
    }

    if (PowerWriteDCValueIndex(nullptr, active, &GUID_ENERGY_SAVER_SUBGROUP, &GUID_ENERGY_SAVER_BATTERY_THRESHOLD, target) == ERROR_SUCCESS)
        ok = PowerSetActiveScheme(nullptr, active) == ERROR_SUCCESS;
    LocalFree(active);
    return ok;
}

std::optional<bool> ReadDarkMode()
{
    const auto apps = ReadRegDword(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme");
    const auto system = ReadRegDword(HKEY_CURRENT_USER, kPersonalizeKey, L"SystemUsesLightTheme");
    if (!apps.has_value() || !system.has_value())
        return std::nullopt;
    return *apps == 0 && *system == 0;
}

bool SetDarkMode(bool on)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kPersonalizeKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    const DWORD val = on ? 0 : 1;
    const LSTATUS r1 = RegSetValueExW(key, L"AppsUseLightTheme", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
    const LSTATUS r2 = RegSetValueExW(key, L"SystemUsesLightTheme", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
    RegCloseKey(key);
    if (r1 != ERROR_SUCCESS || r2 != ERROR_SUCCESS)
        return false;

    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet"), SMTO_ABORTIFHUNG, 5000, nullptr);
    return true;
}

std::optional<bool> ReadMobileHotspot()
{
    const auto mgr = OpenTetheringManager();
    if (!mgr)
        return std::nullopt;
    TetheringOperationalState state = TetheringOperationalState_Unknown;
    if (FAILED(mgr->get_TetheringOperationalState(&state)))
        return std::nullopt;
    return state == TetheringOperationalState_On;
}

bool SetMobileHotspot(bool on)
{
    const auto mgr = OpenTetheringManager();
    if (!mgr)
        return false;
    ComPtr<IAsyncOperation<NetworkOperatorTetheringOperationResult*>> op;
    const HRESULT hr = on ? mgr->StartTetheringAsync(&op) : mgr->StopTetheringAsync(&op);
    if (FAILED(hr))
        return false;
    return AwaitTetheringOp(op.Get());
}

} // namespace atrium
