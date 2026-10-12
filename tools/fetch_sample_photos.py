#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Downloads the sample photos for the screenshots (tools/screenshots, docs/RELEASING.md
"Screenshots") into a folder, ready to be opened by the screenshot tool.

The photos are CC0 1.0 files from Wikimedia Commons listed in
tools/screenshots/sample-photos.json, each pinned by the SHA-256 of the original file; a
download with another hash is rejected. Nothing is committed: the originals are cached in
<out>/.originals (git-ignored build directory), the prepared copies are written to <out>:

  * downscaled to `maxEdge` pixels on the long edge (macOS `sips`, keeps EXIF and the ICC
    profile; elsewhere the original is copied unchanged),
  * GPS position from the manifest written into the EXIF data (the Commons files carry their
    position only on the description page), so the folder map has something to show,
  * modification times spread over a day in name order (stable "date modified" sort).

Usage: tools/fetch_sample_photos.py [--out build/sample-photos] [--offline]
  --offline   use the cached originals only (fails if one is missing)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "tools" / "screenshots" / "sample-photos.json"
USER_AGENT = "LumaflipScreenshots/1.0 (https://github.com/brokoskokoli/lumaflip)"
# 2025-06-14 10:00 UTC: first modification time; one photo every 17 minutes after that.
BASE_MTIME = 1749895200
MTIME_STEP = 17 * 60


# ---- EXIF GPS (pure functions, tests/tools/test_sample_photos.py) ----

def _dms(value: float) -> list[tuple[int, int]]:
    """Degrees, minutes, seconds of |value| as EXIF rationals (seconds to 1/10000)."""
    value = abs(value)
    deg = int(value)
    minutes_f = (value - deg) * 60
    minutes = int(minutes_f)
    seconds = round((minutes_f - minutes) * 60 * 10000)
    if seconds >= 60 * 10000:  # rounding up to a full minute
        seconds -= 60 * 10000
        minutes += 1
    if minutes >= 60:
        minutes -= 60
        deg += 1
    return [(deg, 1), (minutes, 1), (seconds, 10000)]


def _gps_ifd(lat: float, lon: float, endian: str, base: int) -> bytes:
    """A GPS IFD placed at TIFF offset `base` (its value data follows the entry table)."""
    entries = [
        (0x0000, 1, 4, bytes([2, 3, 0, 0])),                     # GPSVersionID BYTE[4]
        (0x0001, 2, 2, (b"N" if lat >= 0 else b"S") + b"\0"),    # GPSLatitudeRef ASCII
        (0x0002, 5, 3, b"".join(struct.pack(endian + "II", n, d) for n, d in _dms(lat))),
        (0x0003, 2, 2, (b"E" if lon >= 0 else b"W") + b"\0"),    # GPSLongitudeRef ASCII
        (0x0004, 5, 3, b"".join(struct.pack(endian + "II", n, d) for n, d in _dms(lon))),
    ]
    table_size = 2 + 12 * len(entries) + 4
    table = struct.pack(endian + "H", len(entries))
    data = b""
    for tag, typ, count, value in entries:
        if len(value) <= 4:
            table += struct.pack(endian + "HHI", tag, typ, count) + value.ljust(4, b"\0")
        else:
            table += struct.pack(endian + "HHII", tag, typ, count, base + table_size + len(data))
            data += value
            if len(data) % 2:
                data += b"\0"
    table += struct.pack(endian + "I", 0)
    return table + data


