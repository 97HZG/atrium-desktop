#include "settings.h"

#include "localization.h"
#include "localization_format.h"
#include "log.h"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <tuple>
#include <vector>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

bool ReadRawFile(const std::wstring& path, std::string& bytes, bool& exists)
{
    bytes.clear();
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        exists = err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND;
        return !exists;
    }
    exists = true;
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return false;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    bytes = ss.str();
    return true;
}

std::wstring Utf8ToWideText(std::string bytes)
{
    if (bytes.size() >= 3 && bytes.starts_with("\xEF\xBB\xBF"))
        bytes.erase(0, 3); // UTF-8 BOM, e.g. saved by Notepad or Windows PowerShell
    if (bytes.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), w.data(), n);
    return w;
}

void InspectSettingsFile(const std::wstring& path, SettingsFileState& state, double& fileVersion, Settings& parsed, bool& readError)
{
    state = SettingsFileState::Missing;
    fileVersion = 0;
    parsed = Settings{};
    readError = false;
    std::string bytes;
    bool exists = false;
    if (!ReadRawFile(path, bytes, exists)) {
        readError = true;
        state = SettingsFileState::Corrupt;
        return;
    }
    if (!exists)
        return;
    if (bytes.size() >= 3 && bytes.starts_with("\xEF\xBB\xBF"))
        bytes.erase(0, 3);
    if (bytes.empty())
        return;
    const std::wstring text = Utf8ToWideText(std::move(bytes));
    json::JsonObject o{nullptr};
    if (text.empty() || !json::JsonObject::TryParse(text, o)) {
        state = SettingsFileState::Corrupt;
        return;
    }
    if (o.HasKey(L"version") && o.GetNamedValue(L"version").ValueType() == json::JsonValueType::Number)
        fileVersion = o.GetNamedNumber(L"version");
    Settings::FromJson(text, Settings{}, parsed);
    state = fileVersion > Settings::kVersion ? SettingsFileState::FutureVersion : SettingsFileState::Valid;
}

bool ParseBackupSuffix(std::wstring_view middle, std::wstring& stamp, int& seq)
{
    if (middle.size() < 15 || middle[8] != L'-')
        return false;
    for (size_t i = 0; i < 15; ++i) {
        if (i == 8)
            continue;
        if (middle[i] < L'0' || middle[i] > L'9')
            return false;
    }
    stamp.assign(middle.substr(0, 15));
    seq = 0;
    if (middle.size() == 15)
        return true;
    if (middle[15] != L'.' || middle.size() == 16)
        return false;
    int value = 0;
    for (size_t i = 16; i < middle.size(); ++i) {
        if (middle[i] < L'0' || middle[i] > L'9' || value > 100000)
            return false;
        value = value * 10 + (middle[i] - L'0');
    }
    seq = value;
    return true;
}

struct CorruptBackupEntry {
    std::wstring stamp;
    int seq = 0;
    std::filesystem::path path;
};

std::vector<CorruptBackupEntry> FindCorruptBackups(const std::filesystem::path& dir, const std::wstring& stem, const std::wstring& ext)
{
    const std::wstring prefix = stem + L".corrupt.";
    std::vector<CorruptBackupEntry> entries;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
        if (ec || !item.is_regular_file(ec))
            continue;
        const std::wstring name = item.path().filename().wstring();
        if (name.size() <= prefix.size() + ext.size())
            continue;
        if (_wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) != 0)
            continue;
        if (_wcsicmp(name.c_str() + name.size() - ext.size(), ext.c_str()) != 0)
            continue;
        std::wstring stamp;
        int seq = 0;
        const std::wstring_view mid(name.data() + prefix.size(), name.size() - prefix.size() - ext.size());
        if (ParseBackupSuffix(mid, stamp, seq))
            entries.push_back({std::move(stamp), seq, item.path()});
    }
    std::ranges::sort(entries, [](const CorruptBackupEntry& a, const CorruptBackupEntry& b) { return std::tie(a.stamp, a.seq) < std::tie(b.stamp, b.seq); });
    return entries;
}

