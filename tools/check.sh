#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs all mandatory checks (SPEC 15.1).
# Usage: tools/check.sh [--quick] [--no-gui] [--sanitize] [--stage M1] [--format-all] [--tidy|--tidy-all] [--cppcheck]
#   --quick       build + tests only
#   --no-gui      only tests that open no windows (unit tests, offscreen); no integration
#                 tests, no benchmarks. Use this on a laptop someone is working on.
#   --sanitize    ASan/UBSan build (build/sanitize)
#   --stage Mn    stage for the spec coverage check (default M4 = current milestone, or $LF_STAGE)
#   --format-all  clang-format check of every source file; default: only the lines
#                 changed against `git merge-base HEAD <base>` (base: $LF_BASE_REF,
#                 main or origin/main) incl. uncommitted changes, plus untracked files
#   --tidy        clang-tidy (.clang-tidy) on the changed .cpp/.mm files (same base as
#                 the format check); any finding fails. Skipped if clang-tidy is not
#                 installed, unless LF_REQUIRE_TIDY=1 (CI)
#   --tidy-all    like --tidy, but on every .cpp/.mm file under src/ and tests/
#   --cppcheck    Cppcheck on all .cpp files (tools/cppcheck.sh --report): lists findings,
#                 does not fail; skipped if cppcheck is not installed
# Always (also with --quick --no-gui): tools/i18n_check.py (untranslated texts, catalogues).
# Environment: LF_BENCH_TOLERANCE (benchmark limit factor, default 1.2),
#   LF_BENCH_LOAD_WAIT (seconds to wait for the 1-minute load average to drop to the
#   number of CPU cores before benchmarks, default 300; 0 = do not wait),
#   LF_CLANG_TIDY (clang-tidy binary; default: PATH, $(brew --prefix llvm)/bin, xcrun),
#   LF_REQUIRE_TIDY=1 (fail instead of skipping when clang-tidy is missing).
set -euo pipefail
cd "$(dirname "$0")/.."

QUICK=0; NO_GUI=0; SANITIZE=0; FORMAT_ALL=0; TIDY=0; TIDY_ALL=0; CPPCHECK=0; STAGE="${LF_STAGE:-M4}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --quick) QUICK=1 ;;
    --no-gui) NO_GUI=1 ;;
    --sanitize) SANITIZE=1 ;;
    --stage) STAGE="$2"; shift ;;
    --format-all) FORMAT_ALL=1 ;;
    --tidy) TIDY=1 ;;
    --tidy-all) TIDY=1; TIDY_ALL=1 ;;
    --cppcheck) CPPCHECK=1 ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac; shift
done

[[ -d /opt/homebrew/bin ]] && export PATH="/opt/homebrew/bin:$PATH"
QT_PREFIX="${QT_PREFIX:-$(brew --prefix qt 2>/dev/null || echo /opt/homebrew/opt/qt)}"
BUILD_DIR="build/check"; [[ $SANITIZE == 1 ]] && BUILD_DIR="build/sanitize"
NCPU="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31m%s\033[0m\n' "$*" >&2; exit 1; }

# Benchmarks are only meaningful on a quiet machine (SPEC 15.1): wait (at most
# LF_BENCH_LOAD_WAIT seconds) until the 1-minute load average is <= the core count.
# Runs the benchmarks anyway after the timeout, with a note.
load_avg() {
  if [[ -r /proc/loadavg ]]; then cut -d' ' -f1 /proc/loadavg
  else sysctl -n vm.loadavg 2>/dev/null | awk '{print $2}' || true; fi
}
wait_for_low_load() {
  local max="${LF_BENCH_LOAD_WAIT:-300}" waited=0 load
  while :; do
    load="$(load_avg)"
    [[ -z "$load" ]] && return 0
    if awk -v l="$load" -v n="$NCPU" 'BEGIN { exit !(l <= n) }'; then
      [[ $waited -gt 0 ]] && echo "load average $load <= $NCPU cores after ${waited}s"
      return 0
    fi
    if [[ $waited -ge $max ]]; then
      echo "note: load average still $load > $NCPU cores after ${waited}s - running benchmarks anyway"
      return 0
    fi
    [[ $waited == 0 ]] && echo "load average $load > $NCPU cores - waiting up to ${max}s before benchmarks"
    sleep 10; waited=$((waited + 10))
  done
}

# Merge base for "changed files" (format/tidy). Empty if no base branch exists.
merge_base() {
  local ref
  for ref in "${LF_BASE_REF:-}" main origin/main; do
    [[ -n "$ref" ]] || continue
    if git rev-parse -q --verify "$ref^{commit}" >/dev/null 2>&1; then
      git merge-base HEAD "$ref" 2>/dev/null && return 0
    fi
  done
  return 0
}

