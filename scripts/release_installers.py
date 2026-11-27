"""Assemble two native offline installers and embed their repository attestations."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

from release_sign import apple_identity, apple_sign, notarize, windows_sign

ROOT = Path(__file__).resolve().parents[1]
NAME = "WWMM Remote"
APP = NAME + ".app"
# Thin/fat Mach-O magic bytes in either byte order.
MACH = {
    bytes.fromhex(s)
    for s in (
        "feedface",
        "cefaedfe",
        "feedfacf",
        "cffaedfe",
        "cafebabe",
        "bebafeca",
        "cafebabf",
        "bfbafeca",
    )
}


def run(*args, cwd=None, capture=False, env=None):
    return subprocess.run(
        args, cwd=cwd, env=env, check=True, stdout=subprocess.PIPE if capture else None, text=True
    ).stdout


def helper_notices(notices, env):
    directory = notices / "update-helper"
    directory.mkdir()
    goroot = Path(run("go", "env", "GOROOT", capture=True).strip())
    shutil.copyfile(goroot / "LICENSE", directory / "Go-LICENSE")
    mods = run(
        "go",
        "list",
        "-deps",
        "-f",
        "{{if .Module}}{{.Module.Dir}}{{end}}",
        ".",
        cwd=ROOT / "installer/update",
        env=env,
        capture=True,
    )
    for text in sorted(set(mods.splitlines())):
        if not text:
            continue
        path = Path(text)
        if path == ROOT / "installer/update":
            continue
        for f in path.iterdir():
            if f.is_file() and f.name.upper().startswith(("LICENSE", "NOTICE", "COPYING")):
                out = directory / (
                    hashlib.sha256(text.split("/pkg/mod/")[-1].encode()).hexdigest()[:12]
                    + "-"
                    + path.name
                    + "-"
                    + f.name
                )
                shutil.copyfile(f, out)


def macho(path):
    if path.is_symlink() or not path.is_file():
        return False
    with path.open("rb") as f:
        return f.read(4) in MACH


def mac_payload(inputs, work, version):
    apps = []
    for arch in ("arm64", "x86_64"):
        target = work / arch
        run("ditto", "-x", "-k", next(inputs.rglob(f"*-macos-{arch}.zip")), target)
        apps.append(target / APP)
    app = work / "image" / APP
    app.parent.mkdir()
    run("ditto", apps[0], app)
    paths = {p.relative_to(apps[0]) for p in apps[0].rglob("*") if macho(p)}
    if paths != {p.relative_to(apps[1]) for p in apps[1].rglob("*") if macho(p)}:
        raise ValueError("Native bundles contain different sets of Mach-O files")
    for rel in sorted(paths):
        slices = []
        for idx, arch in enumerate(("arm64", "x86_64")):
            source = apps[idx] / rel
            if run("lipo", "-archs", source, capture=True).strip() != arch:
                thin = work / ("thin-" + arch)
                run("lipo", source, "-thin", arch, "-output", thin)
                source = thin
            slices.append(source)
        run("lipo", "-create", *slices, "-output", app / rel)
        run("lipo", app / rel, "-verify_arch", "arm64", "x86_64")
    with apple_identity() as (identity, opts, auth):
        for p in sorted(paths):
            apple_sign(app / p, identity, opts)
        bundles = [
            p
            for p in app.rglob("*")
            if p.is_dir() and not p.is_symlink() and p.suffix in {".framework", ".app", ".xpc", ".bundle"}
        ]
        for p in sorted(bundles, key=lambda f: len(f.parts), reverse=True) + [app]:
            apple_sign(p, identity, opts)
        run("codesign", "--verify", "--deep", "--strict", "--all-architectures", app)
        notarize(app, auth)
    os.symlink("/Applications", app.parent / "Applications")
    helper = app / "Contents/Helpers/wwmm-update"
    return run(helper, "digest", app, capture=True).strip()


def inno_tool():
    candidates = [
        shutil.which("ISCC"),
        Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Inno Setup 6/ISCC.exe",
        Path(os.getenv("ProgramFiles", "C:/Program Files")) / "Inno Setup 7/ISCC.exe",
    ]
    return next(str(p) for p in candidates if p and Path(p).is_file())


def win_payload(inputs, work, version):
    dirs = []
    for arch in ("x64", "arm64"):
        with zipfile.ZipFile(next(inputs.rglob(f"*-windows-{arch}.zip"))) as z:
            z.extractall(work / arch)
        d = work / arch / NAME
        for exe in (d / (NAME + ".exe"), d / "wwmm-update.exe"):
            windows_sign(exe)
        dirs.append(d)
    iscc = inno_tool()
    # Older runner images omit the official Chinese message file. Install the pinned upstream translation.
    lang = Path(iscc).parent / "Languages/ChineseSimplified.isl"
    script = (ROOT / "installer/windows/setup.iss").read_text()
    if not lang.exists():
        lang = work / "ChineseSimplified.isl"
        run(
            "curl",
            "-fsSL",
            "--retry",
            "3",
            "-o",
            lang,
            "https://raw.githubusercontent.com/jrsoftware/issrc/dfb8c9d5c9926413190ab68923bcd9eecbe448b4/Files/Languages/ChineseSimplified.isl",
        )
        script = script.replace(r"compiler:Languages\ChineseSimplified.isl", str(lang))
    iss = work / "setup.iss"
    iss.write_text(script, encoding="utf-8-sig")
    run(
        iscc,
        "/Qp",
        f"/DAppVersion={version}",
        f"/DBuildNumber={os.environ['GITHUB_RUN_NUMBER']}",
        f"/DX64={dirs[0]}",
        f"/DArm={dirs[1]}",
        f"/DOutput={work}",
        f"/DAppIcon={ROOT/'platform/icons/WWMM.ico'}",
        iss,
    )
    installer = work / "install.exe"
    windows_sign(installer)
    with installer.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def prepare(platform, inputs, work, version):
    work.mkdir(parents=True, exist_ok=True)
    sha = mac_payload(inputs, work, version) if platform == "darwin" else win_payload(inputs, work, version)
    receipt = {
        "Product": "dev.wwmm.remote",
        "Platform": platform,
        "Version": version,
        "Build": int(os.environ["GITHUB_RUN_NUMBER"]),
        "SHA256": sha,
    }
    (work / "release.json").write_bytes((json.dumps(receipt, sort_keys=True) + "\n").encode())


def finish(platform, work, output, version, bundle):
    output.mkdir(parents=True, exist_ok=True)
    if platform == "darwin":
        image = work / "image"
        shutil.copyfile(work / "release.json", image / ".release.json")
        shutil.copyfile(bundle, image / ".release.sigstore.json")
        dmg = output / "WWMM-Remote.dmg"
        run(
            "hdiutil",
            "create",
            "-quiet",
            "-ov",
            "-format",
            "UDZO",
            "-volname",
            NAME,
            "-srcfolder",
            image,
            dmg,
        )
        with apple_identity() as (identity, opts, auth):
            apple_sign(dmg, identity, opts, False)
            notarize(dmg, auth)
            status = (
                "macOS 本地签名（ad hoc），未经 Apple 公证。"
                if identity == "-"
                else (
                    "Developer ID 签名，已通过 Apple 公证。"
                    if auth
                    else "Developer ID 签名，未经 Apple 公证。"
                )
            )
    else:

        def rcpath(p):
            return str(p.resolve()).replace("\\", "/")

        rc = work / "wrapper.rc"
        template = (ROOT / "platform/windows/version.rc.in").read_text()
        nums = version.split(".")
        for key, value in {
            "CMAKE_CURRENT_SOURCE_DIR": rcpath(ROOT),
            "PROJECT_VERSION": version,
            "PROJECT_VERSION_MAJOR": nums[0],
            "PROJECT_VERSION_MINOR": nums[1],
            "PROJECT_VERSION_PATCH": nums[2],
            "WWMM_BUILD_NUMBER": os.environ["GITHUB_RUN_NUMBER"],
        }.items():
            template = template.replace("@" + key + "@", value)
        rc.write_text(
            template
            + '\n1 24 "'
            + rcpath(ROOT / "installer/windows/launcher.manifest")
            + '"\n'
            + "".join(
                f'{idx} RCDATA "{rcpath(p)}"\n'
                for idx, p in [(201, work / "install.exe"), (202, work / "release.json"), (203, bundle)]
            )
        )
        run("rc", "/nologo", "/fo", "wrapper.res", rc, cwd=work)
        exe = output / "WWMM-Remote-Setup.exe"
        run(
            "cl",
            "/nologo",
            "/utf-8",
            "/O1",
            "/std:c++17",
            "/MT",
            "/EHsc",
            "/DUNICODE",
            "/D_UNICODE",
            ROOT / "installer/windows/bootstrap.cpp",
            "wrapper.res",
            "user32.lib",
            "ole32.lib",
            "/link",
            "/subsystem:windows",
            "/manifest:no",
            "/out:" + str(exe),
            cwd=work,
        )
        status = windows_sign(exe)
    (output / (platform + ".txt")).write_text(status + "\n", encoding="utf-8")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("action", choices=("prepare", "finish"))
    p.add_argument("platform", choices=("darwin", "windows"))
    p.add_argument("version")
    p.add_argument("--inputs", type=Path, default=Path("packages"))
    p.add_argument("--work", type=Path, default=Path("build-installer"))
    p.add_argument("--output", type=Path, default=Path("dist/installers"))
    p.add_argument("--bundle", type=Path)
    a = p.parse_args()
    if a.action == "prepare":
        prepare(a.platform, a.inputs.resolve(), a.work.resolve(), a.version)
    else:
        finish(a.platform, a.work.resolve(), a.output.resolve(), a.version, a.bundle.resolve())
