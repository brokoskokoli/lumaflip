#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Richtet die Secrets für die Paketverwaltungen ein (interaktiv, im eigenen Terminal):
#
#   tools/setup_distribution_secrets.sh [homebrew|winget]   # ohne Argument: beide
#
#   HOMEBREW_TAP_DEPLOY_KEY  Job "homebrew-tap" in release.yml: aktualisiert Casks/lumaflip.rb in
#                       brokoskokoli/homebrew-tap nach jedem Release. Das Skript erzeugt einen
#                       SSH-Schlüssel, legt den öffentlichen Teil als Deploy-Key mit Schreibrecht
#                       im Tap an und setzt den privaten Teil als Secret. Läuft nicht ab, gilt nur
#                       für den Tap. Erneuter Lauf ersetzt den Schlüssel.
#   WINGET_TOKEN        Job "winget": Pull Request mit der neuen Version an microsoft/winget-pkgs
#                       (über einen Fork von winget-pkgs in deinem Konto).
#                       Classic PAT mit genau dem Scope "public_repo" (fine-grained PATs
#                       können keine Pull Requests an fremde Repositories öffnen).
#                       https://github.com/settings/tokens/new?scopes=public_repo&description=Lumaflip%20winget
#
# WINGET_TOKEN kann ohne Ablaufdatum angelegt werden („No expiration“); sonst nach Ablauf dieses
# Skript erneut ausführen. Fehlt ein Secret, überspringt der Release-Workflow den Job mit einem
# Hinweis - das Release selbst hängt nie davon ab.
#
# Die Tokens werden verdeckt eingelesen, nur über die Umgebung bzw. stdin an gh übergeben
# und nie ausgegeben. Kein `set -x`. Umgebungsvariablen: LF_RELEASE_ENV, LF_RELEASE_REPO.
# Details: docs/RELEASING.md (Abschnitte "Homebrew Cask" und "winget").
set -euo pipefail

ENV_NAME=${LF_RELEASE_ENV:-release}
REPO=${LF_RELEASE_REPO:-brokoskokoli/lumaflip}
TAP_REPO=brokoskokoli/homebrew-tap

say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
confirm() {
    local def=${2:-n} hint="[j/N]"
    if [ "$def" = "j" ]; then hint="[J/n]"; fi
    read -r -p "$1 $hint " REPLY
    REPLY=${REPLY:-$def}
    [[ "$REPLY" =~ ^[jJyY] ]]
}
read_token() { # read_token "Frage" → TOKEN
    TOKEN=
    while [ -z "$TOKEN" ]; do
        read -r -s -p "$1: " TOKEN; echo
    done
}
set_secret() { # set_secret NAME (Wert in TOKEN)
    printf '%s' "$TOKEN" | gh secret set "$1" --repo "$REPO" --env "$ENV_NAME" >/dev/null
    say "Secret $1 im Environment \"$ENV_NAME\" von $REPO gesetzt."
}

command -v gh >/dev/null || die "gh fehlt (brew install gh)"
gh auth status >/dev/null 2>&1 || die "gh ist nicht angemeldet (gh auth login)"
[ -t 0 ] || die "Bitte interaktiv im Terminal ausführen."
gh api "repos/$REPO/environments/$ENV_NAME" >/dev/null 2>&1 \
    || die "Environment \"$ENV_NAME\" in $REPO fehlt - zuerst tools/setup_release_secrets.sh ausführen."

what=${1:-all}
case "$what" in all|homebrew|winget) ;; *) die "Aufruf: $0 [homebrew|winget]" ;; esac

