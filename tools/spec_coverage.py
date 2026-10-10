#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Spec coverage check (SPEC 15.1).

Collects all requirement IDs (F-XXX-NN, P-NN) of the requested stage from SPEC.md
and verifies that each one is covered by at least one of:

  * a GoogleTest name: the second argument of TEST/TEST_F/TEST_P/TYPED_TEST in
    tests/**/*.cpp|*.mm, e.g. ``TEST(SortEngine, F_SORT_01_FinderNameOrder)``;
  * a Qt Test slot in tests/integration, e.g. ``void F_NAV_01_arrowKeys();``
    (``*_data`` slots do not count, they only feed the real test);
  * a benchmark name, e.g. ``LF_BENCH(P_05_EnumerateAndSort10k, 500.0)``;
  * the first column of a row in docs/MANUAL_CHECKS.md.

A test name may carry several IDs (``F_OPEN_07_F_SORT_07_Refresh``). Mentions in
comments, helper code or other files do NOT count. The manual-check table is also
validated (well-formed rows, known IDs); malformed tables fail the check.

Exit code 1 if anything is uncovered or the manual-check table is malformed.
"""
import argparse, pathlib, re, sys
from collections import defaultdict

ROOT = pathlib.Path(__file__).resolve().parent.parent
STAGES = ["M1", "M2", "M3", "M4"]
MANUAL = ROOT / "docs" / "MANUAL_CHECKS.md"

# One requirement ID inside an identifier, delimited by '_' or the identifier's ends.
ID_IN_NAME = re.compile(r"(?:^|_)(F_[A-Z]+_\d+|P_\d+)(?=_|$)")
GTEST = re.compile(r"^\s*(?:TEST|TEST_F|TEST_P|TYPED_TEST|TYPED_TEST_P)\s*\(\s*\w+\s*,\s*(\w+)\s*\)", re.M)
QT_SLOT = re.compile(r"^\s*void\s+((?:F_[A-Z]+_\d+|P_\d+)\w*)\s*\(\s*\)", re.M)
BENCH = re.compile(r"^\s*LF_BENCH\s*\(\s*(\w+)\s*,", re.M)
ID_TEXT = re.compile(r"^(F-[A-Z]+-\d+|P-\d+)$")


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


def ids_in_name(name: str) -> list[str]:
    return [m.group(1).replace("_", "-") for m in ID_IN_NAME.finditer(name)]


def line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def split_row(line: str) -> list[str]:
    """Cells of a markdown table row (escaped pipes and pipes in `code` stay in the cell)."""
    cells, cur, in_code, i = [], "", False, 0
    body = line.strip()
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body) and body[i + 1] == "|":
            cur += "|"; i += 2; continue
        if c == "`":
            in_code = not in_code
        if c == "|" and not in_code:
            cells.append(cur); cur = ""
        else:
            cur += c
        i += 1
    cells.append(cur)
    # a well-formed row starts and ends with '|' -> empty first/last pseudo cells
    return [c.strip() for c in cells[1:-1]]


def manual_rows(known_ids: set[str]) -> tuple[dict[str, list[str]], list[str]]:
    """Parses docs/MANUAL_CHECKS.md. Returns (id -> locations, errors)."""
    covered: dict[str, list[str]] = defaultdict(list)
    errors: list[str] = []
    if not MANUAL.exists():
        return covered, errors
    rel = MANUAL.relative_to(ROOT)
    lines = MANUAL.read_text().splitlines()
    in_table, ncols = False, 0
    for no, raw in enumerate(lines, 1):
        line = raw.rstrip()
        if not in_table:
            if line.startswith("|"):
                header = split_row(line)
                sep = lines[no] if no < len(lines) else ""
                if not re.fullmatch(r"\|(\s*:?-+:?\s*\|)+", sep.strip()):
                    errors.append(f"{rel}:{no}: table header without separator row")
                in_table, ncols = True, len(header)
                skip_sep = True
            continue
        if skip_sep:
            skip_sep = False
            continue
        if not line.strip():
            in_table = False
            continue
        if not (line.startswith("|") and line.endswith("|")):
            errors.append(f"{rel}:{no}: malformed table row (must start and end with '|'): {line[:60]!r}")
            continue
        cells = split_row(line)
        if len(cells) != ncols:
            errors.append(f"{rel}:{no}: row has {len(cells)} cells, header has {ncols}")
            continue
        if any(not c for c in cells):
            errors.append(f"{rel}:{no}: empty cell")
        rid = cells[0]
        if not ID_TEXT.match(rid):
            errors.append(f"{rel}:{no}: first column is not a requirement ID: {rid!r}")
        elif rid not in known_ids:
            errors.append(f"{rel}:{no}: unknown requirement ID {rid} (not in SPEC.md)")
        else:
            covered[rid].append(f"{rel}:{no} (manual)")
    return covered, errors


def test_names() -> dict[str, list[str]]:
    """id -> list of 'file:line name' for every test/benchmark whose name carries the ID."""
    covered: dict[str, list[str]] = defaultdict(list)
    tests = ROOT / "tests"
    for f in sorted(list(tests.rglob("*.cpp")) + list(tests.rglob("*.mm"))):
        text = f.read_text(errors="ignore")
        rel = f.relative_to(ROOT)
        patterns = [(GTEST, "gtest"), (BENCH, "bench")]
        if f.parent.name == "integration":
            patterns.append((QT_SLOT, "qtest"))
        for pat, kind in patterns:
            for m in pat.finditer(text):
                name = m.group(1)
                if kind == "qtest" and name.endswith("_data"):
                    continue
                for rid in ids_in_name(name):
                    covered[rid].append(f"{rel}:{line_of(text, m.start(1))} {name} ({kind})")
    return covered


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", default="M1", choices=STAGES)
    ap.add_argument("--list", action="store_true", help="list all IDs of the stage")
    ap.add_argument("--verbose", "-v", action="store_true", help="show where each ID is covered")
    args = ap.parse_args()
    spec = (ROOT / "SPEC.md").read_text()
    ids = stage_ids(spec, args.stage)
    if args.list:
        print("\n".join(sorted(ids)))
        return 0
    all_ids = stage_ids(spec, STAGES[-1])
    covered = test_names()
    manual, errors = manual_rows(all_ids)
    for rid, locs in manual.items():
        covered[rid].extend(locs)

    missing = sorted(i for i in ids if not covered.get(i))
    if args.verbose:
        for rid in sorted(ids):
            locs = covered.get(rid, [])
            print(f"{rid}: {'UNCOVERED' if not locs else ''}")
            for loc in locs:
                print(f"    {loc}")
    print(f"{args.stage}: {len(ids) - len(missing)}/{len(ids)} requirement IDs covered")
    rc = 0
    if missing:
        print("Uncovered: " + ", ".join(missing))
        rc = 1
    if errors:
        print("docs/MANUAL_CHECKS.md is malformed:")
        print("\n".join("  " + e for e in errors))
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
