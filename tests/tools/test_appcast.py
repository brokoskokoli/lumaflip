#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""F-MAC-08: the appcast tools of the release pipeline (release.yml): tools/appcast_windows.py
writes the WinSparkle feed, tools/appcast_verify.py checks both feeds. Run by ctest (label
unit) on every platform: python3 -m unittest tests/tools/test_appcast.py
"""
from __future__ import annotations

import base64
import contextlib
import io
import pathlib
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

TOOLS = pathlib.Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))

import appcast_verify  # noqa: E402
import appcast_windows  # noqa: E402

SP = appcast_windows.SP
SIG = base64.b64encode(bytes(range(64))).decode()
URL = "https://github.com/brokoskokoli/lumaflip/releases/download/v{0}/Lumaflip-{0}-windows-x64-setup.exe"


class AppcastWindows(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, version, old="-", notes=None, length=1234, quiet=True):
        out = self.dir / f"appcast-{version}.xml"
        args = ["--old", str(old), "--out", str(out), "--version", version, "--url", URL.format(version),
                "--length", str(length), "--ed-signature", SIG, "--date", "Sat, 10 Oct 2026 12:00:00 GMT"]
        if notes is not None:
            n = self.dir / "notes.html"
            n.write_text(notes, encoding="utf-8")
            args += ["--notes", str(n)]
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(appcast_windows.main(args), 0)
        return out

    def verify(self, old, new, version, os_name="windows", archive=None):
        args = ["--old", str(old), "--new", str(new), "--version", version, "--url", URL.format(version)]
        if os_name:
            args += ["--os", os_name]
        if archive:
            args += ["--archive", str(archive)]
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as err:
            rc = appcast_verify.main(args)
        return rc, err.getvalue()

    def items(self, path):
        return ET.parse(path).getroot().find("channel").findall("item")

    def test_F_MAC_08_new_feed_has_signed_windows_item(self):
        out = self.write("1.3.0", notes="<p>Neu: Grüße ✓</p>")
        (item,) = self.items(out)
        self.assertEqual(item.findtext(SP + "version"), "1.3.0")
        self.assertEqual(item.findtext(SP + "shortVersionString"), "1.3.0")
        enc = item.find("enclosure")
        self.assertEqual(enc.get("url"), URL.format("1.3.0"))
        self.assertEqual(enc.get("length"), "1234")
        self.assertEqual(enc.get(SP + "os"), "windows")
        self.assertEqual(enc.get(SP + "edSignature"), SIG)
        self.assertIn("/SILENT", enc.get(SP + "installerArguments"))
        self.assertIn("/RELAUNCH=1", enc.get(SP + "installerArguments"))
        self.assertIn("Grüße ✓", item.findtext("description"))
        self.assertIn('charset="utf-8"', item.findtext("description"))
        self.assertEqual(self.verify("-", out, "1.3.0"), (0, ""))

    def test_F_MAC_08_old_items_kept_and_newest_first(self):
        first = self.write("1.3.0")
        second = self.write("1.4.0", old=first)
        self.assertEqual([appcast_windows.item_version(i) for i in self.items(second)], ["1.4.0", "1.3.0"])
        old_enc = self.items(first)[0].find("enclosure").attrib
        self.assertEqual(self.items(second)[1].find("enclosure").attrib, old_enc)
        self.assertEqual(self.verify(first, second, "1.4.0")[0], 0)
        # Re-run of the same release: replaced, not duplicated.
        again = self.write("1.4.0", old=second, length=999)
        self.assertEqual(len(self.items(again)), 2)
        self.assertEqual(self.items(again)[0].find("enclosure").get("length"), "999")
        self.assertEqual(self.verify(second, again, "1.4.0")[0], 0)

    def test_F_MAC_08_verify_rejects_dropped_items_and_mixed_feeds(self):
        first = self.write("1.3.0")
        second = self.write("1.4.0", old=first)
        lone = self.write("1.4.0")  # without the old item
        rc, err = self.verify(first, lone, "1.4.0")
        self.assertEqual(rc, 1)
        self.assertIn("missing", err)
        # A Windows feed checked as the macOS feed: refused (Sparkle must never see it).
        rc, err = self.verify(first, second, "1.4.0", os_name=None)
        self.assertEqual(rc, 1)
        self.assertIn("Windows item in the macOS feed", err)
        # Archive length must match.
        archive = self.dir / "setup.exe"
        archive.write_bytes(b"x" * 10)
        rc, err = self.verify(first, second, "1.4.0", archive=archive)
        self.assertEqual(rc, 1)
        self.assertIn("length", err)

    def test_F_MAC_08_verify_rejects_unmarked_or_unsigned_item(self):
        out = self.write("1.3.0")
        tree = ET.parse(out)
        enc = tree.getroot().find("channel/item/enclosure")
        del enc.attrib[SP + "os"]
        del enc.attrib[SP + "edSignature"]
        tree.write(out, encoding="utf-8", xml_declaration=True)
        rc, err = self.verify("-", out, "1.3.0")
        self.assertEqual(rc, 1)
        self.assertIn("sparkle:os", err)
        self.assertIn("edSignature", err)

    def test_F_MAC_08_refuses_bad_input(self):
        out = str(self.dir / "x.xml")
        base = ["--old", "-", "--out", out, "--length", "1", "--ed-signature", SIG]
        for extra in (["--version", "1.3", "--url", "http://example.org/a.exe"],
                      ["--version", "1.3.0-beta", "--url", URL.format("1.3.0")]):
            with self.assertRaises(SystemExit):
                appcast_windows.main(base + extra)
        with self.assertRaises(SystemExit):
            appcast_windows.main(["--old", "-", "--out", out, "--length", "1", "--ed-signature", "c2hvcnQ=",
                                  "--version", "1.3.0", "--url", URL.format("1.3.0")])


if __name__ == "__main__":
    unittest.main()