bool FilesHaveSameBytes(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code ec;
    const auto sizeA = std::filesystem::file_size(a, ec);
    if (ec)
        return false;
    const auto sizeB = std::filesystem::file_size(b, ec);
    if (ec || sizeA != sizeB)
        return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb)
        return false;
    std::array<char, 4096> bufA{}, bufB{};
    while (fa && fb) {
        fa.read(bufA.data(), std::streamsize(bufA.size()));
        fb.read(bufB.data(), std::streamsize(bufB.size()));
        const auto nA = fa.gcount(), nB = fb.gcount();
        if (nA != nB || std::memcmp(bufA.data(), bufB.data(), size_t(nA)) != 0)
            return false;
    }
    return fa.eof() && fb.eof();
}

void PruneCorruptBackups(const std::filesystem::path& dir, const std::wstring& stem, const std::wstring& ext, int maxBackups)
{
    if (maxBackups < 1)
        return;
    const auto entries = FindCorruptBackups(dir, stem, ext);
    if (entries.size() <= size_t(maxBackups))
        return;
    const size_t removeCount = entries.size() - size_t(maxBackups);
    for (size_t i = 0; i < removeCount; ++i)
        DeleteFileW(entries[i].path.c_str());
}

std::wstring FormatSystemError(DWORD code)
{
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring reason = text ? text : L"";
    LocalFree(text);
    while (!reason.empty() && (reason.back() == L'\n' || reason.back() == L'\r' || reason.back() == L' '))
        reason.pop_back();
    return reason;
}

} // namespace

void Settings::Clamp()
{
    auto c = [](double& v, double lo, double hi, double fallback) {
        v = v != v ? fallback : std::clamp(v, lo, hi); // NaN -> fallback
    };
    Settings d;
    c(transparency, 0, 1, d.transparency);
    c(cornerRadius, 0, 200, d.cornerRadius);
    c(blur, 0, 1, d.blur);
    c(refraction, 0, 60, d.refraction);
    c(edge, 1, 80, d.edge);
    c(dispersion, 0, 0.5, d.dispersion);
    c(highlight, 0, 1, d.highlight);
    c(shadow, 0, 1, d.shadow);
    c(width, 200, 4000, d.width);
    c(height, 32, 160, d.height);
    c(margin, 0, 400, d.margin);
    c(iconSize, 16, 64, d.iconSize);
    c(magnify, 1, 2, d.magnify);
    c(notifyTimeout, 3, 30, d.notifyTimeout);
    c(nearEnter, 2, 160, d.nearEnter);
    c(nearLeave, 2, 200, d.nearLeave);
    c(dockDisplayDelay, 100, 2000, d.dockDisplayDelay);
    tint &= 0xFFFFFF;
    if (!localization::ValidTag(language))
        language = L"auto";
    else if (language == L"zh")
        language = L"zh-Hans";
    if (textColor != L"light" && textColor != L"dark")
        textColor = L"auto";
    if (autoHide != L"never" && autoHide != L"always")
        autoHide = L"fullscreen";
    if (barEdge != L"top" && barEdge != L"left" && barEdge != L"right")
        barEdge = L"bottom";
    if (statusStyle != L"icons")
        statusStyle = L"ring";
    if (startSize != L"small" && startSize != L"medium" && startSize != L"large" && startSize != L"xlarge")
        startSize.clear();
    if (startIcon != L"small" && startIcon != L"medium" && startIcon != L"large" && startIcon != L"xlarge")
        startIcon.clear();
    if (dockDisplayMode != L"follow" && dockDisplayMode != L"fixed" && dockDisplayMode != L"all")
        dockDisplayMode = L"primary";
    if (taskbarApps != L"primaryAndWindow" && taskbarApps != L"window")
        taskbarApps = L"all";
    if (notifyPlacement != L"topLeft" && notifyPlacement != L"topRight" && notifyPlacement != L"bottomLeft" && notifyPlacement != L"bottomRight")
        notifyPlacement = L"dock";
    if (widgetBackdrop != L"wallpaper")
        widgetBackdrop = L"desktop";
    if (widgetMaterial != L"dusk" && widgetMaterial != L"color")
        widgetMaterial = L"glass";
    constexpr std::wstring_view kAsciiWs = L" \t\r\n\v\f";
    for (auto& entry : stageExcluded) {
        const size_t first = entry.find_first_not_of(kAsciiWs);
        if (first == std::wstring::npos) {
            entry.clear();
            continue;
        }
        entry = entry.substr(first, entry.find_last_not_of(kAsciiWs) - first + 1);
    }
    std::erase_if(stageExcluded, [](const std::wstring& entry) { return entry.empty(); });
    if (pins && pins->size() > 64)
        pins->resize(64);
    if (preset != L"liquid" && preset != L"frosted")
        preset = L"custom";
}

