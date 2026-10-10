#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks that a Sparkle EdDSA private key file (exported with `generate_keys -x`) belongs
# to the public key the app ships (SUPublicEDKey, LF_SPARKLE_PUBLIC_KEY) - so a release
# never publishes updates that the installed apps would reject (F-MAC-08, RELEASING.md).
#
# Usage: tools/sparkle_key_check.sh <private-key-file> <expected-public-key-base64>
#        tools/sparkle_key_check.sh --print-public <private-key-file>
#
# Key formats (base64 in the file): 32 bytes = ed25519 seed (current generate_keys),
# 96 bytes = legacy 64-byte secret key + 32-byte public key. The public key of a seed is
# derived with OpenSSL 3 (ed25519). The private key never appears on a command line or in
# the output; only the derived public key is printed.
set -euo pipefail

usage() { sed -n '7,8p' "$0" >&2; exit 2; }
PRINT=0
if [[ "${1:-}" == --print-public ]]; then PRINT=1; shift; [[ $# == 1 ]] || usage
else [[ $# == 2 ]] || usage; fi
KEY_FILE=$1
EXPECTED=${2:-}

OPENSSL=""
for c in /opt/homebrew/opt/openssl@3/bin/openssl /usr/local/opt/openssl@3/bin/openssl "$(command -v openssl || true)"; do
  if [[ -n "$c" && -x "$c" ]] && "$c" version 2>/dev/null | grep -q '^OpenSSL 3'; then OPENSSL=$c; break; fi
done

[[ -r "$KEY_FILE" ]] || { echo "sparkle_key_check: cannot read $KEY_FILE" >&2; exit 1; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/lumaflip-keycheck.XXXXXX")
trap 'rm -f -- "$WORK/raw" "$WORK/der"; rmdir -- "$WORK"' EXIT
umask 077
tr -d ' \r\n' < "$KEY_FILE" | base64 -d > "$WORK/raw" 2>/dev/null \
  || { echo "sparkle_key_check: key file is not base64" >&2; exit 1; }
size=$(wc -c < "$WORK/raw" | tr -d ' ')
case "$size" in
  32)
    [[ -n "$OPENSSL" ]] || { echo "sparkle_key_check: needs OpenSSL 3 (brew install openssl@3)" >&2; exit 1; }
    # PKCS#8 wrapper for a raw ed25519 private key (RFC 8410), then the public key's DER
    # (SubjectPublicKeyInfo) ends with the 32 raw key bytes.
    { printf '\x30\x2e\x02\x01\x00\x30\x05\x06\x03\x2b\x65\x70\x04\x22\x04\x20'; cat "$WORK/raw"; } > "$WORK/der"
    public=$("$OPENSSL" pkey -inform DER -in "$WORK/der" -pubout -outform DER | tail -c 32 | base64)
    ;;
  96)
    public=$(tail -c 32 "$WORK/raw" | base64)
    ;;
  *)
    echo "sparkle_key_check: unknown key format ($size bytes)" >&2; exit 1 ;;
esac

if [[ $PRINT == 1 ]]; then
  echo "$public"
  exit 0
fi
if [[ "$public" != "$EXPECTED" ]]; then
  echo "sparkle_key_check: private key does not match the public key $EXPECTED (it belongs to $public)" >&2
  exit 1
fi
echo "Sparkle key pair ok (public key $public)"
