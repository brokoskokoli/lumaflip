#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs all mandatory checks (SPEC 15.1).
# Usage: tools/check.sh [--quick] [--sanitize] [--stage M1] [--format-all] [--tidy|--tidy-all]
#   --quick       build + tests only
#   --sanitize    ASan/UBSan build (build/sanitize)
#   --stage Mn    stage for the spec coverage check (default M1, or $LF_STAGE)
#   --format-all  clang-format check of every source file; default: only the lines
#                 changed against `git merge-base HEAD <base>` (base: $LF_BASE_REF,
#                 main or origin/main) incl. uncommitted changes, plus untracked files
#   --tidy        clang-tidy (.clang-tidy) on the changed .cpp/.mm files (same base as
#                 the format check); any finding fails. Skipped if clang-tidy is not
#                 installed, unless LF_REQUIRE_TIDY=1 (CI)
#   --tidy-all    like --tidy, but on every .cpp/.mm file under src/ and tests/
# Environment: LF_BENCH_TOLERANCE (benchmark limit factor, default 1.2),
#   LF_CLANG_TIDY (clang-tidy binary; default: PATH, $(brew --prefix llvm)/bin, xcrun),
#   LF_REQUIRE_TIDY=1 (fail instead of skipping when clang-tidy is missing).
set -euo pipefail
cd "$(dirname "$0")/.."

QUICK=0; SANITIZE=0; FORMAT_ALL=0; TIDY=0; TIDY_ALL=0; STAGE="${LF_STAGE:-M1}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --quick) QUICK=1 ;;
    --sanitize) SANITIZE=1 ;;
    --stage) STAGE="$2"; shift ;;
    --format-all) FORMAT_ALL=1 ;;
    --tidy) TIDY=1 ;;
    --tidy-all) TIDY=1; TIDY_ALL=1 ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac; shift
done

[[ -d /opt/homebrew/bin ]] && export PATH="/opt/homebrew/bin:$PATH"
QT_PREFIX="${QT_PREFIX:-$(brew --prefix qt 2>/dev/null || echo /opt/homebrew/opt/qt)}"
BUILD_DIR="build/check"; [[ $SANITIZE == 1 ]] && BUILD_DIR="build/sanitize"
NCPU="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31m%s\033[0m\n' "$*" >&2; exit 1; }

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

step "Format corpus"
# Generates tests/corpus/generated/ once (stamp-checked, no-op when up to date).
tests/corpus/make_corpus.sh

step "Tests"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
ctest --test-dir "$BUILD_DIR" --output-on-failure -j "$NCPU"

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

  if [[ -x "$BUILD_DIR/tests/lf_bench" ]]; then
    step "Benchmarks (tolerance ${LF_BENCH_TOLERANCE:-1.2})"
    # The view benchmark (P-03) needs Metal: the offscreen platform has no QRhi.
    if [[ "$(uname)" == Darwin ]]; then
      QT_QPA_PLATFORM=cocoa "$BUILD_DIR/tests/lf_bench" | tee "$BUILD_DIR/bench.log"
    else
      "$BUILD_DIR/tests/lf_bench" | tee "$BUILD_DIR/bench.log"
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
    if [[ $TIDY_ALL == 1 ]]; then
      tidy_files=$(git ls-files 'src/*.cpp' 'src/*.mm' 'tests/*.cpp' 'tests/*.mm')
    else
      tidy_files=$(changed_sources "$BASE" | grep -E '\.(cpp|mm)$' || true)
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

printf '\n\033[1;32mAll checks passed.\033[0m\n'