std::wstring Settings::ToJson() const
{
    json::JsonObject o;
    auto num = [&](const wchar_t* k, double v) { o.SetNamedValue(k, json::JsonValue::CreateNumberValue(v)); };
    wchar_t tintText[8];
    swprintf_s(tintText, L"#%06X", tint);
    num(L"version", kVersion);
    o.SetNamedValue(L"preset", json::JsonValue::CreateStringValue(preset));
    num(L"transparency", transparency);
    num(L"cornerRadius", cornerRadius);
    num(L"blur", blur);
    num(L"refraction", refraction);
    num(L"edge", edge);
    num(L"dispersion", dispersion);
    num(L"highlight", highlight);
    num(L"shadow", shadow);
    o.SetNamedValue(L"tint", json::JsonValue::CreateStringValue(tintText));
    num(L"width", width);
    num(L"height", height);
    num(L"margin", margin);
    num(L"iconSize", iconSize);
    num(L"magnify", magnify);
    o.SetNamedValue(L"hoverRise", json::JsonValue::CreateBooleanValue(hoverRise));
    o.SetNamedValue(L"riseNear", json::JsonValue::CreateBooleanValue(riseNear));
    o.SetNamedValue(L"magnifyNear", json::JsonValue::CreateBooleanValue(magnifyNear));
    o.SetNamedValue(L"startTray", json::JsonValue::CreateBooleanValue(startTray));
    o.SetNamedValue(L"startStatus", json::JsonValue::CreateBooleanValue(startStatus));
    o.SetNamedValue(L"startCentered", json::JsonValue::CreateBooleanValue(startCentered));
    o.SetNamedValue(L"startWithBar", json::JsonValue::CreateBooleanValue(startWithBar));
    if (!startSize.empty())
        o.SetNamedValue(L"startSize", json::JsonValue::CreateStringValue(startSize));
    if (!startIcon.empty())
        o.SetNamedValue(L"startIcon", json::JsonValue::CreateStringValue(startIcon));
    o.SetNamedValue(L"showClock", json::JsonValue::CreateBooleanValue(showClock));
    o.SetNamedValue(L"showClockSeconds", json::JsonValue::CreateBooleanValue(showClockSeconds));
    o.SetNamedValue(L"showTray", json::JsonValue::CreateBooleanValue(showTray));
    o.SetNamedValue(L"showStatus", json::JsonValue::CreateBooleanValue(showStatus));
    o.SetNamedValue(L"showNotifications", json::JsonValue::CreateBooleanValue(showNotifications));
    o.SetNamedValue(L"showDesktop", json::JsonValue::CreateBooleanValue(showDesktop));
    o.SetNamedValue(L"autoWidth", json::JsonValue::CreateBooleanValue(autoWidth));
    o.SetNamedValue(L"showRunning", json::JsonValue::CreateBooleanValue(showRunning));
    o.SetNamedValue(L"language", json::JsonValue::CreateStringValue(language));
    o.SetNamedValue(L"textColor", json::JsonValue::CreateStringValue(textColor));
    o.SetNamedValue(L"autoHide", json::JsonValue::CreateStringValue(autoHide));
    o.SetNamedValue(L"barEdge", json::JsonValue::CreateStringValue(barEdge));
    o.SetNamedValue(L"statusStyle", json::JsonValue::CreateStringValue(statusStyle));
    o.SetNamedValue(L"dockDisplayMode", json::JsonValue::CreateStringValue(dockDisplayMode));
    num(L"dockDisplayDelay", dockDisplayDelay);
    o.SetNamedValue(L"dockDisplay", json::JsonValue::CreateStringValue(dockDisplay));
    o.SetNamedValue(L"desktopDisplay", json::JsonValue::CreateStringValue(desktopDisplay));
    o.SetNamedValue(L"taskbarApps", json::JsonValue::CreateStringValue(taskbarApps));
    o.SetNamedValue(L"taskbarRightPrimaryOnly", json::JsonValue::CreateBooleanValue(taskbarRightPrimaryOnly));
    o.SetNamedValue(L"windowsContextMenu", json::JsonValue::CreateBooleanValue(windowsContextMenu));
    num(L"notifyTimeout", notifyTimeout);
    num(L"nearEnter", nearEnter);
    num(L"nearLeave", nearLeave);
    o.SetNamedValue(L"notifyPlacement", json::JsonValue::CreateStringValue(notifyPlacement));
    o.SetNamedValue(L"notifyHideBody", json::JsonValue::CreateBooleanValue(notifyHideBody));
    o.SetNamedValue(L"imeStatus", json::JsonValue::CreateBooleanValue(imeStatus));
    o.SetNamedValue(L"notifyListener", json::JsonValue::CreateBooleanValue(notifyListener));
    o.SetNamedValue(L"notifyWeChat", json::JsonValue::CreateBooleanValue(notifyWeChat));
    o.SetNamedValue(L"notifyFeishu", json::JsonValue::CreateBooleanValue(notifyFeishu));
    o.SetNamedValue(L"notifyQQ", json::JsonValue::CreateBooleanValue(notifyQQ));
    o.SetNamedValue(L"notifyWeCom", json::JsonValue::CreateBooleanValue(notifyWeCom));
    o.SetNamedValue(L"notifyDingTalk", json::JsonValue::CreateBooleanValue(notifyDingTalk));
    o.SetNamedValue(L"attentionLight", json::JsonValue::CreateBooleanValue(attentionLight));
    o.SetNamedValue(L"attentionBounce", json::JsonValue::CreateBooleanValue(attentionBounce));
    o.SetNamedValue(L"bar", json::JsonValue::CreateBooleanValue(bar));
    o.SetNamedValue(L"widgets", json::JsonValue::CreateBooleanValue(widgets));
    o.SetNamedValue(L"widgetsOnRight", json::JsonValue::CreateBooleanValue(widgetsOnRight));
    o.SetNamedValue(L"widgetBackdrop", json::JsonValue::CreateStringValue(widgetBackdrop));
    o.SetNamedValue(L"widgetMaterial", json::JsonValue::CreateStringValue(widgetMaterial));
    o.SetNamedValue(L"stageManager", json::JsonValue::CreateBooleanValue(stageManager));
    o.SetNamedValue(L"stageOnRight", json::JsonValue::CreateBooleanValue(stageOnRight));
    o.SetNamedValue(L"stageAutoHide", json::JsonValue::CreateBooleanValue(stageAutoHide));
    o.SetNamedValue(L"stageMinimize", json::JsonValue::CreateBooleanValue(stageMinimize));
    {
        json::JsonArray a;
        for (const std::wstring& app : stageExcluded)
            a.Append(json::JsonValue::CreateStringValue(app));
        o.SetNamedValue(L"stageExcluded", a);
    }
    o.SetNamedValue(L"hideTaskbar", json::JsonValue::CreateBooleanValue(hideTaskbar));
    o.SetNamedValue(L"hideBanners", json::JsonValue::CreateBooleanValue(hideBanners));
    o.SetNamedValue(L"trayHost", json::JsonValue::CreateBooleanValue(trayHost));
    o.SetNamedValue(L"remoteVisible", json::JsonValue::CreateBooleanValue(remoteVisible));
    o.SetNamedValue(L"winKeyStart", json::JsonValue::CreateBooleanValue(winKeyStart));
    o.SetNamedValue(L"winNumbers", json::JsonValue::CreateBooleanValue(winNumbers));
    o.SetNamedValue(L"startOnLogin", json::JsonValue::CreateBooleanValue(startOnLogin));
    if (pins) {
        json::JsonArray a;
        for (const auto& p : *pins) {
            json::JsonObject e;
            e.SetNamedValue(L"name", json::JsonValue::CreateStringValue(p.name));
            e.SetNamedValue(L"path", json::JsonValue::CreateStringValue(p.path));
            e.SetNamedValue(L"args", json::JsonValue::CreateStringValue(p.args));
            e.SetNamedValue(L"aumid", json::JsonValue::CreateStringValue(p.aumid));
            if (p.multipleWindows)
                e.SetNamedValue(L"multipleWindows", json::JsonValue::CreateBooleanValue(*p.multipleWindows));
            a.Append(e);
        }
        o.SetNamedValue(L"pins", a);
    }
    return std::wstring(o.Stringify());
}

