#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Installs, updates and uninstalls the Windows installer the way WinSparkle uses it
(F-MAC-08, docs/WINDOWS.md) - CI job "Windows app" and the Windows release job. Windows only;
changes the current user's installation of Lumaflip (CI runners only).

    python tools/windows_installer_check.py dist/Lumaflip-<v>-windows-x64-setup.exe --version <v>
        [--expect-winsparkle] [--log-dir DIR]

1. Fresh per-user installation (/VERYSILENT): the uninstall entry of the AppId exists with the
   version, Lumaflip.exe, lumaflip-decoder.exe (and WinSparkle.dll) are in place.
2. The update as WinSparkle starts it - the same installer again with the appcast's arguments
   (tools/appcast_windows.py INSTALLER_ARGUMENTS: /SILENT ... /CLOSEAPPLICATIONS /RELAUNCH=1)
   while Lumaflip is running: the running copy is closed, the installation is upgraded in
   place (same folder, one uninstall entry) and the updated Lumaflip is started again.
3. Silent uninstallation: entry and files are gone.
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from appcast_windows import INSTALLER_ARGUMENTS  # noqa: E402

APP_ID = "{6E0D5D8A-6A43-4C6B-9C6F-4C1A5B7E1F2D}"  # Lumaflip.iss [Setup] AppId
UNINSTALL_KEY = rf"Software\Microsoft\Windows\CurrentVersion\Uninstall\{APP_ID}_is1"


def fail(msg: str) -> None:
    raise SystemExit(f"windows_installer_check: {msg}")


def uninstall_entry() -> dict[str, str] | None:
    import winreg  # Windows only

    for root in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
        try:
            with winreg.OpenKey(root, UNINSTALL_KEY) as k:
                out = {}
                for name in ("InstallLocation", "DisplayVersion", "UninstallString", "DisplayName"):
                    try:
                        out[name] = str(winreg.QueryValueEx(k, name)[0])
                    except OSError:
                        out[name] = ""
                return out
        except OSError:
            continue
    return None


def lumaflip_pids(install_dir: pathlib.Path) -> set[int]:
    """PIDs of Lumaflip.exe processes started from install_dir."""
    script = ("Get-Process -Name Lumaflip -ErrorAction SilentlyContinue | "
              "ForEach-Object { \"$($_.Id)`t$($_.Path)\" }")
    r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                       capture_output=True, text=True, check=False)
    pids = set()
    want = str(install_dir.resolve()).lower()
    for line in r.stdout.splitlines():
        pid, _, path = line.partition("\t")
        if pid.strip().isdigit() and path.lower().startswith(want):
            pids.add(int(pid))
    return pids


def kill(pids: set[int]) -> None:
    for pid in pids:
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True, check=False)


def run_setup(setup: pathlib.Path, args: list[str], log: pathlib.Path, timeout: int = 600) -> None:
    cmd = [str(setup), *args, f"/LOG={log}"]
    print("+", " ".join(cmd), flush=True)
    rc = subprocess.run(cmd, timeout=timeout, check=False).returncode
    if rc != 0:
        if log.is_file():
            print(log.read_text(encoding="utf-8", errors="replace")[-6000:])
        fail(f"installer exited with {rc} ({' '.join(args)})")


def wait_for(cond, timeout: float, what: str):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = cond()
        if value:
            return value
        time.sleep(1)
    fail(f"timed out waiting for {what}")


def main() -> int:
    if sys.platform != "win32":
        fail("Windows only")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("setup", type=pathlib.Path)
    ap.add_argument("--version", required=True)
    ap.add_argument("--expect-winsparkle", action="store_true")
    ap.add_argument("--log-dir", type=pathlib.Path, default=pathlib.Path("."))
    a = ap.parse_args()
    a.log_dir.mkdir(parents=True, exist_ok=True)
    if uninstall_entry():
        fail(f"Lumaflip is already installed ({UNINSTALL_KEY}) - run on a clean machine")

    # 1. fresh installation
    run_setup(a.setup, ["/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"], a.log_dir / "install.log")
    entry = uninstall_entry()
    if not entry:
        fail("no uninstall entry after the installation")
    install_dir = pathlib.Path(entry["InstallLocation"])
    print(f"installed {entry['DisplayVersion']} in {install_dir}")
    if entry["DisplayVersion"] != a.version:
        fail(f"DisplayVersion {entry['DisplayVersion']!r}, expected {a.version!r}")
    expected = ["Lumaflip.exe", "lumaflip-decoder.exe"] + (["WinSparkle.dll"] if a.expect_winsparkle else [])
    missing = [f for f in expected if not (install_dir / f).is_file()]
    if missing:
        fail(f"missing in {install_dir}: {missing}")

    # 2. update in place while the app runs, as WinSparkle starts it
    running = subprocess.Popen([str(install_dir / "Lumaflip.exe")])
    wait_for(lambda: running.pid in lumaflip_pids(install_dir), 30, "Lumaflip to start")
    time.sleep(3)
    run_setup(a.setup, INSTALLER_ARGUMENTS.split(), a.log_dir / "update.log")
    try:
        running.wait(timeout=30)
    except subprocess.TimeoutExpired:
        fail("the running Lumaflip was not closed by the update")
    relaunched = wait_for(lambda: lumaflip_pids(install_dir) - {running.pid}, 60,
                          "the updated Lumaflip to start again (/RELAUNCH=1)")
    print(f"relaunched after the update: pid {sorted(relaunched)}")
    time.sleep(3)
    kill(relaunched)
    after = uninstall_entry()
    if not after or pathlib.Path(after["InstallLocation"]) != install_dir:
        fail(f"update did not upgrade in place: {after}")
    if after["DisplayVersion"] != a.version:
        fail(f"DisplayVersion after the update: {after['DisplayVersion']!r}")
    print(f"upgraded in place: {install_dir}")

    # 3. uninstallation (the uninstaller copies itself to %TEMP% and returns at once)
    wait_for(lambda: not lumaflip_pids(install_dir), 30, "Lumaflip to exit")
    uninstaller = install_dir / "unins000.exe"
    if not uninstaller.is_file():
        fail(f"{uninstaller} missing")
    subprocess.run([str(uninstaller), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"], check=False)
    wait_for(lambda: uninstall_entry() is None and not (install_dir / "Lumaflip.exe").exists(), 120,
             "the uninstallation")
    print("uninstalled")
    return 0


if __name__ == "__main__":
    sys.exit(main())
