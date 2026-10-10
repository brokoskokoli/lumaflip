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

With ``--build-dir build/check`` (tools/check.sh passes it) a test name only counts if
that build really registers and runs it: ``lf_unit_tests --gtest_list_tests`` (no
DISABLED_ tests, TEST_P only when instantiated), the Qt Test binaries known to
``ctest -N`` and their ``-functions``, and ``lf_bench --list``. Test names found in the
sources but not registered are listed; IDs covered only by them are uncovered.

Exit code 1 if anything is uncovered or the manual-check table is malformed.
"""
import argparse, os, pathlib, re, subprocess, sys
from collections import defaultdict

ROOT = pathlib.Path(__file__).resolve().parent.parent
STAGES = ["M1", "M2", "M3", "M4"]
MANUAL = ROOT / "docs" / "MANUAL_CHECKS.md"

# One requirement ID inside an identifier, delimited by '_' or the identifier's ends.
ID_IN_NAME = re.compile(r"(?:^|_)(F_[A-Z][A-Z0-9]*_\d+|P_\d+)(?=_|$)")
GTEST = re.compile(r"^\s*(?:TEST|TEST_F|TEST_P|TYPED_TEST|TYPED_TEST_P)\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)", re.M)
QT_SLOT = re.compile(r"^\s*void\s+((?:F_[A-Z][A-Z0-9]*_\d+|P_\d+)\w*)\s*\(\s*\)", re.M)
BENCH = re.compile(r"^\s*LF_BENCH\s*\(\s*(\w+)\s*,", re.M)
ID_TEXT = re.compile(r"^(F-[A-Z][A-Z0-9]*-\d+|P-\d+)$")


def stage_ids(spec: str, stage: str) -> set[str]:
    wanted = set(STAGES[: STAGES.index(stage) + 1])
    ids = set()
    # table rows: | F-XXX-01 | text | M1 |   (stage column may be "M1/M2": first stage counts)
    for m in re.finditer(r"^\|\s*(F-[A-Z][A-Z0-9]*-\d+)\s*\|.*\|\s*(M\d)[^|]*\|\s*$", spec, re.M):
        if m.group(2) in wanted:
            ids.add(m.group(1))
    # bullet form: - F-XXX-01 (M1): text
    for m in re.finditer(r"^- (F-[A-Z][A-Z0-9]*-\d+) \((M\d)\)", spec, re.M):
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
                # Only tables whose first header cell is "ID" list requirements; other
                # tables (e.g. release checklists) are validated for shape only.
                id_table = bool(header) and header[0] == "ID"
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
        if not id_table:
            continue
        if not ID_TEXT.match(rid):
            errors.append(f"{rel}:{no}: first column is not a requirement ID: {rid!r}")
        elif rid not in known_ids:
            errors.append(f"{rel}:{no}: unknown requirement ID {rid} (not in SPEC.md)")
        else:
            covered[rid].append(f"{rel}:{no} (manual)")
    return covered, errors


class Registered:
    """Tests that the build in --build-dir actually registers and runs.

    * GoogleTest: ``lf_unit_tests --gtest_list_tests`` -> (suite, name); parameterized and
      typed tests are reduced to their source suite/name, DISABLED_ tests do not count.
    * Qt Test: binaries listed by ``ctest -N`` (tst_*) and their ``-functions``.
    * Benchmarks: ``lf_bench --list``.
    """

    def __init__(self, build_dir: pathlib.Path):
        self.errors: list[str] = []
        self.gtests = self._gtests(build_dir / "tests" / "lf_unit_tests")
        ctest_names = self._ctest(build_dir)
        self.qtests: dict[str, set[str]] = {}
        for binary in sorted(n for n in ctest_names if n.startswith("tst_")):
            self.qtests[binary] = self._qt_functions(build_dir / "tests" / binary)
        self.benches = self._benches(build_dir / "tests" / "lf_bench")

    def _run(self, cmd: list[str], env_extra: dict[str, str] | None = None) -> str | None:
        env = dict(os.environ, **(env_extra or {}))
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=120, env=env)
        except (OSError, subprocess.TimeoutExpired) as e:
            self.errors.append(f"{' '.join(cmd)}: {e}")
            return None
        if out.returncode != 0:
            self.errors.append(f"{' '.join(cmd)}: exit code {out.returncode}")
            return None
        return out.stdout

    def _gtests(self, binary: pathlib.Path) -> set[tuple[str, str]]:
        found: set[tuple[str, str]] = set()
        if not binary.exists():
            return found
        out = self._run([str(binary), "--gtest_list_tests"]) or ""
        suite = ""
        for line in out.splitlines():
            if not line.strip() or line.startswith("Running main()"):
                continue
            if not line.startswith(" "):
                # "Suite.", "Prefix/Suite." (TEST_P), "Suite/0." (TYPED_TEST)
                parts = [p for p in line.split("#")[0].strip().rstrip(".").split("/") if not p.isdigit()]
                suite = parts[-1] if parts else ""
                continue
            name = line.split("#")[0].strip().split("/")[0]
            if suite.startswith("DISABLED_") or name.startswith("DISABLED_"):
                continue
            found.add((suite, name))
        return found

    def _ctest(self, build_dir: pathlib.Path) -> set[str]:
        out = self._run(["ctest", "--test-dir", str(build_dir), "-N"]) or ""
        return set(re.findall(r"^\s*Test\s+#\d+:\s+(\S+)", out, re.M))

    def _qt_functions(self, binary: pathlib.Path) -> set[str]:
        if not binary.exists():
            self.errors.append(f"{binary}: registered in ctest but not built")
            return set()
        out = self._run([str(binary), "-functions"], {"QT_QPA_PLATFORM": "offscreen"}) or ""
        return {m.group(1) for m in re.finditer(r"^(\w+)\(\)\s*$", out, re.M)}

    def _benches(self, binary: pathlib.Path) -> set[str]:
        if not binary.exists():
            return set()
        out = self._run([str(binary), "--list"], {"QT_QPA_PLATFORM": "offscreen"}) or ""
        return {line.strip() for line in out.splitlines() if line.strip()}

    def has(self, kind: str, f: pathlib.Path, suite: str, name: str) -> bool:
        if kind == "gtest":
            return (suite, name) in self.gtests
        if kind == "qtest":
            return name in self.qtests.get(f.stem, set())
        return name in self.benches


def test_names(registered: "Registered | None" = None) -> tuple[dict[str, list[str]], list[str]]:
    """id -> list of 'file:line name' for every test/benchmark whose name carries the ID.

    With `registered`, tests the build does not register/run are left out and returned
    as the second element ('file:line name (kind)')."""
    covered: dict[str, list[str]] = defaultdict(list)
    unregistered: list[str] = []
    tests = ROOT / "tests"
    for f in sorted(list(tests.rglob("*.cpp")) + list(tests.rglob("*.mm"))):
        text = f.read_text(errors="ignore")
        rel = f.relative_to(ROOT)
        patterns = [(GTEST, "gtest"), (BENCH, "bench")]
        if f.parent.name == "integration":
            patterns.append((QT_SLOT, "qtest"))
        for pat, kind in patterns:
            for m in pat.finditer(text):
                name = m.group(m.lastindex)
                suite = m.group(1) if kind == "gtest" else ""
                if kind == "qtest" and name.endswith("_data"):
                    continue
                ids = ids_in_name(name)
                if not ids:
                    continue
                loc = f"{rel}:{line_of(text, m.start(m.lastindex))} {name} ({kind})"
                if registered is not None and not registered.has(kind, f, suite, name):
                    unregistered.append(loc)
                    continue
                for rid in ids:
                    covered[rid].append(loc)
    return covered, unregistered


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", default="M1", choices=STAGES)
    ap.add_argument("--list", action="store_true", help="list all IDs of the stage")
    ap.add_argument("--verbose", "-v", action="store_true", help="show where each ID is covered")
    ap.add_argument("--build-dir", type=pathlib.Path,
                    help="build directory (e.g. build/check): count only tests that this build "
                         "registers and runs (gtest list, ctest -N + Qt Test -functions, lf_bench --list)")
    args = ap.parse_args()
    spec = (ROOT / "SPEC.md").read_text()
    ids = stage_ids(spec, args.stage)
    if args.list:
        print("\n".join(sorted(ids)))
        return 0
    all_ids = stage_ids(spec, STAGES[-1])
    registered = None
    if args.build_dir:
        build_dir = args.build_dir if args.build_dir.is_absolute() else pathlib.Path.cwd() / args.build_dir
        if not (build_dir / "CMakeCache.txt").exists():
            print(f"--build-dir {args.build_dir}: not a CMake build directory")
            return 1
        registered = Registered(build_dir)
        if registered.errors:
            print("Could not list the registered tests:")
            print("\n".join("  " + e for e in registered.errors))
            return 1
    covered, unregistered = test_names(registered)
    if unregistered:
        print(f"{len(unregistered)} test name(s) with requirement IDs are not registered/run by "
              f"{args.build_dir} and do not count:")
        print("\n".join("  " + u for u in unregistered))
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
