"""Apply publisher signatures to a completed native release ZIP when CI credentials are configured."""
import base64
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import shutil
import subprocess
import sys
import tempfile
import zipfile


def run(*args, capture=False):
    # Arguments can contain a temporary keychain or certificate password.
    result = subprocess.run(args, stdout=subprocess.PIPE if capture else None, text=True)
    if result.returncode:
        raise RuntimeError(f"{Path(args[0]).name} failed with exit code {result.returncode}")
    return result.stdout


def secret_file(directory, name, value):
    target = directory / name
    target.write_bytes(base64.b64decode(value, validate=True))
    target.chmod(0o600)
    return target


def zip_app(app, archive):
    archive.unlink(missing_ok=True)
    run("ditto", "-c", "-k", "--keepParent", app, archive)


def sign_mac(archive, directory):
    cert = os.environ.get("MACOS_CERTIFICATE_P12")
    password = os.environ.get("MACOS_CERTIFICATE_PASSWORD", "")
    notary = [os.environ.get(name) for name in ("APPLE_NOTARY_KEY_P8", "APPLE_NOTARY_KEY_ID", "APPLE_NOTARY_ISSUER_ID")]
    if any(notary) and (not all(notary) or not cert):
        raise ValueError("Notarization requires all three API key settings and a Developer ID identity")
    if not cert:
        if password:
            raise ValueError("Missing Developer ID certificate")
        return "macOS 本地签名（ad hoc），未经 Apple 公证"
    keychain = directory / "release.keychain-db"
    secret = secrets.token_urlsafe(32)
    previous = shlex.split(run("security", "list-keychains", "-d", "user", capture=True))
    p12 = secret_file(directory, "identity.p12", cert)
    run("security", "create-keychain", "-p", secret, keychain)
    try:
        run("security", "set-keychain-settings", "-lut", "3600", keychain)
        run("security", "unlock-keychain", "-p", secret, keychain)
        run("security", "list-keychains", "-d", "user", "-s", keychain, *previous)
        run("security", "import", p12, "-k", keychain, "-P", password, "-T", "/usr/bin/codesign")
        run("security", "set-key-partition-list", "-S", "apple-tool:,apple:,codesign:", "-s", "-k", secret, keychain)
        identities = run("security", "find-identity", "-v", "-p", "codesigning", keychain, capture=True)
        match = re.search(r'([0-9A-Fa-f]{40}) "Developer ID Application:', identities)
        if not match:
            raise ValueError("Developer ID Application identity missing from certificate")
        stage = directory / "stage"
        run("ditto", "-x", "-k", archive, stage)
        app = stage / "WWMM Remote.app"
        nested = [p for p in app.rglob("*") if not p.is_symlink() and
                  p.suffix in {".framework", ".dylib", ".app", ".xpc"}]
        for item in [*sorted(nested, key=lambda p: len(p.parts), reverse=True), app]:
            run("codesign", "--force", "--options", "runtime", "--timestamp", "--keychain", keychain,
                "--sign", match.group(1), item)
        run("codesign", "--verify", "--deep", "--strict", app)
        packed = directory / "signed.zip"
        zip_app(app, packed)
        status = "Developer ID 签名，未经 Apple 公证"
        if all(notary):
            key = secret_file(directory, "notary.p8", notary[0])
            response = run("xcrun", "notarytool", "submit", packed, "--key", key, "--key-id", notary[1],
                           "--issuer", notary[2], "--wait", "--output-format", "json", capture=True)
            if json.loads(response).get("status") != "Accepted":
                raise RuntimeError("Apple did not accept this archive for notarization")
            run("xcrun", "stapler", "staple", app)
            run("xcrun", "stapler", "validate", app)
            zip_app(app, packed)
            status = "Developer ID 签名，已通过 Apple 公证"
        packed.replace(archive)
        return status
    finally:
        run("security", "delete-keychain", keychain)
        run("security", "list-keychains", "-d", "user", "-s", *previous)


def sign_windows(archive, directory, arch):
    cert = os.environ.get("WINDOWS_CERTIFICATE_PFX")
    password = os.environ.get("WINDOWS_CERTIFICATE_PASSWORD", "")
    if not cert:
        if password:
            raise ValueError("Missing Authenticode certificate")
        return "应用未使用发布者证书签名"
    pfx = secret_file(directory, "identity.pfx", cert)
    tool = shutil.which("signtool")
    if not tool:
        sdk = Path(os.environ["ProgramFiles(x86)"]) / "Windows Kits/10/bin"
        tools = list(sdk.glob(f"10.*/{arch}/signtool.exe"))
        tool = max(tools, key=lambda p: tuple(map(int, p.parent.parent.name.split("."))))
    stage = directory / "stage"
    with zipfile.ZipFile(archive) as incoming:
        incoming.extractall(stage)
    exe = stage / "WWMM Remote/WWMM Remote.exe"
    options = ["/p", password] if password else []
    run(tool, "sign", "/f", pfx, *options, "/fd", "SHA256", "/tr", "http://timestamp.digicert.com",
        "/td", "SHA256", exe)
    run(tool, "verify", "/pa", "/all", exe)
    packed = directory / "signed.zip"
    with zipfile.ZipFile(packed, "w", zipfile.ZIP_DEFLATED) as outgoing:
        for path in sorted(stage.rglob("*")):
            if path.is_file():
                outgoing.write(path, path.relative_to(stage))
    packed.replace(archive)
    return "应用已使用 Authenticode 签名并加盖时间戳"


def main(target, version):
    output = Path("dist") / version
    archive = output / f"WWMM-Remote-{version}-{target}.zip"
    if not archive.is_file():
        raise FileNotFoundError(archive)
    with tempfile.TemporaryDirectory(dir=output) as work:
        directory = Path(work).resolve()
        os_name, arch = target.split("-")
        status = sign_mac(archive, directory) if os_name == "macos" else sign_windows(archive, directory, arch)
    (output / f"signing-{target}.txt").write_text(f"- {target} · {status}\n", encoding="utf-8")
    print(f"{target} signing step complete")


if __name__ == "__main__":
    main(*sys.argv[1:])
