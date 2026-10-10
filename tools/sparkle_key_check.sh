#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks that a Sparkle EdDSA private key file (exported with `generate_keys -x`) belongs
# to the public key the app ships (SUPublicEDKey, LF_SPARKLE_PUBLIC_KEY) - so a release
# never publishes updates that the installed apps would reject (F-MAC-08, RELEASING.md).
#
# Usage: tools/sparkle_key_check.sh <private-key-file> <expected-public-key-base64>
#        tools/sparkle_key_check.sh --print-public <private-key-file>
#        tools/sparkle_key_check.sh --self-test
#
# Key formats (base64 in the file), as Sparkle's generate_keys stores them:
#   32 bytes  ed25519 seed (current format). Public key = clamp(SHA-512(seed)[0:32]) * B.
#   96 bytes  legacy: 64-byte orlp/ed25519 secret key (SHA-512 of the seed, first half
#             clamped = the scalar a, second half = nonce prefix) + 32-byte public key.
#             The seed is not stored; the public key is DERIVED from the scalar a
#             (a * B) and compared - the stored 32-byte suffix is not trusted (a file
#             with a foreign suffix fails, and a suffix that differs from the derived
#             key is reported).
# The derivation is plain Python 3 (RFC 8032 arithmetic, no extra packages). The private
# key never appears on a command line or in the output; only public keys are printed.
#
# --self-test generates throw-away keys with OpenSSL 3 in a fresh mktemp directory (never
# in a keychain), writes them in both formats and checks: matching pairs pass, a wrong
# expected key fails, a legacy file with a foreign public-key suffix fails. OpenSSL is the
# independent reference for the public key. Skipped (exit 0) without OpenSSL 3.
set -euo pipefail

usage() { sed -n '7,9p' "$0" >&2; exit 2; }

