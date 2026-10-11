#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Richtet einmalig alles für die Release-Pipeline ein (interaktiv, nur auf dem eigenen Mac):
#
#   tools/setup_release_secrets.sh
#
# Ablauf:
#   1. Ziel: Environment "release" (empfohlen, optional mit Pflicht-Freigabe und
#      Tag-/Branch-Regeln) oder Repository-Secrets.
#   2. Developer-ID-Zertifikat samt privatem Schlüssel als .p12:
#      a) automatisch aus dem Anmelde-Schlüsselbund (`security export`; macOS fragt
#         pro privatem Schlüssel einmal nach dem Anmeldepasswort), oder
#      b) eine selbst in der Schlüsselbundverwaltung exportierte .p12-Datei.
#      Das .p12 wird auf genau diese Identität (plus Zwischenzertifikat) reduziert und
#      mit einem neuen Passwort verschlüsselt.
#   3. App Store Connect API Key für die Notarisierung: .p8, Key-ID, Issuer-ID
#      (optional gleich bei Apple geprüft).
#   4. Sparkle-Schlüsselpaar für automatische Updates (F-MAC-08): eigenes Paar im
#      Schlüsselbund (Konto "lumaflip"), wird bei Bedarf erzeugt.
#   5. Secrets per `gh secret set`, Variable SPARKLE_PUBLIC_ED_KEY per `gh variable set`.
#   6. Optional: GitHub Pages (Branch gh-pages) einschalten - Homepage und Update-Feed.
#   7. Temporäre Dateien löschen (auch bei Abbruch).
#
# Gesetzt werden: MACOS_CERT_P12_BASE64, MACOS_CERT_PASSWORD, NOTARY_API_KEY_P8_BASE64,
# NOTARY_API_KEY_ID, NOTARY_API_ISSUER_ID, SPARKLE_ED_PRIVATE_KEY (Secrets) und
# SPARKLE_PUBLIC_ED_KEY (Repository-Variable). Details: docs/RELEASING.md.
#
# Voraussetzungen: gh (angemeldet, Admin-Rechte am Repo), das Zertifikat
# "Developer ID Application: …" im Anmelde-Schlüsselbund, Xcode oder die Command Line Tools.
#
# Nichts davon landet im Repo oder in der Shell-History; Werte werden nie ausgegeben.
# Kein `set -x`. Umgebungsvariablen: LF_RELEASE_IDENTITY, LF_RELEASE_ENV, LF_RELEASE_REPO.
set -euo pipefail

IDENTITY=${LF_RELEASE_IDENTITY:-}
ENV_NAME=${LF_RELEASE_ENV:-release}
REPO=${LF_RELEASE_REPO:-}
SPARKLE_ACCOUNT=lumaflip
LOGIN_KC="$HOME/Library/Keychains/login.keychain-db"
# LibreSSL des Systems: liest das PKCS#12 von `security export` und schreibt eines,
# das `security import` auf dem Runner versteht (3DES/SHA1).
OPENSSL=/usr/bin/openssl
SECRETS=(MACOS_CERT_P12_BASE64 MACOS_CERT_PASSWORD NOTARY_API_KEY_P8_BASE64
         NOTARY_API_KEY_ID NOTARY_API_ISSUER_ID SPARKLE_ED_PRIVATE_KEY)
SECRET_SCOPE=()

WORK=
cleanup() {
    if [ -n "$WORK" ] && [ -d "$WORK" ]; then
        # Überschreiben vor dem Löschen; auf APFS/SSD ist das keine Garantie (Copy-on-Write),
        # die Dateien lagen aber nur verschlüsselt bzw. kurz in einem Verzeichnis mit 0700.
        find "$WORK" -type f -exec sh -c 'for f; do
            dd if=/dev/urandom of="$f" bs=1k count="$(( ($(stat -f %z "$f") + 1023) / 1024 ))" conv=notrunc 2>/dev/null
        done' sh {} + 2>/dev/null || true
        rm -rf "$WORK"
    fi
}

