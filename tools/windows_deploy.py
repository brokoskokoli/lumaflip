#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Assembles the distributable Windows folder of Lumaflip (CI job "Windows app").

    python tools/windows_deploy.py <build dir> <output dir> [--vcpkg-installed <dir>]

Layout of <output dir> (= the installation, also packed as the portable ZIP):
    Lumaflip.exe, lumaflip-decoder.exe        the app and its decoder helper
    Qt6*.dll, platforms/, imageformats/, ...   Qt (windeployqt) + platforms/qoffscreen.dll
                                               for the helper (it runs without a window)
    translations/                              lumaflip_<lang>.qm + Qt's own catalogues
    msvcp140.dll, vcruntime140*.dll            the MSVC runtime, app-local
    licenses/                                  licence texts + index.json (About dialog)

Needs: windeployqt of the Qt used for the build on PATH (or QT_ROOT_DIR), the MSVC developer
environment (VCToolsRedistDir) for the runtime DLLs.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
UI_LANGUAGES = ["de", "en", "fr", "es", "it", "pt_BR", "pt_PT", "nl", "pl", "sv", "da", "nb", "fi", "cs",
                "ja", "zh_CN", "zh_TW", "ko"]

# Statically linked libraries (vcpkg, x64-windows-static-md): licence file in vcpkg's
# share/<port>/copyright. Only those the build actually used are listed (static-components
# are detected by the presence of the vcpkg port).
VCPKG_COMPONENTS = {
    "libjpeg-turbo": {"name": "libjpeg-turbo – TurboJPEG (verlustfreies Drehen von JPEG-Dateien)",
                      "license": "IJG AND BSD-3-Clause AND Zlib", "homepage": "https://libjpeg-turbo.org"},
    "libarchive": {"name": "libarchive – RAR/CBR- und 7z/CB7-Archive", "license": "BSD-2-Clause",
                   "homepage": "https://www.libarchive.org"},
    "zlib": {"name": "zlib", "license": "Zlib", "homepage": "https://zlib.net"},
    "bzip2": {"name": "bzip2 (libarchive)", "license": "bzip2-1.0.6", "homepage": "https://sourceware.org/bzip2/"},
    "lz4": {"name": "LZ4 (libarchive)", "license": "BSD-2-Clause", "homepage": "https://lz4.org"},
    "liblzma": {"name": "XZ Utils liblzma (libarchive)", "license": "0BSD", "homepage": "https://tukaani.org/xz/"},
    "zstd": {"name": "Zstandard (libarchive)", "license": "BSD-3-Clause", "homepage": "https://facebook.github.io/zstd/"},
}


def run(*cmd: str) -> None:
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def find_windeployqt() -> str:
    exe = shutil.which("windeployqt") or shutil.which("windeployqt6")
    if not exe and os.environ.get("QT_ROOT_DIR"):
        cand = pathlib.Path(os.environ["QT_ROOT_DIR"]) / "bin" / "windeployqt.exe"
        if cand.is_file():
            exe = str(cand)
    if not exe:
        raise SystemExit("error: windeployqt not found (Qt's bin directory on PATH or QT_ROOT_DIR)")
    return exe


def qt_root(windeployqt: str) -> pathlib.Path:
    return pathlib.Path(windeployqt).resolve().parent.parent


def copy_runtime(out: pathlib.Path) -> list[str]:
    redist = os.environ.get("VCToolsRedistDir")
    if not redist:
        print("warning: VCToolsRedistDir not set - MSVC runtime not copied (needs the VC++ redistributable)")
        return []
    copied = []
    for crt in sorted(pathlib.Path(redist).glob("x64/Microsoft.VC*.CRT")):
        for dll in crt.glob("*.dll"):
            shutil.copy2(dll, out / dll.name)
            copied.append(dll.name)
    print(f"MSVC runtime: {', '.join(copied)}")
    return copied


