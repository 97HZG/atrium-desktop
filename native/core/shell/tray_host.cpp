#include "shell/tray_host.h"

#include "log.h"
#include "shell/shell_taskbar.h"

#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <iterator>

namespace atrium {

namespace {

constexpr UINT_PTR kKeepTimer = 1;   // keep the host first among Shell_TrayWnd windows; post changes
constexpr UINT kKeepMs = 100;
constexpr UINT_PTR kPruneTimer = 2;  // icons of windows that are gone without a NIM_DELETE
constexpr UINT kPruneMs = 2000;
constexpr DWORD kSignature = 0x34753423;
constexpr uint64_t kImageBase = 1ull << 62; // apart from TrayModel's own picture versions

// The shell's wire format (WM_COPYDATA dwData 1): handles as 32-bit values, the
// same for 32- and 64-bit apps.
struct NotifyIconData32 {
    DWORD cbSize;
    DWORD hWnd;
    UINT uID;
    UINT uFlags;
    UINT uCallbackMessage;
    DWORD hIcon;
    WCHAR szTip[128];
    DWORD dwState;
    DWORD dwStateMask;
    WCHAR szInfo[256];
    UINT uVersion; // also uTimeout
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID guidItem;
    DWORD hBalloonIcon;
};
struct TrayNotifyData {
    DWORD signature;
    DWORD message;
    NotifyIconData32 nid;
};
// Shell_NotifyIconGetRect (WM_COPYDATA dwData 3).
struct NotifyIconIdentifier32 {
    DWORD signature;
    DWORD message; // 1: the top left corner, 2: the bottom right
    DWORD cbSize;
    DWORD hWnd;
    UINT uID;
    GUID guidItem;
};

HWND Handle(DWORD value) { return reinterpret_cast<HWND>(static_cast<ULONG_PTR>(value)); }

std::wstring KeyOf(HWND hwnd, UINT uid, const GUID* guid)
{
    wchar_t key[64];
    if (guid && *guid != GUID{}) {
        StringFromGUID2(*guid, key, 64);
        return L"guid:" + std::wstring(key);
    }
    swprintf_s(key, L"hwnd:%p:%u", static_cast<void*>(hwnd), uid);
    return key;
}

std::wstring ProcessPath(DWORD pid)
{
    std::wstring out;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH * 2];
        DWORD size = DWORD(std::size(path));
        if (QueryFullProcessImageNameW(p, 0, path, &size))
            out.assign(path, size);
        CloseHandle(p);
    }
    std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return out;
}

DWORD ShellPid()
{
    DWORD pid = 0;
    if (HWND shell = ShellTaskbar())
        GetWindowThreadProcessId(shell, &pid);
    return pid;
}

const wchar_t* Name(DWORD message)
{
    switch (message) {
    case NIM_ADD: return L"add";
    case NIM_MODIFY: return L"modify";
    case NIM_DELETE: return L"delete";
    case NIM_SETFOCUS: return L"focus";
    case NIM_SETVERSION: return L"version";
    default: return L"?";
    }
}

} // namespace

TrayHost::TrayHost(HWND notify, UINT msg) : notify_(notify), msg_(msg)
{
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this, ready] {
        threadId_ = GetCurrentThreadId();
        Run(ready);
    });
    WaitForSingleObject(ready, 3000);
    CloseHandle(ready);
}

TrayHost::~TrayHost()
{
    if (HWND w = window_.load())
        PostMessageW(w, WM_CLOSE, 0, 0);
    else if (threadId_)
        PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
    if (thread_.joinable())
        thread_.join();
}

