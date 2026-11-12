// Pipeline and evidence contracts: docs/TRAY.md. All shell/UIA work stays on Worker.
#include "shell/tray_model.h"
#include "shell/tray_match.h"
#include "shell/tray_click.h"

#include "log.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <uiautomation.h>
#include <wincodec.h>

#include <winrt/base.h>

#include <algorithm>
#include <map>
#include <set>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "shlwapi.lib")

namespace atrium {

namespace {

constexpr int kIconPx = 64;
static_assert(tray::providerTimeout == int32_t(UIA_E_TIMEOUT));

std::wstring Lower(std::wstring s)
{
    if (!s.empty())
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), int(s.size()), s.data(), int(s.size()), nullptr, nullptr, 0);
    return s;
}

std::wstring VersionString(const std::wstring& path, const wchar_t* field)
{
    DWORD handle = 0, size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size)
        return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return {};
    struct Lang {
        WORD lang, codepage;
    }* langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) || len < sizeof(Lang))
        return {};
    wchar_t sub[96];
    swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\%s", langs[0].lang, langs[0].codepage, field);
    wchar_t* value = nullptr;
    if (VerQueryValueW(data.data(), sub, reinterpret_cast<void**>(&value), &len) && value && len)
        return value;
    return {};
}

// "{6D809377-...}\Tailscale\tailscale-ipn.exe": a known folder, then the rest.
std::wstring ResolveKnownFolder(const std::wstring& path)
{
    if (path.size() < 39 || path[0] != L'{')
        return path;
    GUID id{};
    if (FAILED(CLSIDFromString(path.substr(0, 38).c_str(), &id)))
        return path;
    PWSTR base = nullptr;
    std::wstring out = path;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &base)))
        out = std::wstring(base) + path.substr(38);
    CoTaskMemFree(base);
    return out;
}

bool DecodePng(const std::vector<BYTE>& bytes, IconImage& out)
{
    winrt::com_ptr<IWICImagingFactory> wic;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::com_ptr<IWICFormatConverter> conv;
    winrt::com_ptr<IStream> stream;
    stream.attach(SHCreateMemStream(bytes.data(), UINT(bytes.size())));
    if (!stream || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()))) ||
        FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())) || FAILED(wic->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (!w || !h || w > 512 || h > 512)
        return false;
    out.width = int(w);
    out.height = int(h);
    out.bgra.resize(size_t(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, UINT(out.bgra.size()), out.bgra.data()))) return false;
    for (size_t i = 3; i < out.bgra.size(); i += 4)
        if (out.bgra[i]) return true;
    return false; // an empty startup snapshot must fall back to the executable's artwork
}

// Whether an icon is a glyph in one light (1) or dark (-1) neutral shade, or
// neither (0): most of its opaque pixels white, or black, without colour. A
// coloured badge on it (a sync arrow's dot) does not count against it; an icon
// on a plate of its own (NVIDIA's logo on black) is not a glyph, nor one drawn
// with both an outline and a fill (OneDrive's), which shows on either glass.
int GlyphTone(const IconImage& img)
{
    size_t opaque = 0, light = 0, dark = 0;
    for (size_t i = 0; i + 3 < img.bgra.size(); i += 4) {
        const int a = img.bgra[i + 3];
        if (a < 64)
            continue;
        ++opaque;
        // Premultiplied: the colour itself is the value over alpha.
        const int b = img.bgra[i] * 255 / a, g = img.bgra[i + 1] * 255 / a, r = img.bgra[i + 2] * 255 / a;
        const int chroma = std::max({r, g, b}) - std::min({r, g, b}), lum = (54 * r + 183 * g + 19 * b) >> 8;
        if (chroma < 40 && lum > 200)
            ++light;
        else if (chroma < 40 && lum < 60)
            ++dark;
    }
    if (opaque < 8 || opaque * 4 > img.bgra.size() / 4 * 3)
        return 0;
    return light * 10 >= opaque * 6 ? 1 : dark * 10 >= opaque * 6 ? -1 : 0;
}

// The glyph's neutral pixels (and their soft edges) in Atrium's ink of the
// other kind (DESIGN.md, ink): #111317 at 92 % for a white glyph, white at 96 %
// for a black one. Alpha and coloured pixels stay.
IconImage InkedGlyph(const IconImage& img, int tone)
{
    IconImage out = img;
    const int ink[3] = {tone > 0 ? 0x17 : 255, tone > 0 ? 0x13 : 255, tone > 0 ? 0x11 : 255}; // b, g, r
    const int strength = tone > 0 ? 235 : 245;
    for (size_t i = 0; i + 3 < out.bgra.size(); i += 4) {
        const int a = out.bgra[i + 3];
        if (!a)
            continue;
        const int b = out.bgra[i] * 255 / a, g = out.bgra[i + 1] * 255 / a, r = out.bgra[i + 2] * 255 / a;
        const int chroma = std::max({r, g, b}) - std::min({r, g, b}), lum = (54 * r + 183 * g + 19 * b) >> 8;
        if (chroma >= 48 || (tone > 0 ? lum < 128 : lum >= 128))
            continue;
        const int na = a * strength / 255;
        for (int k = 0; k < 3; ++k)
            out.bgra[i + size_t(k)] = uint8_t(ink[k] * na / 255);
        out.bgra[i + 3] = uint8_t(na);
    }
    return out;
}

// Windows' record of the icons that have been in the tray.
struct Record {
    std::wstring id;      // Windows' record identity; separates multiple icons from one program
    std::wstring exe;     // full path, lower case
    std::wstring tip;     // tooltip when first seen; often empty
    std::vector<BYTE> png; // icon snapshot
    bool promoted = false; // shown on the taskbar
    bool hasUid = false;   // the icon's id within its program (NOTIFYICONDATA.uID)
    UINT uid = 0;
    GUID guid{};
    size_t order = SIZE_MAX; // place in Windows' order of the icons (UIOrderList), if listed
};