say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# ask "Frage" [Standard] → REPLY
ask() {
    local prompt=$1 def=${2:-}
    if [ -n "$def" ]; then prompt="$prompt [$def]"; fi
    read -r -p "$prompt: " REPLY
    REPLY=${REPLY:-$def}
}
# confirm "Frage" [j|n] → 0 bei Ja
confirm() {
    local def=${2:-n} hint="[j/N]"
    if [ "$def" = "j" ]; then hint="[J/n]"; fi
    read -r -p "$1 $hint " REPLY
    REPLY=${REPLY:-$def}
    [[ "$REPLY" =~ ^[jJyY] ]]
}
# ask_secret "Frage" [mindestlänge] → REPLY (zweimal eingeben)
ask_secret() {
    local a b min=${2:-1}
    while true; do
        read -r -s -p "$1: " a; echo
        if [ "${#a}" -lt "$min" ]; then say "Mindestens $min Zeichen."; continue; fi
        read -r -s -p "Wiederholen: " b; echo
        if [ "$a" = "$b" ]; then REPLY=$a; return 0; fi
        say "Stimmt nicht überein, bitte noch einmal."
    done
}

# Zerlegt die PEM-Ausgabe von `openssl pkcs12 -nodes` (stdin) in Blöcke und gibt je nach
# mode aus:
#   list  localKeyID jeder Identität "$id" (Zertifikat mit passendem Schlüssel), je Zeile
#   cert  das Zertifikat mit localKeyID "$want"
#   pair  Schlüssel + Zertifikat mit localKeyID "$want"
# shellcheck disable=SC2016
P12_AWK='
    /^Bag Attributes/ { lkid = ""; fname = ""; subj = ""; next }
    /^[[:space:]]+localKeyID:/ { sub(/^[[:space:]]+localKeyID:[[:space:]]*/, ""); lkid = $0; next }
    /^[[:space:]]+friendlyName:/ { sub(/^[[:space:]]+friendlyName:[[:space:]]*/, ""); fname = $0; next }
    /^subject=/ { subj = $0; next }
    /^-----BEGIN / { inpem = 1; type = ($0 ~ /CERTIFICATE/) ? "cert" : "key"; pem = $0 "\n"; next }
    inpem {
        pem = pem $0 "\n"
        if ($0 ~ /^-----END /) {
            inpem = 0; n++
            T[n] = type; L[n] = lkid; F[n] = fname; S[n] = subj; P[n] = pem
        }
        next
    }
    # Mehrere Zertifikate können denselben Schlüssel (dieselbe localKeyID) haben; deshalb
    # immer zusätzlich über den Namen auswählen.
    function named(i) { return id == "" || F[i] == id || index(S[i], "CN=" id "/") || S[i] ~ ("CN=" id "$") }
    END {
        for (i = 1; i <= n; i++) if (T[i] == "key" && L[i] != "") K[L[i]] = i
        for (i = 1; i <= n; i++) {
            if (T[i] != "cert" || L[i] == "" || !(L[i] in K) || !named(i)) continue
            if (mode == "list") {
                print L[i]
            } else if (L[i] == want) {
                if (mode == "pair") printf "%s", P[K[L[i]]]
                printf "%s", P[i]
                exit 0
            }
        }
    }'

# PEM-Ausgabe eines PKCS#12 (privater Schlüssel unverschlüsselt, nur in der Pipe).
p12_dump() { IN_PASS=$2 "$OPENSSL" pkcs12 -in "$1" -nodes -passin env:IN_PASS 2>/dev/null; }

# notAfter eines Zertifikats (PEM auf stdin) als Unix-Zeit.
not_after_epoch() {
    local d
    d=$("$OPENSSL" x509 -noout -enddate | cut -d= -f2 | tr -s ' ')
    LC_ALL=C date -j -u -f '%b %d %T %Y GMT' "$d" +%s
}