def _with_gps(tiff: bytes, lat: float, lon: float) -> bytes:
    """The TIFF structure of an Exif block with a GPS IFD. Everything is appended: a copy of
    IFD0 with the GPSInfo pointer (replacing an old one) and the GPS IFD; the header then
    points at the new IFD0. All other offsets stay valid."""
    if tiff[:2] == b"II":
        endian = "<"
    elif tiff[:2] == b"MM":
        endian = ">"
    else:
        raise ValueError("not a TIFF header")
    if struct.unpack(endian + "H", tiff[2:4])[0] != 42:
        raise ValueError("not a TIFF header")
    ifd0 = struct.unpack(endian + "I", tiff[4:8])[0]
    count = struct.unpack(endian + "H", tiff[ifd0:ifd0 + 2])[0]
    entries = [tiff[ifd0 + 2 + 12 * i: ifd0 + 14 + 12 * i] for i in range(count)]
    next_ifd = tiff[ifd0 + 2 + 12 * count: ifd0 + 6 + 12 * count]
    entries = [e for e in entries if struct.unpack(endian + "H", e[:2])[0] != 0x8825]

    out = bytearray(tiff)
    if len(out) % 2:
        out += b"\0"
    new_ifd0 = len(out)
    table_size = 2 + 12 * (len(entries) + 1) + 4
    gps_at = new_ifd0 + table_size
    entries.append(struct.pack(endian + "HHII", 0x8825, 4, 1, gps_at))
    entries.sort(key=lambda e: struct.unpack(endian + "H", e[:2])[0])
    out += struct.pack(endian + "H", len(entries)) + b"".join(entries) + next_ifd
    out += _gps_ifd(lat, lon, endian, gps_at)
    out[4:8] = struct.pack(endian + "I", new_ifd0)
    return bytes(out)


def _empty_tiff() -> bytes:
    return b"MM" + struct.pack(">HI", 42, 8) + struct.pack(">H", 0) + struct.pack(">I", 0)


def add_gps(jpeg: bytes, lat: float, lon: float) -> bytes:
    """JPEG bytes with the GPS position in the Exif block (created if there is none)."""
    if jpeg[:2] != b"\xff\xd8":
        raise ValueError("not a JPEG file")
    pos = 2
    insert_at = 2
    while pos + 4 <= len(jpeg) and jpeg[pos] == 0xFF:
        marker = jpeg[pos + 1]
        if marker == 0xDA or marker == 0xD9:  # start of scan / end: no more metadata
            break
        length = struct.unpack(">H", jpeg[pos + 2:pos + 4])[0]
        body = jpeg[pos + 4:pos + 2 + length]
        if marker == 0xE1 and body[:6] == b"Exif\0\0":
            tiff = _with_gps(body[6:], lat, lon)
            segment = b"Exif\0\0" + tiff
            if len(segment) + 2 > 0xFFFF:
                raise ValueError("Exif block too large")
            return jpeg[:pos] + b"\xff\xe1" + struct.pack(">H", len(segment) + 2) + segment + \
                jpeg[pos + 2 + length:]
        if marker == 0xE0:  # JFIF/JFXX: the Exif block goes after it
            insert_at = pos + 2 + length
        pos += 2 + length
    segment = b"Exif\0\0" + _with_gps(_empty_tiff(), lat, lon)
    return jpeg[:insert_at] + b"\xff\xe1" + struct.pack(">H", len(segment) + 2) + segment + jpeg[insert_at:]


def read_gps(jpeg: bytes) -> tuple[float, float] | None:
    """The GPS position of a JPEG's Exif block (tests and the self-check after writing)."""
    pos = 2
    while pos + 4 <= len(jpeg) and jpeg[pos] == 0xFF and jpeg[pos + 1] not in (0xDA, 0xD9):
        length = struct.unpack(">H", jpeg[pos + 2:pos + 4])[0]
        body = jpeg[pos + 4:pos + 2 + length]
        if jpeg[pos + 1] == 0xE1 and body[:6] == b"Exif\0\0":
            tiff = body[6:]
            endian = "<" if tiff[:2] == b"II" else ">"

            def ifd(offset: int) -> dict[int, tuple[int, int, bytes]]:
                n = struct.unpack(endian + "H", tiff[offset:offset + 2])[0]
                result = {}
                for i in range(n):
                    tag, typ, count = struct.unpack(endian + "HHI", tiff[offset + 2 + 12 * i:offset + 10 + 12 * i])
                    result[tag] = (typ, count, tiff[offset + 10 + 12 * i:offset + 14 + 12 * i])
                return result

            root = ifd(struct.unpack(endian + "I", tiff[4:8])[0])
            if 0x8825 not in root:
                return None
            gps = ifd(struct.unpack(endian + "I", root[0x8825][2])[0])

            def coord(tag: int, ref: int, negative: bytes) -> float:
                at = struct.unpack(endian + "I", gps[tag][2])[0]
                parts = [struct.unpack(endian + "II", tiff[at + 8 * i:at + 8 * i + 8]) for i in range(3)]
                value = sum(n / d / f for (n, d), f in zip(parts, (1, 60, 3600)))
                return -value if gps[ref][2][:1] == negative else value

            return coord(2, 1, b"S"), coord(4, 3, b"W")
        pos += 2 + length
    return None