// Reads the records; `ordered` tells whether Windows' order of them was there.
std::vector<Record> ReadRecords(bool& ordered)
{
    std::vector<Record> out;
    ordered = false;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return out;
    // UIOrderList: the records' ids (their key names) as 64-bit numbers, in
    // the order the taskbar and its overflow window show the icons.
    std::map<unsigned long long, size_t> order;
    DWORD bytes = 0;
    if (RegGetValueW(root, nullptr, L"UIOrderList", RRF_RT_REG_BINARY, nullptr, nullptr, &bytes) == ERROR_SUCCESS && bytes >= 8 && bytes < (1u << 16)) {
        std::vector<unsigned long long> ids(bytes / 8);
        DWORD got = DWORD(ids.size() * 8);
        if (RegGetValueW(root, nullptr, L"UIOrderList", RRF_RT_REG_BINARY, nullptr, ids.data(), &got) == ERROR_SUCCESS)
            for (size_t i = 0; i < ids.size(); ++i)
                order.try_emplace(ids[i], i);
    }
    ordered = !order.empty();
    for (DWORD i = 0;; ++i) {
        wchar_t name[128];
        DWORD len = 128;
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        Record r;
        r.id = name;
        wchar_t buf[1024];
        DWORD size = sizeof(buf);
        if (RegGetValueW(root, name, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            r.exe = Lower(AppExecutable(ResolveKnownFolder(buf)));
        size = sizeof(buf);
        if (RegGetValueW(root, name, L"InitialTooltip", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            r.tip = buf;
        size = 0;
        if (RegGetValueW(root, name, L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, nullptr, &size) == ERROR_SUCCESS && size && size < (1u << 20)) {
            r.png.resize(size);
            RegGetValueW(root, name, L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, r.png.data(), &size);
        }
        DWORD uid = 0;
        size = sizeof(uid);
        if (RegGetValueW(root, name, L"UID", RRF_RT_REG_DWORD, nullptr, &uid, &size) == ERROR_SUCCESS)
            r.hasUid = true, r.uid = uid;
        size = sizeof(buf);
        if (RegGetValueW(root, name, L"IconGuid", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            CLSIDFromString(buf, &r.guid);
        DWORD promoted = 0;
        size = sizeof(promoted);
        if (RegGetValueW(root, name, L"IsPromoted", RRF_RT_REG_DWORD, nullptr, &promoted, &size) == ERROR_SUCCESS)
            r.promoted = promoted != 0;
        if (auto it = order.find(wcstoull(name, nullptr, 10)); it != order.end())
            r.order = it->second;
        if (!r.exe.empty())
            out.push_back(std::move(r));
    }
    RegCloseKey(root);
    return out;
}

struct Program {
    std::vector<std::wstring> titles; // of its visible windows
    std::vector<HWND> windows;        // all its top-level and message-only windows
};

// Running programs by path (lower case).
std::map<std::wstring, Program> RunningPrograms()
{
    std::map<DWORD, std::wstring> paths;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    if (snap != INVALID_HANDLE_VALUE) {
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            // Elevated/service-owned icons still have a shell window and UID,
            // even when Windows will not expose their full process path.
            paths[pe.th32ProcessID] = L"|" + Lower(pe.szExeFile);
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
                wchar_t buf[MAX_PATH * 2];
                DWORD len = DWORD(std::size(buf));
                if (QueryFullProcessImageNameW(p, 0, buf, &len))
                    paths[pe.th32ProcessID] = Lower(buf);
                CloseHandle(p);
            }
        }
        CloseHandle(snap);
    }
    std::map<std::wstring, Program> out;
    for (const auto& [pid, path] : paths)
        out[path];
    struct Ctx {
        std::map<DWORD, std::wstring>* paths;
        std::map<std::wstring, Program>* out;
        bool titles;
    } ctx{&paths, &out, true};
    auto each = [](HWND h, LPARAM l) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(l);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        auto it = c->paths->find(pid);
        if (it == c->paths->end())
            return TRUE;
        Program& p = (*c->out)[it->second];
        p.windows.push_back(h);
        wchar_t title[256];
        if (c->titles && IsWindowVisible(h) && InternalGetWindowText(h, title, 256) > 0) // sends no message (a hung window)
            p.titles.push_back(title);
        return TRUE;
    };
    EnumWindows(each, reinterpret_cast<LPARAM>(&ctx));
    // Tray icons often belong to message-only windows, which EnumWindows skips.
    ctx.titles = false;
    for (HWND h = FindWindowExW(HWND_MESSAGE, nullptr, nullptr, nullptr); h; h = FindWindowExW(HWND_MESSAGE, h, nullptr, nullptr))
        each(h, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

// Whether one of a program's windows has the tray icon `uid` now (the shell
// knows the icon's place; it fails for icons that are not there).
bool HasIcon(const std::vector<HWND>& windows, UINT uid, RECT& bounds, std::wstring& identity)
{
    for (HWND h : windows) {
        NOTIFYICONIDENTIFIER id{sizeof(id), h, uid, GUID_NULL};
        RECT r{};
        if (SUCCEEDED(Shell_NotifyIconGetRect(&id, &r))) {
            bounds = r;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            identity = L"uid:" + std::to_wstring(pid) + L":" + std::to_wstring(reinterpret_cast<uintptr_t>(h)) + L":" + std::to_wstring(uid);
            return true;
        }
    }
    return false;
}

bool LiveIcon(const Record& record, const Program& program, RECT& bounds, std::wstring& identity)
{
    if (record.guid != GUID_NULL) {
        NOTIFYICONIDENTIFIER id{sizeof(id), nullptr, 0, record.guid};
        if (FAILED(Shell_NotifyIconGetRect(&id, &bounds))) return false;
        wchar_t guid[40]{};
        StringFromGUID2(record.guid, guid, 40);
        identity = L"guid:" + std::wstring(guid); // liveness only, NOT process ownership
        return true;
    }
    return record.hasUid && HasIcon(program.windows, record.uid, bounds, identity);
}

struct Found {
    winrt::com_ptr<IUIAutomationElement> element;
    HWND toolbar = nullptr; // a button of the classic tray's toolbar (Windows 10): clicked by messages to it
    std::wstring tip;
    bool promoted = false;
    int index = 0;
    RECT bounds{};
    std::wstring recordId;
    tray::Method method = tray::Method::Unknown;
    std::vector<int> runtimeId;
};

tray::ShellSession ReadShellSession()
{
    tray::ShellSession session;
    const HWND window = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD pid = 0;
    if (!window || !GetWindowThreadProcessId(window, &pid)) return session;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(process, &created, &exited, &kernel, &user))
            session = {pid, (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime,
                       uint64_t(reinterpret_cast<uintptr_t>(window))};
        CloseHandle(process);
    }
    return session;
}

std::vector<int> RuntimeId(IUIAutomationElement* element)
{
    SAFEARRAY* id = nullptr;
    std::vector<int> out;
    if (element && SUCCEEDED(element->GetRuntimeId(&id)) && id) {
        LONG first = 0, last = -1;
        if (SUCCEEDED(SafeArrayGetLBound(id, 1, &first)) && SUCCEEDED(SafeArrayGetUBound(id, 1, &last)))
            for (LONG i = first; i <= last && i - first < 64; ++i) {
                int value = 0;
                if (FAILED(SafeArrayGetElement(id, &i, &value))) { out.clear(); break; }
                out.push_back(value);
            }
    }
    if (id) SafeArrayDestroy(id);
    return out;
}

// The tray buttons in the taskbar and in the overflow window, in order.
std::vector<Found> FindButtons(IUIAutomation* uia, bool& available)
{
    std::vector<Found> out;
    available = false;
    auto collect = [&](HWND host, bool promoted) {
        HWND bridge = host ? FindWindowExW(host, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) : nullptr;
        winrt::com_ptr<IUIAutomationElement> root;
        if (!host || FAILED(uia->ElementFromHandle(bridge ? bridge : host, root.put())) || !root)
            return;
        available = true;
        winrt::com_ptr<IUIAutomationCondition> cond;
        VARIANT v;
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(L"NotifyItemIcon");
        HRESULT hr = uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, cond.put());
        VariantClear(&v);
        winrt::com_ptr<IUIAutomationElementArray> all;
        if (FAILED(hr) || FAILED(root->FindAll(TreeScope_Descendants, cond.get(), all.put())) || !all)
            return;
        int n = 0;
        all->get_Length(&n);
        for (int i = 0; i < n; ++i) {
            Found f;
            all->GetElement(i, f.element.put());
            BSTR name = nullptr;
            if (f.element)
                f.element->get_CurrentName(&name);
            f.tip = name ? name : L"";
            SysFreeString(name);
            f.promoted = promoted;
            f.index = int(std::count_if(out.begin(), out.end(), [&](const Found& o) { return o.promoted == promoted; }));
            if (f.element)
                f.element->get_CurrentBoundingRectangle(&f.bounds);
            f.runtimeId = RuntimeId(f.element.get());
            out.push_back(std::move(f));
        }
    };
    collect(FindWindowW(L"Shell_TrayWnd", nullptr), true);
    collect(FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr), false);
    if (!out.empty())
        return out;
    // No XAML tray (Windows 10, and Windows 11 before its new tray): the classic one, a toolbar of
    // buttons on the taskbar and another in the overflow window, named by their tooltips too.
    auto legacy = [&](HWND toolbar, bool promoted) {
        winrt::com_ptr<IUIAutomationElement> root;
        if (!toolbar || FAILED(uia->ElementFromHandle(toolbar, root.put())) || !root)
            return;
        available = true;
        VARIANT v;
        v.vt = VT_I4;
        v.lVal = UIA_ButtonControlTypeId;
        winrt::com_ptr<IUIAutomationCondition> cond;
        winrt::com_ptr<IUIAutomationElementArray> all;
        if (FAILED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, cond.put())) || FAILED(root->FindAll(TreeScope_Children, cond.get(), all.put())) || !all)
            return;
        int n = 0;
        all->get_Length(&n);
        for (int i = 0; i < n; ++i) {
            Found f;
            all->GetElement(i, f.element.put());
            if (!f.element)
                continue;
            BSTR name = nullptr;
            f.element->get_CurrentName(&name);
            f.tip = name ? name : L"";
            SysFreeString(name);
            f.promoted = promoted;
            f.index = int(std::count_if(out.begin(), out.end(), [&](const Found& o) { return o.promoted == promoted; }));
            f.element->get_CurrentBoundingRectangle(&f.bounds);
            f.toolbar = toolbar;
            f.runtimeId = RuntimeId(f.element.get());
            out.push_back(std::move(f));
        }
    };
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND notify = tray ? FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    HWND pager = notify ? FindWindowExW(notify, nullptr, L"SysPager", nullptr) : nullptr;
    legacy(pager ? FindWindowExW(pager, nullptr, L"ToolbarWindow32", nullptr) : nullptr, true);
    HWND overflow = FindWindowW(L"NotifyIconOverflowWindow", nullptr);
    legacy(overflow ? FindWindowExW(overflow, nullptr, L"ToolbarWindow32", nullptr) : nullptr, false);
    return out;
}

