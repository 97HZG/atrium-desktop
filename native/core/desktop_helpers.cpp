// Small desktop helpers the bar's menus and entries use (desktop_helpers.h).
#include "desktop_helpers.h"

#include "shell/jump_list.h"

#include <algorithm>
#include <iterator>
#include <vector>

namespace atrium {

void SendWinShortcut(WORD key, bool ctrl)
{
    std::vector<INPUT> in;
    auto add = [&](WORD vk, bool up) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = vk;
        i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        in.push_back(i);
    };
    add(VK_LWIN, false);
    if (ctrl)
        add(VK_CONTROL, false);
    add(key, false);
    add(key, true);
    if (ctrl)
        add(VK_CONTROL, true);
    add(VK_LWIN, true);
    SendInput(UINT(in.size()), in.data(), sizeof(INPUT));
}

std::wstring ProgramOf(HWND window)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    std::wstring path;
    if (HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr) {
        wchar_t buf[MAX_PATH * 2];
        DWORD len = DWORD(std::size(buf));
        if (QueryFullProcessImageNameW(p, 0, buf, &len))
            path.assign(buf, len);
        CloseHandle(p);
    }
    return path;
}

bool IsDirectoryPath(const std::wstring& path)
{
    if (IsRemotePath(path))
        return false; // an offline share would answer in tens of seconds
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsWorkspaceFile(const std::wstring& path)
{
    const size_t dot = path.find_last_of(L'.');
    const size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        return false;
    const wchar_t* extension = path.c_str() + dot;
    static const wchar_t* const kProjectKinds[] = {L".code-workspace",  L".sln",      L".slnx",        L".vcxproj", L".uvprojx", L".workspace",
                                                   L".sublime-project", L".uproject", L".xcworkspace", L".pro",     L".csproj"};
    return std::ranges::any_of(kProjectKinds, [&](const wchar_t* kind) { return _wcsicmp(extension, kind) == 0; });
}

} // namespace atrium