bool Settings::FromJson(const std::wstring& text, const Settings& base, Settings& out)
{
    json::JsonObject o{nullptr};
    if (!json::JsonObject::TryParse(text, o))
        return false;
    Settings s = base;
    auto num = [&](const wchar_t* k, double& v) {
        if (o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Number)
            v = o.GetNamedNumber(k);
    };
    if (o.HasKey(L"preset") && o.GetNamedValue(L"preset").ValueType() == json::JsonValueType::String)
        s.preset = std::wstring(o.GetNamedString(L"preset"));
    num(L"transparency", s.transparency);
    num(L"cornerRadius", s.cornerRadius);
    num(L"blur", s.blur);
    num(L"refraction", s.refraction);
    num(L"edge", s.edge);
    num(L"dispersion", s.dispersion);
    num(L"highlight", s.highlight);
    num(L"shadow", s.shadow);
    num(L"width", s.width);
    num(L"height", s.height);
    num(L"margin", s.margin);
    if (o.HasKey(L"tint") && o.GetNamedValue(L"tint").ValueType() == json::JsonValueType::String) {
        std::wstring t(o.GetNamedString(L"tint"));
        auto hex = [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'); };
        if (t.size() == 7 && t[0] == L'#' && std::all_of(t.begin() + 1, t.end(), hex))
            s.tint = static_cast<uint32_t>(wcstoul(t.c_str() + 1, nullptr, 16));
    }
    num(L"iconSize", s.iconSize);
    num(L"magnify", s.magnify);
    auto flag = [&](const wchar_t* k, bool& v) {
        if (o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Boolean)
            v = o.GetNamedBoolean(k);
    };
    flag(L"hoverRise", s.hoverRise);
    flag(L"riseNear", s.riseNear);
    flag(L"magnifyNear", s.magnifyNear);
    flag(L"startTray", s.startTray);
    flag(L"startStatus", s.startStatus);
    flag(L"startCentered", s.startCentered);
    flag(L"startWithBar", s.startWithBar);
    if (o.HasKey(L"startSize") && o.GetNamedValue(L"startSize").ValueType() == json::JsonValueType::String)
        s.startSize = std::wstring(o.GetNamedString(L"startSize"));
    if (o.HasKey(L"startIcon") && o.GetNamedValue(L"startIcon").ValueType() == json::JsonValueType::String)
        s.startIcon = std::wstring(o.GetNamedString(L"startIcon"));
    flag(L"showClock", s.showClock);
    flag(L"showClockSeconds", s.showClockSeconds);
    flag(L"showTray", s.showTray);
    flag(L"showStatus", s.showStatus);
    flag(L"showNotifications", s.showNotifications);
    flag(L"showDesktop", s.showDesktop);
    flag(L"autoWidth", s.autoWidth);
    flag(L"showRunning", s.showRunning);
    num(L"notifyTimeout", s.notifyTimeout);
    num(L"nearEnter", s.nearEnter);
    num(L"nearLeave", s.nearLeave);
    flag(L"notifyHideBody", s.notifyHideBody);
    flag(L"imeStatus", s.imeStatus);
    // Version 1 saved the old default (off) along with everything else; from
    // version 2 the notifications are on unless turned off since.
    bool v1 = o.HasKey(L"version") && o.GetNamedValue(L"version").ValueType() == json::JsonValueType::Number && o.GetNamedNumber(L"version") < 2;
    if (!v1)
        flag(L"notifyListener", s.notifyListener);
    flag(L"notifyWeChat", s.notifyWeChat);
    flag(L"notifyFeishu", s.notifyFeishu);
    flag(L"notifyQQ", s.notifyQQ);
    flag(L"notifyWeCom", s.notifyWeCom);
    flag(L"notifyDingTalk", s.notifyDingTalk);
    flag(L"attentionLight", s.attentionLight);
    flag(L"attentionBounce", s.attentionBounce);
    flag(L"bar", s.bar);
    flag(L"widgets", s.widgets);
    flag(L"widgetsOnRight", s.widgetsOnRight);
    flag(L"stageManager", s.stageManager);
    flag(L"stageOnRight", s.stageOnRight);
    flag(L"stageAutoHide", s.stageAutoHide);
    flag(L"stageMinimize", s.stageMinimize);
    flag(L"hideTaskbar", s.hideTaskbar);
    flag(L"hideBanners", s.hideBanners);
    flag(L"trayHost", s.trayHost);
    if (o.HasKey(L"stageExcluded") && o.GetNamedValue(L"stageExcluded").ValueType() == json::JsonValueType::Array) {
        std::vector<std::wstring> apps;
        for (const auto& v : o.GetNamedArray(L"stageExcluded"))
            if (v.ValueType() == json::JsonValueType::String)
                apps.emplace_back(v.GetString());
        s.stageExcluded = std::move(apps);
    }
    flag(L"remoteVisible", s.remoteVisible);
    flag(L"winKeyStart", s.winKeyStart);
    flag(L"winNumbers", s.winNumbers);
    flag(L"startOnLogin", s.startOnLogin);
    if (o.HasKey(L"widgetBackdrop") && o.GetNamedValue(L"widgetBackdrop").ValueType() == json::JsonValueType::String)
        s.widgetBackdrop = std::wstring(o.GetNamedString(L"widgetBackdrop"));
    if (o.HasKey(L"widgetMaterial") && o.GetNamedValue(L"widgetMaterial").ValueType() == json::JsonValueType::String)
        s.widgetMaterial = std::wstring(o.GetNamedString(L"widgetMaterial"));
    if (o.HasKey(L"language") && o.GetNamedValue(L"language").ValueType() == json::JsonValueType::String)
        s.language = std::wstring(o.GetNamedString(L"language"));
    if (o.HasKey(L"textColor") && o.GetNamedValue(L"textColor").ValueType() == json::JsonValueType::String)
        s.textColor = std::wstring(o.GetNamedString(L"textColor"));
    if (o.HasKey(L"autoHide") && o.GetNamedValue(L"autoHide").ValueType() == json::JsonValueType::String)
        s.autoHide = std::wstring(o.GetNamedString(L"autoHide"));
    if (o.HasKey(L"barEdge") && o.GetNamedValue(L"barEdge").ValueType() == json::JsonValueType::String)
        s.barEdge = std::wstring(o.GetNamedString(L"barEdge"));
    if (o.HasKey(L"statusStyle") && o.GetNamedValue(L"statusStyle").ValueType() == json::JsonValueType::String)
        s.statusStyle = std::wstring(o.GetNamedString(L"statusStyle"));
    auto display = [&](const wchar_t* key, std::wstring& value) {
        if (o.HasKey(key) && o.GetNamedValue(key).ValueType() == json::JsonValueType::String)
            value = std::wstring(o.GetNamedString(key));
    };
    display(L"dockDisplayMode", s.dockDisplayMode);
    num(L"dockDisplayDelay", s.dockDisplayDelay);
    display(L"dockDisplay", s.dockDisplay);
    display(L"desktopDisplay", s.desktopDisplay);
    display(L"taskbarApps", s.taskbarApps);
    flag(L"taskbarRightPrimaryOnly", s.taskbarRightPrimaryOnly);
    flag(L"windowsContextMenu", s.windowsContextMenu);
    display(L"notifyPlacement", s.notifyPlacement);
    if (o.HasKey(L"pins") && o.GetNamedValue(L"pins").ValueType() == json::JsonValueType::Array) {
        std::vector<PinnedApp> pins;
        for (const auto& v : o.GetNamedArray(L"pins")) {
            if (v.ValueType() != json::JsonValueType::Object)
                continue;
            auto e = v.GetObject();
            auto str = [&](const wchar_t* k) {
                return e.HasKey(k) && e.GetNamedValue(k).ValueType() == json::JsonValueType::String ? std::wstring(e.GetNamedString(k)) : std::wstring();
            };
            PinnedApp p{str(L"name"), str(L"path"), str(L"args"), str(L"aumid")};
            if (e.HasKey(L"multipleWindows") && e.GetNamedValue(L"multipleWindows").ValueType() == json::JsonValueType::Boolean)
                p.multipleWindows = e.GetNamedBoolean(L"multipleWindows");
            if (!p.path.empty() || !p.aumid.empty())
                pins.push_back(std::move(p));
        }
        s.pins = std::move(pins);
    }
    s.Clamp();
    out = s;
    return true;
}

std::wstring SettingsPath()
{
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)))
        dir = std::wstring(base) + L"\\Atrium";
    CoTaskMemFree(base);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.json";
}

