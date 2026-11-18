#include "shell/win_key.h"

#include "log.h"
#include "shell/keyboard_hook.h"

namespace atrium {

namespace {

// The window in front runs elevated: keys Atrium sends would not reach it.
bool ForegroundElevated()
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg || !GetWindowThreadProcessId(fg, &pid) || !pid)
        return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return true;
    bool elevated = true; // its token closed to us: elevated (or protected)
    HANDLE token = nullptr;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e{};
        DWORD size = 0;
        elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated;
        CloseHandle(token);
    }
    CloseHandle(process);
    return elevated;
}

INPUT Key(DWORD vk, DWORD scan, bool extended)
{
    INPUT i{};
    i.type = INPUT_KEYBOARD;
    i.ki.wVk = WORD(vk);
    i.ki.wScan = WORD(scan);
    i.ki.dwFlags = extended ? KEYEVENTF_EXTENDEDKEY : 0;
    i.ki.dwExtraInfo = kAtriumSyntheticKey;
    return i;
}

} // namespace

WinKey::WinKey(HWND notify, UINT msg, bool alone, bool numbers) : notify_(notify), msg_(msg), alone_(alone), numbers_(numbers)
{
    keyToken_ = KeyboardHook::Instance().Add(&WinKey::OnKey, this);
    Log(L"winkey: watching the Windows key (%s%s%s)", alone_ ? L"start menu" : L"", alone_ && numbers_ ? L", " : L"", numbers_ ? L"Win + digits" : L"");
}

WinKey::~WinKey() { KeyboardHook::Instance().Remove(keyToken_); }

bool WinKey::OnKey(void* context, DWORD vk, DWORD scan, bool extended, bool down, bool /*injected*/, ULONG_PTR extraInfo)
{
    WinKey* self = static_cast<WinKey*>(context);
    if (!self || extraInfo == kAtriumSyntheticKey)
        return false; // Atrium's own replay: let it through
    const bool win = vk == VK_LWIN || vk == VK_RWIN;
    if (win && down) {
        if (!self->held_) { // not the key's auto-repeat
            self->held_ = true;
            self->used_ = false;
            self->key_ = vk;
            self->scan_ = scan;
            self->extended_ = extended;
            self->passed_ = ForegroundElevated();
        }
        return !self->passed_; // held back
    }
    if (win) {
        if (self->held_ && vk == self->key_) {
            const bool held = !self->passed_;
            self->held_ = false;
            if (held) { // Windows never saw it go down
                if (self->used_)
                    return true;
                if (self->alone_) { // no start menu of its own
                    PostMessageW(self->notify_, self->msg_, 0, 0);
                    return true;
                }
                // Only the digits are ours: Windows' start menu, as if never held.
                INPUT keys[2] = {Key(self->key_, self->scan_, self->extended_), Key(self->key_, self->scan_, self->extended_)};
                keys[1].ki.dwFlags |= KEYEVENTF_KEYUP;
                SendInput(2, keys, sizeof(INPUT));
                return true;
            }
        }
        return false;
    }
    if (self->digit_ && vk == self->digit_) { // the kept digit: its repeats and its release
        if (!down)
            self->digit_ = 0;
        return true;
    }
    if (down && self->held_ && !self->passed_) {
        const bool digit = vk >= '0' && vk <= '9';
        const bool modified = (GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU)) & 0x8000;
        if (digit && self->numbers_ && !modified) {
            self->used_ = true;
            self->digit_ = vk;
            PostMessageW(self->notify_, self->msg_, vk == '0' ? 10 : vk - '0', 0);
            return true;
        }
        // Another key: a shortcut. The Windows key goes on to Windows first, then this key.
        self->passed_ = true;
        INPUT keys[2] = {Key(self->key_, self->scan_, self->extended_), Key(vk, scan, extended)};
        SendInput(2, keys, sizeof(INPUT));
        return true;
    }
    return false;
}

} // namespace atrium
