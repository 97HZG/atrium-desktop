// Battery, volume and network state for the bar's status icon, the control
// centre and the start menu, through public APIs only (GetSystemPowerStatus,
// Core Audio, Network List Manager, WLAN), and the power actions.
#pragma once

#include "status.h"

#include <windows.h>

#include <string>
#include <thread>

namespace atrium {

SystemStatus QuerySystemStatus();
// One background reader shared by the bar and its panels. A refresh never
// waits on Core Audio, Network List Manager or WLAN on the UI thread.
class SystemStatusReader {
public:
    SystemStatusReader(HWND notify, UINT readyMessage, UINT volumeMessage);
    ~SystemStatusReader();
    SystemStatusReader(const SystemStatusReader&) = delete;
    SystemStatusReader& operator=(const SystemStatusReader&) = delete;
    void Refresh();

private:
    void Run();
    HWND notify_;
    UINT readyMessage_, volumeMessage_;
    HANDLE quit_, wake_;
    std::thread thread_;
};
// Changes the default playback volume by `delta` percent points; returns the new level.
int AdjustVolume(int delta);
void SetVolume(int percent); // 0..100; unmutes above 0
bool ToggleMute();
// Posts `msg` to `hwnd` whenever the default playback device's volume or mute
// changes (volume keys, other programs). Call again after the default device
// changed: it follows the current one. Call on an STA/MTA thread with COM.
void WatchVolume(HWND hwnd, UINT msg);

enum class PowerAction { Sleep, Hibernate, Restart, Shutdown };
bool CanHibernate();
bool RunPowerAction(PowerAction action);

} // namespace atrium