def write_licenses(out: pathlib.Path, qt_version: str, vcpkg_installed: pathlib.Path | None) -> None:
    lic = out / "licenses"
    lic.mkdir(exist_ok=True)
    index = []
    (lic / "lumaflip").mkdir(exist_ok=True)
    shutil.copy2(REPO / "LICENSE", lic / "lumaflip" / "LICENSE")
    index.append({"id": "lumaflip", "name": "Lumaflip", "version": "", "license": "GPL-3.0-or-later",
                  "homepage": "https://github.com/brokoskokoli/lumaflip", "files": ["lumaflip/LICENSE"],
                  "binaries": ["Lumaflip.exe", "lumaflip-decoder.exe"]})
    (lic / "qt").mkdir(exist_ok=True)
    files = []
    for rel, name in (("packaging/licenses/LGPL-3.0.txt", "LGPL-3.0.txt"), ("LICENSE", "GPL-3.0.txt"),
                      ("packaging/windows/Qt-NOTICE-windows.txt", "Qt-NOTICE.txt")):
        shutil.copy2(REPO / rel, lic / "qt" / name)
        files.append(f"qt/{name}")
    index.append({"id": "qt", "name": "Qt 6", "version": qt_version, "license": "LGPL-3.0-only",
                  "homepage": "https://www.qt.io/licensing/open-source-lgpl-obligations", "files": files,
                  "binaries": sorted(p.name for p in out.glob("Qt6*.dll"))})
    if vcpkg_installed:
        share = vcpkg_installed / "share"
        for port, meta in VCPKG_COMPONENTS.items():
            copyright_file = share / port / "copyright"
            if not copyright_file.is_file():
                continue
            (lic / port).mkdir(exist_ok=True)
            shutil.copy2(copyright_file, lic / port / "LICENSE.txt")
            index.append({"id": port, "name": meta["name"], "version": "", "license": meta["license"],
                          "homepage": meta["homepage"], "files": [f"{port}/LICENSE.txt"],
                          "binaries": ["Lumaflip.exe (statisch)", "lumaflip-decoder.exe (statisch)"]})
    (lic / "index.json").write_text(json.dumps(index, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"licenses: {len(index)} components")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build", type=pathlib.Path)
    ap.add_argument("out", type=pathlib.Path)
    ap.add_argument("--vcpkg-installed", type=pathlib.Path, default=None,
                    help="vcpkg_installed/<triplet> of the build (licence texts of static libraries)")
    args = ap.parse_args()
    build: pathlib.Path = args.build
    out: pathlib.Path = args.out
    # qt_standard_project_setup() puts the executables into the top of the build tree.
    exe_dir = build if (build / "Lumaflip.exe").is_file() else build / "src"
    app = exe_dir / "Lumaflip.exe"
    helper = exe_dir / "lumaflip-decoder.exe"
    for f in (app, helper):
        if not f.is_file():
            raise SystemExit(f"error: {f} missing - build first")
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    shutil.copy2(app, out / app.name)
    shutil.copy2(helper, out / helper.name)
    (out / "translations").mkdir()
    qms = sorted((build / "translations").glob("lumaflip_*.qm"))
    if len(qms) < len(UI_LANGUAGES):
        raise SystemExit(f"error: only {len(qms)} translation files in {build / 'translations'}")
    for qm in qms:
        shutil.copy2(qm, out / "translations" / qm.name)

    windeployqt = find_windeployqt()
    langs = ",".join(sorted({lang.split("_")[0] for lang in UI_LANGUAGES} | {"pt_BR", "zh_CN", "zh_TW"}))
    run(windeployqt, "--release", "--no-compiler-runtime", "--no-opengl-sw", "--no-system-d3d-compiler",
        "--translations", langs, "--dir", str(out), str(out / app.name))
    run(windeployqt, "--release", "--no-compiler-runtime", "--no-opengl-sw", "--no-system-d3d-compiler",
        "--no-translations", "--dir", str(out), str(out / helper.name))
    # The helper runs with QT_QPA_PLATFORM=offscreen (no window, no desktop access).
    root = qt_root(windeployqt)
    offscreen = root / "plugins" / "platforms" / "qoffscreen.dll"
    if not offscreen.is_file():
        raise SystemExit(f"error: {offscreen} missing")
    (out / "platforms").mkdir(exist_ok=True)
    shutil.copy2(offscreen, out / "platforms" / offscreen.name)
    copy_runtime(out)
    qt_version = root.parent.name if root.parent.name[:1].isdigit() else ""
    write_licenses(out, qt_version, args.vcpkg_installed)
    missing = [p for p in ("Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Svg.dll", "platforms/qwindows.dll",
                           "platforms/qoffscreen.dll") if not (out / p).exists()]
    if missing:
        raise SystemExit(f"error: missing after deployment: {missing}")
    size = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    print(f"deployed {out} ({size / 1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