void TrayHost::Run(HANDLE ready)
{
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"Shell_TrayWnd";
    WNDCLASSEXW child{sizeof(child)};
    child.lpfnWndProc = DefWindowProcW;
    child.hInstance = instance;
    child.lpszClassName = L"TrayNotifyWnd";
    const bool registered = RegisterClassExW(&wc) != 0;
    RegisterClassExW(&child);
    RECT r{};
    if (HWND shell = ShellTaskbar())
        GetWindowRect(shell, &r);
    HWND w = registered ? CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"Shell_TrayWnd", L"", WS_POPUP, r.left, r.top,
                                          r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, this)
                        : nullptr;
    if (!w) {
        Log(L"tray host: no window (%lu)", GetLastError());
        SetEvent(ready);
        return;
    }
    // Some apps place things by the notification area inside the taskbar.
    CreateWindowExW(0, L"TrayNotifyWnd", L"", WS_CHILD, 0, 0, r.right - r.left, r.bottom - r.top, w, nullptr, instance, nullptr);
    // Apps at a lower integrity send their icons too, as they do to Explorer.
    ChangeWindowMessageFilterEx(w, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    window_ = w;
    KeepFirst();
    SetTimer(w, kKeepTimer, kKeepMs, nullptr);
    SetTimer(w, kPruneTimer, kPruneMs, nullptr);
    SetEvent(ready);
    // The apps already running add their icons again, now to the host.
    SendNotifyMessageW(HWND_BROADCAST, taskbarCreated_, 0, 0);
    Log(L"tray host: on, apps asked to add their icons again");

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    window_ = nullptr;
    UnregisterClassW(L"Shell_TrayWnd", instance);
    UnregisterClassW(L"TrayNotifyWnd", instance);
    Log(L"tray host: off");
}

