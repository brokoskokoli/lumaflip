#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Downloads a Qt 6 binary build for Windows from the official online repository (CI).

    python tools/fetch_qt_windows.py <version> <output dir> [--arch win64_msvc2022_64] [--addons a,b]

Since Qt 6.11 the online repository keeps every architecture in a folder of its own
(qt6_6112/qt6_6112_msvc2022_64/), which aqtinstall 3.3 does not read yet. This script reads
that folder's Updates.xml, downloads the archives of the base package (without docs, QML
and the software OpenGL fallback) and of the requested add-ons, checks their SHA-1 sums
from the repository and unpacks them with 7-Zip into the Qt prefix
<output dir>/<version>/msvc2022_64 (the archives hold bin/, lib/, ... without a version
folder), which it prints.
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import shutil
import subprocess
import sys
import urllib.request
import xml.etree.ElementTree as ET

BASE = "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop"
# Archives of the base package Lumaflip does not need.
SKIP = ("qtdoc-", "qtdeclarative-", "opengl32sw-")


def fetch(url: str) -> bytes:
    for attempt in range(4):
        try:
            with urllib.request.urlopen(url, timeout=120) as r:
                return r.read()
        except OSError as e:
            if attempt == 3:
                raise
            print(f"retry {url}: {e}", flush=True)
    raise AssertionError


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("version")
    ap.add_argument("out", type=pathlib.Path)
    ap.add_argument("--arch", default="win64_msvc2022_64")
    ap.add_argument("--addons", default="qtimageformats,qtshadertools")
    args = ap.parse_args()
    ver = args.version.replace(".", "")
    short_arch = args.arch.split("_", 1)[1]  # msvc2022_64
    folder = f"{BASE}/qt6_{ver}/qt6_{ver}_{short_arch}"
    root = ET.fromstring(fetch(f"{folder}/Updates.xml"))
    wanted = {f"qt.qt6.{ver}.{args.arch}"} | {f"qt.qt6.{ver}.addons.{a}.{args.arch}"
                                               for a in args.addons.split(",") if a}
    found = set()
    prefix = args.out / args.version / short_arch
    prefix.mkdir(parents=True, exist_ok=True)
    seven = shutil.which("7z") or r"C:\Program Files\7-Zip\7z.exe"
    tmp = args.out / "_download"
    tmp.mkdir(exist_ok=True)
    for pkg in root.iter("PackageUpdate"):
        name = pkg.findtext("Name")
        if name not in wanted:
            continue
        found.add(name)
        version = pkg.findtext("Version")
        archives = [a.strip() for a in (pkg.findtext("DownloadableArchives") or "").split(",") if a.strip()]
        for archive in archives:
            if archive.startswith(SKIP):
                continue
            url = f"{folder}/{name}/{version}{archive}"
            data = fetch(url)
            want = fetch(url + ".sha1").decode().split()[0].strip()
            got = hashlib.sha1(data).hexdigest()
            if got != want:
                raise SystemExit(f"error: SHA-1 mismatch for {archive}: {got} != {want}")
            path = tmp / archive
            path.write_bytes(data)
            print(f"{archive}: {len(data) / 1e6:.1f} MB, SHA-1 ok", flush=True)
            subprocess.run([seven, "x", "-y", "-bso0", "-bsp0", f"-o{prefix}", str(path)], check=True)
            path.unlink()
    missing = wanted - found
    if missing:
        raise SystemExit(f"error: packages not in the repository: {sorted(missing)}")
    shutil.rmtree(tmp, ignore_errors=True)
    if not (prefix / "bin" / "Qt6Core.dll").is_file():
        raise SystemExit(f"error: {prefix} has no bin/Qt6Core.dll")
    # Like the Qt installer: a qt.conf is not needed (Qt 6 is relocatable).
    print(prefix)
    return 0


if __name__ == "__main__":
    sys.exit(main())
