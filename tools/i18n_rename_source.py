#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rename German source texts in all translations/*.ts so existing translations stay attached.

German is Lumaflip's source language. When a German UI text changes only in wording (same
meaning), lupdate would otherwise drop the old entry and add the new text as "unfinished" in
all 17 languages. This tool renames the <source> in every catalogue first; afterwards
`cmake --build build/check --target lf_lupdate` finds the new text with its old translation.

Mapping file (TSV, '#' comments, UTF-8):
    context<TAB>old source<TAB>new source[<TAB>plural substitutions]
  - context "*" applies to every context that contains the old source.
  - plural substitutions (optional, numerus messages only): "old=>new" pairs separated by
    " ;; ", applied to each <numerusform> of lumaflip_de.ts (which holds the German plural
    forms) so the German plural texts follow the new wording.
  - "\\n" in a field stands for a line break.

If the new source already exists in the same context, the renamed entry is dropped (the
existing entry and its translation win) and the tool reports it.

Usage:
    python3 tools/i18n_rename_source.py MAPPING.tsv [--ts-dir translations] [--src src] [--dry-run]
With --src, string literals "old" in C++/ObjC++ files below that directory are replaced too
(every replacement is printed, so ambiguous short words can be reviewed).
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys


def xml_escape(s: str) -> str:
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            .replace('"', "&quot;").replace("'", "&apos;"))


def c_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def load_mapping(path: pathlib.Path):
    rows = []
    for no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) not in (3, 4):
            sys.exit(f"{path}:{no}: expected 3 or 4 tab-separated fields, got {len(parts)}")
        ctx, old, new = (p.replace("\\n", "\n") for p in parts[:3])
        subs = []
        if len(parts) == 4 and parts[3].strip():
            for pair in parts[3].split(" ;; "):
                a, sep, b = pair.partition("=>")
                if not sep:
                    sys.exit(f"{path}:{no}: plural substitution without '=>': {pair!r}")
                subs.append((a, b))
        if old == new:
            sys.exit(f"{path}:{no}: old and new source are identical")
        rows.append((ctx, old, new, subs))
    return rows


CONTEXT_RE = re.compile(r"(<context>\s*<name>(.*?)</name>)(.*?)(</context>)", re.S)
MESSAGE_RE = re.compile(r"(\s*<message[^>]*>)(.*?)(</message>)", re.S)
SOURCE_RE = re.compile(r"<source>(.*?)</source>", re.S)


def rename_ts(path: pathlib.Path, rows, is_de: bool, report: list[str]) -> int:
    text = path.read_text(encoding="utf-8")
    renamed = 0

    def fix_context(m: re.Match) -> str:
        nonlocal renamed
        head, name, body, tail = m.group(1), m.group(2), m.group(3), m.group(4)
        applicable = [r for r in rows if r[0] in ("*", name)]
        if not applicable:
            return m.group(0)
        existing = {sm.group(1) for sm in SOURCE_RE.finditer(body)}
        by_old = {xml_escape(old): (xml_escape(new), subs) for _, old, new, subs in applicable}
        out = []
        pos = 0
        for mm in MESSAGE_RE.finditer(body):
            out.append(body[pos:mm.start()])
            pos = mm.end()
            msg = mm.group(0)
            sm = SOURCE_RE.search(msg)
            src = sm.group(1) if sm else None
            if src in by_old:
                new, subs = by_old[src]
                if new in existing:
                    report.append(f"{path.name}: {name}: „{src}“ merged into existing „{new}“")
                    continue  # drop: existing entry with that source wins
                msg = msg.replace(f"<source>{src}</source>", f"<source>{new}</source>", 1)
                if is_de and subs:
                    def sub_form(fm: re.Match) -> str:
                        t = fm.group(1)
                        for a, b in subs:
                            t = t.replace(xml_escape(a), xml_escape(b))
                        return f"<numerusform>{t}</numerusform>"
                    msg = re.sub(r"<numerusform>(.*?)</numerusform>", sub_form, msg, flags=re.S)
                existing.add(new)
                renamed += 1
            out.append(msg)
        out.append(body[pos:])
        return head + "".join(out) + tail

    new_text = CONTEXT_RE.sub(fix_context, text)
    if new_text != text:
        path.write_text(new_text, encoding="utf-8")
    return renamed


def rename_src(src_dir: pathlib.Path, rows, dry: bool) -> list[str]:
    missing = []
    files = [p for p in src_dir.rglob("*") if p.suffix in (".cpp", ".h", ".mm")]
    contents = {p: p.read_text(encoding="utf-8") for p in files}
    done = set()
    for ctx, old, new, _ in rows:
        if (old, new) in done:
            continue
        done.add((old, new))
        # The literal may be split into adjacent pieces ("abc " "def") across lines.
        pat = re.compile('"' + "".join(re.escape(c_escape(ch)) + r'(?:"\s*")?' for ch in old[:-1])
                         + re.escape(c_escape(old[-1])) + '"')
        lit_new = f'"{c_escape(new)}"'
        hits = 0
        for p, t in contents.items():
            matches = list(pat.finditer(t))
            if matches:
                for mt in matches:
                    print(f"  {p}:{t.count(chr(10), 0, mt.start()) + 1}: {old!r} -> {new!r}")
                hits += len(matches)
                contents[p] = pat.sub(lambda _m: lit_new, t)
        if not hits:
            missing.append(f"{ctx}: {old!r}")
    if not dry:
        for p, t in contents.items():
            if t != p.read_text(encoding="utf-8"):
                p.write_text(t, encoding="utf-8")
    return missing


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mapping", type=pathlib.Path)
    ap.add_argument("--ts-dir", type=pathlib.Path, default=pathlib.Path("translations"))
    ap.add_argument("--src", type=pathlib.Path, help="also replace string literals below this directory")
    ap.add_argument("--dry-run", action="store_true", help="only report (source files and .ts stay unchanged)")
    args = ap.parse_args()

    rows = load_mapping(args.mapping)
    seen = {}
    for ctx, old, new, _ in rows:
        key = (ctx, old)
        if key in seen and seen[key] != new:
            sys.exit(f"conflicting mapping for {ctx}: {old!r}")
        seen[key] = new

    if args.src:
        missing = rename_src(args.src, rows, args.dry_run)
        for m in missing:
            print(f"not found as a single literal in {args.src} (split string? fix by hand): {m}")

    # Every mapping row must match a message in a full catalogue (lumaflip_en.ts).
    ref = args.ts_dir / "lumaflip_en.ts"
    if ref.exists():
        ref_text = ref.read_text(encoding="utf-8")
        present = {(cm.group(2), sm.group(1)) for cm in CONTEXT_RE.finditer(ref_text)
                   for sm in SOURCE_RE.finditer(cm.group(3))}
        for ctx, old, _, _ in rows:
            if not any(c == ctx or ctx == "*" for c, s in present if s == xml_escape(old)):
                print(f"not in {ref.name} (already renamed or typo?): {ctx}: {old!r}")

    report: list[str] = []
    for ts in sorted(args.ts_dir.glob("*.ts")):
        if args.dry_run:
            continue
        n = rename_ts(ts, rows, ts.name.endswith("_de.ts"), report)
        print(f"{ts.name}: {n} sources renamed")
    for r in report:
        print(r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
