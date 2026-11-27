"""Check the launch checkbox in the packaged Inno Setup wizard on a Windows CI runner."""

import ctypes
from ctypes import wintypes as win
import subprocess
import time

user = ctypes.windll.user32
visit_window = ctypes.WINFUNCTYPE(win.BOOL, win.HWND, win.LPARAM)
user.SendMessageW.argtypes = (win.HWND, win.UINT, win.WPARAM, win.LPARAM)
user.SendMessageW.restype = win.LPARAM
user.GetParent.restype = win.HWND
user.PostMessageW.argtypes = (win.HWND, win.UINT, win.WPARAM, win.LPARAM)


def visible(parent, class_name):
    handles = []
    name = ctypes.create_unicode_buffer(128)

    @visit_window
    def visit(handle, _):
        window = win.HWND(handle)
        if user.IsWindowVisible(window) and user.GetClassNameW(window, name, len(name)) and name.value == class_name:
            handles.append(window)
        return True

    user.EnumChildWindows(parent, visit, 0)
    return handles


def button_text(button):
    buf = ctypes.create_unicode_buffer(128)
    user.SendMessageW(button, 0x000D, len(buf), ctypes.addressof(buf))  # WM_GETTEXT
    return buf.value.replace("&", "")


def click(button):
    # Notify only this wizard's button parent. The CI desktop may be covered.
    if not user.PostMessageW(user.GetParent(button), 0x0111, user.GetDlgCtrlID(button) & 0xffff, button.value):
        raise ctypes.WinError()  # WM_COMMAND / BN_CLICKED


def check_launch_option(package, language):
    expected, finish = ("运行 WWMM Remote", "完成") if language == "zh" else ("Launch WWMM Remote", "Finish")
    setup = subprocess.Popen([package, "/SP-", "/NORESTART", f"/LANG={language}"])
    deadline = time.monotonic() + 90
    clicked = set()
    title = ctypes.create_unicode_buffer(256)
    try:
        while setup.poll() is None and time.monotonic() < deadline:
            for form in visible(None, "TWizardForm"):
                user.GetWindowTextW(form, title, len(title))
                if "WWMM Remote" not in title.value:
                    continue
                for button in visible(form, "TNewButton"):
                    if not user.IsWindowEnabled(button):
                        continue
                    caption = button_text(button)
                    if caption.startswith(finish):
                        lists = visible(form, "TNewCheckListBox")
                        if len(lists) != 1 or user.SendMessageW(lists[0], 0x018B, 0, 0) != 1:  # LB_GETCOUNT
                            raise RuntimeError("Expected one launch option on the visible finish page")
                        length = user.SendMessageW(lists[0], 0x018A, 0, 0)  # LB_GETTEXTLEN
                        if not 0 < length < 1024:
                            raise RuntimeError(f"Invalid launch option length {length}")
                        buf = ctypes.create_unicode_buffer(length + 1)
                        user.SendMessageW(lists[0], 0x0189, 0, ctypes.addressof(buf))  # LB_GETTEXT
                        if buf.value != expected:
                            raise RuntimeError(f"Expected {ascii(expected)}, saw {ascii(buf.value)}")
                        click(button)
                        if setup.wait(timeout=30):
                            raise RuntimeError("Installation failed after Finish")
                        print(f"Verified Windows wizard /LANG={language} checkbox {ascii(expected)}", flush=True)
                        return
                    if caption.startswith(("下一步", "安装", "Next", "Install")) and caption not in clicked:
                        clicked.add(caption)
                        click(button)
                        break
            time.sleep(0.1)
        raise RuntimeError(f"Installer failed to reach the {language} finish page after {ascii(sorted(clicked))}")
    finally:
        if setup.poll() is None:
            subprocess.run(["taskkill", "/PID", str(setup.pid), "/T", "/F"], timeout=15)
