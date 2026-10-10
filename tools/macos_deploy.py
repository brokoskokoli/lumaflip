#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes Lumaflip.app self-contained (SPEC 10.1, 11). Called by tools/package.sh.

Replaces macdeployqt, which with Homebrew's split Qt kegs fails to resolve several
@rpath references and drags in unrelated modules (Qt Quick via the virtual keyboard
plugin, Qt PDF, ...). This script deploys exactly:

  * the Qt plugins listed in PLUGINS,
  * the transitive closure of non-system libraries of the executable and those plugins
    (Qt frameworks stay frameworks and stay dynamically linked - LGPL, SPEC 11),
  * Qt's translations for every shipped UI language (Contents/Resources/translations,
    next to Lumaflip's own lumaflip_<lang>.qm that the build put there - checked here),
  * qt.conf, so Qt never looks at the Homebrew prefix,
  * the licence texts of every bundled component (Contents/Resources/licenses) plus
    index.json, which the About dialog reads - including the third-party libraries
    linked statically into the executable (libjpeg-turbo), listed by the build in
    Contents/Resources/static-components.json and checked against STATIC_COMPONENTS,
  * Sparkle.framework (automatic updates, F-MAC-08) if the app links it: copied as a
    whole (helpers Autoupdate, Updater.app, XPC services; symlinks kept) and left
    untouched otherwise - package.sh signs its parts inside-out (Sparkle's instructions).

All install names are rewritten to @rpath/..., all foreign LC_RPATHs are removed.
The result is unsigned (install_name_tool invalidates signatures); package.sh signs it.
"""
import argparse
import json
import os
import pathlib
import plistlib
import re
import shutil
import subprocess
import sys

# Qt plugins shipped in the bundle (relative to <qt>/share/qt/plugins).
# Not shipped on purpose:
#   imageformats/libqpdf   - pulls in Qt PDF (+ Qt Network); PDFs are M4 (SPEC 13)
#   imageformats/libqmng   - MNG is not a supported format; pulls in libmng + Little CMS
#   imageformats/libqjp2   - JPEG 2000 is decoded by ImageIO; pulls in JasPer
#   platforms/minimal      - only for tests
#   platforminputcontexts  - Qt Virtual Keyboard pulls in Qt Quick/QML
# platforms/offscreen is shipped for the decoder helper (HELPER below): fonts and image
# plugins without a window server connection.
PLUGINS = [
    "platforms/libqcocoa.dylib",
    "platforms/libqoffscreen.dylib",
    "styles/libqmacstyle.dylib",
    "iconengines/libqsvgicon.dylib",
    "imageformats/libqgif.dylib",
    "imageformats/libqicns.dylib",
    "imageformats/libqico.dylib",
    "imageformats/libqjpeg.dylib",
    "imageformats/libqmacheif.dylib",
    "imageformats/libqsvg.dylib",
    "imageformats/libqtga.dylib",
    "imageformats/libqtiff.dylib",
    "imageformats/libqwbmp.dylib",
    "imageformats/libqwebp.dylib",
]

# Decoder helper process (SPEC 8.5), next to the app binary in Contents/MacOS: deployed like
# the app (dependencies, rpath) and signed before the bundle by tools/package.sh.
HELPER = "lumaflip-decoder"

# UI languages (SPEC 4.16, F-I18N-02; src/core/Language.cpp, translations/CMakeLists.txt).
# German is the source language (lumaflip_de.qm holds its plural forms only).
UI_LANGUAGES = ["de", "en", "fr", "es", "it", "pt_BR", "pt_PT", "nl", "pl", "sv", "da", "nb", "fi",
                "cs", "ja", "zh_CN", "zh_TW", "ko"]
# Qt's catalogue per UI language: the first existing one is shipped (Qt has no pt_PT and
# no nb catalogue; app/Translations.cpp falls back the same way: pt_PT -> pt_BR, nb -> nn).
QT_CATALOG_CANDIDATES = {"pt_PT": ["pt_PT", "pt", "pt_BR"], "nb": ["nb", "no", "nn"]}


def qt_catalogs(qt_translations: pathlib.Path) -> list[str]:
    """qtbase_<lang>.qm files to ship, one per UI language (deduplicated)."""
    out = []
    for lang in UI_LANGUAGES:
        for cand in QT_CATALOG_CANDIDATES.get(lang, [lang]):
            name = f"qtbase_{cand}.qm"
            if (qt_translations / name).exists():
                if name not in out:
                    out.append(name)
                break
        else:
            raise SystemExit(f"error: no Qt translation for UI language {lang} in {qt_translations}")
    return out

SYSTEM_PREFIXES = ("/System/", "/usr/lib/")

# Prebuilt frameworks that are embedded unchanged (not from Homebrew): name -> licence
# metadata. Their nested helpers must not be rewritten or thinned.
EMBEDDED_FRAMEWORKS = {
    "Sparkle": {"id": "sparkle", "name": "Sparkle 2 (automatische Updates)", "license": "MIT",
                "homepage": "https://sparkle-project.org"},
}

# Third-party libraries linked statically into the executable: no dylib in the bundle
# reveals them, so the build lists the ones it linked in Contents/Resources/
# static-components.json ([{"id": ..., "library": <real path of the .a>}], written by
# src/CMakeLists.txt). Every listed id must be described here, with licence texts checked
# in under packaging/licenses/. `symbols`: if the executable exports one of them, the
# component must be in the manifest (catches a static library added without an entry).
STATIC_COMPONENTS = {
    "jpeg-turbo": {
        "name": "libjpeg-turbo – TurboJPEG (verlustfreies Drehen von JPEG-Dateien)",
        "license": "IJG AND BSD-3-Clause AND Zlib",
        "homepage": "https://libjpeg-turbo.org",
        "files": ["packaging/licenses/libjpeg-turbo-LICENSE.txt",
                  "packaging/licenses/libjpeg-turbo-README.ijg.txt"],
        "symbols": ["_tj3Transform", "_tj3Init"],
    },
}
STATIC_MANIFEST = "static-components.json"

# Components whose Homebrew keg carries no licence file: licence text from
# packaging/licenses/ (path relative to the repository root).
# Every Qt module keg ("qtbase", "qtsvg", "qtimageformats", ...) uses the Qt entry.
QT_LICENSE_FILES = ["packaging/licenses/LGPL-3.0.txt", "LICENSE", "packaging/licenses/Qt-NOTICE.txt"]
LICENSE_FALLBACK: dict[str, list[str]] = {}
# Additional texts for kegs whose own licence file does not cover the bundled library
# (gettext ships the GPL text only, but libintl is LGPL-2.1-or-later).
LICENSE_EXTRA = {"gettext": ["packaging/licenses/LGPL-2.1.txt"]}
LICENSE_FILE_RE = re.compile(r"^(LICEN[CS]E|COPYING|NOTICE|COPYRIGHT|LGPL|GPL|MIT|BSD|PATENTS)", re.I)

# Human-readable component names for the About dialog.
DISPLAY_NAMES = {
    "qtbase": "Qt 6 – Qt Base (Core, Gui, Widgets, Concurrent, DBus)",
    "qtsvg": "Qt 6 – Qt SVG",
    "qtimageformats": "Qt 6 – Qt Image Formats (Plugins)",
    "gettext": "gettext – libintl",
    "icu4c@78": "ICU",
    "xz": "xz – liblzma",
    "webp": "libwebp",
    "jpeg-turbo": "libjpeg-turbo",
    "dbus": "D-Bus (libdbus)",
}


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def is_macho(path: pathlib.Path) -> bool:
    try:
        with open(path, "rb") as f:
            magic = f.read(4)
    except OSError:
        return False
    return magic in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xce\xfa\xed\xfe", b"\xbe\xba\xfe\xca")


def load_commands(binary: pathlib.Path) -> tuple[str | None, list[str], list[str]]:
    """Returns (install id, dependencies, rpaths) of a Mach-O file."""
    out = run("otool", "-l", str(binary))
    ident, deps, rpaths = None, [], []
    cmd = None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("cmd "):
            cmd = line.split()[1]
        elif line.startswith("name ") and cmd in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB",
                                                  "LC_REEXPORT_DYLIB", "LC_LAZY_LOAD_DYLIB"):
            deps.append(line[5:].rsplit(" (offset", 1)[0])
        elif line.startswith("name ") and cmd == "LC_ID_DYLIB":
            ident = line[5:].rsplit(" (offset", 1)[0]
        elif line.startswith("path ") and cmd == "LC_RPATH":
            rpaths.append(line[5:].rsplit(" (offset", 1)[0])
    return ident, deps, rpaths


def expand(path: str, binary: pathlib.Path, exe: pathlib.Path) -> str:
    return path.replace("@loader_path", str(binary.parent)).replace("@executable_path", str(exe.parent))


def resolve(dep: str, binary: pathlib.Path, rpaths: list[str], exe: pathlib.Path) -> pathlib.Path:
    if dep.startswith("@rpath/"):
        rest = dep[len("@rpath/"):]
        for rp in rpaths:
            cand = pathlib.Path(expand(rp, binary, exe)) / rest
            if cand.exists():
                return cand
        raise SystemExit(f"error: cannot resolve {dep} of {binary} (rpaths: {rpaths})")
    cand = pathlib.Path(expand(dep, binary, exe))
    if not cand.exists():
        raise SystemExit(f"error: {dep} (needed by {binary}) does not exist")
    return cand


def framework_parts(path: str) -> tuple[str, str] | None:
    """'/x/QtCore.framework/Versions/A/QtCore' -> ('QtCore', 'A')."""
    m = re.search(r"([^/]+)\.framework/(?:Versions/([^/]+)/)?([^/]+)$", path)
    if not m:
        return None
    return m.group(1), m.group(2) or "A"


class Deployer:
    def __init__(self, app: pathlib.Path, qt_prefix: pathlib.Path, repo: pathlib.Path,
                 sparkle_dir: pathlib.Path | None = None):
        self.app = app
        self.contents = app / "Contents"
        self.frameworks = self.contents / "Frameworks"
        self.plugins_dir = self.contents / "PlugIns"
        self.exe = self.contents / "MacOS" / app.stem
        self.helper = self.contents / "MacOS" / HELPER
        self.qt = qt_prefix
        self.repo = repo
        self.copied: dict[str, pathlib.Path] = {}  # new install name -> path in bundle
        self.origin: dict[str, pathlib.Path] = {}  # new install name -> real source path
        self.users: dict[str, set[str]] = {}       # new install name -> bundled users
        self.sparkle_dir = sparkle_dir or repo / "third_party" / "Sparkle"
        self.embedded: dict[str, pathlib.Path] = {}  # framework name -> source directory

    # ---- copying ---------------------------------------------------------------------

    def bundle_name(self, dep: str) -> str:
        fw = framework_parts(dep)
        if fw:
            name, ver = fw
            return f"@rpath/{name}.framework/Versions/{ver}/{name}"
        return "@rpath/" + pathlib.PurePosixPath(dep).name

    def copy_dependency(self, source: pathlib.Path, new_id: str) -> pathlib.Path:
        real = source.resolve()
        rel = new_id[len("@rpath/"):]
        target = self.frameworks / rel
        fw = framework_parts(new_id)
        if fw:
            name, ver = fw
            fw_src = real.parent  # .../Name.framework/Versions/<ver>
            fw_dst = self.frameworks / f"{name}.framework"
            vdir = fw_dst / "Versions" / ver
            vdir.mkdir(parents=True, exist_ok=True)
            shutil.copy2(real, vdir / name)
            if (fw_src / "Resources").is_dir():
                shutil.copytree(fw_src / "Resources", vdir / "Resources", dirs_exist_ok=True,
                                symlinks=False)
            for link, dest in (("Versions/Current", ver), (name, f"Versions/Current/{name}"),
                               ("Resources", "Versions/Current/Resources")):
                p = fw_dst / link
                if not p.is_symlink() and (link != "Resources" or (vdir / "Resources").exists()):
                    p.symlink_to(dest)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(real, target)
        os.chmod(target, 0o755)
        return target

    def describe(self, binary: pathlib.Path) -> str:
        return str(binary.relative_to(self.contents))

    # ---- rewriting -------------------------------------------------------------------

    def rewrite(self, binary: pathlib.Path, new_rpath: str, new_id: str | None) -> None:
        ident, deps, rpaths = load_commands(binary)
        args = ["install_name_tool"]
        if new_id and ident != new_id:
            args += ["-id", new_id]
        for dep in deps:
            if not dep.startswith(SYSTEM_PREFIXES):
                nd = self.bundle_name(dep)
                if nd != dep:
                    args += ["-change", dep, nd]
        # Keep new_rpath if the build already set it (install_name_tool refuses to delete
        # and add the same rpath in one call); drop every other one.
        for rp in rpaths:
            if rp != new_rpath:
                args += ["-delete_rpath", rp]
        if new_rpath not in rpaths:
            args += ["-add_rpath", new_rpath]
        subprocess.run(args + [str(binary)], check=True, capture_output=True, text=True)

    # ---- main steps ------------------------------------------------------------------

    def deploy(self) -> None:
        plugin_root = self.qt / "share" / "qt" / "plugins"
        binaries: list[pathlib.Path] = [self.exe]
        for rel in PLUGINS:
            src = plugin_root / rel
            if not src.exists():
                raise SystemExit(f"error: Qt plugin {src} missing")
            dst = self.plugins_dir / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src.resolve(), dst)
            os.chmod(dst, 0o755)
            self.origin["plugin:" + rel] = src.resolve()
            binaries.append(dst)

        executables = [self.exe] + ([self.helper] if self.helper.exists() else [])
        for b in binaries + executables[1:]:
            orig = b if b in executables else self.origin.get("plugin:" + str(b.relative_to(self.plugins_dir)))
            self.collect_with_origin(b, orig)

        # Rewrite install names and rpaths.
        for exe in executables:
            self.rewrite(exe, "@executable_path/../Frameworks", None)
        for b in binaries[1:]:
            self.rewrite(b, "@loader_path/../../Frameworks", None)
        for new_id, path in self.copied.items():
            rp = "@loader_path/../../.." if framework_parts(new_id) else "@loader_path"
            self.rewrite(path, rp, new_id)

        # qt.conf: paths are relative to Contents/. Stops Qt from using its built-in
        # Homebrew prefix for plugins and translations.
        res = self.contents / "Resources"
        res.mkdir(exist_ok=True)
        (res / "qt.conf").write_text("[Paths]\nPlugins = PlugIns\nTranslations = Resources/translations\n")

        tr_dir = res / "translations"
        tr_dir.mkdir(exist_ok=True)
        qt_tr = self.qt / "share" / "qt" / "translations"
        for qm in qt_catalogs(qt_tr):
            shutil.copy2((qt_tr / qm).resolve(), tr_dir / qm)
            os.chmod(tr_dir / qm, 0o644)
        # Lumaflip's own catalogues come from the build (src/CMakeLists.txt).
        missing = [lang for lang in UI_LANGUAGES if not (tr_dir / f"lumaflip_{lang}.qm").exists()]
        if missing:
            raise SystemExit(f"error: {tr_dir} lacks lumaflip_<lang>.qm for {', '.join(missing)}")

    def collect_with_origin(self, binary: pathlib.Path, original: pathlib.Path) -> None:
        _, deps, _ = load_commands(binary)
        _, _, src_rpaths = load_commands(original)
        for dep in deps:
            if dep.startswith(SYSTEM_PREFIXES):
                continue
            new_id = self.bundle_name(dep)
            self.users.setdefault(new_id, set()).add(self.describe(binary))
            fw = framework_parts(dep)
            if fw and fw[0] in EMBEDDED_FRAMEWORKS:
                self.embed_framework(fw[0])
                continue
            if new_id in self.copied:
                continue
            src = resolve(dep, original, src_rpaths, self.exe)
            dst = self.copy_dependency(src, new_id)
            self.copied[new_id] = dst
            self.origin[new_id] = src.resolve()
            self.collect_with_origin(dst, src.resolve())

    def embed_framework(self, name: str) -> None:
        """Copies a prebuilt framework as a whole (the build already embeds it; else from
        its source directory). Symlinks are kept; nothing inside is rewritten."""
        if name in self.embedded:
            return
        src = self.sparkle_dir / f"{name}.framework"
        if not (src / "Versions" / "Current").exists():
            raise SystemExit(f"error: {src} missing (tools/fetch_sparkle.sh)")
        dst = self.frameworks / f"{name}.framework"
        if not dst.exists():
            self.frameworks.mkdir(parents=True, exist_ok=True)
            shutil.copytree(src, dst, symlinks=True)
        if not (dst / "Versions" / "Current" / name).exists():
            raise SystemExit(f"error: {dst} is incomplete")
        self.embedded[name] = src.parent

    # ---- licences --------------------------------------------------------------------

    def formula_of(self, real: pathlib.Path) -> tuple[str, str, pathlib.Path] | None:
        parts = real.parts
        if "Cellar" not in parts:
            return None
        i = parts.index("Cellar")
        return parts[i + 1], parts[i + 2], pathlib.Path(*parts[: i + 3])

    def static_components(self) -> list[dict]:
        """Statically linked third-party libraries (manifest written by the build), checked
        against STATIC_COMPONENTS: every entry needs its licence texts; a library whose
        symbols are in the executable must be listed."""
        manifest = self.contents / "Resources" / STATIC_MANIFEST
        listed = json.loads(manifest.read_text()) if manifest.is_file() else []
        ids = [e.get("id") for e in listed]
        try:
            exported = set(run("nm", "-gU", str(self.exe)).split())
        except (subprocess.CalledProcessError, FileNotFoundError):
            exported = set()
        for cid, meta in STATIC_COMPONENTS.items():
            if cid not in ids and any(s in exported for s in meta.get("symbols", [])):
                raise SystemExit(f"error: {self.exe.name} contains {cid} ({meta['symbols'][0]}) but "
                                 f"{STATIC_MANIFEST} does not list it - licence would be missing")
        result = []
        for entry in listed:
            cid = entry.get("id")
            meta = STATIC_COMPONENTS.get(cid)
            if not meta:
                raise SystemExit(f"error: statically linked component '{cid}' has no licence entry "
                                 f"(add it to STATIC_COMPONENTS in {__file__})")
            files = [self.repo / rel for rel in meta["files"]]
            missing = [str(f) for f in files if not f.is_file()]
            if not files or missing:
                raise SystemExit(f"error: licence text(s) for statically linked '{cid}' missing: {missing}")
            f = self.formula_of(pathlib.Path(entry.get("library", "")).resolve())
            result.append({"id": cid, "meta": meta, "files": files, "version": f[1] if f else ""})
        return result

    def licenses(self) -> list[dict]:
        out_dir = self.contents / "Resources" / "licenses"
        out_dir.mkdir(parents=True, exist_ok=True)
        components: dict[str, dict] = {}
        for key, real in self.origin.items():
            f = self.formula_of(real)
            if not f:
                raise SystemExit(f"error: cannot map {real} to a Homebrew formula (licence unknown)")
            formula, version, keg = f
            c = components.setdefault(formula, {"formula": formula, "version": version, "keg": keg,
                                                "binaries": set()})
            if key.startswith("plugin:"):
                c["binaries"].add("PlugIns/" + key[len("plugin:"):])
            else:
                c["binaries"].add("Frameworks/" + key[len("@rpath/"):])

        info = {}
        brew = shutil.which("brew")
        if brew:
            try:
                data = json.loads(run(brew, "info", "--json=v2", *sorted(components)))
                for fm in data.get("formulae", []):
                    info[fm["name"]] = fm
            except subprocess.CalledProcessError as e:
                print(f"warning: brew info failed: {e.stderr}", file=sys.stderr)

        index = []
        # Lumaflip itself first.
        (out_dir / "lumaflip").mkdir(exist_ok=True)
        shutil.copy2(self.repo / "LICENSE", out_dir / "lumaflip" / "LICENSE")
        index.append({"id": "lumaflip", "name": "Lumaflip", "version": "", "license": "GPL-3.0-or-later",
                      "homepage": "https://github.com/brokoskokoli/lumaflip",
                      "files": ["lumaflip/LICENSE"],
                      "binaries": ["MacOS/Lumaflip"] + ([f"MacOS/{HELPER}"] if self.helper.exists() else [])})
        for name, src_dir in sorted(self.embedded.items()):
            meta = EMBEDDED_FRAMEWORKS[name]
            lic = src_dir / "LICENSE"
            if not lic.is_file():
                raise SystemExit(f"error: no licence text for {name} ({lic})")
            dst = out_dir / meta["id"]
            dst.mkdir(exist_ok=True)
            shutil.copy2(lic, dst / "LICENSE")
            os.chmod(dst / "LICENSE", 0o644)
            plist = self.frameworks / f"{name}.framework" / "Versions" / "Current" / "Resources" / "Info.plist"
            version = ""
            if plist.is_file():
                with open(plist, "rb") as f:
                    version = plistlib.load(f).get("CFBundleShortVersionString", "")
            index.append({"id": meta["id"], "name": meta["name"], "version": version,
                          "license": meta["license"], "homepage": meta["homepage"],
                          "files": [f"{meta['id']}/LICENSE"],
                          "binaries": [f"Frameworks/{name}.framework"]})
        for formula in sorted(components):
            c = components[formula]
            dst = out_dir / formula
            dst.mkdir(exist_ok=True)
            files = []
            for p in sorted(c["keg"].iterdir()):
                if p.is_file() and LICENSE_FILE_RE.match(p.name) and p.suffix not in (".html", ".htm"):
                    shutil.copy2(p, dst / p.name)
                    files.append(f"{formula}/{p.name}")
            if not files:
                fallback = QT_LICENSE_FILES if formula.startswith("qt") else LICENSE_FALLBACK.get(formula, [])
                for rel in fallback:
                    src = self.repo / rel
                    name = "GPL-3.0.txt" if rel == "LICENSE" else src.name
                    shutil.copy2(src, dst / name)
                    files.append(f"{formula}/{name}")
            for rel in LICENSE_EXTRA.get(formula, []):
                src = self.repo / rel
                shutil.copy2(src, dst / src.name)
                files.append(f"{formula}/{src.name}")
            if not files:
                raise SystemExit(f"error: no licence text for bundled component '{formula}' "
                                 f"(add it to LICENSE_FALLBACK in {__file__})")
            for f in files:
                os.chmod(out_dir / f, 0o644)
            fi = info.get(formula, {})
            index.append({
                "id": formula,
                "name": DISPLAY_NAMES.get(formula, formula),
                "version": c["version"],
                "license": fi.get("license") or "",
                "homepage": fi.get("homepage") or "",
                "files": files,
                "binaries": sorted(c["binaries"]),
            })
        # Statically linked libraries: their own texts (also when the same project is bundled
        # as a dylib, e.g. libjpeg for Qt's JPEG plugin - the entry then lists both uses).
        exe_rels = [f"MacOS/{self.exe.name}"] + ([f"MacOS/{HELPER}"] if self.helper.exists() else [])
        for sc in self.static_components():
            cid, meta = sc["id"], sc["meta"]
            dst = out_dir / cid
            dst.mkdir(exist_ok=True)
            files = []
            for src in sc["files"]:
                shutil.copy2(src, dst / src.name)
                os.chmod(dst / src.name, 0o644)
                files.append(f"{cid}/{src.name}")
            existing = next((e for e in index if e["id"] == cid), None)
            if existing:
                existing["files"] = sorted(set(existing["files"]) | set(files))
                existing["binaries"] = sorted(set(existing["binaries"]) | {e + " (statisch)" for e in exe_rels})
                existing["license"] = existing["license"] or meta["license"]
            else:
                index.append({"id": cid, "name": meta["name"], "version": sc["version"],
                              "license": meta["license"], "homepage": meta["homepage"],
                              "files": files, "binaries": [e + " (statisch)" for e in exe_rels]})
        (out_dir / "index.json").write_text(json.dumps(index, indent=2, ensure_ascii=False) + "\n")
        return index


FOREIGN_RE = re.compile(r"/opt/homebrew|/usr/local|/Cellar/|/opt/local|/Users/|/private/|/tmp/")


def verify(app: pathlib.Path) -> int:
    """Fails if any Mach-O in the bundle references a library outside the bundle/system."""
    contents = app / "Contents"
    exe = contents / "MacOS" / app.stem
    problems, count = [], 0
    for p in sorted(contents.rglob("*")):
        if p.is_symlink() or not p.is_file() or not is_macho(p):
            continue
        count += 1
        rel = p.relative_to(contents)
        listing = run("otool", "-L", str(p))
        for line in listing.splitlines()[1:]:
            # Universal files list each architecture under a header "<path> (architecture x):".
            if line.endswith(":") and not line.startswith(("\t", " ")):
                continue
            if FOREIGN_RE.search(line):
                problems.append(f"{rel}: links {line.strip()}")
        _, deps, rpaths = load_commands(p)
        for rp in rpaths:
            if FOREIGN_RE.search(rp) or not rp.startswith(("@loader_path", "@executable_path")):
                problems.append(f"{rel}: foreign LC_RPATH {rp}")
        for dep in deps:
            if dep.startswith(SYSTEM_PREFIXES):
                continue
            if not dep.startswith("@rpath/"):
                problems.append(f"{rel}: non-relocatable dependency {dep}")
                continue
            if not any((pathlib.Path(expand(rp, p, exe)) / dep[7:]).exists() for rp in rpaths):
                problems.append(f"{rel}: {dep} not found inside the bundle")
    for line in problems:
        print("  " + line, file=sys.stderr)
    if problems:
        print(f"error: {len(problems)} problem(s) in {count} Mach-O files", file=sys.stderr)
        return 1
    print(f"ok: {count} Mach-O files, all dependencies inside the bundle or the system")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("app", type=pathlib.Path)
    ap.add_argument("--qt-prefix", type=pathlib.Path)
    ap.add_argument("--sparkle-dir", type=pathlib.Path,
                    help="directory with Sparkle.framework and LICENSE (default: third_party/Sparkle)")
    ap.add_argument("--verify", action="store_true",
                    help="only check that nothing references libraries outside the bundle")
    args = ap.parse_args()
    if args.verify:
        return verify(args.app.resolve())
    if not args.qt_prefix:
        ap.error("--qt-prefix is required")
    repo = pathlib.Path(__file__).resolve().parent.parent
    d = Deployer(args.app.resolve(), args.qt_prefix.resolve(), repo,
                 args.sparkle_dir.resolve() if args.sparkle_dir else None)
    d.deploy()
    index = d.licenses()
    print(f"deployed {len(PLUGINS)} plugins, {len(d.copied)} libraries/frameworks, "
          f"{len(d.embedded)} embedded framework(s), {len(index)} licence entries")
    for e in index:
        print(f"  {e['name']:<55} {e['version']:<12} {e['license']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