# Schreibt aus einem PKCS#12 mit beliebig vielen Identitäten genau eine Identität $3
# (Zertifikat + passender privater Schlüssel) in ein neues PKCS#12. Gibt es mehrere (z. B.
# nach einer Verlängerung), gewinnt die nicht abgelaufene mit dem spätesten Ablaufdatum.
#   extract_identity <in.p12> <in-passwort> <identität> <out.p12> <out-passwort> [kette.pem]
# Rückgabe: 0 ok, 1 Fehler (Passwort, Export), 3 Identität fehlt, 5 alle abgelaufen.
# Der private Schlüssel liegt dabei nur im Speicher (Pipe), nie unverschlüsselt auf der Platte.
extract_identity() {
    local in=$1 in_pass=$2 identity=$3 out=$4 out_pass=$5 chain=${6:-}
    local certfile=() lkids lkid pem end best="" best_end=0
    if [ -n "$chain" ] && [ -s "$chain" ]; then certfile=(-certfile "$chain"); fi
    IN_PASS=$in_pass "$OPENSSL" pkcs12 -in "$in" -nokeys -passin env:IN_PASS >/dev/null 2>&1 || return 1
    lkids=$(p12_dump "$in" "$in_pass" | awk -v mode=list -v id="$identity" "$P12_AWK")
    [ -n "$lkids" ] || return 3
    while IFS= read -r lkid; do
        pem=$(p12_dump "$in" "$in_pass" | awk -v mode=cert -v want="$lkid" -v id="$identity" "$P12_AWK")
        "$OPENSSL" x509 -noout -checkend 0 <<<"$pem" >/dev/null 2>&1 || continue
        end=$(not_after_epoch <<<"$pem") || continue
        if [ "$end" -gt "$best_end" ]; then best=$lkid; best_end=$end; fi
    done <<<"$lkids"
    [ -n "$best" ] || return 5
    p12_dump "$in" "$in_pass" | awk -v mode=pair -v want="$best" -v id="$identity" "$P12_AWK" \
        | OUT_PASS=$out_pass "$OPENSSL" pkcs12 -export -name "$identity" \
            ${certfile[@]+"${certfile[@]}"} \
            -keypbe PBE-SHA1-3DES -certpbe PBE-SHA1-3DES -macalg sha1 \
            -passout env:OUT_PASS -out "$out" 2>/dev/null \
        || { rm -f "$out"; return 1; }
}

# Prüft ein PKCS#12: Identität vorhanden, Schlüssel passt zum Zertifikat, nicht abgelaufen.
verify_p12() {
    local p12=$1 pass=$2 identity=$3 cert subj
    cert=$(P=$pass "$OPENSSL" pkcs12 -in "$p12" -nokeys -clcerts -passin env:P 2>/dev/null) \
        || { say "  .p12 lässt sich mit diesem Passwort nicht öffnen."; return 1; }
    subj=$("$OPENSSL" x509 -noout -subject <<<"$cert")
    if [[ "$subj" != *"CN=$identity"* ]]; then
        say "  Zertifikat im .p12 ist nicht \"$identity\" ($subj)."
        return 1
    fi
    if ! "$OPENSSL" x509 -noout -checkend 0 <<<"$cert" >/dev/null; then
        say "  Zertifikat ist abgelaufen."
        return 1
    fi
    local pub_cert pub_key
    pub_cert=$("$OPENSSL" x509 -noout -pubkey <<<"$cert")
    pub_key=$(P=$pass "$OPENSSL" pkcs12 -in "$p12" -nocerts -nodes -passin env:P 2>/dev/null \
        | "$OPENSSL" pkey -pubout 2>/dev/null) || true
    if [ -z "$pub_key" ] || [ "$pub_cert" != "$pub_key" ]; then
        say "  Kein passender privater Schlüssel im .p12."
        return 1
    fi
    say "  OK: $subj, gültig bis $("$OPENSSL" x509 -noout -enddate <<<"$cert" | cut -d= -f2)"
}

# --- Schritte ------------------------------------------------------------------
step_target() {
    command -v gh >/dev/null || die "gh (GitHub CLI) fehlt: brew install gh"
    gh auth status >/dev/null 2>&1 || die "gh ist nicht angemeldet: gh auth login"
    if [ -z "$REPO" ]; then
        REPO=$(gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null || echo brokoskokoli/lumaflip)
    fi
    ask "Repository" "$REPO"; REPO=$REPLY

    say ""
    say "Secrets im Environment \"$ENV_NAME\" sind nur für Jobs sichtbar, die dieses Environment"
    say "nutzen (release.yml), und lassen sich mit einer Pflicht-Freigabe schützen (empfohlen)."
    if confirm "Secrets im Environment \"$ENV_NAME\" ablegen (sonst Repository-Secrets)?" j; then
        SECRET_SCOPE=(--env "$ENV_NAME")
        if ! gh api "repos/$REPO/environments/$ENV_NAME" >/dev/null 2>&1; then
            confirm "Environment \"$ENV_NAME\" existiert nicht. Anlegen?" j || die "Abgebrochen."
            gh api -X PUT "repos/$REPO/environments/$ENV_NAME" >/dev/null
            say "  Environment angelegt."
        fi
        step_protection
    else
        SECRET_SCOPE=()
    fi
}

