"""Sign assembled installers using the platform identity configured for the release."""

import base64
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import shutil
import subprocess
import tempfile


def run(*args, capture=False):
    result = subprocess.run(args, stdout=subprocess.PIPE if capture else None, text=True)
    if result.returncode:
        raise RuntimeError(f"{Path(args[0]).name} failed with exit code {result.returncode}")
    return result.stdout


def secret_file(directory, name, value):
    target = directory / name
    target.touch(mode=0o600, exist_ok=False)
    target.write_bytes(base64.b64decode(value, validate=True))
    return target


@contextmanager
def apple_identity():
    certificate = os.getenv("MACOS_CERTIFICATE_P12")
    notary = [
        os.getenv(v) for v in ("APPLE_NOTARY_KEY_P8", "APPLE_NOTARY_KEY_ID", "APPLE_NOTARY_ISSUER_ID")
    ]
    if any(notary) and not (all(notary) and certificate):
        raise ValueError("Incomplete Apple notarization configuration")
    if not certificate:
        yield "-", [], []
        return
    with tempfile.TemporaryDirectory() as work:
        directory = Path(work)
        keychain = directory / "release.keychain-db"
        password = secrets.token_urlsafe(32)
        previous = shlex.split(run("security", "list-keychains", "-d", "user", capture=True))
        run("security", "create-keychain", "-p", password, keychain)
        try:
            run("security", "set-keychain-settings", "-lut", "3600", keychain)
            run("security", "unlock-keychain", "-p", password, keychain)
            run("security", "list-keychains", "-d", "user", "-s", keychain, *previous)
            run(
                "security",
                "import",
                secret_file(directory, "identity.p12", certificate),
                "-k",
                keychain,
                "-P",
                os.getenv("MACOS_CERTIFICATE_PASSWORD", ""),
                "-T",
                "/usr/bin/codesign",
            )
            run(
                "security",
                "set-key-partition-list",
                "-S",
                "apple-tool:,apple:,codesign:",
                "-s",
                "-k",
                password,
                keychain,
            )
            ids = run("security", "find-identity", "-v", "-p", "codesigning", keychain, capture=True)
            match = re.search(r'([0-9A-Fa-f]{40}) "Developer ID Application:', ids)
            if not match:
                raise RuntimeError("Developer ID Application identity missing")
            auth = (
                [
                    "--key",
                    secret_file(directory, "notary.p8", notary[0]),
                    "--key-id",
                    notary[1],
                    "--issuer",
                    notary[2],
                ]
                if all(notary)
                else []
            )
            yield match.group(1), ["--keychain", keychain], auth
        finally:
            run("security", "list-keychains", "-d", "user", "-s", *previous)
            run("security", "delete-keychain", keychain)


def apple_sign(path, identity, options, executable=True):
    flags = ["--timestamp=none"] if identity == "-" else ["--timestamp"]
    if identity != "-" and executable:
        flags += ["--options", "runtime"]
    run("codesign", "--force", "--sign", identity, *options, *flags, path)


def notarize(path, auth):
    if not auth:
        return
    with tempfile.TemporaryDirectory() as work:
        upload = path
        if path.suffix == ".app":
            upload = Path(work) / "application.zip"
            run("ditto", "-c", "-k", "--keepParent", path, upload)
        result = run(
            "xcrun",
            "notarytool",
            "submit",
            upload,
            *auth,
            "--wait",
            "--output-format",
            "json",
            capture=True,
        )
        if json.loads(result)["status"] != "Accepted":
            raise RuntimeError("Apple notarization did not succeed")
    run("xcrun", "stapler", "staple", path)
    run("xcrun", "stapler", "validate", path)


def windows_sign(path):
    certificate = os.getenv("WINDOWS_CERTIFICATE_PFX")
    if not certificate:
        return "Windows 应用和安装包未使用 Authenticode 发布者证书签名。"
    tool = shutil.which("signtool")
    if not tool:
        kits = Path(os.environ["ProgramFiles(x86)"]) / "Windows Kits/10/bin"
        tool = max(
            kits.glob("10.*/x64/signtool.exe"),
            key=lambda p: tuple(map(int, p.parent.parent.name.split("."))),
        )
    with tempfile.TemporaryDirectory() as work:
        pfx = secret_file(Path(work), "identity.pfx", certificate)
        password = os.getenv("WINDOWS_CERTIFICATE_PASSWORD")
        run(
            tool or "signtool.exe",
            "sign",
            "/f",
            pfx,
            *(["/p", password] if password else []),
            "/fd",
            "SHA256",
            "/tr",
            "http://timestamp.digicert.com",
            "/td",
            "SHA256",
            path,
        )
        run(tool or "signtool.exe", "verify", "/pa", "/all", path)
    return "Windows 应用和安装包已使用 Authenticode 签名并加盖时间戳。"
