// Manual integration probe. No CTest registration: --click changes foreground/menu state.
// Compile the production adapter unchanged; omit task-model artwork/shortcut loading only.
#include "../core/shell/tray_model.cpp"
#include <iostream>

namespace atrium {
std::wstring AppExecutable(const std::wstring& path) { return path; }
bool LoadAtriumIcon(const std::wstring&, int, IconImage&) { return false; }
bool LoadShellImage(const std::wstring&, int, IconImage&, bool) { return false; }
}
namespace {
constexpr UINT kResult = WM_APP + 42;
int completion = -1;
LRESULT CALLBACK ProbeProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == kResult && w) completion = int(w);
    return DefWindowProcW(h, m, w, l);
}
void Pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}
}
int wmain(int argc, wchar_t** argv)
{
    using namespace atrium;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    winrt::com_ptr<IUIAutomation> uia;
    winrt::check_hresult(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.put())));
    if (argc == 2 && std::wstring(argv[1]) == L"--fixture") {
        WNDCLASSW wc{};
        wc.lpfnWndProc = ProbeProc; wc.lpszClassName = L"AtriumTrayFixture"; wc.hInstance = GetModuleHandleW(nullptr);
        RegisterClassW(&wc);
        HWND window = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        struct Fixture {
            NOTIFYICONDATAW uid{}, guid{};
            ~Fixture() { Shell_NotifyIconW(NIM_DELETE, &uid); Shell_NotifyIconW(NIM_DELETE, &guid); }
        } fixture;
        auto& uid = fixture.uid; auto& guid = fixture.guid;
        uid.cbSize = sizeof(uid); uid.hWnd = window; uid.uID = 4242;
        uid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        uid.hIcon = LoadIconW(nullptr, IDI_APPLICATION); uid.uCallbackMessage = WM_APP + 43;
        wcscpy_s(uid.szTip, L"Atrium tray fixture UID");
        guid = uid; guid.uID = 4243; guid.uFlags |= NIF_GUID;
        CLSIDFromString(L"{DCA21D39-29A7-4E50-82A9-D8CA9B6A29F9}", &guid.guidItem);
        wcscpy_s(guid.szTip, L"Atrium tray fixture GUID");
        if (!Shell_NotifyIconW(NIM_ADD, &uid) || !Shell_NotifyIconW(NIM_ADD, &guid)) return 1;
        wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
        auto expect = [&](size_t expected) {
            size_t count = 0;
            for (int i = 0; i < 20; ++i) {
                Pump(); Sleep(50);
                bool available = false, ordered = false;
                auto buttons = FindButtons(uia.get(), available);
                std::vector<Record> records;
                const auto match = MatchButtons(buttons, records, ordered);
                count = 0;
                for (int r : match) if (r >= 0 && _wcsicmp(records[size_t(r)].exe.c_str(), path) == 0) ++count;
                if (count == expected) break;
            }
            std::wcout << L"fixture expected=" << expected << L" actual=" << count << L'\n';
            return count == expected;
        };
        bool ok = expect(2);
        Shell_NotifyIconW(NIM_DELETE, &uid);
        ok = expect(1) && ok;
        Shell_NotifyIconW(NIM_DELETE, &guid);
        ok = expect(0) && ok;
        DestroyWindow(window);
        return ok ? 0 : 1;
    }
    bool available = false, ordered = false;
    auto buttons = FindButtons(uia.get(), available);
    const auto buttonCount = buttons.size();
    std::vector<Record> records;
    const auto begin = GetTickCount64();
    const auto match = MatchButtons(buttons, records, ordered);
    std::wcout << L"buttons=" << buttonCount << L" reconciled=" << buttons.size() << L" records=" << records.size()
               << L" matchMs=" << GetTickCount64() - begin << L" taskbarVisible=" << IsWindowVisible(FindWindowW(L"Shell_TrayWnd", nullptr)) << L'\n';
    for (size_t i = 0; i < buttons.size(); ++i)
        std::wcout << i << L" source=" << (buttons[i].element ? L"uia" : L"record") << L" match=" << tray::MethodName(buttons[i].method)
                   << L" identity=" << buttons[i].recordId << L" exe=" << (match[i] >= 0 ? records[size_t(match[i])].exe : L"?") << L'\n';
    if (argc == 1) return 0; // read-only by default
    if (argc == 2 && std::wstring(argv[1]) == L"--tree") {
        for (HWND host : {FindWindowW(L"Shell_TrayWnd", nullptr), OverflowWindow()}) {
            if (HWND bridge = FindWindowExW(host, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr)) host = bridge;
            winrt::com_ptr<IUIAutomationElement> root;
            winrt::com_ptr<IUIAutomationCondition> all;
            winrt::com_ptr<IUIAutomationElementArray> elements;
            uia->CreateTrueCondition(all.put());
            if (!host || FAILED(uia->ElementFromHandle(host, root.put())) || !root ||
                FAILED(root->FindAll(TreeScope_Descendants, all.get(), elements.put())) || !elements) continue;
            int count = 0; elements->get_Length(&count);
            for (int i = 0; i < count; ++i) {
                winrt::com_ptr<IUIAutomationElement> e;
                elements->GetElement(i, e.put());
                if (!e) continue;
                BSTR cls = nullptr, id = nullptr;
                BOOL offscreen = TRUE; RECT r{};
                e->get_CurrentClassName(&cls); e->get_CurrentAutomationId(&id);
                e->get_CurrentIsOffscreen(&offscreen); e->get_CurrentBoundingRectangle(&r);
                std::wcout << L"class=" << (cls ? cls : L"") << L" id=" << (id ? id : L"") << L" offscreen=" << offscreen
                           << L" rect=" << r.left << L"," << r.top << L"," << r.right << L"," << r.bottom << L'\n';
                SysFreeString(cls); SysFreeString(id);
            }
        }
        return 0;
    }
    if (argc != 4 || (std::wstring(argv[1]) != L"--click" && std::wstring(argv[1]) != L"--visible-click") ||
        (std::wstring(argv[3]) != L"left" && std::wstring(argv[3]) != L"right" && std::wstring(argv[3]) != L"double")) {
        std::wcerr << L"usage: tray_live_probe [--click FULL_EXE_PATH left|right|double]\n";
        return 2;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = ProbeProc; wc.lpszClassName = L"AtriumTrayProbe"; wc.hInstance = GetModuleHandleW(nullptr);
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    int result = 2;
    {
        TrayModel model(window, kResult);
        model.Refresh();
        ULONGLONG deadline = GetTickCount64() + 10000;
        while (!model.Loaded() && GetTickCount64() < deadline) { Pump(); Sleep(10); }
        const auto icons = model.Icons();
        const TrayIcon* chosen = nullptr;
        for (const auto& icon : icons) if (_wcsicmp(icon.exe.c_str(), argv[2]) == 0) {
            if (chosen) { std::wcerr << L"Ambiguous executable; refusing to choose an icon\n"; return 2; }
            chosen = &icon;
        }
        if (!chosen) std::wcerr << L"No live icon; no application was launched\n";
        else {
            if (std::wstring(argv[1]) == L"--visible-click") {
                std::wcout << L"prepared=" << ShowOverflow(uia.get()) << L'\n';
            }
            if (std::wstring(argv[3]) == L"double") model.DoubleClick(*chosen);
            else model.Click(*chosen, std::wstring(argv[3]) == L"right");
            deadline = GetTickCount64() + 10000;
            while (completion < 0 && GetTickCount64() < deadline) { Pump(); Sleep(10); }
            std::wcout << L"completion=" << completion << L" (1=observed,2=system-popup,3=needs-user)\n";
            result = completion == 1 ? 0 : 1;
        }
    }
    DestroyWindow(window);
    return result;
}
