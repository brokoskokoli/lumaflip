#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Smoke test of the deployed Windows app (CI job "Windows app").

    python tools/windows_smoke.py <deployed folder>

Starts the deployed Lumaflip.exe (offscreen, no visible window) with a generated PNG and
checks that it is still running after a few seconds: a missing DLL or plugin of the
deployment ends the process at once. The folder must not depend on the build tree or on
Qt's installation, so PATH and the Qt variables are cleared for the child.
"""
from __future__ import annotations

import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import time
import zlib


def tiny_png(path: pathlib.Path) -> None:
    w, h = 64, 48
    raw = b"".join(b"\x00" + bytes(c for x in range(w) for c in (x * 4 % 256, y * 5 % 256, 120))
                   for y in range(h))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def main() -> int:
    folder = pathlib.Path(sys.argv[1]).resolve()
    exe = folder / "Lumaflip.exe"
    if not exe.is_file():
        raise SystemExit(f"error: {exe} missing")
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith(("QT_", "QML"))}
    windir = os.environ.get("SystemRoot", r"C:\Windows")
    env["PATH"] = os.pathsep.join([str(folder), os.path.join(windir, "System32"), windir])
    env["QT_QPA_PLATFORM"] = "offscreen"
    with tempfile.TemporaryDirectory() as tmp:
        image = pathlib.Path(tmp) / "smoke.png"
        tiny_png(image)
        proc = subprocess.Popen([str(exe), str(image)], env=env, cwd=tmp,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        time.sleep(8)
        alive = proc.poll() is None
        if alive:
            proc.kill()
        out, _ = proc.communicate(timeout=30)
        text = out.decode("utf-8", "replace")
        print(text[-4000:])
        if not alive:
            raise SystemExit(f"error: Lumaflip.exe exited early with code {proc.returncode}")
    print("smoke test passed: the deployed app starts and keeps running")
    return 0


if __name__ == "__main__":
    sys.exit(main())
