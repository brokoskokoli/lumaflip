#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Notarizes a signed Lumaflip.app or DMG with Apple and staples the ticket (SPEC 10.1).
#
#   tools/notarize.sh [--dry-run] <Lumaflip.app | file.dmg | file.zip> [--staple <path>]...
#
# * .app  : zipped (ditto) for the upload; the ticket is stapled to the app.
# * .dmg  : uploaded as is; the ticket is stapled to the DMG.
# * .zip  : uploaded as is; nothing can be stapled to a zip - pass --staple <app>.
# --staple adds further paths to staple after a successful notarization.
#
# Credentials (one of):
#   LF_NOTARY_PROFILE=<name>   keychain profile created with
#                              xcrun notarytool store-credentials <name> ...
#   LF_NOTARY_KEY_ID, LF_NOTARY_ISSUER, LF_NOTARY_KEY_PATH
#                              App Store Connect API key (.p8) - used in CI
#
# --dry-run prints every command instead of running it (no network, no credentials
# needed). Requires notarytool + stapler: Xcode 13+ or the Command Line Tools
# (both ship them; `xcrun --find notarytool`).
set -euo pipefail

DRY=0; INPUT=""; STAPLE=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run) DRY=1 ;;
    --staple) STAPLE+=("$2"); shift ;;
    -h|--help) sed -n '3,21p' "$0"; exit 0 ;;
    -*) echo "notarize.sh: unknown option $1" >&2; exit 2 ;;
    *) [[ -z "$INPUT" ]] || { echo "notarize.sh: only one input file" >&2; exit 2; }; INPUT="$1" ;;
  esac; shift
done
[[ -n "$INPUT" ]] || { echo "usage: tools/notarize.sh [--dry-run] <app|dmg|zip> [--staple path]..." >&2; exit 2; }
[[ -e "$INPUT" ]] || { echo "notarize.sh: $INPUT does not exist" >&2; exit 2; }

say() { printf '\033[1;34m==> %s\033[0m\n' "$*"; }
# Prints the command (quoted) in dry-run mode, runs it otherwise.
run() {
  if [[ $DRY == 1 ]]; then printf '+'; printf ' %q' "$@"; printf '\n'; else "$@"; fi
}

for tool in notarytool stapler; do
  if ! xcrun --find "$tool" >/dev/null 2>&1; then
    echo "notarize.sh: '$tool' not found. Install Xcode 13+ or the Command Line Tools" \
         "(xcode-select --install)." >&2
    [[ $DRY == 1 ]] || exit 1
  fi
done

# ---- credentials ------------------------------------------------------------------
AUTH=()
if [[ -n "${LF_NOTARY_PROFILE:-}" ]]; then
  AUTH=(--keychain-profile "$LF_NOTARY_PROFILE")
elif [[ -n "${LF_NOTARY_KEY_ID:-}" && -n "${LF_NOTARY_ISSUER:-}" && -n "${LF_NOTARY_KEY_PATH:-}" ]]; then
  [[ $DRY == 1 || -f "$LF_NOTARY_KEY_PATH" ]] || { echo "notarize.sh: LF_NOTARY_KEY_PATH=$LF_NOTARY_KEY_PATH missing" >&2; exit 1; }
  AUTH=(--key "$LF_NOTARY_KEY_PATH" --key-id "$LF_NOTARY_KEY_ID" --issuer "$LF_NOTARY_ISSUER")
elif [[ $DRY == 1 ]]; then
  echo "notarize.sh: no credentials set - using placeholders for the dry run" >&2
  AUTH=(--keychain-profile "<LF_NOTARY_PROFILE>")
else
  echo "notarize.sh: set LF_NOTARY_PROFILE or LF_NOTARY_KEY_ID + LF_NOTARY_ISSUER + LF_NOTARY_KEY_PATH" >&2
  exit 1
fi

