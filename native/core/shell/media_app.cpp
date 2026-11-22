// The media session's player as an app: its window brought forward, its name as the shell shows it.
#include "shell/media_app.h"

#include "win32_path.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <winrt/base.h>

#include <cwctype>

namespace atrium {

namespace {

bool ProcessMatchesAppId(DWORD pid, const std::wstring& targetFolded, const std::wstring& targetStem, std::wstring* outImagePath = nullptr)
{
    const winrt::handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!process)
        return false;
    const std::wstring aumid = FoldCase(ProcessAumid(process.get()));
    const std::wstring rawPath = ProcessImagePath(process.get());
    const std::wstring path = FoldCase(rawPath);
    const bool matched =
        (!aumid.empty() && aumid == targetFolded) || (!path.empty() && (path == targetFolded || (!targetStem.empty() && ExeStemFolded(path) == targetStem)));
    if (matched && outImagePath)
        *outImagePath = rawPath;
    return matched;
}

std::wstring FindRunningProcessImage(const std::wstring& targetFolded, const std::wstring& targetStem)
{
    const winrt::handle snap{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (!snap)
        return {};
    PROCESSENTRY32W pe{sizeof(pe)};
    std::wstring imagePath;
    for (BOOL ok = Process32FirstW(snap.get(), &pe); ok; ok = Process32NextW(snap.get(), &pe)) {
        if (ProcessMatchesAppId(pe.th32ProcessID, targetFolded, targetStem, &imagePath) && !imagePath.empty())
            return imagePath;
    }
    return {};
}

struct FindWindowCtx {
    std::wstring targetFolded, targetStem;
    HWND found = nullptr;
};

BOOL CALLBACK EnumAppWindowProc(HWND hwnd, LPARAM lParam)
{
    auto* ctx = reinterpret_cast<FindWindowCtx*>(lParam);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) || (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
        return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid && ProcessMatchesAppId(pid, ctx->targetFolded, ctx->targetStem)) {
        ctx->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

} // namespace

void ActivatePlayerApp(const std::wstring& appId)
{
    if (appId.empty())
        return;
    FindWindowCtx ctx{FoldCase(appId), ExeStemFolded(appId), nullptr};
    EnumWindows(EnumAppWindowProc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found) {
        DWORD pid = 0;
        GetWindowThreadProcessId(ctx.found, &pid);
        if (IsIconic(ctx.found))
            ShowWindowAsync(ctx.found, SW_RESTORE);
        if (pid != 0)
            AllowSetForegroundWindow(pid);
        SetForegroundWindow(ctx.found);
        if (GetForegroundWindow() != ctx.found)
            SwitchToThisWindow(ctx.found, TRUE);
        return;
    }
    winrt::com_ptr<IApplicationActivationManager> aam;
    DWORD pid = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(aam.put()))) && aam &&
        SUCCEEDED(aam->ActivateApplication(appId.c_str(), nullptr, AO_NONE, &pid)))
        return;

    std::wstring target = FindRunningProcessImage(ctx.targetFolded, ctx.targetStem);
    if (target.empty())
        target = appId.find(L'!') != std::wstring::npos ? (L"shell:AppsFolder\\" + appId) : appId;

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = target.c_str();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

std::wstring ResolveAppName(const std::wstring& appId, std::unordered_map<std::wstring, std::wstring>& cache)
{
    if (appId.empty())
        return {};
    if (const auto it = cache.find(appId); it != cache.end())
        return it->second;
    std::wstring name;
    winrt::com_ptr<IShellItem> item;
    const std::wstring parsing = L"shell:AppsFolder\\" + appId;
    if (SUCCEEDED(SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(item.put()))) && item) {
        PWSTR raw = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &raw)) && raw) {
            name = raw;
            CoTaskMemFree(raw);
        }
    }
    if (name.empty()) {
        const size_t slash = appId.find_last_of(L"\\/");
        name = slash == std::wstring::npos ? appId : appId.substr(slash + 1);
        if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0)
            name.resize(name.size() - 4);
        if (!name.empty() && std::iswlower(static_cast<wint_t>(name.front())))
            name.front() = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(name.front())));
    }
    cache[appId] = name;
    return name;
}

} // namespace atrium