step_protection() {
    say ""
    say "Schutz des Environments (jederzeit änderbar unter Settings → Environments → $ENV_NAME):"
    say "  - Pflicht-Freigabe: Jeder Release-Lauf wartet, bis du ihn in GitHub freigibst."
    say "  - Nur Tags v* und der Branch main dürfen das Environment nutzen (main: Probelauf)."
    confirm "Beides jetzt einrichten?" j || return 0
    local current uid
    current=$(gh api "repos/$REPO/environments/$ENV_NAME" --jq \
        '[(.protection_rules // [])[] | .type] | join(", ")' 2>/dev/null || true)
    if [ -n "$current" ]; then
        say "warning: Das Environment hat schon Schutzregeln ($current). Sie werden ersetzt:"
        say "         Reviewer nur noch du, Wartezeit und andere Regeln entfallen,"
        say "         Deployment-Regeln auf \"ausgewählte Branches und Tags\"."
        confirm "Vorhandene Schutzregeln überschreiben?" n || { say "  Unverändert gelassen."; return 0; }
    fi
    uid=$(gh api user --jq .id)
    if ! printf '{"reviewers":[{"type":"User","id":%s}],"prevent_self_review":false,"deployment_branch_policy":{"protected_branches":false,"custom_branch_policies":true}}' "$uid" \
        | gh api -X PUT "repos/$REPO/environments/$ENV_NAME" --input - >/dev/null; then
        say "warning: Konnte den Schutz nicht setzen; bitte von Hand (docs/RELEASING.md)."
        return 0
    fi
    local existing
    existing=$(gh api "repos/$REPO/environments/$ENV_NAME/deployment-branch-policies" \
        --jq '.branch_policies[] | "\(.type):\(.name)"' 2>/dev/null || true)
    grep -qx 'tag:v\*' <<<"$existing" || gh api -X POST \
        "repos/$REPO/environments/$ENV_NAME/deployment-branch-policies" -f name='v*' -f type=tag >/dev/null
    grep -qx 'branch:main' <<<"$existing" || gh api -X POST \
        "repos/$REPO/environments/$ENV_NAME/deployment-branch-policies" -f name=main -f type=branch >/dev/null
    say "  Pflicht-Freigabe und Tag-/Branch-Regeln gesetzt."
}

# Wählt die Developer-ID-Identität: LF_RELEASE_IDENTITY, sonst aus dem Schlüsselbund.
choose_identity() {
    [ -n "$IDENTITY" ] && return 0
    local found=()
    while IFS= read -r line; do
        [ -n "$line" ] && found+=("$line")
    done < <(security find-identity -v -p codesigning 2>/dev/null \
        | sed -nE 's/.*"(Developer ID Application: [^"]+)".*/\1/p' | sort -u)
    if [ "${#found[@]}" -eq 0 ]; then
        say "warning: Keine gültige \"Developer ID Application\"-Identität im Schlüsselbund gefunden."
        say "         Zertifikat unter developer.apple.com → Certificates anlegen und doppelklicken,"
        say "         oder unten Weg b) mit einer exportierten .p12 nutzen."
        ask "Name der Identität (Developer ID Application: Name (TEAMID))"
        IDENTITY=$REPLY
    elif [ "${#found[@]}" -eq 1 ]; then
        IDENTITY=${found[0]}
    else
        say "Mehrere Developer-ID-Identitäten:"
        local i
        for i in "${!found[@]}"; do say "  $((i + 1))) ${found[$i]}"; done
        ask "Nummer" 1
        IDENTITY=${found[$((REPLY - 1))]:-}
    fi
    [[ "$IDENTITY" == "Developer ID Application: "* ]] || die "Keine Developer-ID-Identität: $IDENTITY"
}