// An explicit fallback opens ONLY the popup, never briefly shows/hides the taskbar.
// Verify realized content; HWND visibility alone does not imply a usable XAML popup.
HWND OverflowWindow()
{
    HWND h = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr);
    return h ? h : FindWindowW(L"NotifyIconOverflowWindow", nullptr);
}
bool OverflowUsable(IUIAutomation* uia)
{
    HWND overflow = OverflowWindow();
    if (!uia || !overflow || !IsWindowVisible(overflow)) return false;
    bool available = false;
    const auto buttons = FindButtons(uia, available);
    for (const auto& button : buttons) {
        if (button.promoted || !button.element || IsRectEmpty(&button.bounds)) continue;
        BOOL offscreen = TRUE;
        if (FAILED(button.element->get_CurrentIsOffscreen(&offscreen)) || offscreen) continue;
        const POINT at{button.bounds.left + (button.bounds.right - button.bounds.left) / 2,
                       button.bounds.top + (button.bounds.bottom - button.bounds.top) / 2};
        HWND hit = WindowFromPoint(at);
        if (hit == overflow || GetAncestor(hit, GA_ROOT) == overflow) return true;
    }
    return false;
}
bool ShowOverflow(IUIAutomation* uia)
{
    if (OverflowUsable(uia)) return true;
    // Showing the HWND alone does NOT uncollapse XAML content. Invoke the shell's chevron.
    // Its non-localized structural signature is a NormalButton/SystemTrayIcon before the
    // first promoted NotifyItemIcon. Require that anchor and a later normal system button;
    // builds without this signature fail closed instead of guessing from translated names.
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND bridge = tray ? FindWindowExW(tray, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) : nullptr;
    if (tray && !bridge) {
        // Classic shell: the unique Button directly inside TrayNotifyWnd is its chevron.
        // BM_CLICK is the documented Button action, not pointer injection or toolbar memory IO.
        HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
        HWND button = notify ? FindWindowExW(notify, nullptr, L"Button", nullptr) : nullptr;
        if (!button || FindWindowExW(notify, button, L"Button", nullptr)) return false;
        const BOOL queued = PostMessageW(button, BM_CLICK, 0, 0);
        Log(L"tray: overflow classic BM_CLICK queued=%d error=%lu", queued, queued ? 0 : GetLastError());
        if (!queued) return false;
        for (int i = 0; i < 10; ++i) {
            if (OverflowUsable(uia)) return true;
            Sleep(20);
        }
        return false;
    }
    winrt::com_ptr<IUIAutomationElement> root, chevron;
    winrt::com_ptr<IUIAutomationCondition> condition;
    winrt::com_ptr<IUIAutomationElementArray> elements;
    if (!uia || !bridge || FAILED(uia->ElementFromHandle(bridge, root.put())) || !root) return false;
    VARIANT value{};
    value.vt = VT_I4; value.lVal = UIA_ButtonControlTypeId;
    if (FAILED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, value, condition.put())) ||
        FAILED(root->FindAll(TreeScope_Descendants, condition.get(), elements.put())) || !elements) return false;
    int count = 0, before = 0, after = 0;
    bool anchor = false;
    elements->get_Length(&count);
    for (int i = 0; i < count; ++i) {
        winrt::com_ptr<IUIAutomationElement> element;
        elements->GetElement(i, element.put());
        if (!element) continue;
        BSTR id = nullptr, cls = nullptr;
        element->get_CurrentAutomationId(&id); element->get_CurrentClassName(&cls);
        if (id && wcscmp(id, L"NotifyItemIcon") == 0) anchor = true;
        if (id && cls && wcscmp(id, L"SystemTrayIcon") == 0 && wcscmp(cls, L"SystemTray.NormalButton") == 0) {
            if (!anchor) { ++before; chevron = element; }
            else ++after;
        }
        SysFreeString(id); SysFreeString(cls);
    }
    if (!anchor || before != 1 || after < 1 || !chevron) {
        Log(L"tray: overflow chevron ambiguous anchor=%d before=%d after=%d", anchor, before, after);
        return false;
    }
    winrt::com_ptr<IUIAutomationInvokePattern> invoke;
    HRESULT hr = chevron->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invoke.put()));
    if (SUCCEEDED(hr)) hr = invoke ? invoke->Invoke() : E_NOINTERFACE;
    Log(L"tray: overflow chevron hr=0x%08X taskbarVisible=%d", unsigned(hr), IsWindowVisible(tray));
    if (FAILED(hr)) return false;
    for (int i = 0; i < 10; ++i) {
        if (OverflowUsable(uia)) {
            Log(L"tray: overflow verified visible content and hit target");
            return true;
        }
        Sleep(20);
    }
    Log(L"tray: overflow accepted but content not usable");
    return false;
}