LRESULT CALLBACK TrayHost::Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE)
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<TrayHost*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Message(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT TrayHost::Message(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COPYDATA: {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(l);
        if (cds && cds->dwData == 1)
            return OnNotifyIcon(h, w, cds);
        if (cds && cds->dwData == 3)
            return OnIconRect(h, w, cds);
        return Forward(m, w, l); // app bars (0) and the rest: Explorer's
    }
    case WM_TIMER:
        if (w == kKeepTimer) {
            KeepFirst();
            if (dirty_.exchange(false))
                PostMessageW(notify_, msg_, 0, 0);
        } else if (w == kPruneTimer) {
            Prune();
        }
        return 0;
    case WM_WINDOWPOSCHANGING:
        // Never shown, whoever asks: it only stands in for Explorer's taskbar.
        reinterpret_cast<WINDOWPOS*>(l)->flags &= ~SWP_SHOWWINDOW;
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    if (m == taskbarCreated_)
        return 0; // our own broadcast, or Explorer restarted: the apps add their icons
    if (m == WM_COMMAND || m >= WM_USER)
        return Forward(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT TrayHost::Forward(UINT m, WPARAM w, LPARAM l)
{
    HWND shell = ShellTaskbar();
    if (!shell)
        return 0;
    if (m == WM_COPYDATA || InSendMessage()) {
        DWORD_PTR result = 0;
        if (!SendMessageTimeoutW(shell, m, w, l, SMTO_ABORTIFHUNG, 4000, &result))
            return 0;
        return LRESULT(result);
    }
    PostMessageW(shell, m, w, l);
    return 0;
}

LRESULT TrayHost::OnNotifyIcon(HWND, WPARAM w, const COPYDATASTRUCT* cds)
{
    const auto* data = static_cast<const TrayNotifyData*>(cds->lpData);
    if (!data || cds->cbData < offsetof(TrayNotifyData, nid.szInfo) || data->signature != kSignature)
        return Forward(WM_COPYDATA, w, reinterpret_cast<LPARAM>(cds));
    const NotifyIconData32& nid = data->nid;
    const bool hasGuid = (nid.uFlags & NIF_GUID) && cds->cbData >= offsetof(TrayNotifyData, nid.guidItem) + sizeof(GUID);
    const HWND hwnd = Handle(nid.hWnd);
    const std::wstring key = KeyOf(hwnd, nid.uID, hasGuid ? &nid.guidItem : nullptr);

    // The picture is the app's: copied now, while the app waits for this call.
    std::shared_ptr<const IconImage> image;
    if ((data->message == NIM_ADD || data->message == NIM_MODIFY) && (nid.uFlags & NIF_ICON) && nid.hIcon) {
        IconImage img;
        if (HIconToImage(reinterpret_cast<HICON>(static_cast<ULONG_PTR>(nid.hIcon)), img))
            image = std::make_shared<const IconImage>(std::move(img));
    }

    LRESULT result = Forward(WM_COPYDATA, w, reinterpret_cast<LPARAM>(cds));
    const bool shell = ShellTaskbar() != nullptr;
    if (data->message == NIM_ADD && !result && shell) {
        // TaskbarCreated came from the host, not from a new Explorer: Explorer has the
        // icon already and refuses a second add. It is the same icon, brought up to date.
        std::vector<BYTE> copy(static_cast<const BYTE*>(cds->lpData), static_cast<const BYTE*>(cds->lpData) + cds->cbData);
        reinterpret_cast<TrayNotifyData*>(copy.data())->message = NIM_MODIFY;
        COPYDATASTRUCT again{cds->dwData, cds->cbData, copy.data()};
        result = Forward(WM_COPYDATA, w, reinterpret_cast<LPARAM>(&again));
    }
    if (!shell)
        result = TRUE; // Explorer is gone for the moment: the host keeps the icon

    std::lock_guard g(mutex_);
    auto it = icons_.find(key);
    if (data->message == NIM_DELETE) {
        if (it != icons_.end()) {
            icons_.erase(it);
            dirty_ = true;
        }
        return result;
    }
    if (data->message == NIM_SETVERSION) {
        if (it != icons_.end())
            it->second.version = nid.uVersion;
        return result;
    }
    if (data->message != NIM_ADD && data->message != NIM_MODIFY)
        return result;
    // A change of an icon the host has not seen (added before it started, and the
    // app ignored TaskbarCreated) that Explorer took: the host learns it from here.
    if (it == icons_.end()) {
        if (data->message == NIM_MODIFY && !result)
            return result;
        // The shell's own icons (volume, network...) stay Explorer's: Atrium has its control centre.
        DWORD pid = 0;
        if (hwnd) GetWindowThreadProcessId(hwnd, &pid);
        if (!pid || pid == ShellPid())
            return result;
        HostIcon icon;
        icon.key = key;
        icon.added = ++nextAdded_;
        it = icons_.emplace(key, std::move(icon)).first;
    }
    HostIcon& icon = it->second;
    if (hwnd && icon.hwnd != hwnd) {
        icon.hwnd = hwnd;
        GetWindowThreadProcessId(hwnd, &icon.pid);
        icon.exe = ProcessPath(icon.pid);
        if (!icon.exe.empty())
            icon.exe = AppExecutable(icon.exe);
        std::transform(icon.exe.begin(), icon.exe.end(), icon.exe.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    }
    icon.uid = nid.uID;
    if (nid.uFlags & NIF_MESSAGE)
        icon.callback = nid.uCallbackMessage;
    if (nid.uFlags & NIF_ICON) {
        icon.image = std::move(image);
        icon.imageVersion = kImageBase + ++nextImage_;
    }
    if (nid.uFlags & NIF_TIP)
        icon.tip.assign(nid.szTip, wcsnlen(nid.szTip, std::size(nid.szTip)));
    if ((nid.uFlags & NIF_STATE) && (nid.dwStateMask & NIS_HIDDEN))
        icon.hidden = (nid.dwState & NIS_HIDDEN) != 0;
    if (data->message == NIM_ADD)
        Log(L"tray host: %s '%s' (%s) v%u -> %ld", Name(data->message), icon.tip.substr(0, icon.tip.find(L'\n')).c_str(),
            icon.exe.empty() ? L"-" : icon.exe.c_str(), icon.version, long(result));
    dirty_ = true;
    return result;
}

LRESULT TrayHost::OnIconRect(HWND, WPARAM w, const COPYDATASTRUCT* cds)
{
    const auto* id = static_cast<const NotifyIconIdentifier32*>(cds->lpData);
    if (!id || cds->cbData < sizeof(NotifyIconIdentifier32) || id->signature != kSignature)
        return Forward(WM_COPYDATA, w, reinterpret_cast<LPARAM>(cds));
    const std::wstring key = KeyOf(Handle(id->hWnd), id->uID, &id->guidItem);
    RECT r{};
    {
        std::lock_guard g(mutex_);
        if (icons_.contains(key))
            r = key == clickedKey_ && !IsRectEmpty(&clicked_) ? clicked_ : anchor_;
    }
    if (IsRectEmpty(&r))
        return Forward(WM_COPYDATA, w, reinterpret_cast<LPARAM>(cds));
    return id->message == 1 ? MAKELONG(short(r.left), short(r.top)) : MAKELONG(short(r.right), short(r.bottom));
}

// Shell_NotifyIcon finds the first Shell_TrayWnd in the z-order. Explorer's comes
// before the host now and then (its taskbar is topmost too): the host moves back.
void TrayHost::KeepFirst()
{
    HWND w = window_.load();
    if (!w)
        return;
    HWND shell = ShellTaskbar();
    RECT want{}, have{};
    if (shell && GetWindowRect(shell, &want) && GetWindowRect(w, &have) && !EqualRect(&want, &have))
        SetWindowPos(w, nullptr, want.left, want.top, want.right - want.left, want.bottom - want.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    if (FindWindowW(L"Shell_TrayWnd", nullptr) == w)
        return;
    SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    const ULONGLONG now = GetTickCount64();
    if (now - lostAt_ > 60000) // not on every round of a tug of war
        Log(L"tray host: Explorer's taskbar came first; moved back (%s)", FindWindowW(L"Shell_TrayWnd", nullptr) == w ? L"first again" : L"still second");
    lostAt_ = now;
}

void TrayHost::Prune()
{
    std::lock_guard g(mutex_);
    const size_t before = icons_.size();
    std::erase_if(icons_, [](const auto& item) { return item.second.hwnd && !IsWindow(item.second.hwnd); });
    if (icons_.size() != before)
        dirty_ = true;
}

std::vector<HostIcon> TrayHost::Icons() const
{
    std::lock_guard g(mutex_);
    std::vector<HostIcon> out;
    out.reserve(icons_.size());
    for (const auto& [key, icon] : icons_)
        out.push_back(icon);
    std::sort(out.begin(), out.end(), [](const HostIcon& a, const HostIcon& b) { return a.added < b.added; });
    return out;
}

void TrayHost::SetPlaces(RECT anchor)
{
    std::lock_guard g(mutex_);
    anchor_ = anchor;
}

// What Explorer sends for a click on the icon: the mouse messages, then the
// version 3+ notifications. Version 4 puts the pointer in wParam and the icon's
// id in lParam's high word; older versions pass the id as wParam.
bool TrayHost::Click(const std::wstring& key, tray::Action action, POINT at)
{
    HWND hwnd = nullptr;
    UINT uid = 0, callback = 0, version = 0;
    DWORD pid = 0;
    {
        std::lock_guard g(mutex_);
        auto it = icons_.find(key);
        if (it == icons_.end())
            return false;
        hwnd = it->second.hwnd;
        uid = it->second.uid;
        callback = it->second.callback;
        version = it->second.version;
        pid = it->second.pid;
        clickedKey_ = key;
        clicked_ = {at.x - 16, at.y - 16, at.x + 16, at.y + 16};
    }
    if (!callback || !hwnd || !IsWindow(hwnd))
        return false;
    // Atrium has the foreground (its panel): the app may take it for its window or menu.
    AllowSetForegroundWindow(pid);
    bool ok = true;
    auto send = [&](UINT mouse) {
        const WPARAM wp = version >= 4 ? MAKEWPARAM(at.x, at.y) : WPARAM(uid);
        const LPARAM lp = version >= 4 ? MAKELPARAM(mouse, uid) : LPARAM(mouse);
        ok = SendNotifyMessageW(hwnd, callback, wp, lp) && ok;
    };
    send(WM_MOUSEMOVE);
    if (action == tray::Action::Right) {
        send(WM_RBUTTONDOWN);
        send(WM_RBUTTONUP);
        if (version >= 3)
            send(WM_CONTEXTMENU);
    } else if (action == tray::Action::DoubleContinuation) {
        send(WM_LBUTTONDBLCLK);
        send(WM_LBUTTONUP);
    } else {
        send(WM_LBUTTONDOWN);
        send(WM_LBUTTONUP);
        if (version >= 3)
            send(NIN_SELECT);
        if (action == tray::Action::Double) {
            send(WM_LBUTTONDBLCLK);
            send(WM_LBUTTONUP);
        }
    }
    Log(L"tray host: %s on %s v%u -> %s", action == tray::Action::Right ? L"right click" : action == tray::Action::Left ? L"click" : L"double click",
        key.c_str(), version, ok ? L"sent" : L"refused");
    return ok;
}

} // namespace atrium
