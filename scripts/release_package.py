"""Build, deploy and verify one native WWMM Remote release archive."""
import argparse
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zipfile

from release_qt import VERSION as QT_VERSION, install_license_files, install_sdk

ROOT = Path(__file__).resolve().parents[1]
APP = "WWMM Remote"
TARGETS = ("macos-arm64", "macos-x86_64", "windows-x64", "windows-arm64")


def require_file(path):
    if not path.is_file():
        raise FileNotFoundError(path)
    return path


def windows_machine(path):
    with require_file(path).open("rb") as binary:
        if binary.read(2) != b"MZ":
            raise ValueError(f"Missing DOS header in {path}")
        binary.seek(0x3c)
        binary.seek(struct.unpack("<I", binary.read(4))[0])
        if binary.read(4) != b"PE\0\0":
            raise ValueError(f"Missing PE header in {path}")
        return struct.unpack("<H", binary.read(2))[0]


def package_windows(stage, archive, arch):
    required = [f"{APP}.exe", "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Network.dll",
                "plugins/platforms/qwindows.dll", f"vc_redist.{arch}.exe"]
    for name in required:
        require_file(stage / name)
    machine = {"x64": 0x8664, "arm64": 0xaa64}[arch]
    binaries = [stage / required[0], *stage.rglob("*.dll")]
    for binary in binaries:
        actual = windows_machine(binary)
        hybrid_dll = arch == "arm64" and binary.suffix.lower() == ".dll" and actual == 0xa64e
        if actual != machine and not hybrid_dll:
            raise ValueError(f"{binary.name} has the wrong architecture for {arch}")
    # Template-only uses of QtConcurrent can be fully inlined. Check the linked imports.
    imports = subprocess.check_output(["dumpbin", "/nologo", "/dependents", *binaries],
                                      text=True, errors="replace")
    for name in set(re.findall(r"^\s*(Qt6[\w-]+\.dll)\s*$", imports, re.IGNORECASE | re.MULTILINE)):
        require_file(stage / name)
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
        for path in sorted(stage.rglob("*")):
            if path.is_file():
                output.write(path, Path(APP) / path.relative_to(stage))


def package_mac(stage, archive, arch):
    app = stage / f"{APP}.app"
    contents = app / "Contents"
    exe = require_file(contents / "MacOS" / APP)
    actual = subprocess.check_output(["lipo", "-archs", exe], text=True).strip()
    if actual != arch:
        raise ValueError(f"Expected {arch} executable, found {actual}")
    require_file(contents / "Frameworks/QtWidgets.framework/QtWidgets")
    require_file(contents / "PlugIns/platforms/libqcocoa.dylib")
    for framework in (contents / "Frameworks").glob("*.framework"):
        subprocess.run(["lipo", framework / framework.stem, "-verify_arch", arch], check=True)
    for plugin in (contents / "PlugIns").rglob("*.dylib"):
        subprocess.run(["lipo", plugin, "-verify_arch", arch], check=True)
    # Resource notices are now complete. Seal the bundle after macdeployqt has signed the nested code.
    subprocess.run(["codesign", "--force", "--sign", "-", "--timestamp=none", app], check=True)
    subprocess.run(["codesign", "--verify", "--deep", "--strict", app], check=True)
    subprocess.run(["ditto", "-c", "-k", "--keepParent", app, archive], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=TARGETS)
    parser.add_argument("--qt", type=Path, help="Use an installed Qt 6.12.1 base SDK")
    args = parser.parse_args()
    os_name, arch = args.target.split("-")
    if (os.name == "nt") != (os_name == "windows"):
        parser.error("Run the packager on the target operating system")
    build = ROOT / f"build-release-{args.target}"
    build.mkdir(exist_ok=True)
    qt = args.qt.resolve() if args.qt else install_sdk(args.target, Path(os.getenv("RUNNER_TEMP", build)))
    require_file(qt / "lib/cmake/Qt6/Qt6Config.cmake")
    os.environ["PATH"] = str(qt / "bin") + os.pathsep + os.environ["PATH"]
    windows = os_name == "windows"
    targets = ["wwmm_remote"]
    configure = ["cmake", "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                 f"-DBUILD_TESTING={'ON' if windows else 'OFF'}", f"-DCMAKE_PREFIX_PATH={qt}"]
    if windows:
        targets.extend(["wwmm_pair_tests", "wwmm_desktop_tests"])
        configure.extend(["-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl", "-DCMAKE_INSTALL_BINDIR=.",
                          f"-DQT_HOST_PATH={qt}"])
    else:
        configure.append(f"-DCMAKE_OSX_ARCHITECTURES={arch}")
    subprocess.run(configure, check=True)
    subprocess.run(["cmake", "--build", build, "--target", *targets, "--parallel",
                    str(min(os.cpu_count() or 2, 8))], check=True)
    if windows:
        env = dict(os.environ, QT_QPA_PLATFORM="windows", QT_PLUGIN_PATH=str(qt / "plugins"))
        for binary, case in [("wwmm_pair_tests.exe", "readOnlyRequestButtons"),
                             ("wwmm_desktop_tests.exe", "controlConsentAndDelivery")]:
            subprocess.run([build / binary, case], env=env, check=True, timeout=90)
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    version = re.search(r"^CMAKE_PROJECT_VERSION:STATIC=([\d.]+)$", cache, re.MULTILINE).group(1)
    output = ROOT / "dist" / version
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f"WWMM-Remote-{version}-{args.target}.zip"
    with tempfile.TemporaryDirectory(dir=build) as temp:
        stage = Path(temp) / "stage"
        subprocess.run(["cmake", "--install", build, "--prefix", stage], check=True)
        notices = (stage if os_name == "windows" else stage / f"{APP}.app/Contents/Resources") / "third-party"
        for name in ["BoringSSL-LICENSE", "WebRTC-LICENSE", "WebRTC-PATENTS", "Abseil-LICENSE",
                     "libSRTP-LICENSE", "Opus-LICENSE", f"qtbase-{QT_VERSION}.spdx"]:
            require_file(notices / name)
        install_license_files(notices)
        packed = Path(temp) / archive.name
        if os_name == "windows":
            package_windows(stage, packed, arch)
        else:
            package_mac(stage, packed, arch)
        with zipfile.ZipFile(packed) as result:
            if result.testzip() is not None:
                raise ValueError(f"Archive verification failed: {archive.name}")
        packed.replace(archive)
    print(f"{archive.name}: {archive.stat().st_size} bytes", flush=True)


if __name__ == "__main__":
    main()
