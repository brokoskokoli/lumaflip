#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes the WinSparkle appcast of the Windows build (appcast-windows.xml on gh-pages,
release.yml job "appcast"; F-MAC-08, docs/RELEASING.md).

    tools/appcast_windows.py --old OLD.xml|- --out NEW.xml --version 1.3.0 --url URL \\
        --length BYTES --ed-signature BASE64 [--notes NOTES.html] [--date RFC822]

* Every item of the old feed is kept unchanged (the installers stay in their GitHub
  Releases); an item for --version that already exists is replaced (re-run of a release).
* The new item: <sparkle:version> and <sparkle:shortVersionString> = --version, the release
  notes inline as <description> (WinSparkle shows them in its window), the enclosure with
  the installer URL, its length, its EdDSA signature (sparkle:edSignature, from Sparkle's
  sign_update with the same key as the macOS feed), sparkle:os="windows" and the Inno Setup
  arguments WinSparkle starts it with (INSTALLER_ARGUMENTS, see packaging/windows/Lumaflip.iss).
* Items are ordered newest first (WinSparkle itself picks the highest version).

The macOS feed (appcast.xml) is a different file: Sparkle never sees Windows items.
tools/appcast_verify.py --os windows checks the result before it is published.
"""
from __future__ import annotations

import argparse
import base64
import email.utils
import html
import re
import sys
import xml.etree.ElementTree as ET

SPARKLE_NS = "http://www.andymatuschak.org/xml-namespaces/sparkle"
DC_NS = "http://purl.org/dc/elements/1.1/"
SP = "{" + SPARKLE_NS + "}"
FEED_TITLE = "Lumaflip (Windows)"
FEED_LINK = "https://github.com/brokoskokoli/lumaflip"
MIN_WINDOWS = "10.0.17763"  # Windows 10 1809 (MinVersion in Lumaflip.iss)
# Inno Setup: silent with a progress window, no "This will install" prompt, no message boxes,
# close running copies, never reboot, start the updated app again (Lumaflip.iss [Run]).
INSTALLER_ARGUMENTS = "/SILENT /SP- /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /RELAUNCH=1"
VERSION_RE = re.compile(r"^[0-9]+(\.[0-9]+){1,3}$")


def version_key(v: str) -> tuple:
    return tuple(int(p) if p.isdigit() else 0 for p in re.split(r"[.]", v))


def item_version(item: ET.Element) -> str:
    v = item.findtext(SP + "version")
    if not v:
        enc = item.find("enclosure")
        v = enc.get(SP + "version", "") if enc is not None else ""
    return (v or "").strip()


def notes_document(fragment: str) -> str:
    """The release notes (HTML fragment from GitHub's Markdown API) as a small page for
    WinSparkle's web view: UTF-8, the system font."""
    return ("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<style>body{font-family:'Segoe UI',sans-serif;font-size:10pt;margin:8px}"
            "h1,h2,h3{font-size:11pt}</style></head><body>"
            + fragment.strip() + "</body></html>")


def build(old_path: str | None, version: str, url: str, length: int, signature: str,
          notes_html: str | None, date: str | None) -> ET.ElementTree:
    ET.register_namespace("sparkle", SPARKLE_NS)
    ET.register_namespace("dc", DC_NS)
    if old_path:
        tree = ET.parse(old_path)
        rss = tree.getroot()
        channel = rss.find("channel")
        if rss.tag != "rss" or channel is None:
            raise SystemExit(f"appcast_windows: {old_path} is no RSS appcast")
    else:
        rss = ET.Element("rss", {"version": "2.0"})
        channel = ET.SubElement(rss, "channel")
        ET.SubElement(channel, "title").text = FEED_TITLE
        ET.SubElement(channel, "link").text = FEED_LINK
        ET.SubElement(channel, "description").text = "Updates von Lumaflip für Windows"
        ET.SubElement(channel, "language").text = "de"
        tree = ET.ElementTree(rss)

    old_items = channel.findall("item")
    for it in old_items:
        if item_version(it) == version:
            channel.remove(it)  # re-run: replaced by the new item below

    item = ET.Element("item")
    ET.SubElement(item, "title").text = f"Lumaflip {version}"
    ET.SubElement(item, "pubDate").text = date or email.utils.formatdate(usegmt=True)
    ET.SubElement(item, SP + "version").text = version
    ET.SubElement(item, SP + "shortVersionString").text = version
    ET.SubElement(item, SP + "minimumSystemVersion").text = MIN_WINDOWS
    if notes_html:
        ET.SubElement(item, "description").text = notes_document(notes_html)
    ET.SubElement(item, "enclosure", {
        "url": url,
        "length": str(length),
        "type": "application/octet-stream",
        SP + "os": "windows",
        SP + "installerArguments": INSTALLER_ARGUMENTS,
        SP + "edSignature": signature,
    })

    # Newest first: insert the new item before the first item with a lower version.
    items = channel.findall("item")
    index = len(list(channel))
    for it in items:
        if version_key(item_version(it)) < version_key(version):
            index = list(channel).index(it)
            break
    channel.insert(index, item)
    ET.indent(tree, space="  ")
    return tree


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", required=True, help="the current appcast-windows.xml, or - for none")
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--url", required=True, help="download URL of the installer (GitHub Release asset)")
    ap.add_argument("--length", required=True, type=int, help="size of the installer in bytes")
    ap.add_argument("--ed-signature", required=True, help="EdDSA signature of the installer (base64)")
    ap.add_argument("--notes", help="release notes as an HTML fragment")
    ap.add_argument("--date", help="pubDate (RFC 822); default: now")
    a = ap.parse_args(argv)

    if not VERSION_RE.match(a.version):
        raise SystemExit(f"appcast_windows: version {a.version!r} is not numeric (x.y.z)")
    if not a.url.startswith("https://"):
        raise SystemExit("appcast_windows: the installer URL must use https")
    if a.length <= 0:
        raise SystemExit("appcast_windows: --length must be positive")
    try:
        sig = base64.b64decode(a.ed_signature, validate=True)
    except ValueError:
        sig = b""
    if len(sig) != 64:
        raise SystemExit("appcast_windows: --ed-signature is not a base64 ed25519 signature (64 bytes)")
    notes = None
    if a.notes:
        with open(a.notes, encoding="utf-8") as f:
            notes = f.read()
    tree = build(None if a.old == "-" else a.old, a.version, a.url, a.length, a.ed_signature, notes, a.date)
    tree.write(a.out, encoding="utf-8", xml_declaration=True)
    with open(a.out, "a", encoding="utf-8") as f:
        f.write("\n")
    n = len(tree.getroot().find("channel").findall("item"))
    print(f"appcast-windows: {n} item(s), newest {html.escape(a.version)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
