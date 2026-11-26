#include "native_input.h"
#include <windows.h>
#include <uiautomationclient.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace wwmm {
namespace {
INPUT keyInput(WORD scan, DWORD flags)
{
    INPUT i{};
    i.type = INPUT_KEYBOARD;
    i.ki.wScan = scan;
    i.ki.dwFlags = flags;
    return i;
}

INPUT mouseInput(DWORD flags, DWORD data = 0)
{
    INPUT i{};
    i.type = INPUT_MOUSE;
    i.mi.dwFlags = flags;
    i.mi.mouseData = data;
    return i;
}

INPUT moveTo(ScreenPoint p)
{
    auto i = mouseInput(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK);
    auto axis = [](quint16 u, int primary, int origin, int span) {
        const qint64 px = std::llround(u * (primary - 1) / 65535.) - origin;
        return LONG(std::clamp<qint64>((px * 65536 + 32768) / std::max(1, span), 0, 65535));
    };
    i.mi.dx = axis(p.x, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_XVIRTUALSCREEN),
                   GetSystemMetrics(SM_CXVIRTUALSCREEN));
    i.mi.dy = axis(p.y, GetSystemMetrics(SM_CYSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                   GetSystemMetrics(SM_CYVIRTUALSCREEN));
    return i;
}

class WinInput final : public InputSink {
  public:
    bool key(quint8 usage, bool down, bool) override
    {
        const int code = nativeKeyCode(DesktopOS::windows, usage);
        if (code < 0)
            return true;
        INPUT i =
            keyInput(WORD(code & 255), KEYEVENTF_SCANCODE | ((code & 0x100) ? KEYEVENTF_EXTENDEDKEY : 0) |
                                           (down ? 0 : KEYEVENTF_KEYUP));
        if (usage == 72) {
            i.ki.wVk = VK_PAUSE;
            i.ki.wScan = 0;
            i.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        }
        return SendInput(1, &i, sizeof(INPUT)) == 1;
    }

    quint8 letterKey(quint8 letter) override
    {
        auto layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
        auto scan = MapVirtualKeyExW(letter, MAPVK_VK_TO_VSC_EX, layout);
        return hidKey(DesktopOS::windows, int(scan & 255) | ((scan & 0xff00) == 0xe000 ? 0x100 : 0));
    }

    bool text(const QString &s) override
    {
        QList<INPUT> batch;
        batch.reserve(s.size() * 2);
        for (QChar c : s) {
            batch.append(keyInput(c.unicode(), KEYEVENTF_UNICODE));
            batch.append(keyInput(c.unicode(), KEYEVENTF_UNICODE | KEYEVENTF_KEYUP));
        }
        const UINT n = UINT(batch.size());
        return !n || SendInput(n, batch.data(), sizeof(INPUT)) == n;
    }

    bool pointer(ScreenPoint p, MouseButton b, InputKind kind) override
    {
        INPUT events[2] = {moveTo(p), {}};
        if (kind == InputKind::Move)
            return SendInput(1, events, sizeof(INPUT)) == 1;
        const int index = int(b) - 1;
        constexpr DWORD down[] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN,
                                  MOUSEEVENTF_XDOWN, MOUSEEVENTF_XDOWN};
        constexpr DWORD up[] = {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP,
                                MOUSEEVENTF_XUP, MOUSEEVENTF_XUP};
        events[1] =
            mouseInput(kind == InputKind::ButtonDown ? down[index] : up[index], index == 3   ? XBUTTON1
                                                                                : index == 4 ? XBUTTON2
                                                                                             : 0);
        return SendInput(2, events, sizeof(INPUT)) == 2;
    }

    bool wheel(ScreenPoint p, qint16 x, qint16 y, bool pixels) override
    {
        INPUT events[3] = {moveTo(p), {}, {}};
        UINT n = 1;
        const int factor = pixels ? 3 : 1;
        if (y)
            events[n++] = mouseInput(MOUSEEVENTF_WHEEL, DWORD(y * factor));
        // Qt horizontal deltas are positive to the left; Windows wheel messages are positive to the right.
        if (x)
            events[n++] = mouseInput(MOUSEEVENTF_HWHEEL, DWORD(-x * factor));
        return SendInput(n, events, sizeof(INPUT)) == n;
    }
};

struct Automation {
    HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Microsoft::WRL::ComPtr<IUIAutomation> ui;

    Automation()
    {
        if (SUCCEEDED(apartment))
            CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ui));
    }

    ~Automation()
    {
        ui.Reset();
        if (SUCCEEDED(apartment))
            CoUninitialize();
    }
};

QString windowClass(HWND hwnd)
{
    wchar_t name[256]{};
    return QString::fromWCharArray(name, GetClassNameW(hwnd, name, 256)).toLower();
}
} // namespace

bool allowRemoteInput(QString &)
{
    return true;
}

std::unique_ptr<InputSink> createInputSink()
{
    return std::make_unique<WinInput>();
}

InputFocus focusedInputKind()
{
    HWND front = GetForegroundWindow();
    if (!front)
        return InputFocus::Raw;
    DWORD pid = 0;
    const auto thread = GetWindowThreadProcessId(front, &pid);
    const auto top = windowClass(front);
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    const auto focus = GetGUIThreadInfo(thread, &info) && info.hwndFocus ? windowClass(info.hwndFocus) : top;
    if (info.hwndFocus && (focus == "edit" || focus.startsWith("richedit")) &&
        (GetWindowLongPtrW(info.hwndFocus, GWL_STYLE) & ES_PASSWORD))
        return InputFocus::Raw;
    QString exe;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        wchar_t path[1024];
        DWORD n = 1024;
        if (QueryFullProcessImageNameW(process, 0, path, &n))
            exe = QString::fromWCharArray(path, int(n)).section(u'\\', -1).toLower();
        CloseHandle(process);
    }
    if (top == "cascadia_hosting_window_class" || top == "consolewindowclass" ||
        top == "virtualconsoleclass" || exe == "windowsterminal.exe" || exe == "wezterm-gui.exe" ||
        exe == "alacritty.exe")
        return InputFocus::Terminal;
    thread_local Automation a;
    Microsoft::WRL::ComPtr<IUIAutomationElement> el;
    if (a.ui && SUCCEEDED(a.ui->GetFocusedElement(&el)) && el) {
        BOOL password = FALSE;
        if (SUCCEEDED(el->get_CurrentIsPassword(&password)) && password)
            return InputFocus::Raw;
        CONTROLTYPEID type = 0;
        if (SUCCEEDED(el->get_CurrentControlType(&type)) &&
            (type == UIA_EditControlTypeId || type == UIA_DocumentControlTypeId))
            return InputFocus::Text;
    }
    if (focus == "edit" || focus.startsWith("richedit") || focus.startsWith("scintilla") || focus == "_wwg" ||
        exe == "winword.exe" || exe == "excel.exe" || exe == "notepad.exe" || exe == "notepad++.exe")
        return InputFocus::Text;
    if (exe == "explorer.exe" || exe == "msedge.exe" || exe == "chrome.exe" || exe == "firefox.exe" ||
        exe == "powerpnt.exe" || exe == "outlook.exe" || exe == "code.exe")
        return InputFocus::Office;
    return InputFocus::Raw;
}
} // namespace wwmm
