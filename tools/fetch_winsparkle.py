#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Downloads WinSparkle (MIT, https://winsparkle.org), the Windows counterpart of Sparkle,
for automatic updates of the Windows build (F-MAC-08, docs/WINDOWS.md) into
third_party/WinSparkle/ (git-ignored):

    third_party/WinSparkle/x64/WinSparkle.dll   shipped next to Lumaflip.exe (loaded at run time)
    third_party/WinSparkle/include/             winsparkle.h (reference only, not compiled in)
    third_party/WinSparkle/COPYING              licence text (About dialog)

    python tools/fetch_winsparkle.py [--force] [--dest DIR]

The release archive is pinned by version and SHA-256 (the "digest" GitHub publishes for the
asset WinSparkle-<version>.zip of https://github.com/vslavik/winsparkle/releases/tag/v<version>);
any mismatch is refused. Lumaflip loads the DLL with LoadLibraryEx from its own folder and
needs WinSparkle >= 0.9 (EdDSA signatures, win_sparkle_set_eddsa_public_key).
Updating: change VERSION and SHA256 together, run the script, build, and repeat the update
test of docs/MANUAL_CHECKS.md on Windows.

Runs on Windows (CI) and anywhere else with Python 3 (only downloads and unpacks).
"""
from __future__ import annotations

import argparse
import hashlib
import io
import pathlib
import shutil
import sys
import tempfile
import urllib.request
import zipfile

VERSION = "0.9.4"
SHA256 = "6037df37fc263bd1650a1c4949681a9d40ffe991d01f35892a406cb5d103c976"
URL = f"https://github.com/vslavik/winsparkle/releases/download/v{VERSION}/WinSparkle-{VERSION}.zip"

REPO = pathlib.Path(__file__).resolve().parent.parent
STAMP_NAME = ".lumaflip-winsparkle"
# Archive member (below WinSparkle-<version>/) -> destination below the target directory.
FILES = {
    "x64/Release/WinSparkle.dll": "x64/WinSparkle.dll",
    "include/winsparkle.h": "include/winsparkle.h",
    "include/winsparkle-version.h": "include/winsparkle-version.h",
    "COPYING": "COPYING",
    "NEWS": "NEWS",
}


def download(url: str) -> bytes:
    last: Exception | None = None
    for _ in range(3):
        try:
            with urllib.request.urlopen(url, timeout=120) as r:
                return r.read()
        except OSError as e:  # network hiccup on a CI runner: retry
            last = e
    raise SystemExit(f"fetch_winsparkle: download failed: {last}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--force", action="store_true", help="download even if the pinned version is present")
    ap.add_argument("--dest", type=pathlib.Path, default=REPO / "third_party" / "WinSparkle")
    a = ap.parse_args()
    dest: pathlib.Path = a.dest
    stamp = dest / STAMP_NAME
    want = f"{VERSION} {SHA256}"
    if not a.force and stamp.is_file() and stamp.read_text().strip() == want \
            and (dest / "x64" / "WinSparkle.dll").is_file():
        print(f"WinSparkle {VERSION} already in {dest}")
        return 0
    if dest.exists() and not stamp.is_file():
        raise SystemExit(f"fetch_winsparkle: {dest} exists but was not created by this script - move it away")

    print(f"Downloading WinSparkle {VERSION}")
    data = download(URL)
    actual = hashlib.sha256(data).hexdigest()
    if actual != SHA256:
        raise SystemExit(f"fetch_winsparkle: SHA-256 mismatch: expected {SHA256}, got {actual}")
    print(f"SHA-256 ok: {actual}")

    dest.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lumaflip-winsparkle.", dir=dest.parent) as tmp:
        work = pathlib.Path(tmp) / "x"
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            prefix = f"WinSparkle-{VERSION}/"
            for member, target in FILES.items():
                try:
                    content = z.read(prefix + member)
                except KeyError:
                    raise SystemExit(f"fetch_winsparkle: unexpected archive layout ({prefix + member} missing)")
                out = work / target
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(content)
        dll = (work / "x64" / "WinSparkle.dll").read_bytes()
        if dll[:2] != b"MZ" or b"win_sparkle_set_eddsa_public_key" not in dll:
            raise SystemExit("fetch_winsparkle: WinSparkle.dll is no x64 DLL with EdDSA support")
        (work / STAMP_NAME).write_text(want + "\n")
        if dest.exists():
            shutil.rmtree(dest)
        shutil.move(str(work), str(dest))
    print(f"WinSparkle {VERSION} in {dest} (x64/WinSparkle.dll, include/, COPYING)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