std::wstring ReadTextFile(const std::wstring& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return Utf8ToWideText(ss.str());
}

bool WriteTextFile(const std::wstring& path, const std::wstring& text)
{
    static std::atomic<uint64_t> seq{0};
    const std::wstring tmp =
        path + L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(seq.fetch_add(1, std::memory_order_relaxed)) + L".tmp";
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), bytes.data(), n, nullptr, nullptr);
    // Win32 rather than a stream: a failure leaves its reason in GetLastError.
    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(file);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        SetLastError(error != ERROR_SUCCESS ? error : ERROR_WRITE_FAULT);
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD moveError = GetLastError();
        DeleteFileW(tmp.c_str());
        SetLastError(moveError);
        return false;
    }
    return true;
}

bool BackupCorruptSettings(const std::wstring& path, int maxBackups)
{
    const std::filesystem::path filePath(path);
    const std::filesystem::path dir = filePath.has_parent_path() ? filePath.parent_path() : std::filesystem::path(L".");
    std::wstring stem = filePath.stem().wstring();
    if (stem.empty())
        stem = L"settings";
    std::wstring ext = filePath.extension().wstring();
    if (ext.empty())
        ext = L".json";
    const auto existing = FindCorruptBackups(dir, stem, ext);
    if (!existing.empty() && FilesHaveSameBytes(filePath, existing.back().path))
        return true;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stamp[32]{};
    swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    for (int i = 0; i < 100; ++i) {
        const std::wstring suffix = i == 0 ? L"" : L"." + std::to_wstring(i);
        const std::filesystem::path candidate = dir / (stem + L".corrupt." + stamp + suffix + ext);
        if (CopyFileW(path.c_str(), candidate.c_str(), TRUE)) {
            PruneCorruptBackups(dir, stem, ext, maxBackups);
            return true;
        }
        const DWORD err = GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS)
            return false;
    }
    return false;
}

