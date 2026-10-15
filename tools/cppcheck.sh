#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Static analysis with Cppcheck (second engine next to clang-tidy, SPEC 9.3).
# Usage: tools/cppcheck.sh [--build-dir build/check] [--report]
#   Checks every .cpp under src/ and tests/ that the build compiles (compile_commands.json).
#   Objective-C++ (.mm) is skipped: Cppcheck cannot parse it (clang-tidy covers it).
#   Suppressions: tools/cppcheck-suppressions.txt, or inline `// cppcheck-suppress id`.
#   --report      do not fail (findings are still listed); report: build/cppcheck/report.txt
# Exit code 1 on findings (without --report). Results are cached in build/cppcheck/cache.
# Environment: LF_CPPCHECK (binary; default: PATH), LF_CPPCHECK_JOBS (default: CPU count).
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="build/check"; REPORT=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift ;;
    --report) REPORT=1 ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac; shift
done

[[ -d /opt/homebrew/bin ]] && export PATH="/opt/homebrew/bin:$PATH"
BIN="${LF_CPPCHECK:-$(command -v cppcheck || true)}"
[[ -z "$BIN" ]] && { echo "cppcheck not found - brew install cppcheck" >&2; exit 2; }
[[ -f "$BUILD_DIR/compile_commands.json" ]] || { echo "$BUILD_DIR/compile_commands.json missing - build first" >&2; exit 2; }
JOBS="${LF_CPPCHECK_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"
OUT="build/cppcheck"; mkdir -p "$OUT/cache"

# Own sources only: no .mm, nothing generated (moc, rcc, autogen) and no third_party.
python3 - "$BUILD_DIR/compile_commands.json" "$OUT/compile_commands.json" "$PWD" <<'EOF'
import json, os, sys
src, dst, root = sys.argv[1:4]
keep = []
for e in json.load(open(src)):
    f = os.path.relpath(os.path.join(e["directory"], e["file"]), root)
    if f.endswith(".cpp") and f.split(os.sep)[0] in ("src", "tests"):
        keep.append(e)
json.dump(keep, open(dst, "w"))
print(f"cppcheck: {len(keep)} files")
EOF

"$BIN" --version
# Qt's headers are not parsed (too slow, framework includes): give Cppcheck the platform
# macros Qt would define, otherwise #ifdef Q_OS_* code looks dead ("condition always false").
case "$(uname)" in
  Darwin) OS_DEFS=(-DQ_OS_UNIX -DQ_OS_DARWIN -DQ_OS_MACOS -DQ_OS_MAC) ;;
  Linux) OS_DEFS=(-DQ_OS_UNIX -DQ_OS_LINUX) ;;
  *) OS_DEFS=(-DQ_OS_WIN -DQ_OS_WINDOWS) ;;
esac
set +e
"$BIN" --project="$OUT/compile_commands.json" \
  --cppcheck-build-dir="$OUT/cache" -j "$JOBS" \
  --std=c++20 --library=qt --platform=native "${OS_DEFS[@]}" -DQTEST_SET_MAIN_SOURCE_PATH= \
  --enable=warning,style,performance,portability \
  --inline-suppr --suppressions-list=tools/cppcheck-suppressions.txt \
  -i build/ -i third_party/ --suppress='*:*/build/*' --suppress='*:*/third_party/*' \
  --suppress='*:/opt/homebrew/*' --suppress='*:/Applications/*' \
  --template='{file}:{line}:{column}: {severity}: {message} [{id}]' \
  --error-exitcode=1 --quiet \
  --output-file="$OUT/report.txt" 2>/dev/null
rc=$?
set -e
[[ -s "$OUT/report.txt" ]] && cat "$OUT/report.txt"
n=$(grep -cE ': (error|warning|style|performance|portability): ' "$OUT/report.txt" 2>/dev/null || true)
echo "cppcheck: ${n:-0} findings (report: $OUT/report.txt)"
[[ $REPORT == 1 ]] && exit 0
exit $rc