if [ "$what" != winget ]; then
    say ""
    say "== HOMEBREW_TAP_DEPLOY_KEY (Homebrew-Tap $TAP_REPO) =="
    say "Ein neuer SSH-Schlüssel wird erzeugt: der öffentliche Teil als Deploy-Key mit Schreibrecht"
    say "im Tap, der private Teil als Secret. Er läuft nicht ab und gilt nur für $TAP_REPO."
    if confirm "Deploy-Key jetzt einrichten?" j; then
        keydir=$(mktemp -d "${TMPDIR:-/tmp}/lumaflip-tapkey.XXXXXX")
        chmod 700 "$keydir"
        trap 'rm -rf -- "$keydir"' EXIT
        ssh-keygen -q -t ed25519 -N "" -C "lumaflip release workflow" -f "$keydir/key" \
            || die "ssh-keygen fehlgeschlagen."
        title="Lumaflip release workflow"
        # Alten Schlüssel gleichen Namens ersetzen (erneuter Lauf).
        for id in $(gh api "repos/$TAP_REPO/keys" --jq ".[] | select(.title == \"$title\") | .id" 2>/dev/null); do
            gh api -X DELETE "repos/$TAP_REPO/keys/$id" >/dev/null && say "  Alten Deploy-Key ($id) entfernt."
        done
        gh repo deploy-key add "$keydir/key.pub" --repo "$TAP_REPO" --title "$title" --allow-write >/dev/null \
            || die "Deploy-Key konnte nicht angelegt werden (Admin-Rechte auf $TAP_REPO nötig)."
        gh secret set HOMEBREW_TAP_DEPLOY_KEY --repo "$REPO" --env "$ENV_NAME" < "$keydir/key" >/dev/null \
            || die "Secret konnte nicht gesetzt werden."
        rm -rf -- "$keydir"
        say "Deploy-Key in $TAP_REPO angelegt, Secret HOMEBREW_TAP_DEPLOY_KEY im Environment \"$ENV_NAME\" gesetzt."
        if gh secret list --repo "$REPO" --env "$ENV_NAME" 2>/dev/null | grep -q '^HOMEBREW_TAP_TOKEN'; then
            gh secret delete HOMEBREW_TAP_TOKEN --repo "$REPO" --env "$ENV_NAME" >/dev/null \
                && say "Altes Secret HOMEBREW_TAP_TOKEN entfernt (wird nicht mehr gebraucht)."
        fi
    fi
fi

if [ "$what" != homebrew ]; then
    say ""
    say "== WINGET_TOKEN (Pull Requests an microsoft/winget-pkgs) =="
    say "Classic Token anlegen: https://github.com/settings/tokens/new?scopes=public_repo&description=Lumaflip%20winget"
    say "  Scope: nur public_repo, Expiration: \"No expiration\" (oder z. B. 1 Jahr)"
    say "  Einmalig github.com/microsoft/winget-pkgs forken (Fork → in dein Konto)."
    if confirm "Token jetzt eintragen?" j; then
        read_token "WINGET_TOKEN (ghp_…)"
        scopes=$(GH_TOKEN=$TOKEN gh api -i user 2>/dev/null | tr -d '\r' \
                 | sed -nE 's/^[Xx]-[Oo][Aa]uth-[Ss]copes: *(.*)$/\1/p')
        list=",${scopes// /},"
        [[ "$list" == *",public_repo,"* || "$list" == *",repo,"* ]] \
            || die "Der Token hat nicht den Scope public_repo (Scopes: ${scopes:-keine})."
        set_secret WINGET_TOKEN
        login=$(GH_TOKEN=$TOKEN gh api user --jq .login)
        if ! gh api "repos/$login/winget-pkgs" --jq .fork 2>/dev/null | grep -qx true; then
            say "Hinweis: $login hat noch keinen Fork von microsoft/winget-pkgs - komac legt ihn beim"
            say "ersten Lauf an, oder jetzt: gh repo fork microsoft/winget-pkgs --clone=false"
        fi
    fi
fi

say ""
say "Fertig. Die Jobs \"homebrew-tap\" und \"winget\" laufen beim nächsten Release-Tag; für ein"
say "bereits veröffentlichtes Release: Actions → Release → Lauf des Tags → beim Job"
say "\"Homebrew tap\" bzw. \"winget …\" auf \"Re-run this job\". Details: docs/RELEASING.md."
