#include "shell/app_usage.h"

#include "shell/app_catalog.h"
#include "settings.h"

#include <windows.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

namespace json = winrt::Windows::Data::Json;

namespace atrium {
namespace {

std::mutex g_mu;
std::map<std::wstring, uint64_t> g_counts;
bool g_loaded = false;

std::wstring UsagePath()
{
    std::wstring path = SettingsPath();
    return path.substr(0, path.find_last_of(L'\\') + 1) + L"app-usage.json";
}

std::wstring FoldKey(std::wstring s)
{
    for (wchar_t& c : s)
        c = wchar_t(towlower(c));
    return s;
}

void LoadLocked()
{
    if (g_loaded)
        return;
    g_loaded = true;
    const std::wstring text = ReadTextFile(UsagePath());
    json::JsonObject o{nullptr};
    if (text.empty() || !json::JsonObject::TryParse(text, o))
        return;
    if (!o.HasKey(L"counts") || o.GetNamedValue(L"counts").ValueType() != json::JsonValueType::Object)
        return;
    const json::JsonObject counts = o.GetNamedObject(L"counts");
    for (const auto& kv : counts) {
        if (kv.Value().ValueType() == json::JsonValueType::Number)
            g_counts[FoldKey(std::wstring(kv.Key()))] = uint64_t(std::max(0.0, kv.Value().GetNumber()));
    }
}

void SaveLocked()
{
    json::JsonObject counts;
    for (const auto& [id, n] : g_counts)
        counts.SetNamedValue(id, json::JsonValue::CreateNumberValue(double(n)));
    json::JsonObject o;
    o.SetNamedValue(L"version", json::JsonValue::CreateNumberValue(1));
    o.SetNamedValue(L"counts", counts);
    (void)WriteTextFile(UsagePath(), std::wstring(o.Stringify()));
}

} // namespace

void RecordAppLaunch(const std::wstring& id)
{
    if (id.empty())
        return;
    std::lock_guard lock(g_mu);
    LoadLocked();
    ++g_counts[FoldKey(id)];
    // Debounce disk write — launching must not hitch on JSON flush.
    static ULONGLONG s_lastSave = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_lastSave >= 2500) {
        s_lastSave = now;
        SaveLocked();
    }
}

void FlushAppUsage()
{
    std::lock_guard lock(g_mu);
    if (!g_loaded && g_counts.empty())
        return;
    LoadLocked();
    SaveLocked();
}

uint64_t AppLaunchCount(const std::wstring& id)
{
    std::lock_guard lock(g_mu);
    LoadLocked();
    const auto it = g_counts.find(FoldKey(id));
    return it == g_counts.end() ? 0 : it->second;
}

uint64_t AppSwitchedCount(const CatalogApp& app)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FeatureUsage\\AppSwitched", 0, KEY_READ, &key) !=
        ERROR_SUCCESS)
        return 0;
    auto read = [&](const std::wstring& name) -> uint64_t {
        if (name.empty())
            return 0;
        DWORD type = 0, value = 0, size = sizeof(value);
        if (RegQueryValueExW(key, name.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS && type == REG_DWORD)
            return value;
        return 0;
    };
    uint64_t n = 0;
    if (!app.target.empty())
        n = std::max(n, read(app.target));
    if (!app.aumid.empty())
        n = std::max(n, read(app.aumid));
    if (!app.target.empty()) {
        const size_t slash = app.target.find_last_of(L"\\/");
        const std::wstring file = slash == std::wstring::npos ? app.target : app.target.substr(slash + 1);
        const size_t dot = file.find_last_of(L'.');
        n = std::max(n, read(dot == std::wstring::npos ? file : file.substr(0, dot)));
    }
    RegCloseKey(key);
    return n;
}

void SortAppsByUsage(std::vector<CatalogApp>& apps)
{
    // Score once per app (opening AppSwitched on every comparator call is much slower).
    std::vector<uint64_t> scores(apps.size());
    {
        std::lock_guard lock(g_mu);
        LoadLocked();
        for (size_t i = 0; i < apps.size(); ++i) {
            const auto it = g_counts.find(FoldKey(apps[i].id));
            const uint64_t ours = it == g_counts.end() ? 0 : it->second;
            // Prefer our launches; fall back to Windows AppSwitched so the first runs already rank by habit.
            scores[i] = ours * 1000ull + AppSwitchedCount(apps[i]);
        }
    }
    std::vector<size_t> order(apps.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t ia, size_t ib) {
        if (scores[ia] != scores[ib])
            return scores[ia] > scores[ib];
        const CatalogApp& a = apps[ia];
        const CatalogApp& b = apps[ib];
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a.name.c_str(), int(a.name.size()), b.name.c_str(),
                               int(b.name.size()), nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    std::vector<CatalogApp> sorted;
    sorted.reserve(apps.size());
    for (size_t i : order)
        sorted.push_back(std::move(apps[i]));
    apps = std::move(sorted);
}

} // namespace atrium