step_certificate() {
    choose_identity
    say ""
    say "== Zertifikat: $IDENTITY"
    say "Passwort für das .p12 (wird als MACOS_CERT_PASSWORD gespeichert; am besten ein"
    say "neues, zufälliges, z. B. aus dem Passwortmanager; keine Zeilenumbrüche):"
    ask_secret "Passwort" 12
    P12_PASS=$REPLY
    P12="$WORK/developer-id.p12"

    local chain="$WORK/chain.pem"
    security find-certificate -a -c "Developer ID Certification Authority" -p \
        /Library/Keychains/System.keychain "$LOGIN_KC" > "$chain" 2>/dev/null || true

    say ""
    say "  a) automatisch aus dem Anmelde-Schlüsselbund exportieren (security export;"
    say "     macOS fragt pro privatem Schlüssel einmal nach dem Anmeldepasswort,"
    say "     dort \"Erlauben\" wählen)"
    say "  b) eine selbst exportierte .p12-Datei angeben"
    ask "Weg" a
    if [ "$REPLY" = "a" ]; then
        local all="$WORK/all.p12" tmp_pass
        tmp_pass=$("$OPENSSL" rand -hex 24)
        say "==> security export (alle Identitäten, wird gleich auf eine reduziert)"
        if ! security export -k "$LOGIN_KC" -t identities -f pkcs12 -P "$tmp_pass" -o "$all"; then
            manual_export_help
            die "security export fehlgeschlagen. Weg b) nutzen."
        fi
        local rc=0
        extract_identity "$all" "$tmp_pass" "$IDENTITY" "$P12" "$P12_PASS" "$chain" || rc=$?
        rm -f "$all"
        if [ "$rc" -eq 5 ]; then
            die "Alle Zertifikate \"$IDENTITY\" sind abgelaufen. Neues Developer-ID-Zertifikat anlegen."
        elif [ "$rc" -ne 0 ]; then
            manual_export_help
            die "Identität \"$IDENTITY\" samt Schlüssel nicht im Export gefunden. Weg b) nutzen."
        fi
    else
        manual_export_help
        ask "Pfad zur exportierten .p12"
        local src=${REPLY/#\~/$HOME} src_pass
        [ -f "$src" ] || die "Datei nicht gefunden: $src"
        read -r -s -p "Passwort dieser .p12: " src_pass; echo
        extract_identity "$src" "$src_pass" "$IDENTITY" "$P12" "$P12_PASS" "$chain" \
            || die "Identität \"$IDENTITY\" samt Schlüssel nicht in $src gefunden (oder falsches Passwort)."
        unset src_pass
        say "  Hinweis: Die Datei $src kannst du nach dem Einrichten löschen."
    fi
    verify_p12 "$P12" "$P12_PASS" "$IDENTITY" || die ".p12 ungültig."
}

manual_export_help() {
    cat <<MSG

  Export von Hand (Schlüsselbundverwaltung):
    1. Programme → Dienstprogramme → Schlüsselbundverwaltung öffnen.
    2. Links "Anmeldung", oben "Meine Zertifikate".
    3. "$IDENTITY" suchen; das Dreieck davor muss einen privaten Schlüssel zeigen.
    4. Rechtsklick auf das Zertifikat → "… exportieren", Format
       "Persönlicher Informationsaustausch (.p12)", Passwort vergeben.
    5. Diese Datei hier angeben (Weg b).
MSG
}

step_api_key() {
    say ""
    say "== App Store Connect API Key (Notarisierung)"
    say "   App Store Connect → Users and Access → Integrations → App Store Connect API →"
    say "   Team Keys, Rolle \"Developer\". Den Key von Disk Rings kannst du wiederverwenden."
    ask "Pfad zur .p8-Datei (AuthKey_XXXXXXXXXX.p8)"
    P8=${REPLY/#\~/$HOME}
    [ -f "$P8" ] || die "Datei nicht gefunden: $P8"
    grep -q -- '-----BEGIN PRIVATE KEY-----' "$P8" || die "$P8 ist keine .p8-Datei."
    local guess=
    if [[ "$(basename "$P8")" =~ ^AuthKey_([A-Z0-9]{10})\.p8$ ]]; then guess=${BASH_REMATCH[1]}; fi
    while true; do
        ask "Key-ID" "$guess"; KEY_ID=$REPLY
        [[ "$KEY_ID" =~ ^[A-Z0-9]{10}$ ]] && break
        say "Die Key-ID hat 10 Zeichen (A-Z, 0-9)."
    done
    while true; do
        ask "Issuer-ID (UUID über der Key-Liste)"; ISSUER_ID=$REPLY
        [[ "$ISSUER_ID" =~ ^[0-9a-fA-F]{8}-([0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12}$ ]] && break
        say "Die Issuer-ID ist eine UUID (8-4-4-4-12)."
    done
    if confirm "API-Key jetzt bei Apple prüfen (notarytool history)?" j; then
        xcrun notarytool history --key "$P8" --key-id "$KEY_ID" --issuer "$ISSUER_ID" >/dev/null \
            || die "API-Key wird nicht akzeptiert."
        say "  API-Key funktioniert."
    fi
}

step_sparkle() {
    say ""
    say "== Sparkle-Schlüsselpaar für automatische Updates (Schlüsselbund-Konto \"$SPARKLE_ACCOUNT\")"
    local gk=third_party/Sparkle/bin/generate_keys
    if [ ! -x "$gk" ]; then
        say "==> tools/fetch_sparkle.sh (Sparkle-Werkzeuge laden)"
        tools/fetch_sparkle.sh >/dev/null || die "tools/fetch_sparkle.sh fehlgeschlagen."
    fi
    if SPARKLE_PUB=$("$gk" --account "$SPARKLE_ACCOUNT" -p 2>/dev/null) && [ -n "$SPARKLE_PUB" ]; then
        say "  Vorhandenes Paar gefunden (öffentlicher Schlüssel $SPARKLE_PUB)."
        say "  Nicht neu erzeugen: Installierte Versionen vertrauen nur diesem Schlüssel."
    else
        say "  Noch kein Paar. Es wird jetzt erzeugt und im Anmelde-Schlüsselbund gespeichert."
        say "  WICHTIG: Den privaten Schlüssel nie verlieren - ohne ihn bekommen installierte"
        say "  Versionen keine Updates mehr. Das Skript bietet gleich eine Sicherungskopie an."
        confirm "Erzeugen?" j || die "Abgebrochen."
        "$gk" --account "$SPARKLE_ACCOUNT" >/dev/null || die "generate_keys fehlgeschlagen."
        SPARKLE_PUB=$("$gk" --account "$SPARKLE_ACCOUNT" -p) || die "Öffentlichen Schlüssel nicht lesbar."
        say "  Erzeugt: öffentlicher Schlüssel $SPARKLE_PUB"
    fi
    SPARKLE_KEY="$WORK/sparkle_private.key"
    say "==> privaten Schlüssel exportieren (macOS fragt evtl. nach dem Schlüsselbund-Zugriff)"
    "$gk" --account "$SPARKLE_ACCOUNT" -x "$SPARKLE_KEY" >/dev/null || die "Export fehlgeschlagen."
    tools/sparkle_key_check.sh "$SPARKLE_KEY" "$SPARKLE_PUB" >/dev/null \
        || die "Privater und öffentlicher Sparkle-Schlüssel passen nicht zusammen."
    say "  Schlüsselpaar passt."
    say "  Sicherung: \"$gk --account $SPARKLE_ACCOUNT -x <datei>\" exportiert ihn jederzeit"
    say "  erneut - die Datei dann im Passwortmanager ablegen, nicht im Repo."
}

set_secret() {
    local name=$1
    gh secret set "$name" --repo "$REPO" ${SECRET_SCOPE[@]+"${SECRET_SCOPE[@]}"} >/dev/null
    say "  $name gesetzt"
}

step_set_secrets() {
    local where="Repository-Secrets"
    if [ "${#SECRET_SCOPE[@]}" -gt 0 ]; then where="Environment \"$ENV_NAME\""; fi
    say ""
    say "== Secrets setzen in $REPO ($where): ${SECRETS[*]}"
    say "   und die Repository-Variable SPARKLE_PUBLIC_ED_KEY"
    confirm "Jetzt setzen (vorhandene werden überschrieben)?" j || die "Abgebrochen, nichts gesetzt."
    base64 -i "$P12" | tr -d '\n' | set_secret MACOS_CERT_P12_BASE64
    printf '%s' "$P12_PASS" | set_secret MACOS_CERT_PASSWORD
    base64 -i "$P8" | tr -d '\n' | set_secret NOTARY_API_KEY_P8_BASE64
    printf '%s' "$KEY_ID" | set_secret NOTARY_API_KEY_ID
    printf '%s' "$ISSUER_ID" | set_secret NOTARY_API_ISSUER_ID
    tr -d '\n' < "$SPARKLE_KEY" | set_secret SPARKLE_ED_PRIVATE_KEY
    gh variable set SPARKLE_PUBLIC_ED_KEY --repo "$REPO" --body "$SPARKLE_PUB" >/dev/null
    say "  SPARKLE_PUBLIC_ED_KEY (Variable) gesetzt"
}

step_pages() {
    say ""
    say "== GitHub Pages (Homepage und Update-Feed, Branch gh-pages)"
    local src
    src=$(gh api "repos/$REPO/pages" --jq '"\(.source.branch) \(.source.path)"' 2>/dev/null || true)
    if [ "$src" = "gh-pages /" ]; then
        say "  Schon eingeschaltet (gh-pages, /)."
        return 0
    fi
    if [ -n "$src" ]; then
        say "warning: Pages nutzt derzeit \"$src\". Homepage und Update-Feed brauchen gh-pages, /."
        confirm "Auf gh-pages umstellen?" n || return 0
        gh api -X PUT "repos/$REPO/pages" -f 'source[branch]=gh-pages' -f 'source[path]=/' >/dev/null \
            && say "  Umgestellt." || say "warning: Umstellen fehlgeschlagen; bitte unter Settings → Pages."
        return 0
    fi
    if ! gh api "repos/$REPO/branches/gh-pages" >/dev/null 2>&1; then
        say "  Der Branch gh-pages existiert noch nicht. Er entsteht beim ersten Lauf des Workflows"
        say "  \"Website\" (Push nach website/ oder Actions → Website → Run workflow) bzw. beim"
        say "  ersten Release. Danach dieses Skript erneut starten oder Settings → Pages:"
        say "  \"Deploy from a branch\", gh-pages, / (root)."
        return 0
    fi
    confirm "Pages jetzt einschalten (gh-pages, /)?" j || return 0
    gh api -X POST "repos/$REPO/pages" -f 'source[branch]=gh-pages' -f 'source[path]=/' >/dev/null \
        && say "  Eingeschaltet: https://${REPO%%/*}.github.io/${REPO#*/}/" \
        || say "warning: Einschalten fehlgeschlagen; bitte unter Settings → Pages."
}

main() {
    case "${1:-}" in
        "") ;;
        -h|--help) sed -n '3,33p' "$0" | sed 's/^# \{0,1\}//'; return 0 ;;
        *) die "Unbekannte Option: $1" ;;
    esac
    [ "$(uname -s)" = "Darwin" ] || die "Nur auf macOS (Schlüsselbund)."
    [ -t 0 ] || die "Interaktives Skript: bitte im Terminal starten."
    cd "$(dirname "$0")/.."
    WORK=$(mktemp -d "${TMPDIR:-/tmp}/lumaflip-secrets.XXXXXX")
    chmod 700 "$WORK"
    trap cleanup EXIT
    trap 'exit 130' INT TERM

    say "Lumaflip: Secrets für die Release-Pipeline einrichten (docs/RELEASING.md)"
    step_target
    step_certificate
    step_api_key
    step_sparkle
    step_set_secrets
    unset P12_PASS
    step_pages

    cat <<MSG

Fertig. Nächste Schritte:
  - Probelauf (baut, signiert, notarisiert, veröffentlicht nichts):
      GitHub → Actions → Release → Run workflow (Branch main),
      oder: gh workflow run release.yml --repo $REPO --ref main
  - Release: Version in CMakeLists.txt und CHANGELOG.md setzen, committen, dann
      git tag v<version> && git push origin v<version>
  - Die .p8 bewahrst du offline auf (Apple bietet sie nur einmal zum Download an)
    oder löschst sie; bei Bedarf einfach einen neuen Key anlegen.
  - Temporäre Dateien werden jetzt gelöscht.
MSG
}

# Nur beim direkten Aufruf ausführen (beim Einbinden mit `source` nur die Funktionen).
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    main "$@"
fi