struct ReadDiagnostics { size_t records = 0, live = 0, aliases = 0, shared = 0, unknown = 0; };
std::vector<int> MatchButtons(std::vector<Found>& buttons, std::vector<Record>& records, bool& ordered,
                             const std::vector<PinnedApp>& apps = {}, ReadDiagnostics* diagnostics = nullptr)
{
    records = ReadRecords(ordered);
    const auto running = RunningPrograms();
    std::map<std::wstring, std::vector<std::wstring>> labels;
    for (const auto& app : apps)
        if (!app.path.empty() && !app.name.empty()) labels[Lower(AppExecutable(app.path))].push_back(app.name);
    static std::map<std::wstring, std::pair<std::wstring, std::wstring>> version;
    if (version.size() > 512) version.clear();
    auto names = [&](const std::wstring& path, const Program& program, const std::wstring& tip,
                     std::vector<std::wstring>& out, size_t& firstTitle) {
        auto [at, fresh] = version.try_emplace(path);
        if (fresh) at->second = {VersionString(path, L"FileDescription"), VersionString(path, L"ProductName")};
        std::wstring stem = PathFindFileNameW(path.c_str());
        if (auto dot = stem.rfind(L'.'); dot != std::wstring::npos) stem.resize(dot);
        out = {tip, at->second.first, at->second.second, stem};
        if (auto it = labels.find(path); it != labels.end()) out.insert(out.end(), it->second.begin(), it->second.end());
        firstTitle = out.size();
        out.insert(out.end(), program.titles.begin(), program.titles.end());
    };
    auto rect = [](const RECT& r) { return tray::Rect{r.left, r.top, r.right, r.bottom}; };
    std::vector<tray::Button> bs;
    for (const auto& b : buttons) bs.push_back({b.tip, b.promoted, rect(b.bounds)});
    std::vector<tray::Record> rs;
    const Program empty;
    for (const auto& r : records) {
        auto run = running.find(r.exe);
        // A filename-only process result is not proof of a full executable path.
        const Program& program = run == running.end() ? empty : run->second;
        tray::Record value;
        value.id = r.id; value.exe = r.exe; value.tip = r.tip;
        value.promoted = r.promoted; value.order = r.order;
        RECT bounds{};
        value.live = LiveIcon(r, program, bounds, value.identity);
        value.bounds = rect(bounds);
        if (value.live) names(r.exe, program, r.tip, value.names, value.firstTitle);
        rs.push_back(std::move(value));
    }
    std::vector<tray::Program> ps;
    const auto initial = tray::Match(bs, rs);
    const bool needPrograms = std::any_of(initial.items.begin(), initial.items.end(), [](const auto& item) { return item.button != tray::none && item.record == tray::none; });
    for (const auto& [path, program] : running) {
        if (!needPrograms || path.empty() || path[0] == L'|' || program.windows.empty() ||
            std::any_of(rs.begin(), rs.end(), [&](const auto& r) { return r.live && r.exe == path; })) continue;
        tray::Program value;
        value.exe = path;
        names(path, program, {}, value.names, value.firstTitle);
        ps.push_back(std::move(value));
    }
    const auto result = tray::Match(bs, rs, ps);
    if (diagnostics) {
        *diagnostics = {};
        diagnostics->records = rs.size();
        for (size_t r = 0; r < rs.size(); ++r) {
            diagnostics->live += rs[r].live;
            diagnostics->aliases += result.aliases[r] != tray::none && result.aliases[r] != r;
            diagnostics->shared += result.sharedPlace[r];
        }
        for (const auto& item : result.items) diagnostics->unknown += item.method == tray::Method::Unknown;
    }
    std::vector<int> match;
    for (const auto& item : result.items) {
        if (item.button == tray::none) {
            const auto& r = rs[item.record];
            Found f;
            f.tip = r.tip;
            if (f.tip.empty() && r.names.size() > 1) f.tip = r.names[1];
            if (f.tip.empty() && r.names.size() > 3) f.tip = r.names[3];
            f.promoted = r.promoted;
            f.index = int(std::count_if(buttons.begin(), buttons.end(), [&](const Found& b) { return b.promoted == f.promoted; }));
            buttons.push_back(std::move(f)); // no element or fake clickable rectangle
        }
        Found& f = buttons[match.size()];
        f.method = item.method;
        if (item.record != tray::none) {
            f.recordId = rs[item.record].identity;
            match.push_back(int(item.record));
        } else if (item.program != tray::none) {
            Record r;
            r.exe = ps[item.program].exe;
            r.id = L"run:" + r.exe;
            records.push_back(std::move(r));
            match.push_back(int(records.size() - 1));
        } else match.push_back(-1);
    }
    return match;
}

