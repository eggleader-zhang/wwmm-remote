"""Install the Qt base SDK used by the release runners."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess

VERSION = "6.12.1"
ROOT = "https://download.qt.io/online/qtsdkrepository"
MAC_KIT = "mac_x64/desktop/qt6_6121/qt6_6121/qt.qt6.6121.clang_64"
WIN_KIT = "windows_x86/desktop/qt6_6121/qt6_6121_{kit}/qt.qt6.6121.win64_{kit}"
WIN_BASE = "6.12.1-0-202611180427qtbase-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2"
MAC_BASE = "6.12.1-0-202611180803qtbase-MacOS-MacOS_26-Clang-MacOS-MacOS_26-X86_64-ARM64.7z"

SDK = {
    "macos": (f"{MAC_KIT}/{MAC_BASE}", "e62976c77ce878b1aa04eac204a5e77eafe08cba7aae7fbf545f00b7df12978c"),
    "windows-x64": (f"{WIN_KIT.format(kit='msvc2022_64')}/{WIN_BASE}-X86_64.7z",
                    "7e96dac9569f350c4e328be9f4a53ed18175ebfeec3e997cdd16832582ceb71e"),
    "windows-arm64": (f"{WIN_KIT.format(kit='msvc2022_arm64_cross_compiled')}/{WIN_BASE}-ARM64.7z",
                      "27e47d61aeb15735916cad1238ea2419111896dff0575ebdc1ad7e001a0862e3"),
}
LICENSES = {
    "LGPL-3.0-only.txt": "da7eabb7bafdf7d3ae5e9f223aa5bdc1eece45ac569dc21b3b037520b4464768",
    "GPL-3.0-only.txt": "8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903",
}


def download(url, output, expected):
    if not output.exists() or hashlib.sha256(output.read_bytes()).hexdigest() != expected:
        subprocess.run(["curl", "--fail", "--silent", "--show-error", "--location", "--retry", "3",
                        "--connect-timeout", "20", "--max-time", "360", "--output", output, url], check=True)
        if hashlib.sha256(output.read_bytes()).hexdigest() != expected:
            raise RuntimeError(f"SHA-256 mismatch for {output.name}")


def install_sdk(target, directory):
    key = "macos" if target.startswith("macos-") else target
    path, digest = SDK[key]
    directory.mkdir(parents=True, exist_ok=True)
    archive = directory / f"qt-{VERSION}-{key}.7z"
    download(f"{ROOT}/{path}", archive, digest)
    kit = directory / f"qt-{VERSION}-{key}"
    kit.mkdir(exist_ok=True)
    if os.name == "nt":
        seven_zip = shutil.which("7z") or str(Path(os.environ["ProgramFiles"]) / "7-Zip/7z.exe")
        subprocess.run([seven_zip, "x", "-y", "-bso0", "-bsp0", f"-o{kit}", archive], check=True)
    else:
        subprocess.run(["/usr/bin/tar", "-xf", archive, "-C", kit], check=True)
    # Extracted SDKs can carry paths from the Qt build machine. Keep host tools relocatable.
    (kit / "bin/qt.conf").write_text("[Paths]\nPrefix=..\n", encoding="utf-8")
    return kit


def install_license_files(notices):
    for name, digest in LICENSES.items():
        download(f"https://raw.githubusercontent.com/qt/qtbase/v{VERSION}/LICENSES/{name}",
                 notices / f"Qt-{name}", digest)
