#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs all mandatory checks (SPEC 15.1). Usage: tools/check.sh [--quick] [--sanitize] [--stage M1]
set -euo pipefail
cd "$(dirname "$0")/.."

QUICK=0; SANITIZE=0; STAGE="${LF_STAGE:-M1}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --quick) QUICK=1 ;;
    --sanitize) SANITIZE=1 ;;
    --stage) STAGE="$2"; shift ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac; shift
done

export PATH="/opt/homebrew/bin:$PATH"
QT_PREFIX="$(brew --prefix qt 2>/dev/null || echo /opt/homebrew/opt/qt)"
BUILD_DIR="build/check"; [[ $SANITIZE == 1 ]] && BUILD_DIR="build/sanitize"

step() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }

step "Configure ($BUILD_DIR)"
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
  -DLF_WERROR=ON -DLF_SANITIZE=$([[ $SANITIZE == 1 ]] && echo ON || echo OFF) >/dev/null

step "Build"
cmake --build "$BUILD_DIR"

step "Format corpus"
# Generates tests/corpus/generated/ once (stamp-checked, no-op when up to date).
tests/corpus/make_corpus.sh

step "Tests"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
ctest --test-dir "$BUILD_DIR" --output-on-failure -j "$(sysctl -n hw.ncpu)"

if [[ $QUICK == 0 ]]; then
  step "clang-format"
  if command -v clang-format >/dev/null; then
    files=$(git ls-files 'src/*.cpp' 'src/*.h' 'src/*.mm' 'tests/*.cpp' 'tests/*.h' 2>/dev/null || true)
    [[ -n "$files" ]] && clang-format --dry-run --Werror $files
  else
    echo "clang-format not installed - skipped (brew install clang-format)"
  fi

  step "SPDX headers"
  missing=$(git ls-files 'src/*' 'tests/*.cpp' 'tests/*.h' 'tools/*' | grep -vE '\.(json|png|jpg|txt|md)$' \
            | xargs grep -L 'SPDX-License-Identifier' 2>/dev/null || true)
  if [[ -n "$missing" ]]; then echo "Missing SPDX header:"; echo "$missing"; exit 1; fi

  step "Spec coverage ($STAGE)"
  python3 tools/spec_coverage.py --stage "$STAGE"

  if [[ -x "$BUILD_DIR/tests/lf_bench" ]]; then
    step "Benchmarks"
    "$BUILD_DIR/tests/lf_bench"
  fi
fi

printf '\n\033[1;32mAll checks passed.\033[0m\n'