# ---- download and preparation ----

def sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(url: str, dest: pathlib.Path, expected: str) -> None:
    if not url.startswith("https://upload.wikimedia.org/"):
        raise SystemExit(f"refusing to download from {url} (only upload.wikimedia.org)")
    tmp = dest.with_suffix(dest.suffix + ".part")
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=120) as response, tmp.open("wb") as f:
        shutil.copyfileobj(response, f)
    actual = sha256(tmp)
    if actual != expected:
        tmp.unlink()
        raise SystemExit(f"SHA-256 mismatch for {url}: expected {expected}, got {actual}")
    tmp.replace(dest)


def downscale(src: pathlib.Path, dest: pathlib.Path, max_edge: int) -> None:
    sips = shutil.which("sips")
    if not sips:  # not macOS: keep the original size
        shutil.copyfile(src, dest)
        return
    subprocess.run([sips, "-Z", str(max_edge), "-s", "formatOptions", "90", str(src), "--out", str(dest)],
                   check=True, stdout=subprocess.DEVNULL)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", default=str(ROOT / "build" / "sample-photos"))
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args()

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    out = pathlib.Path(args.out).resolve()
    stamp = out / ".lumaflip-sample-photos"
    # Ours: empty, stamped, or only the cache (restored by CI without the stamp).
    if out.exists() and not stamp.exists() and any(f.name != ".originals" for f in out.iterdir()):
        raise SystemExit(f"{out} exists and was not created by this script - choose another --out")
    cache = out / ".originals"
    cache.mkdir(parents=True, exist_ok=True)
    stamp.touch()
    names = sorted(p["name"] for p in manifest["photos"])
    for photo in manifest["photos"]:
        name = photo["name"]
        if "/" in name or name.startswith("."):
            raise SystemExit(f"bad file name in the manifest: {name}")
        original = cache / (photo["sha256"] + ".jpg")
        if not original.exists() or sha256(original) != photo["sha256"]:
            if args.offline:
                raise SystemExit(f"{name}: not cached ({original})")
            print(f"downloading {name}")
            download(photo["url"], original, photo["sha256"])
        dest = out / name
        with tempfile.TemporaryDirectory() as tmp:
            scaled = pathlib.Path(tmp) / "scaled.jpg"
            downscale(original, scaled, int(manifest["maxEdge"]))
            data = add_gps(scaled.read_bytes(), float(photo["lat"]), float(photo["lon"]))
            got = read_gps(data)
            if not got or abs(got[0] - photo["lat"]) > 1e-5 or abs(got[1] - photo["lon"]) > 1e-5:
                raise SystemExit(f"{name}: GPS self-check failed ({got})")
            dest.write_bytes(data)
        mtime = BASE_MTIME + MTIME_STEP * names.index(name)
        os.utime(dest, (mtime, mtime))
    # Nothing else in the folder (a renamed manifest entry leaves no stale file behind).
    wanted = set(names)
    for f in out.iterdir():
        if f.is_file() and f.name not in wanted and not f.name.startswith("."):
            f.unlink()
    print(f"{len(names)} sample photos in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
