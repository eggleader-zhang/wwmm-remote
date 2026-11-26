"""Build, deploy and verify one native WWMM Remote release archive."""
import argparse
import os
from pathlib import Path
import re
import shutil
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
    # App-local MSVC libraries also allow a clean Windows installation to launch the portable exe.
    crt_dir = Path(os.environ["VCToolsRedistDir"]) / arch
    crt = next(crt_dir.glob("Microsoft.VC*.CRT"))
    for library in crt.glob("*.dll"):
        shutil.copy2(library, stage)
    required = [f"{APP}.exe", "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Network.dll",
                "Qt6Concurrent.dll", "vcruntime140.dll", "msvcp140.dll", "plugins/platforms/qwindows.dll"]
    for name in required:
        require_file(stage / name)
    machine = {"x64": 0x8664, "arm64": 0xaa64}[arch]
    for binary in [stage / required[0], *stage.rglob("*.dll")]:
        if windows_machine(binary) != machine:
            raise ValueError(f"{binary.name} has the wrong architecture for {arch}")
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
        subprocess.run(["lipo", "-verify_arch", arch, framework / framework.stem], check=True)
    for plugin in (contents / "PlugIns").rglob("*.dylib"):
        subprocess.run(["lipo", "-verify_arch", arch, plugin], check=True)
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
    configure = ["cmake", "-S", ROOT, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                 "-DBUILD_TESTING=OFF", f"-DCMAKE_PREFIX_PATH={qt}"]
    if os_name == "windows":
        configure.extend(["-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl", "-DCMAKE_INSTALL_BINDIR=.",
                          f"-DQT_HOST_PATH={qt}"])
    else:
        configure.append(f"-DCMAKE_OSX_ARCHITECTURES={arch}")
    subprocess.run(configure, check=True)
    subprocess.run(["cmake", "--build", build, "--target", "wwmm_remote", "--parallel",
                    str(min(os.cpu_count() or 2, 8))], check=True)
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
                     "libSRTP-LICENSE", f"qtbase-{QT_VERSION}.spdx"]:
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
