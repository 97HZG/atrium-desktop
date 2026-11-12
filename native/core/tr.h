#pragma once
#include <windows.h>

namespace atrium {
enum class Text { TrayManualTitle, TrayManualBody, TrayUnavailableTitle, TrayUnavailableBody };
// The native shell currently has no language override. Follow Windows UI language;
// use English for languages without a translation.
inline const wchar_t* Tr(Text text)
{
    const bool zh = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
    switch (text) {
    case Text::TrayManualTitle: return zh ? L"请在系统托盘中操作" : L"Use the Windows notification area";
    case Text::TrayManualBody: return zh ? L"请直接点击 Windows 隐藏图标区中的图标。" : L"Select the icon in the Windows hidden icons panel.";
    case Text::TrayUnavailableTitle: return zh ? L"系统托盘暂时无法打开" : L"Windows notification area is unavailable";
    case Text::TrayUnavailableBody: return zh ? L"请临时关闭“隐藏系统任务栏”，打开隐藏图标区后重试。" : L"Temporarily turn off Hide system taskbar, open the hidden icons panel, and try again.";
    }
    return L"";
}
}
