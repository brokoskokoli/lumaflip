#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Richtet die Secrets für die Paketverwaltungen ein (interaktiv, im eigenen Terminal):
#
#   tools/setup_distribution_secrets.sh [homebrew|winget]   # ohne Argument: beide
#
#   HOMEBREW_TAP_TOKEN  Job "homebrew-tap" in release.yml: aktualisiert Casks/lumaflip.rb in
#                       brokoskokoli/homebrew-lumaflip nach jedem Release.
#                       Fine-grained PAT: Resource owner = dein Konto, Repository access =
#                       "Only select repositories" → brokoskokoli/homebrew-lumaflip, Permissions →
#                       Repository → Contents: "Read and write" (Metadata: read kommt automatisch).
#                       https://github.com/settings/personal-access-tokens/new
#   WINGET_TOKEN        Job "winget": Pull Request mit der neuen Version an microsoft/winget-pkgs
#                       (über einen Fork von winget-pkgs in deinem Konto).
#                       Classic PAT mit genau dem Scope "public_repo" (fine-grained PATs
#                       können keine Pull Requests an fremde Repositories öffnen).
#                       https://github.com/settings/tokens/new?scopes=public_repo&description=Lumaflip%20winget
#
# Beide Tokens laufen ab (Ablaufdatum beim Anlegen wählen, z. B. 1 Jahr): danach dieses Skript
# erneut ausführen. Fehlt ein Secret, überspringt der Release-Workflow den Job mit einem
# Hinweis - das Release selbst hängt nie davon ab.
#
# Die Tokens werden verdeckt eingelesen, nur über die Umgebung bzw. stdin an gh übergeben
# und nie ausgegeben. Kein `set -x`. Umgebungsvariablen: LF_RELEASE_ENV, LF_RELEASE_REPO.
# Details: docs/RELEASING.md (Abschnitte "Homebrew Cask" und "winget").
set -euo pipefail

ENV_NAME=${LF_RELEASE_ENV:-release}
REPO=${LF_RELEASE_REPO:-brokoskokoli/lumaflip}
TAP_REPO=brokoskokoli/homebrew-lumaflip

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
    say "== HOMEBREW_TAP_TOKEN (Homebrew-Tap $TAP_REPO) =="
    say "Fine-grained Token anlegen: https://github.com/settings/personal-access-tokens/new"
    say "  Token name: Lumaflip homebrew-tap, Expiration: z. B. 1 Jahr"
    say "  Repository access: Only select repositories → $TAP_REPO"
    say "  Permissions → Repository permissions → Contents: Read and write"
    if confirm "Token jetzt eintragen?" j; then
        read_token "HOMEBREW_TAP_TOKEN (github_pat_…)"
        [[ "$TOKEN" == github_pat_* ]] || confirm "Das ist kein fine-grained Token (github_pat_…). Trotzdem verwenden?" \
            || die "Abgebrochen."
        # Lesezugriff auf den Tap prüfen (Schreibrecht lässt sich ohne Schreiben nicht testen).
        GH_TOKEN=$TOKEN gh api "repos/$TAP_REPO" --jq .full_name >/dev/null 2>&1 \
            || die "Der Token sieht $TAP_REPO nicht - Repository access prüfen."
        set_secret HOMEBREW_TAP_TOKEN
    fi
fi

if [ "$what" != homebrew ]; then
    say ""
    say "== WINGET_TOKEN (Pull Requests an microsoft/winget-pkgs) =="
    say "Classic Token anlegen: https://github.com/settings/tokens/new?scopes=public_repo&description=Lumaflip%20winget"
    say "  Scope: nur public_repo, Expiration: z. B. 1 Jahr"
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
