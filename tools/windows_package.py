#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Packages the deployed Windows folder (tools/windows_deploy.py) - CI job "Windows app" and
the Windows release job (release.yml), which signs the folder before and the installer after:

    python tools/windows_package.py --zip       dist/Lumaflip --version 1.3.0 --out dist
    python tools/windows_package.py --installer dist/Lumaflip --version 1.3.0 --out dist --icon <ico>
    python tools/windows_package.py --sha256    dist/Lumaflip --version 1.3.0 --out dist

  --zip        Lumaflip-<v>-windows-x64.zip (folder Lumaflip/ inside, portable)
  --installer  Lumaflip-<v>-windows-x64-setup.exe (Inno Setup 6, packaging/windows/Lumaflip.iss;
               ISCC.exe from PATH, ISCC or the default installation folder)
  --sha256     <file>.sha256 next to the ZIP and the installer ("<hash>  <name>", sha256sum -c)
Several actions in one call run in this order.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import zipfile

REPO = pathlib.Path(__file__).resolve().parent.parent


def names(version: str) -> tuple[str, str]:
    return f"Lumaflip-{version}-windows-x64.zip", f"Lumaflip-{version}-windows-x64-setup.exe"


def make_zip(folder: pathlib.Path, target: pathlib.Path) -> None:
    files = sorted(p for p in folder.rglob("*") if p.is_file())
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in files:
            z.write(p, pathlib.PurePosixPath("Lumaflip", *p.relative_to(folder).parts).as_posix())
    print(f"{target} ({len(files)} files, {target.stat().st_size / 1e6:.1f} MB)")


def find_iscc() -> str:
    for cand in (os.environ.get("ISCC"), shutil.which("ISCC"), shutil.which("iscc")):
        if cand and pathlib.Path(cand).is_file():
            return cand
    pf86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    cand = pathlib.Path(pf86) / "Inno Setup 6" / "ISCC.exe"
    if cand.is_file():
        return str(cand)
    raise SystemExit("error: ISCC.exe (Inno Setup 6) not found - choco install innosetup, or set ISCC")


def make_installer(folder: pathlib.Path, version: str, out: pathlib.Path, icon: pathlib.Path | None) -> None:
    cmd = [find_iscc(), "/Qp", f"/DAppVersion={version}", f"/DSourceDir={folder.resolve()}",
           f"/DOutputDir={out.resolve()}"]
    if icon:
        cmd.append(f"/DIconFile={icon.resolve()}")
    cmd.append(str(REPO / "packaging" / "windows" / "Lumaflip.iss"))
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)
    setup = out / names(version)[1]
    if not setup.is_file():
        raise SystemExit(f"error: {setup} was not created")
    print(f"{setup} ({setup.stat().st_size / 1e6:.1f} MB)")


def write_sha256(path: pathlib.Path) -> None:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    line = f"{h.hexdigest()}  {path.name}\n"
    (path.parent / (path.name + ".sha256")).write_text(line, encoding="ascii", newline="\n")
    print(line, end="")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", type=pathlib.Path, help="the deployed folder (tools/windows_deploy.py)")
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", type=pathlib.Path, required=True)
    ap.add_argument("--icon", type=pathlib.Path)
    ap.add_argument("--zip", action="store_true")
    ap.add_argument("--installer", action="store_true")
    ap.add_argument("--sha256", action="store_true")
    a = ap.parse_args()
    if not (a.zip or a.installer or a.sha256):
        ap.error("nothing to do: --zip, --installer and/or --sha256")
    if not (a.folder / "Lumaflip.exe").is_file():
        raise SystemExit(f"error: {a.folder / 'Lumaflip.exe'} missing - run tools/windows_deploy.py first")
    a.out.mkdir(parents=True, exist_ok=True)
    zip_name, setup_name = names(a.version)
    if a.zip:
        make_zip(a.folder, a.out / zip_name)
    if a.installer:
        make_installer(a.folder, a.version, a.out, a.icon)
    if a.sha256:
        for name in (zip_name, setup_name):
            if not (a.out / name).is_file():
                raise SystemExit(f"error: {a.out / name} missing")
            write_sha256(a.out / name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