# derive <key-file>: prints "<derived-public-b64> <stored-suffix-b64 or ->"
derive() {
  python3 -I - "$1" <<'PY'
import base64, hashlib, sys

p = 2**255 - 19
d = -121665 * pow(121666, p - 2, p) % p
gx = 15112221349535400772501151409588531511454012693041857206046113283949847762202
gy = 46316835694926478169428394003475163141307993866256225615783033603165251855960
G = (gx, gy, 1, gx * gy % p)

def add(P, Q):  # extended twisted Edwards coordinates (RFC 8032, 5.1.4)
    A = (P[1] - P[0]) * (Q[1] - Q[0]) % p
    B = (P[1] + P[0]) * (Q[1] + Q[0]) % p
    C = 2 * P[3] * Q[3] * d % p
    D = 2 * P[2] * Q[2] % p
    E, F, G_, H = B - A, D - C, D + C, B + A
    return (E * F % p, G_ * H % p, F * G_ % p, E * H % p)

def mul(s, P):
    Q = (0, 1, 1, 0)
    while s:
        if s & 1:
            Q = add(Q, P)
        P = add(P, P)
        s >>= 1
    return Q

def compress(P):
    zi = pow(P[2], p - 2, p)
    x, y = P[0] * zi % p, P[1] * zi % p
    return (y | ((x & 1) << 255)).to_bytes(32, "little")

def scalar(b32):
    a = int.from_bytes(b32, "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a

try:
    raw = base64.b64decode("".join(open(sys.argv[1], encoding="ascii").read().split()), validate=True)
except Exception:
    sys.exit("sparkle_key_check: key file is not base64")
if len(raw) == 32:
    a, stored = scalar(hashlib.sha512(raw).digest()[:32]), None
elif len(raw) == 96:
    a, stored = scalar(raw[:32]), raw[64:]
else:
    sys.exit(f"sparkle_key_check: unknown key format ({len(raw)} bytes, expected 32 or 96)")
pub = base64.b64encode(compress(mul(a, G))).decode()
print(pub, base64.b64encode(stored).decode() if stored is not None else "-")
PY
}

# check <key-file> <expected>: exit 0 if the derived public key is <expected>
check() {
  local out public stored
  out=$(derive "$1") || return 1
  public=${out% *}
  stored=${out#* }
  if [[ "$stored" != - && "$stored" != "$public" ]]; then
    echo "sparkle_key_check: legacy key file is inconsistent: its stored public key $stored does not belong to its private key (derived: $public)" >&2
  fi
  if [[ "$public" != "$2" ]]; then
    echo "sparkle_key_check: private key does not match the public key $2 (it belongs to $public)" >&2
    return 1
  fi
  if [[ "$stored" != - && "$stored" != "$public" ]]; then
    return 1
  fi
  echo "Sparkle key pair ok (public key $public)"
}

self_test() {
  local openssl="" c work fails=0
  for c in /opt/homebrew/opt/openssl@3/bin/openssl /usr/local/opt/openssl@3/bin/openssl "$(command -v openssl || true)"; do
    if [[ -n "$c" && -x "$c" ]] && "$c" version 2>/dev/null | grep -q '^OpenSSL 3'; then openssl=$c; break; fi
  done
  if [[ -z "$openssl" ]]; then
    echo "sparkle_key_check --self-test: OpenSSL 3 not found - skipped"
    return 0
  fi
  umask 077
  work=$(mktemp -d "${TMPDIR:-/tmp}/lumaflip-keytest.XXXXXX")
  # shellcheck disable=SC2064 # expand $work now
  trap "rm -f -- '$work'/*; rmdir -- '$work'" EXIT

  # Two independent key pairs; raw seed = last 32 bytes of the PKCS#8 DER, public key =
  # last 32 bytes of the SubjectPublicKeyInfo DER (RFC 8410).
  local i
  for i in 1 2; do
    "$openssl" genpkey -algorithm ed25519 -outform DER -out "$work/k$i.der" 2>/dev/null
    tail -c 32 "$work/k$i.der" > "$work/seed$i"
    "$openssl" pkey -inform DER -in "$work/k$i.der" -pubout -outform DER | tail -c 32 > "$work/pub$i"
    base64 < "$work/seed$i" | tr -d '\n' > "$work/new$i.key"
    # Legacy: SHA-512(seed) with the first half clamped (orlp ed25519_create_keypair) + pub.
    "$openssl" dgst -sha512 -binary "$work/seed$i" > "$work/h$i"
    python3 -I - "$work/h$i" "$work/sk$i" <<'PY'
import sys
h = bytearray(open(sys.argv[1], "rb").read())
h[0] &= 248; h[31] &= 63; h[31] |= 64
open(sys.argv[2], "wb").write(bytes(h))
PY
    cat "$work/sk$i" "$work/pub$i" | base64 | tr -d '\n' > "$work/old$i.key"
  done
  # Legacy file with key 1's secret but key 2's public key as suffix (must not be trusted).
  cat "$work/sk1" "$work/pub2" | base64 | tr -d '\n' > "$work/forged.key"
  local pub1 pub2
  pub1=$(base64 < "$work/pub1" | tr -d '\n')
  pub2=$(base64 < "$work/pub2" | tr -d '\n')

  expect() { # expect <ok|fail> <description> <key> <public>
    local rc=0
    check "$3" "$4" >/dev/null 2>&1 || rc=1
    if [[ ($1 == ok && $rc == 0) || ($1 == fail && $rc == 1) ]]; then
      echo "  ok    $2"
    else
      echo "  FAIL  $2 (expected $1)"; fails=$((fails + 1))
    fi
  }
  echo "sparkle_key_check self-test ($("$openssl" version | cut -d' ' -f1-2), keys in $work)"
  expect ok   "32-byte seed matches its public key"         "$work/new1.key"   "$pub1"
  expect fail "32-byte seed vs. another public key"         "$work/new1.key"   "$pub2"
  expect ok   "96-byte legacy key matches its public key"   "$work/old2.key"   "$pub2"
  expect fail "96-byte legacy key vs. another public key"   "$work/old2.key"   "$pub1"
  expect fail "96-byte legacy key with foreign suffix (claims key 2)" "$work/forged.key" "$pub2"
  expect fail "96-byte legacy key with foreign suffix (own key 1)"    "$work/forged.key" "$pub1"
  local printed
  printed=$(derive "$work/old1.key"); printed=${printed% *}
  if [[ "$printed" == "$pub1" ]]; then echo "  ok    --print-public derives the OpenSSL public key"
  else echo "  FAIL  --print-public: $printed != $pub1"; fails=$((fails + 1)); fi
  [[ $fails == 0 ]] || { echo "sparkle_key_check self-test: $fails failure(s)" >&2; return 1; }
  echo "sparkle_key_check self-test passed"
}

case "${1:-}" in
  --self-test) [[ $# == 1 ]] || usage; self_test; exit $? ;;
  --print-public)
    [[ $# == 2 ]] || usage
    [[ -r "$2" ]] || { echo "sparkle_key_check: cannot read $2" >&2; exit 1; }
    out=$(derive "$2"); echo "${out% *}"; exit 0 ;;
  -h|--help) usage ;;
esac
[[ $# == 2 ]] || usage
[[ -r "$1" ]] || { echo "sparkle_key_check: cannot read $1" >&2; exit 1; }
check "$1" "$2"
