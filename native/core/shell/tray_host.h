// The tray host (setting `trayHost`): Atrium receives the apps' notification-area
// icons itself, as the shell does, instead of reading Explorer's tray.
//
// Shell_NotifyIcon sends its data to the first window of class Shell_TrayWnd
// (WM_COPYDATA). Atrium keeps a hidden window of that class above Explorer's, takes
// a copy of every add, change and removal (the live picture, the callback message,
// the protocol version) and passes each one on to Explorer unchanged, so the
// system tray stays complete and takes over again the moment Atrium is gone.
// Every other message to the window is forwarded to Explorer's taskbar as well.
// Apps that were already running add their icons again on TaskbarCreated, which
// the host broadcasts when it starts. Clicks are the callback messages the shell
// itself sends (NOTIFYICON_VERSION 0..4). Nothing is injected anywhere; the window
// runs on its own thread so that a busy UI thread never holds up an app's call.
// The pattern is ManagedShell's TrayService (Cairo, RetroBar).
#pragma once

#include "shell/task_model.h"
#include "shell/tray_click.h"

#include <windows.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

struct HostIcon {
    std::wstring key;  // "guid:{...}" or "hwnd:<hwnd>:<uid>"
    HWND hwnd = nullptr;
    UINT uid = 0;
    UINT callback = 0; // 0: the app takes no clicks
    UINT version = 0;  // NOTIFYICON_VERSION; 0 for the oldest protocol
    std::wstring tip;
    std::wstring exe;  // lower-case full path of the window's process
    DWORD pid = 0;
    std::shared_ptr<const IconImage> image;
    uint64_t imageVersion = 0;
    bool hidden = false; // NIS_HIDDEN
    uint64_t added = 0;  // order of arrival
};

class TrayHost {
public:
    // Posts `msg` (wParam 0) to `notify` when the icons change.
    TrayHost(HWND notify, UINT msg);
    ~TrayHost();
    TrayHost(const TrayHost&) = delete;
    TrayHost& operator=(const TrayHost&) = delete;

    bool Running() const { return window_ != nullptr; }
    std::vector<HostIcon> Icons() const;
    // The shell's mouse protocol for `key` at the screen point `at` (the panel's
    // pointer). Left, Right, Double (a whole double click) or DoubleContinuation.
    bool Click(const std::wstring& key, tray::Action action, POINT at);
    // Where icons are, for apps that ask (Shell_NotifyIconGetRect): `clicked` for the
    // icon last clicked, `anchor` (the bar's tray entry) for the others.
    void SetPlaces(RECT anchor);

private:
    void Run(HANDLE ready);
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Message(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT OnNotifyIcon(HWND h, WPARAM w, const COPYDATASTRUCT* cds);
    LRESULT OnIconRect(HWND h, WPARAM w, const COPYDATASTRUCT* cds);
    LRESULT Forward(UINT m, WPARAM w, LPARAM l);
    void KeepFirst();
    void Prune();

    HWND notify_;
    UINT msg_;
    UINT taskbarCreated_ = 0;
    std::atomic<HWND> window_{nullptr};
    std::atomic_bool dirty_ = false; // changed since the last notice (posted at most every 100 ms)
    std::thread thread_;
    DWORD threadId_ = 0;
    mutable std::mutex mutex_;
    std::map<std::wstring, HostIcon> icons_;
    uint64_t nextAdded_ = 0, nextImage_ = 0;
    RECT anchor_{}, clicked_{};
    std::wstring clickedKey_;
    ULONGLONG lostAt_ = 0; // when Explorer's taskbar last came before the host
};

} // namespace atrium