# ---- what to upload ---------------------------------------------------------------
INPUT="${INPUT%/}"
UPLOAD="$INPUT"
# Own temporary directory: upload zip of an .app and the notarytool output. Removed on
# exit by deleting exactly the files this script created (no recursive delete).
WORK=$(mktemp -d "${TMPDIR:-/tmp}/lumaflip-notarize.XXXXXX")
APP_ZIP=""
cleanup() {
  [[ -n "${WORK:-}" && -d "$WORK" && "$(basename "$WORK")" == lumaflip-notarize.* ]] || return 0
  rm -f -- "$WORK/submit.out" "$WORK/log.out" ${APP_ZIP:+"$APP_ZIP"}
  rmdir -- "$WORK" 2>/dev/null || echo "notarize.sh: left $WORK (not empty)" >&2
}
trap cleanup EXIT
case "$INPUT" in
  *.app)
    APP_ZIP="$WORK/$(basename "$INPUT" .app).zip"
    UPLOAD="$APP_ZIP"
    say "Zipping $INPUT for the upload"
    run ditto -c -k --sequesterRsrc --keepParent "$INPUT" "$UPLOAD"
    STAPLE=("$INPUT" "${STAPLE[@]+"${STAPLE[@]}"}")
    ;;
  *.dmg) STAPLE=("$INPUT" "${STAPLE[@]+"${STAPLE[@]}"}") ;;
  *.zip) ;;
  *) echo "notarize.sh: unsupported input $INPUT (expected .app, .dmg or .zip)" >&2; exit 2 ;;
esac

# The upload must already carry a Developer ID signature with Hardened Runtime.
if [[ $DRY == 0 && "$INPUT" == *.app ]]; then
  if codesign -dv "$INPUT" 2>&1 | grep -q 'Signature=adhoc'; then
    echo "notarize.sh: $INPUT is only ad-hoc signed - sign with LF_SIGN_IDENTITY first" >&2
    exit 1
  fi
fi

# ---- submit -----------------------------------------------------------------------
# Prints "<id>|<status>" from notarytool output: the JSON object (--output-format json),
# else "id: <uuid>" and "status: <word>" from plain text (timeouts, errors).
parse_submission() {
  python3 -I -c '
import json, re, sys
text = open(sys.argv[1], errors="replace").read()
sid = status = ""
for line in text.splitlines():
    line = line.strip()
    if line.startswith("{"):
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        sid = obj.get("id", sid) or sid
        status = obj.get("status", status) or status
if not sid:
    m = re.search(r"\bid:\s*([0-9a-fA-F]{8}-(?:[0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12})\b", text)
    sid = m.group(1) if m else ""
if not status:
    m = re.search(r"status:\s*([A-Za-z ]+)", text)
    status = m.group(1).strip() if m else ""
print(f"{sid}|{status}")
' "$1"
}

say "Submitting $(basename "$UPLOAD") to the Apple notary service (waits for the result)"
if [[ $DRY == 1 ]]; then
  run xcrun notarytool submit "$UPLOAD" "${AUTH[@]}" --wait --timeout 1h --output-format json
  SUB_ID="<submission-id>"; STATUS="Accepted"; RC=0
else
  # notarytool exits non-zero for Invalid/Rejected, timeouts and upload errors; under
  # `set -e` a failing $(...) would end the script before the log is shown.
  RC=0
  xcrun notarytool submit "$UPLOAD" "${AUTH[@]}" --wait --timeout 1h --output-format json \
    >"$WORK/submit.out" 2>&1 || RC=$?
  cat "$WORK/submit.out"
  IFS="|" read -r SUB_ID STATUS < <(parse_submission "$WORK/submit.out") || true
fi
if [[ $RC != 0 || "$STATUS" != "Accepted" ]]; then
  echo "notarize.sh: notarization failed (exit code $RC, status '${STATUS:-unknown}', submission '${SUB_ID:-none}')" >&2
  if [[ -n "${SUB_ID:-}" ]]; then
    echo "notarize.sh: notary log (xcrun notarytool log $SUB_ID):" >&2
    if ! xcrun notarytool log "$SUB_ID" "${AUTH[@]}" >"$WORK/log.out" 2>&1; then
      echo "notarize.sh: could not fetch the log (a submission still in progress has none yet;" \
           "check later with: xcrun notarytool info/log $SUB_ID)" >&2
    fi
    cat "$WORK/log.out" >&2
  else
    echo "notarize.sh: no submission ID (upload failed?) - see the output above" >&2
  fi
  exit 1
fi
[[ $DRY == 1 ]] && run xcrun notarytool log "$SUB_ID" "${AUTH[@]}"

# ---- staple -----------------------------------------------------------------------
for path in "${STAPLE[@]+"${STAPLE[@]}"}"; do
  say "Stapling $path"
  run xcrun stapler staple "$path"
  run xcrun stapler validate "$path"
done
case "$INPUT" in
  *.app) run spctl --assess --type execute -vvv "$INPUT" ;;
  *.dmg) run spctl --assess --type open --context context:primary-signature -vvv "$INPUT" ;;
esac
say "Notarization done$([[ $DRY == 1 ]] && echo ' (dry run - nothing was sent)')"