Settings LoadSettingsFrom(const std::wstring& path, SettingsFileState* stateOut, bool* backupOkOut)
{
    SettingsFileState state = SettingsFileState::Missing;
    double fileVersion = 0;
    Settings s;
    bool readError = false;
    InspectSettingsFile(path, state, fileVersion, s, readError);
    bool backupOk = false;
    if (state == SettingsFileState::FutureVersion) {
        Log(L"settings: %s uses newer version %.0f (supported %d); reading compatible fields read-only", path.c_str(), fileVersion, Settings::kVersion);
    } else if (state == SettingsFileState::Corrupt) {
        backupOk = !readError && BackupCorruptSettings(path, 3);
        Log(L"settings: %s is not valid JSON (%s), using defaults", path.c_str(), backupOk ? L"backed up" : L"backup failed");
        s = Settings{};
    }
    if (stateOut)
        *stateOut = state;
    if (backupOkOut)
        *backupOkOut = backupOk;
    return s;
}

Settings LoadSettings() { return LoadSettingsFrom(SettingsPath()); }

bool IsDevelopmentBuild()
{
#ifdef ATRIUM_DEVELOPMENT
    return true;
#else
    static const bool kDevelopment = [] {
        int count = 0;
        auto* args = CommandLineToArgvW(GetCommandLineW(), &count);
        bool explicitDevelopment = false;
        for (int i = 1; args && i < count; ++i)
            explicitDevelopment |= wcscmp(args[i], L"--development") == 0;
        LocalFree(static_cast<HLOCAL>(args));
        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
        std::wstring path(exe);
        path = path.substr(0, path.find_last_of(L'\\')) + L"\\development.txt";
        return explicitDevelopment || GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }();
    return kDevelopment;
#endif
}

