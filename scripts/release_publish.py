"""Publish the two tested installers at the exact workflow commit."""

import hashlib
import json
import os
from pathlib import Path
import subprocess


def gh(*args):
    return subprocess.check_output(["gh", *args], text=True)


root = Path("packages")
tag, sha, repository = (os.environ[k] for k in ("TAG", "GITHUB_SHA", "GITHUB_REPOSITORY"))
names = ["WWMM-Remote-Setup.exe", "WWMM-Remote.dmg"]
notes = Path(os.environ["RUNNER_TEMP"]) / "release-notes.md"
link = f"https://github.com/{repository}/releases/download/{tag}/"
text = f"[下载 Windows 安装程序]({link}{names[0]}) · [下载 macOS 安装包]({link}{names[1]})\n\n"
text += Path(f"docs/releases/{tag}.md").read_text()
text += "\n## 发布签名\n\n" + "".join(
    "- " + (root / f"{p}.txt").read_text().strip() + "\n" for p in ("darwin", "windows")
)
notes.write_text(text)


def sha256(p):
    with p.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


expected = {name: sha256(root / name) for name in sorted(names)}
sums = root / "SHA256SUMS.txt"
sums.write_bytes("".join(f"{digest}  {name}\n" for name, digest in expected.items()).encode())
expected[sums.name] = sha256(sums)
ref = subprocess.run(
    ["git", "rev-parse", "--verify", "--quiet", f"refs/tags/{tag}^{{commit}}"],
    capture_output=True,
    text=True,
)
if ref.returncode == 0 and ref.stdout.strip() != sha:
    raise RuntimeError("The release tag already points to a different commit")


def release():
    pages = json.loads(gh("api", "--paginate", "--slurp", f"repos/{repository}/releases?per_page=100"))
    return next((r for page in pages for r in page if r["tag_name"] == tag), None)


r = release()
if r is None:
    gh(
        "release",
        "create",
        tag,
        "--draft",
        "--target",
        sha,
        "--title",
        "WWMM Remote " + tag[1:],
        "--notes-file",
        notes,
    )
elif not r["draft"] or r["target_commitish"] != sha:
    raise RuntimeError("Existing release cannot be replaced")
gh("release", "upload", tag, *(root / name for name in expected), "--clobber")
actual = {a["name"]: a["digest"] for a in release()["assets"]}
if actual != {k: "sha256:" + v for k, v in expected.items()}:
    raise RuntimeError("Uploaded digests do not match installed/tested files")
gh("release", "edit", tag, "--draft=false", "--latest", "--notes-file", notes)
print(gh("release", "view", tag, "--json", "url", "--jq", ".url"))
