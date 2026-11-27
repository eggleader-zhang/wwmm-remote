"""Exercise the exact offline installer on a disposable native Actions runner."""

import base64
import ctypes
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

APP = "WWMM Remote"
BUNDLE = APP + ".app"


def run(*cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def read_identity(windows, profile):
    if windows:
        return (profile / "identity.dpapi").read_bytes()
    return subprocess.check_output(
        [
            "security",
            "find-generic-password",
            "-s",
            "dev.wwmm.remote.pairing.v1",
            "-a",
            "local-device",
            "-w",
        ]
    ).strip()


def seed_profile(windows, profile):
    if profile.exists():
        raise RuntimeError("Migration test requires a clean runner profile")
    profile.mkdir(parents=True)
    key = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
    record = json.dumps(
        {
            "version": 1,
            "seed": base64.b64encode(os.urandom(32)).decode(),
            "peers": {key: "Office PC"},
            "directAccess": {key: True},
        },
        separators=(",", ":"),
    )
    if windows:
        script = (
            "[console]::InputEncoding=[Text.Encoding]::UTF8;Add-Type -AssemblyName System.Security;"
            "$p=[Text.Encoding]::UTF8.GetBytes([Console]::In.ReadToEnd());"
            "$b=[Security.Cryptography.ProtectedData]::Protect($p,$null,'CurrentUser');"
            "[IO.File]::WriteAllBytes($env:WWMM_QA_VAULT,$b)"
        )
        run(
            "powershell",
            "-NoProfile",
            "-Command",
            script,
            input=record.encode(),
            env=dict(os.environ, WWMM_QA_VAULT=str(profile / "identity.dpapi")),
        )
    else:
        exists = subprocess.run(
            ["security", "find-generic-password", "-s", "dev.wwmm.remote.pairing.v1", "-a", "local-device"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if exists.returncode == 0:
            raise RuntimeError("Existing Keychain identity on installation runner")
        run(
            "security",
            "add-generic-password",
            "-A",
            "-s",
            "dev.wwmm.remote.pairing.v1",
            "-a",
            "local-device",
            "-w",
            record,
        )
    (profile / "displays.ini").write_bytes(b"[General]\nfixture=external-display\n")


def start_ui(executable):
    proc = subprocess.Popen([executable])
    try:
        time.sleep(3)
        if proc.poll() is not None:
            raise RuntimeError(
                f"Installed UI exited before reaching the background event loop: {proc.returncode}"
            )
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=15)


def main(target, version):
    if os.getenv("GITHUB_ACTIONS") != "true" or os.getenv("RUNNER_ENVIRONMENT") == "self-hosted":
        raise RuntimeError("Use a disposable GitHub-hosted runner for the installation test")
    windows = target.startswith("windows")
    build = int(os.environ["GITHUB_RUN_NUMBER"])
    profile = (
        (Path(os.environ["LOCALAPPDATA"]) if windows else Path.home() / "Library/Application Support")
        / "WWMM"
        / APP
    )
    seed_profile(windows, profile)
    identity = read_identity(windows, profile)
    display = (profile / "displays.ini").read_bytes()
    with tempfile.TemporaryDirectory() as work:
        root = Path(work)
        archive = next(Path("previous").glob("*.zip"))
        if windows:
            with zipfile.ZipFile(archive) as z:
                z.extractall(root / "old")
            previous = root / "old" / APP / (APP + ".exe")
        else:
            run("ditto", "-x", "-k", archive, root / "old")
            previous = root / "old" / BUNDLE / "Contents/MacOS" / APP
        start_ui(previous)
        if read_identity(windows, profile) != identity:
            raise RuntimeError("Legacy pairing was not preserved by the old edition")
        if windows:
            package = Path("packages/WWMM-Remote-Setup.exe").resolve()
            dest = Path(os.environ["LOCALAPPDATA"]) / "Programs" / APP
            if dest.exists():
                raise RuntimeError("Existing Windows installation")
            run(package, "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART")
            exe = dest / (APP + ".exe")
            hp = dest / "wwmm-update.exe"
            if not list(dest.glob("unins*.exe")):
                raise RuntimeError("Missing uninstaller")
            shortcut = (
                Path(os.environ["APPDATA"]) / "Microsoft/Windows/Start Menu/Programs" / (APP + ".lnk")
            )
            if not shortcut.is_file():
                raise RuntimeError("Missing Start menu shortcut")
        else:
            package = Path("packages/WWMM-Remote.dmg").resolve()
            mount = root / "volume"
            mount.mkdir()
            dest = root / "Applications" / BUNDLE
            dest.parent.mkdir()
            run("hdiutil", "attach", "-readonly", "-noautoopen", "-nobrowse", "-mountpoint", mount, package)
            try:
                if os.readlink(mount / "Applications") != "/Applications":
                    raise RuntimeError("Missing Applications alias")
                run("ditto", mount / BUNDLE, dest)
            finally:
                run("hdiutil", "detach", mount)
            run("codesign", "--verify", "--strict", "--deep", "--all-architectures", dest)
            exe = dest / "Contents/MacOS" / APP
            hp = dest / "Contents/Helpers/wwmm-update"
            run("lipo", exe, "-verify_arch", "arm64", "x86_64")
        info = json.loads(subprocess.check_output([exe, "--installation-info"]))
        expected = "arm64" if target.endswith("arm64") else "x86_64"
        if (info["Build"], info["CPU"], info["Version"]) != (build, expected, version):
            raise RuntimeError(info)
        start_ui(exe)
        helper = root / hp.name
        shutil.copy2(hp, helper)
        stage = root / "staged"
        stage.mkdir()
        manifest = json.loads(
            subprocess.check_output([helper, "prepare", package, stage, str(build - 1), dest])
        )
        if manifest["Build"] != build:
            raise RuntimeError("Wrong offline manifest")
        evidence = stage / "release.sigstore.json"
        original = evidence.read_bytes()
        evidence.write_bytes(b"{}")
        bogus = subprocess.run([helper, "apply", dest, stage, str(build - 1), str(os.getpid()), "check"])
        if bogus.returncode == 0 or (stage / "ready").exists():
            raise RuntimeError("Unsigned update accepted")
        evidence.write_bytes(original)
        payload = stage / ("install.exe" if windows else f"{BUNDLE}/Contents/Info.plist")
        original_size = payload.stat().st_size
        with payload.open("ab") as f:
            f.write(b"wrong release content")
        rejected = subprocess.run([helper, "apply", dest, stage, str(build - 1), str(os.getpid()), "check"])
        if rejected.returncode == 0 or (stage / "ready").exists():
            raise RuntimeError("Changed payload passed release verification")
        with payload.open("rb+") as f:
            f.truncate(original_size)
        if (
            subprocess.run([helper, "apply", dest, stage, str(build), str(os.getpid()), "check"]).returncode
            == 0
        ):
            raise RuntimeError("Reinstall of same build accepted through updater")
        # Model the GUI handoff with an actual live parent process. The helper must wait.
        parent = subprocess.Popen([sys.executable, "-c", "import time;time.sleep(100)"])
        update = subprocess.Popen([helper, "apply", dest, stage, str(build - 1), str(parent.pid), "check"])
        try:
            deadline = time.monotonic() + 90
            while not (stage / "ready").is_file() and update.poll() is None and time.monotonic() < deadline:
                time.sleep(0.1)
            if not (stage / "ready").exists() or update.poll() is not None:
                raise RuntimeError("Handoff did not reach ready")
            (stage / "commit").write_text("install")
        finally:
            parent.terminate()
            parent.wait()
        if update.wait(timeout=180) != 0:
            raise RuntimeError("Offline install failed")
        start_ui(exe)
        if (
            read_identity(windows, profile) != identity
            or (profile / "displays.ini").read_bytes() != display
        ):
            raise RuntimeError("Upgrade changed pairing, direct access or display preferences")
        print(
            f"Verified {target}: ZIP migration, clean install, native {info}, signed offline update and existing profile"
        )


if __name__ == "__main__":
    main(*sys.argv[1:])
