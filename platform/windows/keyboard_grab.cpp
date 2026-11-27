#include "keyboard_grab.h"
#include "desktop_input.h"

namespace wwmm {
thread_local WindowsKeyboardGrab *WindowsKeyboardGrab::current_ = nullptr;

WindowsKeyboardGrab::WindowsKeyboardGrab(HWND surface, bool acceptsText)
    : surface_(surface), acceptsText_(acceptsText)
{
}

WindowsKeyboardGrab::~WindowsKeyboardGrab()
{
    requestInterruption();
    quit();
    wait();
}

void WindowsKeyboardGrab::run()
{
    current_ = this;
    constexpr int keys[] = {VK_LCONTROL, VK_LSHIFT, VK_LMENU, VK_LWIN,
                            VK_RCONTROL, VK_RSHIFT, VK_RMENU, VK_RWIN};
    for (int i = 0; i < 8; ++i)
        if (GetAsyncKeyState(keys[i]) & 0x8000)
            preexisting_ |= quint8(1u << i);
    const auto hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardProc, GetModuleHandleW(nullptr), 0);
    if (hook) {
        installed_.store(true);
        if (!isInterruptionRequested())
            exec();
        installed_.store(false);
        UnhookWindowsHookEx(hook);
    } else {
        emit unavailable();
    }
    current_ = nullptr;
}

bool WindowsKeyboardGrab::hasFocus() const
{
    const HWND foreground = GetForegroundWindow();
    if (!foreground || foreground != GetAncestor(surface_, GA_ROOT))
        return false;
    GUITHREADINFO info{sizeof(info)};
    return GetGUIThreadInfo(0, &info) && !(info.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)) &&
           (info.hwndFocus == surface_ || IsChild(surface_, info.hwndFocus));
}

LRESULT CALLBACK WindowsKeyboardGrab::keyboardProc(int code, WPARAM message, LPARAM data)
{
    auto *grab = current_;
    if (code == HC_ACTION && grab && !grab->isInterruptionRequested() && grab->hasFocus()) {
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        if (grab->handle(*reinterpret_cast<const KBDLLHOOKSTRUCT *>(data), down))
            return 1;
    }
    return CallNextHookEx(nullptr, code, message, data);
}

bool WindowsKeyboardGrab::handle(const KBDLLHOOKSTRUCT &event, bool down)
{
    // Windows marks the synthetic companion Ctrl for AltGr with the 0x200 scan bit.
    // The physical right Alt is a single right Command on the Mac.
    if (event.vkCode == VK_LCONTROL && (event.scanCode & 0x200))
        return !preexisting_ && !releasing_;
    const int scan = int(event.scanCode & 0xff) | ((event.flags & LLKHF_EXTENDED) ? 0x100 : 0);
    const quint8 usage = event.vkCode == VK_PAUSE ? 72 : hidKey(DesktopOS::windows, scan);
    if (!usage || nativeKeyCode(DesktopOS::macOS, usage) < 0)
        return false;
    const auto bit = modifierBit(usage);
    if (preexisting_) {
        if (down)
            preexisting_ |= bit;
        else
            preexisting_ &= ~bit;
        return false;
    }
    auto &route = routes_[usage];
    if (releasing_)
        return route == Route::Remote;
    if (bit) {
        if (down)
            modifiers_ |= bit;
        else
            modifiers_ &= ~bit;
    }
    if (route == Route::Up) {
        if (!down)
            return false;
        // Also pass physical Shift to the Windows IME for composing uppercase text.
        const bool functionKey = (usage >= 58 && usage <= 69) || (usage >= 104 && usage <= 115);
        const bool plainText = acceptsText_.load() && !(modifiers_ & 0xdd) && !functionKey;
        route = plainText || (bit & 0x22) ? Route::Local : Route::Remote;
    }
    const bool remote = route == Route::Remote;
    if (down && usage == 69 && (modifiers_ & 0x11) && (modifiers_ & 0x22) && (modifiers_ & 0x44) &&
        !(modifiers_ & 0x88)) {
        releasing_ = true;
        emit releaseRequested();
        return true;
    }
    if (remote || (bit & 0x22))
        emit key(usage, down, modifiers_);
    if (!down)
        route = Route::Up;
    return remote;
}
} // namespace wwmm

// Keep Win32 headers out of the shared widgets moc unit, which also includes OpenSSL.
#include "moc_keyboard_grab.cpp"