const wchar_t* RouteName(tray::Route route)
{
    switch (route) {
    case tray::Route::Invoke: return L"uia-invoke";
    case tray::Route::ContextMenu: return L"uia-context-menu";
    case tray::Route::LegacyDefault: return L"legacy-default";
    default: return L"toolbar-messages";
    }
}

HRESULT ToolbarClick(const Found& target, tray::Action action)
{
    wchar_t cls[80]{};
    if (!IsWindow(target.toolbar) || !GetClassNameW(target.toolbar, cls, 80) || wcscmp(cls, L"ToolbarWindow32")) return E_INVALIDARG;
    HWND root = GetAncestor(target.toolbar, GA_ROOT);
    GetClassNameW(root, cls, 80);
    if (wcscmp(cls, L"Shell_TrayWnd") && wcscmp(cls, L"NotifyIconOverflowWindow")) return E_INVALIDARG;
    DWORD pid = 0, shellPid = 0;
    GetWindowThreadProcessId(root, &pid);
    GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", nullptr), &shellPid);
    if (!pid || pid != shellPid) return E_INVALIDARG;
    RECT bounds{}, client{};
    if (!target.element || FAILED(target.element->get_CurrentBoundingRectangle(&bounds)) || IsRectEmpty(&bounds) ||
        !GetClientRect(target.toolbar, &client)) return UIA_E_ELEMENTNOTAVAILABLE;
    POINT at{bounds.left + (bounds.right - bounds.left) / 2, bounds.top + (bounds.bottom - bounds.top) / 2};
    if (!ScreenToClient(target.toolbar, &at) || !PtInRect(&client, at) || at.x > 32767 || at.y > 32767) return E_INVALIDARG;
    const LPARAM position = MAKELPARAM(at.x, at.y);
    const bool right = action == tray::Action::Right;
    auto post = [&](UINT message, WPARAM keys) { return PostMessageW(target.toolbar, message, keys, position) != FALSE; };
    auto error = [] { DWORD code = GetLastError(); return code ? HRESULT_FROM_WIN32(code) : E_FAIL; };
    if (!post(WM_MOUSEMOVE, 0) || !post(right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, right ? MK_RBUTTON : MK_LBUTTON)) return error();
    if (!post(right ? WM_RBUTTONUP : WM_LBUTTONUP, 0)) return error();
    if (action == tray::Action::Double && (!post(WM_LBUTTONDBLCLK, MK_LBUTTON) || !post(WM_LBUTTONUP, 0))) return error();
    return S_OK; // queued, not acknowledged; Dispatch still needs response evidence
}

