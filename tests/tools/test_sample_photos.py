#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The sample photos of the screenshots (tools/fetch_sample_photos.py): the manifest is
complete and pinned, and the EXIF GPS writer produces positions that read back exactly.
Run by ctest (label unit) on every platform: python3 -m unittest tests/tools/test_sample_photos.py
"""
from __future__ import annotations

import json
import pathlib
import re
import struct
import sys
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))

import fetch_sample_photos as fsp  # noqa: E402


def segment(marker: int, body: bytes) -> bytes:
    return b"\xff" + bytes([marker]) + struct.pack(">H", len(body) + 2) + body


def tiff(endian: str, entries: list[tuple[int, int, int, bytes]]) -> bytes:
    """A TIFF block with IFD0 at offset 8 (values of at most 4 bytes, inline)."""
    order = b"II" if endian == "<" else b"MM"
    out = order + struct.pack(endian + "HI", 42, 8) + struct.pack(endian + "H", len(entries))
    for tag, typ, count, value in entries:
        out += struct.pack(endian + "HHI", tag, typ, count) + value.ljust(4, b"\0")
    return out + struct.pack(endian + "I", 0)


def jpeg(*segments: bytes) -> bytes:
    # SOI, the segments, a start of scan with "image data" and EOI.
    return b"\xff\xd8" + b"".join(segments) + segment(0xDA, b"\x01\x02") + b"\x12\x34" + b"\xff\xd9"


JFIF = segment(0xE0, b"JFIF\0\x01\x02\0\0\x01\0\x01\0\0")


class ExifGps(unittest.TestCase):
    def test_adds_gps_to_existing_exif_both_byte_orders(self):
        for endian in ("<", ">"):
            orientation = struct.pack(endian + "H", 1)
            exif = segment(0xE1, b"Exif\0\0" + tiff(endian, [(0x0112, 3, 1, orientation)]))
            data = fsp.add_gps(jpeg(JFIF, exif), 37.971918, 23.726041)
            lat, lon = fsp.read_gps(data)
            self.assertAlmostEqual(lat, 37.971918, places=6)
            self.assertAlmostEqual(lon, 23.726041, places=6)
            # The image data after the metadata is untouched.
            self.assertTrue(data.endswith(segment(0xDA, b"\x01\x02") + b"\x12\x34\xff\xd9"))
            # The other IFD0 entry is still there (copied into the new IFD0).
            self.assertIn(b"\x01\x12" if endian == ">" else b"\x12\x01", data)

    def test_creates_exif_after_jfif_when_missing(self):
        data = fsp.add_gps(jpeg(JFIF), -33.856784, -151.215297)
        self.assertEqual(data[:2], b"\xff\xd8")
        self.assertEqual(data[2:2 + len(JFIF)], JFIF)  # Exif follows the JFIF header
        self.assertEqual(data[2 + len(JFIF):2 + len(JFIF) + 2], b"\xff\xe1")
        lat, lon = fsp.read_gps(data)
        self.assertAlmostEqual(lat, -33.856784, places=6)
        self.assertAlmostEqual(lon, -151.215297, places=6)

    def test_replaces_an_existing_position(self):
        once = fsp.add_gps(jpeg(JFIF), 10.0, 20.0)
        twice = fsp.add_gps(once, 35.011545, 24.761528)
        lat, lon = fsp.read_gps(twice)
        self.assertAlmostEqual(lat, 35.011545, places=6)
        self.assertAlmostEqual(lon, 24.761528, places=6)
        self.assertEqual(twice.count(b"Exif\0\0"), 1)

    def test_no_gps(self):
        self.assertIsNone(fsp.read_gps(jpeg(JFIF)))

    def test_rejects_non_jpeg(self):
        with self.assertRaises(ValueError):
            fsp.add_gps(b"\x89PNG\r\n", 1.0, 2.0)

    def test_dms_rounding_carries(self):
        # 0.9999999999 degrees rounds up to a full degree instead of 60 minutes/seconds.
        self.assertEqual(fsp._dms(12.99999999999), [(13, 1), (0, 1), (0, 10000)])
        self.assertEqual(fsp._dms(-1.5), [(1, 1), (30, 1), (0, 10000)])


class Manifest(unittest.TestCase):
    def setUp(self):
        self.manifest = json.loads(fsp.MANIFEST.read_text(encoding="utf-8"))

    def test_entries_are_pinned_and_free(self):
        photos = self.manifest["photos"]
        self.assertGreaterEqual(len(photos), 12)
        names = [p["name"] for p in photos]
        self.assertEqual(len(set(names)), len(names))
        self.assertEqual(len({p["sha256"] for p in photos}), len(photos))
        for p in photos:
            with self.subTest(p["name"]):
                self.assertTrue(p["name"].endswith(".jpg"))
                self.assertNotIn("/", p["name"])
                self.assertRegex(p["sha256"], r"^[0-9a-f]{64}$")
                self.assertTrue(p["url"].startswith("https://upload.wikimedia.org/wikipedia/commons/"))
                self.assertTrue(p["source"].startswith("https://commons.wikimedia.org/wiki/File:"))
                self.assertIn(p["license"], ("CC0-1.0", "PDM-1.0"))
                self.assertTrue(p["author"].strip())
                self.assertTrue(-90 <= p["lat"] <= 90 and -180 <= p["lon"] <= 180)
                self.assertTrue(re.match(r"^[A-Za-z0-9 .,'()-]+$", p["name"]), "ASCII file names only")

    def test_photos_are_credited(self):
        # Every author of the manifest is named in the README credits and on the website.
        root = TOOLS.parent
        for doc in ("README.md", "README.de.md", "website/index.html", "website/de/index.html"):
            text = (root / doc).read_text(encoding="utf-8")
            for author in {p["author"] for p in self.manifest["photos"]}:
                with self.subTest(doc=doc, author=author):
                    self.assertTrue(author in text, f"{author} is not credited in {doc}")


if __name__ == "__main__":
    unittest.main()
