#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Downloads a Qt 6 binary build for Windows from the official online repository (CI).

    python tools/fetch_qt_windows.py <version> <output dir> [--arch win64_msvc2022_64] [--addons a,b]

Since Qt 6.11 the online repository keeps every architecture in a folder of its own
(qt6_6112/qt6_6112_msvc2022_64/), which aqtinstall 3.3 does not read yet. This script reads
that folder's Updates.xml, downloads the archives of the base package (without docs, QML
and the software OpenGL fallback) and of the requested add-ons, checks them and unpacks
them with 7-Zip into the Qt prefix <output dir>/<version>/msvc2022_64 (the archives hold
bin/, lib/, ... without a version folder), which it prints.

Every archive must match the SHA-256 committed below (PINNED): the repository's own SHA-1
files come from the same server and would not catch a tampered mirror. A new Qt version (or
a rebuilt package) fails until its hashes are added - `--print-hashes` downloads the archives,
checks them against the repository's SHA-1 and prints the PINNED entries to review and commit.
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

# SHA-256 of every archive used, by "<package>/<version><archive>" (supply chain: committed,
# not taken from the download server).
_B = "6.11.2-0-202608131017"
_A = "-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z"
PINNED = {
    "6.11.2": {
        f"qt.qt6.6112.win64_msvc2022_64/{_B}qtbase{_A}":
            "fd984b7264361b4dd3fd2a417702ca1258e4086268f2ee6a69b9a393d9c3f6bb",
        f"qt.qt6.6112.win64_msvc2022_64/{_B}qtsvg{_A}":
            "417f44499c835b2303f3ff78043179bb23442f33d5d2816fbf7e8dbf275b1c89",
        f"qt.qt6.6112.win64_msvc2022_64/{_B}qttools{_A}":
            "5f2b387a1f8055102b1388ff4ea743124ed6389d442f75c659728bb5c2a56946",
        f"qt.qt6.6112.win64_msvc2022_64/{_B}qttranslations{_A}":
            "1c9516ce25d286ecf97e7aaaa5c8fa395823f21af5fa3dcb7bd06f648453dd77",
        f"qt.qt6.6112.win64_msvc2022_64/{_B}d3dcompiler_47-x64.7z":
            "b1c379f739fa59a495a388c0878757e3d90fb72e0df771237a1ee4a2c1675d15",
        f"qt.qt6.6112.addons.qtimageformats.win64_msvc2022_64/{_B}qtimageformats{_A}":
            "d6822c2160ebb675525173deb84bbfb68b4be046ee7f126af74a400a285b933d",
        f"qt.qt6.6112.addons.qtshadertools.win64_msvc2022_64/{_B}qtshadertools{_A}":
            "d434c320f5788cf54c71e68ab28488ceb2f1ba725e63241530155347a4c5081e",
    },
}


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
    ap.add_argument("--print-hashes", action="store_true",
                    help="only download, check the repository's SHA-1 and print PINNED entries")
    args = ap.parse_args()
    pinned = PINNED.get(args.version, {})
    if not pinned and not args.print_hashes:
        raise SystemExit(f"error: no SHA-256 pinned for Qt {args.version} (run with --print-hashes, "
                         "review and add them to PINNED)")
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
            key = f"{name}/{version}{archive}"
            url = f"{folder}/{key}"
            data = fetch(url)
            want = fetch(url + ".sha1").decode().split()[0].strip()
            got = hashlib.sha1(data).hexdigest()
            if got != want:
                raise SystemExit(f"error: SHA-1 mismatch for {archive}: {got} != {want}")
            sha256 = hashlib.sha256(data).hexdigest()
            if args.print_hashes:
                print(f'        "{key}":\n            "{sha256}",', flush=True)
                continue
            if pinned.get(key) != sha256:
                raise SystemExit(f"error: {key} is not pinned or its SHA-256 differs ({sha256})")
            path = tmp / archive
            path.write_bytes(data)
            print(f"{archive}: {len(data) / 1e6:.1f} MB, SHA-256 ok", flush=True)
            subprocess.run([seven, "x", "-y", "-bso0", "-bsp0", f"-o{prefix}", str(path)], check=True)
            path.unlink()
    missing = wanted - found
    if missing:
        raise SystemExit(f"error: packages not in the repository: {sorted(missing)}")
    if args.print_hashes:
        return 0
    shutil.rmtree(tmp, ignore_errors=True)
    if not (prefix / "bin" / "Qt6Core.dll").is_file():
        raise SystemExit(f"error: {prefix} has no bin/Qt6Core.dll")
    # Like the Qt installer: a qt.conf is not needed (Qt 6 is relocatable).
    print(prefix)
    return 0


if __name__ == "__main__":
    sys.exit(main())
