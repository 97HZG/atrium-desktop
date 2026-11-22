#include "shell/wechat_source.h"

#include "localization.h"

#include "log.h"
#include "shell/uia_client.h"
#include "../posted.h"
#include "win32_path.h"

#include <tlhelp32.h>
#include <uiautomation.h>

#include <winrt/base.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <utility>
#include <vector>

namespace atrium {

namespace {

struct ChatState {
    int unread = 0;
    std::wstring summary;
};

std::wstring Trim(std::wstring s)
{
    while (!s.empty() && iswspace(s.back()))
        s.pop_back();
    size_t a = s.find_first_not_of(L" \t");
    return a == std::wstring::npos ? std::wstring() : s.substr(a);
}

bool IsDigits(const std::wstring& s)
{
    return !s.empty() && std::ranges::all_of(s, [](wchar_t c) { return iswdigit(c) != 0; });
}

bool EndsWithNoCase(const std::wstring& s, const wchar_t* suffix)
{
    const size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

bool IsMutedLine(const std::wstring& line)
{
    return line == L"消息免打扰" || _wcsicmp(line.c_str(), L"Mute Notifications") == 0 || _wcsicmp(line.c_str(), L"Muted") == 0;
}

bool IsPinLine(const std::wstring& line) { return line == L"已置顶" || _wcsicmp(line.c_str(), L"Pinned") == 0; }

int ParseUnreadTag(std::wstring tag)
{
    tag = Trim(std::move(tag));
    constexpr const wchar_t* kSuffixes[] = {L"条", L" unread", L" messages", L" message"};
    std::wstring num;
    for (const wchar_t* suffix : kSuffixes) {
        if (EndsWithNoCase(tag, suffix)) {
            num = Trim(tag.substr(0, tag.size() - wcslen(suffix)));
            break;
        }
    }
    if (num.empty())
        return 0;
    if (num.back() == L'+')
        num.pop_back();
    if (!IsDigits(num))
        return 0;
    return _wtoi(num.c_str());
}

bool ParseUnreadPrefix(const std::wstring& line, int& unread, std::wstring& rest)
{
    if (line.size() < 4 || line[0] != L'[')
        return false;
    const size_t close = line.find(L']');
    if (close == std::wstring::npos || close <= 1)
        return false;
    const int count = ParseUnreadTag(line.substr(1, close - 1));
    if (count > 0) {
        unread = count;
        rest = Trim(line.substr(close + 1));
        return true;
    }
    // Badge prefixes like [有人@我] can precede [N条] on the same line.
    const std::wstring tail = Trim(line.substr(close + 1));
    if (tail.size() >= 4 && tail[0] == L'[')
        return ParseUnreadPrefix(tail, unread, rest);
    return false;
}

bool IsAmPmTime(const std::wstring& t)
{
    if (t.size() < 4 || (!EndsWithNoCase(t, L"AM") && !EndsWithNoCase(t, L"PM")))
        return false;
    const std::wstring head = Trim(t.substr(0, t.size() - 2));
    return head.find(L':') != std::wstring::npos && std::ranges::all_of(head, [](wchar_t c) { return iswdigit(c) || c == L':'; });
}

bool IsCjkDate(const std::wstring& t)
{
    if (!t.ends_with(L"日") || t.find(L'月') == std::wstring::npos)
        return false;
    bool hasDigit = false;
    for (wchar_t c : t) {
        if (iswdigit(c))
            hasDigit = true;
        else if (c != L'年' && c != L'月' && c != L'日')
            return false;
    }
    return hasDigit;
}

DWORD WindowPid(HWND h)
{
    if (!h)
        return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid;
}

bool IsWeChatPid(const std::vector<std::pair<DWORD, std::wstring>>& procs, DWORD pid)
{
    return pid != 0 && std::ranges::any_of(procs, [pid](const auto& p) { return p.first == pid; });
}

bool IsWeChatBusy(const std::vector<std::pair<DWORD, std::wstring>>& procs)
{
    if (HWND fg = GetForegroundWindow()) {
        if (IsWeChatPid(procs, WindowPid(fg)) || IsWeChatPid(procs, WindowPid(GetWindow(fg, GW_OWNER))) ||
            IsWeChatPid(procs, WindowPid(GetAncestor(fg, GA_ROOTOWNER))))
            return true;
    }
    struct Ctx {
        const std::vector<std::pair<DWORD, std::wstring>>& procs;
        bool busy = false;
    } ctx{procs, false};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(l);
            if (!IsWindowVisible(h))
                return TRUE;
            const DWORD pid = WindowPid(h);
            const bool ownPid = IsWeChatPid(c->procs, pid);
            if ((ownPid || IsWeChatPid(c->procs, WindowPid(GetWindow(h, GW_OWNER)))) && GetWindow(h, GW_OWNER) != nullptr) {
                c->busy = true;
                return FALSE;
            }
            if (ownPid) {
                wchar_t cls[96]{};
                GetClassNameW(h, cls, 96);
                if (wcsstr(cls, L"QWindowIcon") && !IsWindowEnabled(h)) {
                    c->busy = true;
                    return FALSE;
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.busy;
}

bool ShouldReadWeChatWindow(HWND h, DWORD pid, bool* hung = nullptr)
{
    if (WindowPid(h) != pid || GetWindow(h, GW_OWNER) != nullptr)
        return false;
    if ((GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0 || !IsWindowEnabled(h))
        return false;
    wchar_t cls[96]{};
    GetClassNameW(h, cls, 96);
    if (!wcsstr(cls, L"QWindowIcon"))
        return false;
    if (IsHungAppWindow(h)) {
        if (hung)
            *hung = true;
        return false;
    }
    return true;
}

// WeChat's top-level Qt main windows (shown, minimised, or hidden in the tray).
std::vector<HWND> WeChatWindows(DWORD pid, bool& anyHung)
{
    struct Ctx {
        DWORD pid;
        bool& anyHung;
        std::vector<HWND> out;
    } ctx{pid, anyHung, {}};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(l);
            if (ShouldReadWeChatWindow(h, c->pid, &c->anyHung))
                c->out.push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.out;
}

void DiscoverWeChatProcesses(std::vector<std::pair<DWORD, std::wstring>>& procs)
{
    procs.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, L"Weixin.exe") == 0 || _wcsicmp(pe.szExeFile, L"WeChat.exe") == 0)
            procs.emplace_back(pe.th32ProcessID, ProcessImagePath(pe.th32ProcessID));
    if (snap != INVALID_HANDLE_VALUE)
        CloseHandle(snap);
}

void UpdateUnread(const std::vector<std::pair<std::wstring, WeChatRow>>& rows, bool baseline, const std::wstring& exe, HWND notify, UINT msg,
                  std::map<std::wstring, ChatState>& states)
{
    std::map<std::wstring, ChatState> rebuilt;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& [key, r] = rows[i];
        auto old = states.find(key);
        const int oldUnread = old != states.end() ? old->second.unread : 0;
        const bool suppressed = r.muted || r.collapsed;
        // A chat not seen before counts only at the top of the list,
        // where a new message puts it (scrolling brings old ones in).
        const bool countRose = old != states.end() ? r.unread > oldUnread : (r.unread > 0 && i < 3);
        const bool saturatedRose = old != states.end() && r.unread >= 99 && oldUnread >= 99 && !r.summary.empty() && r.summary != old->second.summary;
        const bool rose = baseline && !suppressed && (countRose || saturatedRose);
        // Not gated on baseline: WeChat in front clears it, and reading happens there (#79).
        const bool read = old != states.end() && oldUnread > 0 && r.unread == 0;
        if (rose || read) {
            auto n = std::make_unique<Notice>();
            n->source = L"wechat";
            n->title = r.chat;
            n->unread = rose;
            if (rose) {
                n->app = Tr(L"WeChat");
                n->appId = exe;
                n->body = r.summary.empty() ? Tr(L"New message") : r.summary;
                if (!exe.empty())
                    n->action = L"app:" + exe;
            }
            if (PostOwned(notify, msg, 0, std::move(n)) && rose)
                Log(L"wechat: new message (unread %d)", r.unread);
        }
        ChatState next{r.unread, r.summary};
        if (baseline)
            states[key] = std::move(next);
        else
            rebuilt[key] = std::move(next);
    }
    if (!baseline)
        states = std::move(rebuilt);
    if (states.size() > 512) {
        std::erase_if(states, [&](const auto& kv) { return std::ranges::none_of(rows, [&](const auto& row) { return row.first == kv.first; }); });
        while (states.size() > 512)
            states.erase(states.begin());
    }
}

void ReadRow(IUIAutomationElement* e, std::vector<std::pair<std::wstring, WeChatRow>>& rows)
{
    if (!e)
        return;
    BSTR id = nullptr, name = nullptr;
    if (SUCCEEDED(e->get_CachedAutomationId(&id)) && SUCCEEDED(e->get_CachedName(&name)) && name) {
        std::wstring fallback;
        VARIANT val{};
        if (SUCCEEDED(e->GetCachedPropertyValue(UIA_ValueValuePropertyId, &val))) {
            if (val.vt == VT_BSTR && val.bstrVal && *val.bstrVal)
                fallback = val.bstrVal;
            VariantClear(&val);
        }
        if (fallback.empty()) {
            BSTR help = nullptr;
            if (SUCCEEDED(e->get_CachedHelpText(&help)) && help && *help)
                fallback = help;
            SysFreeString(help);
        }
        WeChatRow r = ParseWeChatRow(name, fallback);
        std::wstring key = id && *id ? id : r.chat;
        if (!r.chat.empty())
            rows.emplace_back(std::move(key), std::move(r));
    }
    SysFreeString(id);
    SysFreeString(name);
}

bool ReadLiveWindows(IUIAutomation* uia, IUIAutomationCondition* cells, IUIAutomationCacheRequest* cache,
                     const std::vector<std::pair<DWORD, std::wstring>>& procs, std::vector<std::pair<std::wstring, WeChatRow>>& rows,
                     std::vector<winrt::com_ptr<IUIAutomationElement>>& held, std::wstring& heldProgram, std::wstring& exe, bool& anyHung)
{
    for (const auto& [p, program] : procs) {
        for (HWND h : WeChatWindows(p, anyHung)) {
            winrt::com_ptr<IUIAutomationElement> root;
            winrt::com_ptr<IUIAutomationElementArray> all;
            if (FAILED(uia->ElementFromHandle(h, root.put())) || !root || FAILED(root->FindAllBuildCache(TreeScope_Descendants, cells, cache, all.put())) ||
                !all)
                continue;
            int n = 0;
            all->get_Length(&n);
            std::vector<winrt::com_ptr<IUIAutomationElement>> elements;
            for (int i = 0; i < n; ++i) {
                winrt::com_ptr<IUIAutomationElement> e;
                all->GetElement(i, e.put());
                ReadRow(e.get(), rows);
                if (e)
                    elements.push_back(std::move(e));
            }
            if (n > 0) {
                held = std::move(elements);
                heldProgram = program;
                exe = program;
                return true;
            }
        }
    }
    return false;
}

bool ReadHeldRows(IUIAutomationCacheRequest* cache, std::vector<winrt::com_ptr<IUIAutomationElement>>& held,
                  std::vector<std::pair<std::wstring, WeChatRow>>& rows)
{
    for (size_t i = 0; i < held.size();) {
        winrt::com_ptr<IUIAutomationElement> fresh;
        if (FAILED(held[i]->BuildUpdatedCache(cache, fresh.put())) || !fresh) {
            held.erase(held.begin() + ptrdiff_t(i)); // gone (WeChat restarted, the list rebuilt)
            continue;
        }
        ReadRow(fresh.get(), rows);
        ++i;
    }
    return !rows.empty();
}

} // namespace

bool IsWeChatTime(const std::wstring& s)
{
    const std::wstring t = Trim(s);
    if (t.empty() || t.size() > 24)
        return false;
    if (t == L"刚刚" || t == L"昨天" || t == L"前天" || t.starts_with(L"星期") || _wcsicmp(t.c_str(), L"Yesterday") == 0)
        return true;
    if (t.ends_with(L"分钟前") && t.size() > 3 && IsDigits(t.substr(0, t.size() - 3)))
        return true;
    if (t.ends_with(L"小时前") && t.size() > 3 && IsDigits(t.substr(0, t.size() - 3)))
        return true;
    if (IsAmPmTime(t) || IsCjkDate(t))
        return true;
    return std::ranges::any_of(t, [](wchar_t c) { return iswdigit(c) != 0; }) &&
           std::ranges::all_of(t, [](wchar_t c) { return iswdigit(c) || c == L':' || c == L'/' || c == L'-'; });
}

bool IsCollapsedWeChatRow(const std::wstring& chat)
{
    const std::wstring t = Trim(chat);
    return t == L"折叠的群聊" || t == L"折叠置顶聊天" || t == L"折叠的聊天" || _wcsicmp(t.c_str(), L"Collapsed Chats") == 0 ||
           _wcsicmp(t.c_str(), L"Folded Group Chats") == 0;
}

// A row's name, line by line: the chat; optional tag/pin lines; "[N条] summary" or
// "[N条]" and the summary on the following line, or only the summary when read; the time;
// and "消息免打扰" for a muted chat.
WeChatRow ParseWeChatRow(const std::wstring& name, const std::wstring& fallbackText)
{
    std::vector<std::wstring> lines;
    size_t from = 0;
    for (;;) {
        const size_t nl = name.find(L'\n', from);
        lines.push_back(Trim(name.substr(from, nl == std::wstring::npos ? std::wstring::npos : nl - from)));
        if (nl == std::wstring::npos)
            break;
        from = nl + 1;
    }
    WeChatRow r;
    if (lines.empty())
        return r;
    r.chat = lines[0];
    r.collapsed = IsCollapsedWeChatRow(r.chat);
    r.muted = r.collapsed;
    bool sawUnread = false;
    bool summaryAfterUnread = false;
    std::wstring preTag;
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::wstring& l = lines[i];
        if (l.empty() || IsPinLine(l))
            continue;
        if (IsMutedLine(l)) {
            r.muted = true;
            continue;
        }
        int count = 0;
        std::wstring rest;
        if (r.unread == 0 && ParseUnreadPrefix(l, count, rest)) {
            r.unread = count;
            sawUnread = true;
            if (!r.summary.empty() && !summaryAfterUnread) {
                preTag = std::move(r.summary);
                r.summary.clear();
            }
            if (!rest.empty() && !IsWeChatTime(rest)) {
                r.summary = std::move(rest);
                summaryAfterUnread = true;
            }
            continue;
        }
        if (!IsWeChatTime(l)) {
            if (sawUnread && !summaryAfterUnread) {
                r.summary = l;
                summaryAfterUnread = true;
            } else if (r.summary.empty()) {
                r.summary = l;
            }
        }
    }
    if (r.summary.empty()) {
        const std::wstring fb = Trim(fallbackText);
        if (!fb.empty() && fb != r.chat && !IsWeChatTime(fb) && !IsMutedLine(fb) && !IsPinLine(fb))
            r.summary = fb;
        else if (!preTag.empty())
            r.summary = std::move(preTag);
    }
    return r;
}

WeChatSource::WeChatSource(HWND notify, UINT msg) : notify_(notify), msg_(msg), wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr))
{
    thread_ = std::thread([this] { Run(); });
}

