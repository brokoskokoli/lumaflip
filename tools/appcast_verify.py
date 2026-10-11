#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks a freshly generated Sparkle or WinSparkle appcast against the previous one before
it is published (release.yml, F-MAC-08):

  * every item of the old appcast is still there (same sparkle:version, same enclosure
    URL) - generate_appcast must never drop or rewrite earlier releases;
  * the new appcast has exactly one item more, or the same number when this version was
    already in the old feed (re-run of a release);
  * the item for --version exists, its enclosure URL is --url, it carries an EdDSA
    signature (sparkle:edSignature) and, with --archive, its length is the archive size;
  * the feeds stay apart: without --os (macOS appcast.xml) no item may be marked for Windows;
    with --os windows (appcast-windows.xml) every item must be marked sparkle:os="windows",
    and the new one must carry the installer arguments for WinSparkle.

Usage: tools/appcast_verify.py --old OLD.xml|- --new NEW.xml --version 1.0.0 --url URL [--archive DMG]
                               [--os windows]
       (--old - : there was no appcast yet)
Prints a short summary; exit code 1 on any violation.
"""
import argparse, os, sys
import xml.etree.ElementTree as ET

SP = "{http://www.andymatuschak.org/xml-namespaces/sparkle}"


def items(path):
    """[(version, enclosure_url, enclosure_attrib)] of an appcast file."""
    root = ET.parse(path).getroot()
    out = []
    for item in root.iter("item"):
        enc = item.find("enclosure")
        attrib = dict(enc.attrib) if enc is not None else {}
        version = item.findtext(SP + "version") or attrib.get(SP + "version") or ""
        out.append((version.strip(), attrib.get("url", ""), attrib))
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--archive")
    ap.add_argument("--os", choices=["windows"], help="the Windows feed (appcast-windows.xml)")
    a = ap.parse_args(argv)

    errors = []
    old = [] if a.old == "-" else items(a.old)
    new = items(a.new)
    new_pairs = {(v, u) for v, u, _ in new}
    for v, u, _ in old:
        if (v, u) not in new_pairs:
            errors.append(f"old item {v or '?'} ({u or 'no enclosure'}) is missing or changed in the new appcast")
    versions_old = [v for v, _, _ in old]
    expected = len(old) + (0 if a.version in versions_old else 1)
    if len(new) != expected:
        errors.append(f"new appcast has {len(new)} items, expected {expected} (old: {len(old)})")
    mine = [(u, at) for v, u, at in new if v == a.version]
    if len(mine) != 1:
        errors.append(f"expected exactly one item for version {a.version}, found {len(mine)}")
    else:
        url, attrib = mine[0]
        if url != a.url:
            errors.append(f"enclosure URL of {a.version} is {url!r}, expected {a.url!r}")
        if not attrib.get(SP + "edSignature"):
            errors.append(f"item {a.version} has no sparkle:edSignature")
        if a.archive:
            size = os.path.getsize(a.archive)
            if attrib.get("length") != str(size):
                errors.append(f"enclosure length of {a.version} is {attrib.get('length')}, archive has {size} bytes")
    for v, _, at in new:
        os_name = at.get(SP + "os", "")
        if a.os == "windows" and os_name != "windows":
            errors.append(f"item {v or '?'} is not marked sparkle:os=\"windows\" (got {os_name!r})")
        if a.os is None and os_name.startswith("windows"):
            errors.append(f"item {v or '?'} is a Windows item in the macOS feed")
    if a.os == "windows" and len(mine) == 1 and not mine[0][1].get(SP + "installerArguments"):
        errors.append(f"item {a.version} has no sparkle:installerArguments (silent Inno Setup update)")
    urls = [u for _, u, _ in new]
    if len(set(urls)) != len(urls):
        errors.append("duplicate enclosure URLs in the new appcast")

    print(f"appcast: {len(old)} item(s) before, {len(new)} after; versions: "
          + ", ".join(v for v, _, _ in new))
    for e in errors:
        print(f"appcast_verify: {e}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