struct ResponseState {
    std::set<HWND> visible, menus;
    HWND foreground = nullptr;
};
ResponseState CaptureResponse(const std::wstring& exe)
{
    ResponseState state;
    if (exe.empty()) return state;
    struct Context { const std::wstring& exe; ResponseState& state; std::map<DWORD, bool> owned; } ctx{exe, state, {}};
    EnumWindows([](HWND h, LPARAM param) -> BOOL {
        auto& c = *reinterpret_cast<Context*>(param);
        if (!IsWindowVisible(h)) return TRUE;
        DWORD pid = 0;
        const DWORD thread = GetWindowThreadProcessId(h, &pid);
        auto [it, fresh] = c.owned.try_emplace(pid, false);
        if (fresh) {
            if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                wchar_t path[32768]; DWORD size = DWORD(std::size(path));
                it->second = QueryFullProcessImageNameW(process, 0, path, &size) && _wcsicmp(path, c.exe.c_str()) == 0;
                CloseHandle(process);
            }
        }
        if (!it->second) return TRUE;
        c.state.visible.insert(h);
        if (GetForegroundWindow() == h) c.state.foreground = h;
        GUITHREADINFO gui{sizeof(gui)};
        if (GetGUIThreadInfo(thread, &gui) && (gui.flags & GUI_INMENUMODE) && gui.hwndMenuOwner) c.state.menus.insert(gui.hwndMenuOwner);
        wchar_t cls[32]{};
        if (GetClassNameW(h, cls, 32) && wcscmp(cls, L"#32768") == 0) c.state.menus.insert(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return state;
}
bool ResponseChanged(const ResponseState& before, const ResponseState& after, tray::Action action)
{
    for (HWND h : after.menus) if (!before.menus.contains(h)) return true;
    for (HWND h : after.visible) if (!before.visible.contains(h)) return true;
    return action != tray::Action::Right && after.foreground && before.foreground != after.foreground;
}
bool ForegroundExisting(const std::wstring& exe)
{
    const auto state = CaptureResponse(exe);
    for (HWND h : state.visible) {
        if (GetWindow(h, GW_OWNER) || (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) continue;
        if (IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
        SetForegroundWindow(h);
        for (int i = 0; i < 5; ++i) {
            if (GetForegroundWindow() == h && !IsIconic(h)) return true;
            Sleep(20);
        }
    }
    return false;
}

} // namespace

std::wstring TrayModel::Title(const std::wstring& tip)
{
    return tray::Title(tip);
}

TrayModel::TrayModel(HWND notify, UINT msg, std::function<std::vector<PinnedApp>()> appNames)
    : notify_(notify), msg_(msg), appNames_(std::move(appNames))
{
    thread_ = std::thread([this] { Worker(); });
}

TrayModel::~TrayModel()
{
    {
        std::lock_guard g(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void TrayModel::Refresh()
{
    std::lock_guard g(mutex_);
    if (std::none_of(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.refresh; }))
        jobs_.push_back({true, {}});
    cv_.notify_all();
}

void TrayModel::Click(const TrayIcon& icon, bool right)
{
    std::lock_guard g(mutex_);
    jobs_.insert(std::find_if(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.refresh; }),
                 {false, icon, right ? tray::Action::Right : tray::Action::Left});
    cv_.notify_all();
}

void TrayModel::DoubleClick(const TrayIcon& icon)
{
    std::lock_guard g(mutex_);
    jobs_.insert(std::find_if(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.refresh; }), {false, icon, tray::Action::Double});
    cv_.notify_all();
}
void TrayModel::OpenSystemTray()
{
    std::lock_guard g(mutex_);
    jobs_.insert(std::find_if(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.refresh; }), {false, {}, tray::Action::Left, true});
    cv_.notify_all();
}

bool TrayModel::OpenExisting(const std::wstring& exe)
{
    if (exe.empty()) return false;
    for (const TrayIcon& icon : Icons()) {
        if (!icon.exe.empty() && _wcsicmp(AppExecutable(icon.exe).c_str(), exe.c_str()) == 0) {
            AllowSetForegroundWindow(ASFW_ANY);
            Click(icon, false);
            return true;
        }
    }
    // At startup the tray's first read may still be in progress. A resident messenger
    // must not be launched again: enqueue its exact program identity for the worker's fresh lookup.
    std::wstring name = Lower(PathFindFileNameW(exe.c_str()));
    if (name != L"feishu.exe" && name != L"qq.exe" && name != L"weixin.exe" && name != L"wechat.exe")
        return false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    bool running = false;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !running; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, name.c_str()) != 0) continue;
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
            wchar_t path[MAX_PATH * 2]; DWORD size = DWORD(std::size(path));
            running = QueryFullProcessImageNameW(process, 0, path, &size) && _wcsicmp(path, exe.c_str()) == 0;
            CloseHandle(process);
        }
    }
    CloseHandle(snap);
    if (running) {
        TrayIcon icon;
        icon.exe = Lower(exe);
        AllowSetForegroundWindow(ASFW_ANY);
        Click(icon, false);
    }
    return running;
}

std::vector<TrayIcon> TrayModel::Icons() const
{
    std::lock_guard g(mutex_);
    return icons_;
}

bool TrayModel::Loaded() const
{
    std::lock_guard g(mutex_);
    return loaded_;
}

bool TrayModel::Available() const
{
    std::lock_guard g(mutex_);
    return available_;
}

