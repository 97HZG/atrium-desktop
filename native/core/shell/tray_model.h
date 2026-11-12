// Tray snapshots and actions; evidence contracts: docs/TRAY.md.
#pragma once

#include "shell/task_model.h"
#include "shell/tray_click.h"
#include "shell/tray_target.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

struct TrayIcon {
    std::wstring tip;      // the tooltip; may span lines
    std::wstring exe;      // the program it belongs to, when recognised
    std::shared_ptr<const IconImage> image; // null when unknown
    // A one-shade glyph (white or black, as many tray icons are drawn for the taskbar's theme)
    // redrawn in the other ink: for bright glass (`inkedOnLight`) or dark. Null for coloured icons.
    std::shared_ptr<const IconImage> inked;
    bool inkedOnLight = false;
    bool promoted = false; // on the system taskbar rather than in its overflow
    int index = 0;         // position among the icons of the same place
    std::vector<int> runtimeId; // UIA identity, never an ordinal click target
    tray::ShellSession shell; // PID + process creation time + taskbar HWND
    std::wstring matchMethod;
    bool fromRecord = false;
    std::wstring recordId; // stable Windows identity, also before the overflow UI exists
    tray::TargetKey Target() const { return {shell, runtimeId, recordId, exe, tip}; }
};

class TrayModel {
public:
    // Posts msg: 0=list changed, 1=observed app response, 2=verified native popup,
    // 3=needs user intervention, 4=accepted/unconfirmed (quiet), 5=explicit popup opened.
    // No HRESULT is treated as an application acknowledgement.
    TrayModel(HWND notify, UINT msg, std::function<std::vector<PinnedApp>()> appNames = {});
    ~TrayModel();
    TrayModel(const TrayModel&) = delete;
    TrayModel& operator=(const TrayModel&) = delete;

    void Refresh(); // asynchronous
    std::vector<TrayIcon> Icons() const;
    bool Loaded() const;
    bool Available() const; // the shell's tray was found in the last read
    // A left click (Invoke) or a right click (the context menu) on `icon`.
    void Click(const TrayIcon& icon, bool right);
    void DoubleClick(const TrayIcon& icon, bool continuation = false);
    void OpenSystemTray(); // explicit asynchronous fallback
    // Opens an existing background session; false when the app is not in the tray/running.
    bool OpenExisting(const std::wstring& exe);

    // First line of a tooltip, for display.
    static std::wstring Title(const std::wstring& tip);

private:
    struct Job {
        bool refresh = false;
        TrayIcon icon;
        tray::Action action = tray::Action::Left;
        bool systemTray = false;
    };
    void Worker();
    HWND notify_;
    UINT msg_;
    std::function<std::vector<PinnedApp>()> appNames_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::vector<TrayIcon> icons_;
    bool loaded_ = false, available_ = false, stop_ = false;
    std::thread thread_;
};

} // namespace atrium
