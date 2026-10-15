#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Static analysis with PC-lint Plus (commercial, optional; third engine next to clang-tidy
# and Cppcheck). Not part of the repository: unpack the distribution to third_party/pclp/
# (git-ignored) and put the license file next to the binary (pclp64_macos).
# On Apple silicon the macOS binary (x86_64) needs Rosetta 2:
#   softwareupdate --install-rosetta --agree-to-license
# Usage: tools/pclint.sh [--build-dir build/check] [--report]
#   Checks every .cpp under src/ and tests/ that the build compiles (compile_commands.json);
#   .mm files are skipped (Objective-C++). Options: tools/pclint.lnt.
#   --report      do not fail on findings; report: build/pclp/report.txt
# Generated configuration (build/pclp/): co-clang.lnt/.h (compiler, once), project.lnt and
# frameworks.lnt (each run), venv/ (Python modules regex + pyyaml for pclp_config.py).
# Environment: LF_PCLP_DIR (default third_party/pclp), LF_PCLP_JOBS (default: CPU count),
#   LF_PCLP_ONLY (regular expression: only files whose path matches, e.g. src/core/Xmp).
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

PCLP_DIR="${LF_PCLP_DIR:-third_party/pclp}"
[[ -d "$PCLP_DIR" ]] && PCLP_DIR="$(cd "$PCLP_DIR" && pwd)"
case "$(uname)" in
  Darwin) BIN="$PCLP_DIR/pclp64_macos" ;;
  Linux) BIN="$PCLP_DIR/pclp64_linux" ;;
  *) BIN="$PCLP_DIR/pclp64.exe" ;;
esac
[[ -x "$BIN" ]] || { echo "PC-lint Plus not found: $BIN (see the header of $0)" >&2; exit 2; }
[[ -f "$BUILD_DIR/compile_commands.json" ]] || { echo "$BUILD_DIR/compile_commands.json missing - build first" >&2; exit 2; }
JOBS="${LF_PCLP_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"
OUT="build/pclp"; mkdir -p "$OUT"
CONFIG="$PCLP_DIR/config"

if [[ ! -x "$OUT/venv/bin/python" ]]; then
  python3 -m venv "$OUT/venv"
  "$OUT/venv/bin/pip" -q install regex pyyaml
fi
PY="$OUT/venv/bin/python"

# Compiler configuration: predefined macros and system include paths of the build's clang.
if [[ ! -f "$OUT/co-clang.lnt" ]]; then
  CXX="$(xcrun --find clang++ 2>/dev/null || command -v clang++)"
  SDK_OPT=""
  [[ "$(uname)" == Darwin ]] && SDK_OPT=" -isysroot $(xcrun --show-sdk-path)"
  "$PY" "$CONFIG/pclp_config.py" --compiler=clang --compiler-bin="$CXX" \
    --compiler-database="$CONFIG/compilers.yaml" --compiler-cpp-options="-std=c++20$SDK_OPT" \
    --config-output-lnt-file="$OUT/co-clang.lnt" --config-output-header-file="$OUT/co-clang.h" \
    --generate-compiler-config
fi

# Project configuration: own .cpp only (no .mm, nothing generated, no third_party).
python3 - "$BUILD_DIR/compile_commands.json" "$OUT/compile_commands.json" "$PWD" "${LF_PCLP_ONLY:-}" <<'PYEOF'
import json, os, re, sys
src, dst, root, only = sys.argv[1:5]
keep = []
for e in json.load(open(src)):
    f = os.path.relpath(os.path.join(e["directory"], e["file"]), root)
    if f.endswith(".cpp") and f.split(os.sep)[0] in ("src", "tests") and re.search(only, f):
        keep.append(e)
json.dump(keep, open(dst, "w"))
print(f"pclp: {len(keep)} files")
PYEOF
"$PY" "$CONFIG/pclp_config.py" --compiler=clang --compilation-db="$OUT/compile_commands.json" \
  --config-output-lnt-file="$OUT/project.lnt" --generate-project-config

# PC-lint Plus has no framework search (-F): <QtCore/qstring.h> is found through a folder of
# links QtCore -> .../QtCore.framework/Headers for every -F directory of the build.
python3 - "$OUT/compile_commands.json" "$OUT" <<'PYEOF'
import glob, json, os, shlex, sys
db, out = sys.argv[1:3]
fw_dirs = []
for e in json.load(open(db)):
    args = e.get("arguments") or shlex.split(e["command"])
    for i, a in enumerate(args):
        d = args[i + 1] if a in ("-F", "-iframework") else a[2:] if a.startswith("-F") else None
        if d and d not in fw_dirs:
            fw_dirs.append(d)
links = os.path.join(out, "frameworks")
os.makedirs(links, exist_ok=True)
for d in fw_dirs:
    for fw in glob.glob(os.path.join(d, "*.framework")):
        name, target = os.path.basename(fw)[: -len(".framework")], os.path.join(fw, "Headers")
        link = os.path.join(links, name)
        if os.path.isdir(target) and not os.path.lexists(link):
            os.symlink(target, link)
with open(os.path.join(out, "frameworks.lnt"), "w") as f:
    f.write(f'-i"{os.path.abspath(links)}"\n')
PYEOF

set +e
ROOT="$PWD"
(cd "$OUT" && "$BIN" -max_threads="$JOBS" co-clang.lnt frameworks.lnt -header\("$ROOT/tools/pclint_builtins.h"\) \
  +libh\("$ROOT/tools/pclint_builtins.h"\) "$ROOT/tools/pclint.lnt" project.lnt) \
  > "$OUT/report.txt" 2>&1
rc=$?
set -e
cat "$OUT/report.txt"
echo "pclp: exit code $rc (report: $OUT/report.txt)"
[[ $REPORT == 1 ]] && exit 0
exit $rc
