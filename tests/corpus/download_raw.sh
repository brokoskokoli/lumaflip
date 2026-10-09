#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# OPTIONAL: downloads a few small RAW samples into tests/corpus/generated/raw/.
# Source: https://raw.pixls.us - all files below are released under CC0 1.0
# (Creative Commons Zero, public domain dedication) according to the site's index.
# Every file is pinned by its SHA-256; mismatching downloads are deleted.
# Tests that use RAW files are skipped when the files are absent - nothing depends
# on this script or on network access.
set -euo pipefail
cd "$(dirname "$0")"
OUT="generated/raw"
mkdir -p "$OUT"

# local name | sha256 | URL (path percent-encoded)
SAMPLES=(
  "canon_eos_r6.cr3|74abb0a113d075ad9887a058082f40dd2a938c4813a08474d82356f11a027778|https://raw.pixls.us/getfile.php/4659/nice/Canon%20-%20EOS%20R6%20-%203%3A2.CR3"
  "nikon_d2h.nef|155edb938f884ea7372ce98d4ff5f965c3e413b43b95bc9923da6e92082cf914|https://raw.pixls.us/getfile.php/5227/nice/Nikon%20-%20D2H%20-%2012bit%2012bit%20compressed%20(Lossy%20(type%201))%20(3%3A2).NEF"
  "sony_ilce7s.arw|a35ebb2fbec929daa5beb20d1ce5c15a8aac7b1a7a231455387f3df8a7442e07|https://raw.pixls.us/getfile.php/1582/nice/Sony%20-%20ILCE-7S%20-%2014bit%2014bit%20compressed%20(3%3A2).ARW"
  "sigma_fp.dng|99bcffd118630af5cf5712eeac6832045420311fe258094e3489b94c8d51236a|https://raw.pixls.us/getfile.php/7278/nice/Sigma%20-%20fp%20-%2012bit%20(16%3A9).DNG"
  "fujifilm_xs10.raf|9419d1408ebf850395e5dd563beb96309466546252caf541466034c9cb7e9724|https://raw.pixls.us/getfile.php/4190/nice/Fujifilm%20-%20X-S10%20-%2014bit%2014bit%20compressed%20(3%3A2).RAF"
)

fail=0
for entry in "${SAMPLES[@]}"; do
  IFS='|' read -r name sha url <<<"$entry"
  dest="$OUT/$name"
  if [[ -f "$dest" ]] && [[ "$(shasum -a 256 "$dest" | cut -d' ' -f1)" == "$sha" ]]; then
    echo "ok (cached)  $name"
    continue
  fi
  echo "download     $name"
  if ! curl -fsSL --retry 2 --max-time 300 -o "$dest.part" "$url"; then
    echo "  failed: $url" >&2
    rm -f "$dest.part"
    fail=1
    continue
  fi
  got="$(shasum -a 256 "$dest.part" | cut -d' ' -f1)"
  if [[ "$got" != "$sha" ]]; then
    echo "  checksum mismatch for $name (got $got)" >&2
    rm -f "$dest.part"
    fail=1
    continue
  fi
  mv "$dest.part" "$dest"
done
exit $fail