# C++/ObjC++ sources under src/ and tests/ that differ from the merge base
# (committed or not) or are untracked; existing files only.
changed_sources() {
  local base="$1"
  {
    [[ -n "$base" ]] && git diff --name-only --diff-filter=ACMR "$base" -- src tests
    git ls-files --others --exclude-standard -- src tests
  } | grep -E '\.(cpp|h|mm)$' | sort -u | while read -r f; do [[ -f "$f" ]] && echo "$f"; done
}

step "Configure ($BUILD_DIR)"
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
  -DLF_WERROR=ON -DLF_SANITIZE="$([[ $SANITIZE == 1 ]] && echo ON || echo OFF)" >/dev/null

step "Build"
cmake --build "$BUILD_DIR"

step "Translations (F-I18N-01/05)"
# No window, no load: texts outside tr(), catalogues up to date with the sources (scratch
# lupdate run) and translated (translations/README.md).
LUPDATE="${LF_LUPDATE:-$QT_PREFIX/bin/lupdate}"
[[ -x "$LUPDATE" ]] || LUPDATE="$(command -v lupdate || true)"
[[ -n "$LUPDATE" ]] || die "lupdate not found (Qt LinguistTools, part of Homebrew's qt)"
python3 tools/i18n_check.py --lupdate "$LUPDATE"

step "Format corpus"
# Generates tests/corpus/generated/ once (stamp-checked, no-op when up to date).
tests/corpus/make_corpus.sh

step "Tests"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
if [[ $NO_GUI == 1 ]]; then
  echo "--no-gui: unit tests only (integration tests and benchmarks open windows)"
  ctest --test-dir "$BUILD_DIR" --output-on-failure -j "$NCPU" -L unit
else
  ctest --test-dir "$BUILD_DIR" --output-on-failure -j "$NCPU"
fi

BASE="$(merge_base)"

if [[ $QUICK == 0 ]]; then
  step "clang-format"
  command -v clang-format >/dev/null || die "clang-format not found - install it: brew install clang-format"
  CF="$(command -v clang-format)"
  if [[ $FORMAT_ALL == 1 ]]; then
    files=$(git ls-files 'src/*.cpp' 'src/*.h' 'src/*.mm' 'tests/*.cpp' 'tests/*.h' 'tests/*.mm')
    # shellcheck disable=SC2086
    "$CF" --dry-run --Werror $files
  else
    if [[ -n "$BASE" ]]; then
      echo "changed lines since $(git rev-parse --short "$BASE") (use --format-all for every file)"
      command -v git-clang-format >/dev/null || die "git-clang-format not found (part of clang-format)"
      rc=0
      out=$(git clang-format --binary "$CF" --diff --extensions cpp,h,mm "$BASE" -- src tests 2>&1) || rc=$?
      # Older git-clang-format versions exit 0 even when they print a diff.
      if [[ $rc != 0 || "$out" == diff* ]]; then
        echo "$out"
        die "clang-format: changed lines are not formatted (fix: git clang-format $BASE)"
      fi
    else
      echo "no base branch (main/origin/main) - checking untracked files only"
    fi
    untracked=$(git ls-files --others --exclude-standard -- src tests | grep -E '\.(cpp|h|mm)$' || true)
    if [[ -n "$untracked" ]]; then
      # shellcheck disable=SC2086
      "$CF" --dry-run --Werror $untracked
    fi
  fi

  step "SPDX headers"
  # Locally a quick grep; CI additionally runs `reuse lint` (REUSE.toml, LICENSES/).
  missing=$(git ls-files 'src/*' 'tests/*.cpp' 'tests/*.h' 'tests/*.mm' 'tools/*' | grep -vE '\.(json|png|jpg|txt|md)$' \
            | xargs grep -L 'SPDX-License-Identifier' 2>/dev/null || true)
  if [[ -n "$missing" ]]; then echo "Missing SPDX header:"; echo "$missing"; exit 1; fi

  step "Spec coverage ($STAGE)"
  # --build-dir: only tests this build registers and runs count (gtest list, ctest, lf_bench).
  python3 tools/spec_coverage.py --stage "$STAGE" --build-dir "$BUILD_DIR"

  step "Sparkle key check self-test"
  # Throw-away keys from OpenSSL 3 in a temp dir (never a keychain); skips without OpenSSL 3.
  tools/sparkle_key_check.sh --self-test

  if [[ $NO_GUI == 0 && -x "$BUILD_DIR/tests/lf_bench" ]]; then
    step "Benchmarks (tolerance ${LF_BENCH_TOLERANCE:-1.2})"
    wait_for_low_load
    # The view benchmark (P-03) needs Metal: the offscreen platform has no QRhi.
    BENCH_QPA="$QT_QPA_PLATFORM"; [[ "$(uname)" == Darwin ]] && BENCH_QPA=cocoa
    rc=0
    QT_QPA_PLATFORM="$BENCH_QPA" "$BUILD_DIR/tests/lf_bench" | tee "$BUILD_DIR/bench.log" || rc=$?
    failed=$(awk '$1 == "FAIL" {print $2}' "$BUILD_DIR/bench.log" | paste -sd, -)
    if [[ $rc != 0 && -n "$failed" ]]; then
      # A benchmark that misses its limit once may have been disturbed by other load:
      # re-run only the failed ones once; they fail the check only if they fail again.
      echo "Benchmarks over the limit: $failed - re-running them once"
      wait_for_low_load
      rc=0
      LF_BENCH_ONLY="$failed" QT_QPA_PLATFORM="$BENCH_QPA" "$BUILD_DIR/tests/lf_bench" \
        | tee "$BUILD_DIR/bench-rerun.log" || rc=$?
      echo "First run:"; grep -E '^FAIL' "$BUILD_DIR/bench.log" || true
      echo "Re-run:"; grep -E '^(OK|FAIL)' "$BUILD_DIR/bench-rerun.log" || true
      [[ $rc == 0 ]] || die "benchmarks failed twice (first run: $BUILD_DIR/bench.log, re-run: $BUILD_DIR/bench-rerun.log)"
      echo "Benchmarks passed on the re-run (first-run failures above were load noise)"
    elif [[ $rc != 0 ]]; then
      die "lf_bench failed (exit code $rc, see $BUILD_DIR/bench.log)"
    fi
  fi
