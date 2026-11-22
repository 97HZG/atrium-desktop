#include "settings.h"

#include "localization_format.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

void ReadBarSettingsJson(const json::JsonObject& o, Settings& s)
{
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
}

void ReadModuleSettingsJson(const json::JsonObject& o, Settings& s)
{
    auto flag = [&](const wchar_t* k, bool& v) {
        if (o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Boolean)
            v = o.GetNamedBoolean(k);
    };
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
}

void ReadPinsJson(const json::JsonObject& o, Settings& s)
{
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
    ReadBarSettingsJson(o, s);
    ReadModuleSettingsJson(o, s);
    auto num = [&](const wchar_t* k, double& v) {
        if (o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Number)
            v = o.GetNamedNumber(k);
    };
    auto flag = [&](const wchar_t* k, bool& v) {
        if (o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Boolean)
            v = o.GetNamedBoolean(k);
    };
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
    ReadPinsJson(o, s);
    s.Clamp();
    out = s;
    return true;
}

} // namespace atrium
