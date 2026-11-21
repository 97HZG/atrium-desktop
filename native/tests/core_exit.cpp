#include "log.h"
#include "shell/wifi.h"
#include "win32_path.h"

#include <chrono>
#include <iostream>
#include <string>

using namespace atrium;

namespace {

bool Require(bool condition, const char* message)
{
    if (condition)
        return true;
    std::cerr << message << '\n';
    return false;
}

} // namespace

int main()
{
    const std::wstring self = ModulePath();
    if (!Require(!self.empty(), "ModulePath must succeed"))
        return 1;

    const std::wstring className = L"AtriumCoreExitTest_" + std::to_wstring(GetCurrentProcessId());
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = className.c_str();
    if (!Require(RegisterClassExW(&wc) != 0, "RegisterClassExW must succeed"))
        return 1;

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, className.c_str(), L"CoreExitTest", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!Require(hwnd != nullptr, "CreateWindowExW must succeed")) {
        UnregisterClassW(className.c_str(), instance);
        return 1;
    }

    const std::wstring otherPath = L"C:\\NonExistent\\OtherDir\\Atrium.exe";
    const bool ok =
        Require(!SameExecutableWindow(nullptr), "null window must not match") &&
        Require(SameExecutableWindow(hwnd), "window owned by current exe must match ModulePath()") &&
        Require(SameExecutableWindow(hwnd, self), "window owned by current exe must match explicit self path") &&
        Require(!SameExecutableWindow(hwnd, otherPath), "window must not match a different executable path") &&
        Require(FindWindowForExecutable(className.c_str()) == hwnd, "FindWindowForExecutable must find matching window") &&
        Require(FindWindowForExecutable(className.c_str(), otherPath) == nullptr, "FindWindowForExecutable must skip windows belonging to other executables") &&
        Require(FindWindowForExecutable(L"AtriumCoreExitMissingClass") == nullptr, "FindWindowForExecutable must return nullptr when class is absent");

    DestroyWindow(hwnd);
    UnregisterClassW(className.c_str(), instance);
    if (!ok || !Require(FindWindowForExecutable(className.c_str()) == nullptr, "destroyed window must not be found"))
        return 1;

    HANDLE canceled = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!Require(canceled != nullptr, "CreateEventW must succeed"))
        return 1;
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<WifiNetwork> nets = ListWifi(true, canceled);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CloseHandle(canceled);
    if (!Require(nets.empty() && ms < 1000, "ListWifi(true, signaled) must return empty without 3.5s sleep"))
        return 1;

    Log(L"core_exit: shutdown check");
    ShutdownLog();
    ShutdownLog();
    Log(L"core_exit: after shutdown");

    std::cout << "core exit: all checks passed\n";
    return 0;
}