fi

if [[ $TIDY == 1 ]]; then
  step "clang-tidy"
  TIDY_BIN="${LF_CLANG_TIDY:-$(command -v clang-tidy || true)}"
  [[ -z "$TIDY_BIN" && -x "$(brew --prefix llvm 2>/dev/null)/bin/clang-tidy" ]] && TIDY_BIN="$(brew --prefix llvm)/bin/clang-tidy"
  [[ -z "$TIDY_BIN" ]] && TIDY_BIN="$(xcrun --find clang-tidy 2>/dev/null || true)"
  if [[ -z "$TIDY_BIN" || ! -x "$TIDY_BIN" ]]; then
    [[ "${LF_REQUIRE_TIDY:-0}" == 1 ]] && die "clang-tidy not found (LF_REQUIRE_TIDY=1) - brew install llvm"
    echo "clang-tidy not found - skipped (it is part of LLVM: brew install llvm)"
  else
    "$TIDY_BIN" --version | head -n 2
    # Homebrew's clang-tidy does not know the macOS SDK on its own (TargetConditionals.h,
    # libc++ headers): without it every Qt include fails to parse.
    if [[ "$(uname)" == Darwin && -z "${SDKROOT:-}" ]]; then
      SDKROOT="$(xcrun --show-sdk-path 2>/dev/null || true)"
      export SDKROOT
    fi
    if [[ $TIDY_ALL == 1 ]]; then
      tidy_files=$(git ls-files 'src/*.cpp' 'src/*.mm' 'tests/*.cpp' 'tests/*.mm')
    else
      tidy_files=$(changed_sources "$BASE" | grep -E '\.(cpp|mm)$' || true)
    fi
    # Only files this build compiles (e.g. src/platform/generic/ is not built on macOS).
    if [[ -n "$tidy_files" && -f "$BUILD_DIR/compile_commands.json" ]]; then
      tidy_files=$(for f in $tidy_files; do
        grep -qF "/$f\"" "$BUILD_DIR/compile_commands.json" && echo "$f"
      done || true)
    fi
    if [[ -z "$tidy_files" ]]; then
      echo "no changed .cpp/.mm files"
    else
      echo "$tidy_files"
      # Several files per process, one process per CPU; xargs fails if any run fails.
      # shellcheck disable=SC2086
      printf '%s\n' $tidy_files | xargs -P "$NCPU" -n 4 \
        "$TIDY_BIN" -p "$BUILD_DIR" --quiet --warnings-as-errors='*' \
        || die "clang-tidy reported findings (see above)"
    fi
  fi
fi

if [[ $CPPCHECK == 1 ]]; then
  step "cppcheck"
  if command -v "${LF_CPPCHECK:-cppcheck}" >/dev/null; then
    tools/cppcheck.sh --build-dir "$BUILD_DIR" --report
  else
    echo "cppcheck not found - skipped (brew install cppcheck)"
  fi
fi

printf '\n\033[1;32mAll checks passed.\033[0m\n'