bool LowIntegrity()
{
    static const bool kLow = [] {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return false;
        DWORD size = 0;
        GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
        std::vector<BYTE> buffer(size);
        bool below = false;
        if (size && GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size)) {
            const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
            const DWORD rid = *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1);
            below = rid < SECURITY_MANDATORY_MEDIUM_RID;
        }
        CloseHandle(token);
        return below;
    }();
    return kLow;
}

bool SaveSettingsTo(const std::wstring& path, const Settings& s, std::wstring* error, const std::function<bool(const std::wstring&)>& backupCorrupt,
                    bool* startupChangedOut)
{
    if (error)
        error->clear();
    if (startupChangedOut)
        *startupChangedOut = false;

    SettingsFileState state = SettingsFileState::Missing;
    double fileVersion = 0;
    Settings existing;
    bool readError = false;
    InspectSettingsFile(path, state, fileVersion, existing, readError);
    if (state == SettingsFileState::FutureVersion) {
        Log(L"settings: refused to overwrite %s (version %.0f > %d)", path.c_str(), fileVersion, Settings::kVersion);
        if (error)
            *error = TrFormat(L"Could not save %s: settings file uses a newer format version (v%.0f > v%d). Update Atrium before saving.", path.c_str(),
                              fileVersion, Settings::kVersion);
        return false;
    }
    if (state == SettingsFileState::Corrupt) {
        const bool backedUp = !readError && (backupCorrupt ? backupCorrupt(path) : BackupCorruptSettings(path, 3));
        if (!backedUp) {
            Log(L"settings: could not back up corrupt %s before saving; original kept", path.c_str());
            if (error)
                *error = TrFormat(L"Could not back up corrupt settings file %s before saving; original file was kept.", path.c_str());
            return false;
        }
        existing = Settings{};
    }

    const bool startupChanged = existing.startOnLogin != s.startOnLogin;
    if (!WriteTextFile(path, s.ToJson())) {
        const DWORD code = GetLastError();
        const std::wstring reason = FormatSystemError(code);
        Log(L"settings: could not write %s (%lu)%s", path.c_str(), code, LowIntegrity() ? L", running at low integrity" : L"");
        if (error) {
            *error = TrFormat(L"Could not write %s: %s (error %lu)", path.c_str(), reason.c_str(), code);
            // Below medium integrity (an executable carrying a Low label) nothing in the profile is writable.
            if (LowIntegrity())
                *error += Tr(
                    L"\nAtrium is running at low integrity and cannot write to your profile. Unzip it to an ordinary folder (such as D:\\\\Atrium), make sure Atrium.exe has no Low integrity label, then start it again.");
        }
        return false;
    }
    if (startupChangedOut)
        *startupChangedOut = startupChanged;
    return true;
}

bool SaveSettings(const Settings& s, std::wstring* error, bool* startupChangedOut) { return SaveSettingsTo(SettingsPath(), s, error, {}, startupChangedOut); }

} // namespace atrium
