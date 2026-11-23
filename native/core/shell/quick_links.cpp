// Windows Win+X quick link menu items and their launch actions (quick_links.h).
#include "shell/quick_links.h"

#include "localization.h"
#include "log.h"
#include "shell/system_status.h"

#include <windows.h>
#include <shellapi.h>

namespace atrium {

namespace {

enum QuickLinkId {
    kInstalledApps = 1,
    kMobilityCenter,
    kPowerOptions,
    kEventViewer,
    kSystem,
    kDeviceManager,
    kNetworkConnections,
    kDiskManagement,
    kComputerManagement,
    kTerminal,
    kTerminalAdmin,
    kTaskManager,
    kSettings,
    kFileExplorer,
    kSearch,
    kRun,
    kSignOut,
    kSleep,
    kHibernate,
    kShutDown,
    kRestart,
    kDesktop,
};

bool HasMobilityCenter()
{
    wchar_t sys[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return false;
    const std::wstring path = std::wstring(sys, n) + L"\\mblctr.exe";
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

const wchar_t* TerminalTarget() { return SearchPathW(nullptr, L"wt.exe", nullptr, 0, nullptr, nullptr) ? L"wt.exe" : L"powershell.exe"; }

void ShellOpen(const wchar_t* target, bool elevated)
{
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = elevated ? L"runas" : nullptr;
    sei.lpFile = target;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        if (!elevated || err != ERROR_CANCELLED)
            Log(L"quick_links: launch %s failed (%lu)", target, err);
    }
}

void SendWinKey(WORD vk)
{
    INPUT in[4]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_LWIN;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = vk;
    in[2].type = INPUT_KEYBOARD;
    in[2].ki.wVk = vk;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD;
    in[3].ki.wVk = VK_LWIN;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

} // namespace

std::vector<QuickLink> QuickLinks()
{
    std::vector<QuickLink> items;
    items.push_back({kInstalledApps, Tr(L"Installed apps")});
    if (HasMobilityCenter())
        items.push_back({kMobilityCenter, Tr(L"Mobility Center")});
    items.push_back({kPowerOptions, Tr(L"Power Options")});
    items.push_back({kEventViewer, Tr(L"Event Viewer")});
    items.push_back({kSystem, Tr(L"System")});
    items.push_back({kDeviceManager, Tr(L"Device Manager")});
    items.push_back({kNetworkConnections, Tr(L"Network Connections")});
    items.push_back({kDiskManagement, Tr(L"Disk Management")});
    items.push_back({kComputerManagement, Tr(L"Computer Management")});
    items.push_back({kTerminal, Tr(L"Terminal")});
    items.push_back({kTerminalAdmin, Tr(L"Terminal (Admin)")});
    items.push_back({});
    items.push_back({kTaskManager, Tr(L"Task Manager")});
    items.push_back({kSettings, Tr(L"Settings")});
    items.push_back({kFileExplorer, Tr(L"File Explorer")});
    items.push_back({kSearch, Tr(L"Search")});
    items.push_back({kRun, Tr(L"Run")});
    items.push_back({});
    items.push_back({0, Tr(L"Shut down or sign out"), true});
    items.push_back({kSignOut, Tr(L"Sign out")});
    items.push_back({kSleep, Tr(L"Sleep")});
    if (CanHibernate())
        items.push_back({kHibernate, Tr(L"Hibernate")});
    items.push_back({kShutDown, Tr(L"Shut down")});
    items.push_back({kRestart, Tr(L"Restart")});
    items.push_back({});
    items.push_back({kDesktop, Tr(L"Desktop")});
    return items;
}

void RunQuickLink(int id)
{
    struct LaunchSpec {
        int id;
        const wchar_t* target;
        bool elevated = false;
    };
    const wchar_t* terminal = TerminalTarget();
    const LaunchSpec launches[] = {
        {kInstalledApps, L"ms-settings:appsfeatures"},
        {kMobilityCenter, L"mblctr.exe"},
        {kPowerOptions, L"ms-settings:powersleep"},
        {kEventViewer, L"eventvwr.msc"},
        {kSystem, L"ms-settings:about"},
        {kDeviceManager, L"devmgmt.msc"},
        {kNetworkConnections, L"ms-settings:network"},
        {kDiskManagement, L"diskmgmt.msc"},
        {kComputerManagement, L"compmgmt.msc"},
        {kTerminal, terminal},
        {kTerminalAdmin, terminal, true},
        {kTaskManager, L"taskmgr.exe"},
        {kSettings, L"ms-settings:"},
        {kFileExplorer, L"explorer.exe"},
        {kRun, L"shell:::{2559a1f3-21d7-11d4-bdaf-00c04f60b9f0}"},
    };
    for (const LaunchSpec& spec : launches) {
        if (spec.id == id) {
            ShellOpen(spec.target, spec.elevated);
            return;
        }
    }
    switch (id) {
    case kSearch: SendWinKey('S'); break;
    case kDesktop: SendWinKey('D'); break;
    case kSignOut: ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_OTHER); break;
    case kSleep: RunPowerAction(PowerAction::Sleep); break;
    case kHibernate: RunPowerAction(PowerAction::Hibernate); break;
    case kShutDown: RunPowerAction(PowerAction::Shutdown); break;
    case kRestart: RunPowerAction(PowerAction::Restart); break;
    default: break;
    }
}

} // namespace atrium
