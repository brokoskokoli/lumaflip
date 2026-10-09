#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Generates the format corpus into tests/corpus/generated/ (not committed).
# Idempotent: does nothing if the generator inputs did not change. Use --force to rebuild.
# Requires swiftc (Xcode Command Line Tools). Optional: cwebp/img2webp (WebP), cjxl (JPEG XL).
set -euo pipefail
cd "$(dirname "$0")"

FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

OUT="generated"
BIN="$OUT/.bin"
STAMP="$OUT/.stamp"
export PATH="/opt/homebrew/bin:$PATH"

inputs_hash() {
  {
    cat tools/gen_corpus.swift make_corpus.sh
    for t in cwebp img2webp cjxl; do command -v "$t" >/dev/null && echo "$t" || true; done
  } | shasum -a 256 | cut -d' ' -f1
}
HASH="$(inputs_hash)"

if [[ $FORCE == 0 && -f "$STAMP" && "$(cat "$STAMP")" == "$HASH" ]]; then
  echo "corpus up to date ($OUT)"
  exit 0
fi

if ! command -v swiftc >/dev/null; then
  echo "swiftc not found - corpus not generated (tests that need it are skipped)" >&2
  exit 0
fi

mkdir -p "$BIN"
echo "compiling corpus generator ..."
swiftc -O -o "$BIN/gen_corpus" tools/gen_corpus.swift
"$BIN/gen_corpus" "$OUT"

# Optional encoders that ImageIO lacks (WebP, JPEG XL). Missing tools -> files skipped.
if command -v cwebp >/dev/null; then
  cwebp -quiet -lossless -exact "$OUT/rgba8.png" -o "$OUT/rgba8.webp"
  cwebp -quiet -q 95 "$OUT/srgb.png" -o "$OUT/lossy.webp"
else
  echo "skip: WebP stills (cwebp not installed)"
fi
if command -v img2webp >/dev/null && [[ -f "$OUT/anim.png" ]]; then
  tmpd="$OUT/.frames"  # single frames written by the generator
  if [[ -f "$tmpd/f0.png" ]]; then
    img2webp -loop 0 -lossless -d 100 "$tmpd/f0.png" -d 200 "$tmpd/f1.png" -d 300 "$tmpd/f2.png" \
      -o "$OUT/anim.webp" >/dev/null 2>&1 || echo "skip: animated WebP (img2webp failed)"
  fi
else
  echo "skip: animated WebP (img2webp not installed)"
fi
if command -v cjxl >/dev/null; then
  cjxl --quiet -d 0 "$OUT/srgb.png" "$OUT/lossless.jxl" >/dev/null 2>&1 \
    || cjxl -d 0 "$OUT/srgb.png" "$OUT/lossless.jxl" >/dev/null 2>&1 || echo "skip: JXL (cjxl failed)"
else
  echo "skip: JPEG XL (cjxl not installed)"
fi

echo "$HASH" > "$STAMP"
echo "corpus generated in tests/corpus/$OUT"