WeChatSource::~WeChatSource()
{
    stop_ = true;
    SetEvent(wake_);
    thread_.join();
    CloseHandle(wake_);
}

void WeChatSource::Run()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    winrt::com_ptr<IUIAutomationCondition> cells;
    winrt::com_ptr<IUIAutomationCacheRequest> cache;
    winrt::com_ptr<IUIAutomation> uia = CreateUia(stop_, wake_, L"wechat", [&](IUIAutomation* u) {
        cells = nullptr;
        VARIANT v{};
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(L"mmui::ChatSessionCell");
        HRESULT hr = u->CreatePropertyCondition(UIA_ClassNamePropertyId, v, cells.put());
        VariantClear(&v);
        if (FAILED(hr))
            return hr;
        return CreateRawCache(u, {UIA_AutomationIdPropertyId, UIA_NamePropertyId, UIA_ValueValuePropertyId, UIA_HelpTextPropertyId}, cache, true);
    });
    std::map<std::wstring, ChatState> states;
    bool baseline = false;
    int state = -1; // 0 no WeChat, 1 no list, 2 reading, 3 reading the held rows (logged on change)
    auto report = [&](int now, const wchar_t* text) {
        if (now != state)
            Log(L"wechat: %s", text);
        state = now;
    };
    std::vector<std::pair<DWORD, std::wstring>> procs;
    ULONGLONG listed = 0;
    ULONGLONG busyUntilTick = 0;
    ULONGLONG lastRead = 0;
    // The chat rows last found in a shown window. Closed to the tray, WeChat's window leaves its
    // rows out of the tree (Qt skips hidden controls), but rows already held still read via
    // BuildUpdatedCache so messages are seen there too.
    std::vector<winrt::com_ptr<IUIAutomationElement>> held;
    std::wstring heldProgram;
    {
        UiaWindowWake windows(wake_, L"QWindowIcon", false);
        while (!stop_ && uia && cells) {
            try {
                const ULONGLONG tick = GetTickCount64();
                if (!listed || tick - listed >= 10000 || (state < 2 && tick - listed >= 2000)) {
                    DiscoverWeChatProcesses(procs);
                    listed = tick;
                    windows.SetProcesses(procs);
                }
                if (procs.empty()) {
                    busyUntilTick = 0;
                    lastRead = 0;
                    report(0, L"WeChat is not running");
                    held.clear();
                    states.clear();
                    baseline = false; // after a restart its list is a new baseline
                    WaitForUiaWake(wake_, 6000);
                    continue;
                }
                if (IsWeChatBusy(procs)) {
                    busyUntilTick = tick + 1200;
                    baseline = false;
                    WaitForUiaWake(wake_, 1000);
                    continue;
                }
                if (tick < busyUntilTick) {
                    baseline = false;
                    WaitForUiaWake(wake_, DWORD(std::min<ULONGLONG>(1000, busyUntilTick - tick)));
                    continue;
                }
                if (lastRead && tick - lastRead < 1000) {
                    WaitForUiaWake(wake_, DWORD(1000 - (tick - lastRead)));
                    continue;
                }
                lastRead = tick;
                std::wstring exe;
                std::vector<std::pair<std::wstring, WeChatRow>> rows;
                bool anyHung = false;
                bool found = ReadLiveWindows(uia.get(), cells.get(), cache.get(), procs, rows, held, heldProgram, exe, anyHung);
                bool fromHeld = false;
                if (!found && !anyHung && !held.empty()) {
                    found = fromHeld = ReadHeldRows(cache.get(), held, rows);
                    if (found) {
                        exe = heldProgram;
                        if (state != 3)
                            Log(L"wechat: window hidden, reading the %zu chats held from it", held.size());
                    }
                }
                if (!found && !anyHung) {
                    report(1, L"WeChat's chat list is not readable (open its window once)");
                    baseline = false;
                } else if (found) {
                    if (state < 2 || (state == 3 && !fromHeld))
                        Log(L"wechat: reading WeChat's chat list (%zu chats in view)", rows.size());
                    state = fromHeld ? 3 : 2;
                    UpdateUnread(rows, baseline, exe, notify_, msg_, states);
                    baseline = true;
                }
            } catch (...) {
                Log(L"wechat: reading failed");
            }
            if (!stop_) {
                const DWORD waitMs = state >= 2 ? 2000 : (state == 1 ? 10000 : 6000);
                WaitForUiaWake(wake_, waitMs);
            }
        }
    }
    held.clear();
    cells = nullptr;
    cache = nullptr;
    uia = nullptr;
    winrt::uninit_apartment();
}

} // namespace atrium
