#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Spec coverage check (SPEC 15.1).

Collects all requirement IDs (F-XXX-NN, P-NN) of the requested stage from SPEC.md
and verifies that each one is referenced by at least one test (IDs appear in test
names/comments with '_' or '-', e.g. F_SORT_01) or listed in docs/MANUAL_CHECKS.md.
Exit code 1 if anything is uncovered.
"""
import argparse, pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
STAGES = ["M1", "M2", "M3", "M4"]

def stage_ids(spec: str, stage: str) -> set[str]:
    wanted = set(STAGES[: STAGES.index(stage) + 1])
    ids = set()
    # table rows: | F-XXX-01 | text | M1 |   (stage column may be "M1/M2": first stage counts)
    for m in re.finditer(r"^\|\s*(F-[A-Z]+-\d+)\s*\|.*\|\s*(M\d)[^|]*\|\s*$", spec, re.M):
        if m.group(2) in wanted:
            ids.add(m.group(1))
    # bullet form: - F-XXX-01 (M1): text
    for m in re.finditer(r"^- (F-[A-Z]+-\d+) \((M\d)\)", spec, re.M):
        if m.group(2) in wanted:
            ids.add(m.group(1))
    # input slot table (SPEC 4.14) has no stage column: F-IN-01 is M1, the rest M2
    for m in re.finditer(r"^\|\s*(F-IN-(\d+))\s*\|", spec, re.M):
        if ("M1" if m.group(2) == "01" else "M2") in wanted:
            ids.add(m.group(1))
    # performance IDs are always required from M1 on
    ids |= set(re.findall(r"^\| (P-\d+) \|", spec, re.M))
    return ids

def referenced_ids() -> set[str]:
    found = set()
    sources = list((ROOT / "tests").rglob("*.cpp")) + list((ROOT / "tests").rglob("*.mm")) \
        + list((ROOT / "tests").rglob("*.py"))
    manual = ROOT / "docs" / "MANUAL_CHECKS.md"
    if manual.exists():
        sources.append(manual)
    for f in sources:
        text = f.read_text(errors="ignore")
        for m in re.finditer(r"\b([FP])[-_]([A-Z]+[-_])?(\d+)\b", text):
            prefix, mid, num = m.group(1), m.group(2), m.group(3)
            found.add(f"{prefix}-{mid[:-1]}-{num}" if mid else f"{prefix}-{num}")
    return found

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", default="M1", choices=STAGES)
    ap.add_argument("--list", action="store_true", help="list all IDs of the stage")
    args = ap.parse_args()
    spec = (ROOT / "SPEC.md").read_text()
    ids = stage_ids(spec, args.stage)
    if args.list:
        print("\n".join(sorted(ids)))
        return 0
    covered = referenced_ids()
    missing = sorted(ids - covered)
    print(f"{args.stage}: {len(ids) - len(missing)}/{len(ids)} requirement IDs covered")
    if missing:
        print("Uncovered: " + ", ".join(missing))
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