void TrayModel::Worker()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    winrt::com_ptr<IUIAutomation> uia;
    HRESULT created = CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.put()));
    if (FAILED(created)) created = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.put()));
    if (FAILED(created))
        Log(L"tray: UI Automation unavailable");
    if (auto options = uia.try_as<IUIAutomation2>()) {
        options->put_ConnectionTimeout(250);
        options->put_TransactionTimeout(250);
    }
    struct Picture {
        std::wstring exe;
        std::vector<BYTE> snapshot;
        std::shared_ptr<const IconImage> image, inked;
        bool inkedOnLight = false;
    };
    std::map<std::wstring, Picture> pictures;
    for (;;) {
        Job job;
        {
            std::unique_lock g(mutex_);
            cv_.wait(g, [&] { return stop_ || !jobs_.empty(); });
            if (stop_)
                break;
            job = jobs_.front();
            jobs_.pop_front();
        }
        if (!uia) {
            if (!job.refresh) PostMessageW(notify_, msg_, ShowOverflow(nullptr) ? 2 : 3, 0);
            else { std::lock_guard g(mutex_); loaded_ = true; available_ = false; }
            continue;
        }
        try {
            bool available = false;
            const ULONGLONG findStart = GetTickCount64();
            if (job.systemTray) {
                const bool shown = ShowOverflow(uia.get());
                PostMessageW(notify_, msg_, shown ? 2 : 3, 0);
                continue;
            }
            auto session = ReadShellSession();
            std::vector<Found> buttons = FindButtons(uia.get(), available);
            const ULONGLONG readMs = GetTickCount64() - findStart;
            auto appNames = appNames_ ? appNames_() : std::vector<PinnedApp>{};
            if (!job.refresh) {
                bool ordered = false;
                std::vector<Record> records;
                auto match = MatchButtons(buttons, records, ordered, appNames);
                auto resolve = [&]() -> Found* {
                    std::vector<tray::TargetKey> candidates;
                    for (size_t b = 0; b < buttons.size(); ++b) {
                        candidates.push_back({session, buttons[b].runtimeId, buttons[b].recordId,
                            match[b] >= 0 ? records[size_t(match[b])].exe : L"", buttons[b].tip});
                    }
                    const auto resolved = tray::ResolveTarget(job.icon.Target(), ReadShellSession(), candidates);
                    Log(L"tray: click resolve=%s shellPid=%lu shellStarted=%llu shellWindow=%llu",
                        tray::ResolutionName(resolved.reason), DWORD(session.pid), session.started, session.window);
                    return resolved.index == tray::none ? nullptr : &buttons[resolved.index];
                };
                Found* target = resolve();
                HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr), overflow = OverflowWindow();
                APPBARDATA abd{sizeof(abd)};
                BOOL offscreen = TRUE, enabled = FALSE;
                HRESULT state = target && target->element ? target->element->get_CurrentIsOffscreen(&offscreen) : UIA_E_ELEMENTNOTAVAILABLE;
                if (target && target->element) target->element->get_CurrentIsEnabled(&enabled);
                Log(L"tray: click begin action=%s tip='%s' exe='%s' identity=%s source=%s match=%s taskbarVisible=%d autoHide=%d overflowExists=%d overflowVisible=%d offscreen=%d enabled=%d stateHr=0x%08X",
                    job.action == tray::Action::Right ? L"right" : job.action == tray::Action::Double ? L"double" : L"left",
                    Title(job.icon.tip).c_str(), job.icon.exe.c_str(), job.icon.recordId.c_str(),
                    target && target->element ? L"uia" : L"record", target ? tray::MethodName(target->method) : L"missing",
                    IsWindowVisible(taskbar), (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) != 0,
                    overflow != nullptr, IsWindowVisible(overflow), offscreen, enabled, unsigned(state));
                const auto before = CaptureResponse(job.icon.exe);
                auto observe = [&](bool accepted) {
                    const ULONGLONG until = GetTickCount64() + (accepted ? 280 : 40);
                    do {
                        if (ResponseChanged(before, CaptureResponse(job.icon.exe), job.action)) return true;
                        Sleep(20);
                    } while (GetTickCount64() < until);
                    return false;
                };
                auto invoke = [&](tray::Route route) -> int32_t {
                    HRESULT hr = UIA_E_ELEMENTNOTAVAILABLE;
                    if (target && target->element && session == ReadShellSession()) {
                        if (route == tray::Route::Toolbar) hr = ToolbarClick(*target, job.action);
                        else if (route == tray::Route::ContextMenu) {
                            auto e3 = target->element.try_as<IUIAutomationElement3>();
                            hr = e3 ? e3->ShowContextMenu() : E_NOINTERFACE;
                        } else if (route == tray::Route::Invoke) {
                            winrt::com_ptr<IUIAutomationInvokePattern> pattern;
                            hr = target->element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(pattern.put()));
                            if (SUCCEEDED(hr)) hr = pattern ? pattern->Invoke() : E_NOINTERFACE;
                        } else {
                            winrt::com_ptr<IUIAutomationLegacyIAccessiblePattern> pattern;
                            hr = target->element->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(pattern.put()));
                            if (SUCCEEDED(hr)) hr = pattern ? pattern->DoDefaultAction() : E_NOINTERFACE;
                        }
                    }
                    Log(L"tray: click attempt route=%s hr=0x%08X", RouteName(route), unsigned(hr));
                    return int32_t(hr);
                };
                auto result = tray::Dispatch(job.action, target && target->toolbar, invoke, observe);
                // Reacquire only after a definite stale-element error. Generic E_FAIL is not evidence of staleness.
                if (target && target->element && result.outcome == tray::Outcome::Unavailable && !result.attempts.empty() &&
                    result.attempts.front().status == int32_t(UIA_E_ELEMENTNOTAVAILABLE)) {
                    session = ReadShellSession();
                    buttons = FindButtons(uia.get(), available);
                    match = MatchButtons(buttons, records, ordered, appNames);
                    target = resolve();
                    Log(L"tray: click reacquire resolved=%d", target != nullptr);
                    result = tray::Dispatch(job.action, target && target->toolbar, invoke, observe);
                }
                bool observed = result.outcome == tray::Outcome::Observed;
                for (const auto& attempt : result.attempts)
                    Log(L"tray: click evidence route=%s observed=%d", RouteName(attempt.route), attempt.observed);
                // Only an existing visible/minimized main window, and only for a left action.
                if (!observed && job.action != tray::Action::Right && !job.icon.exe.empty()) {
                    observed = ForegroundExisting(job.icon.exe);
                    Log(L"tray: click foreground-existing observed=%d", observed);
                }
                const bool shown = !observed && ShowOverflow(uia.get());
                Log(L"tray: click end result=%s elapsed=%llu ms", observed ? L"observed" : shown ? L"system-tray" : L"needs-user",
                    GetTickCount64() - findStart);
                PostMessageW(notify_, msg_, observed ? 1 : shown ? 2 : 3, 0);
                continue;
            }

            bool ordered = false;
            std::vector<Record> records;
            const ULONGLONG readStart = findStart;
            ReadDiagnostics diagnostics;
            std::vector<int> match = MatchButtons(buttons, records, ordered, appNames, &diagnostics);
            if (session != ReadShellSession()) {
                Log(L"tray: discarded refresh spanning shell replacement");
                Refresh();
                continue;
            }
            if (!session.Valid()) {
                buttons.clear(); match.clear();
                available = false;
            }

            std::vector<TrayIcon> icons;
            std::set<std::wstring> livePictures;
            int recognised = 0;
            for (size_t b = 0; b < buttons.size(); ++b) {
                TrayIcon t;
                t.tip = buttons[b].tip;
                t.promoted = buttons[b].promoted;
                t.index = buttons[b].index;
                t.recordId = buttons[b].recordId;
                t.runtimeId = buttons[b].runtimeId;
                t.shell = session;
                t.matchMethod = tray::MethodName(buttons[b].method);
                t.fromRecord = !buttons[b].element;
                if (match[b] >= 0) {
                    const Record& r = records[size_t(match[b])];
                    t.exe = r.exe;
                    ++recognised;
                    const std::wstring& key = r.id;
                    livePictures.insert(key);
                    auto& pic = pictures[key];
                    if (!pic.image || pic.exe != r.exe || pic.snapshot != r.png) {
                        IconImage img;
                        if (!LoadAtriumIcon(r.exe, kIconPx, img) && (r.png.empty() || !DecodePng(r.png, img)) && !LoadShellImage(r.exe, kIconPx, img))
                            img = {};
                        pic.exe = r.exe;
                        pic.snapshot = r.png;
                        const int tone = img.empty() ? 0 : GlyphTone(img);
                        pic.inked = tone ? std::make_shared<const IconImage>(InkedGlyph(img, tone)) : nullptr;
                        if (tone)
                            Log(L"tray: %s draws a %s glyph: it follows the ink", PathFindFileNameW(r.exe.c_str()), tone > 0 ? L"white" : L"black");
                        pic.inkedOnLight = tone > 0;
                        pic.image = img.empty() ? nullptr : std::make_shared<const IconImage>(std::move(img));
                    }
                    t.image = pic.image;
                    t.inked = pic.inked;
                    t.inkedOnLight = pic.inkedOnLight;
                }
                icons.push_back(std::move(t));
            }
            std::erase_if(pictures, [&](const auto& item) { return !livePictures.contains(item.first); });
            bool changed;
            {
                std::lock_guard g(mutex_);
                changed = !loaded_ || available != available_ || icons.size() != icons_.size() ||
                          !std::equal(icons.begin(), icons.end(), icons_.begin(), [](const TrayIcon& a, const TrayIcon& b) {
                              return a.tip == b.tip && a.exe == b.exe && a.image == b.image && a.promoted == b.promoted && a.recordId == b.recordId && a.runtimeId == b.runtimeId && a.shell == b.shell && a.matchMethod == b.matchMethod && a.fromRecord == b.fromRecord;
                          });
                icons_ = std::move(icons);
                loaded_ = true;
                available_ = available;
            }
            if (changed) {
                Log(L"tray: reconcile records=%zu live=%zu aliases=%zu sharedPlace=%zu unknown=%zu uiaMs=%llu",
                    diagnostics.records, diagnostics.live, diagnostics.aliases, diagnostics.shared, diagnostics.unknown, readMs);
                Log(L"tray: %zu icons (%d recognised %s%s) in %llu ms%s", buttons.size(), recognised, ordered ? L"in Windows' order" : L"by name",
                    std::any_of(buttons.begin(), buttons.end(), [](const Found& f) { return f.toolbar != nullptr; }) ? L", classic tray" : L"",
                    GetTickCount64() - readStart, available ? L"" : L", the shell's tray was not found");
                for (size_t b = 0; b < buttons.size(); ++b) {
                    std::wstring exe = match[b] >= 0 ? records[size_t(match[b])].exe : L"-";
                    Log(L"tray: item=%zu source=%s match=%s promoted=%d index=%d identity=%s tip='%s' exe='%s'", b,
                        buttons[b].element ? L"uia" : L"record", tray::MethodName(buttons[b].method), buttons[b].promoted,
                        buttons[b].index, buttons[b].recordId.c_str(), Title(buttons[b].tip).c_str(), exe.c_str());
                }
                PostMessageW(notify_, msg_, 0, 0);
            } else if (GetTickCount64() - readStart > 500)
                Log(L"tray: slow unchanged refresh totalMs=%llu uiaMs=%llu", GetTickCount64() - readStart, readMs);
        } catch (...) {
            Log(L"tray: worker exception job=%s hr=0x%08X", job.refresh ? L"refresh" : L"click", unsigned(winrt::to_hresult()));
            if (!job.refresh) PostMessageW(notify_, msg_, 3, 0);
        }
    }
    uia = nullptr;
    winrt::uninit_apartment();
}

} // namespace atrium
