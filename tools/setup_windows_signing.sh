#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Richtet einmalig die Windows-Codesignatur der Release-Pipeline ein (interaktiv):
#
#   tools/setup_windows_signing.sh
#
# Voraussetzung: ein vorhandenes Azure-Artifact-Signing-Konto (früher "Trusted Signing")
# mit abgeschlossener Identitätsprüfung und einem Zertifikatprofil vom Typ "Public Trust"
# (z. B. das eines anderen Projekts - ein Profil kann mehrere Projekte signieren).
#
# Ablauf:
#   1. Repository und GitHub-Environment "release" (wird bei Bedarf angelegt; Schutzregeln
#      richtet tools/setup_release_secrets.sh ein).
#   2. Azure: Abonnement wählen, Signaturkonto und Zertifikatprofil aus der Liste wählen
#      (az-Erweiterung "artifact-signing", ersatzweise das ältere "trustedsigning").
#   3. Entra-App-Registrierung "lumaflip-github-release" anlegen oder wiederverwenden, samt
#      Dienstprinzipal und föderierter Anmeldeinformation für GitHub Actions:
#        Aussteller  https://token.actions.githubusercontent.com
#        Subjekt     repo:<owner>/<repo>:environment:release
#        Zielgruppe  api://AzureADTokenExchange
#      Kein Client-Secret: GitHub meldet sich per OIDC an (azure/login).
#   4. Rolle "Artifact Signing Certificate Profile Signer" (ältere Tenants: "Trusted Signing
#      Certificate Profile Signer") für den Dienstprinzipal, nur auf das gewählte Profil.
#   5. GitHub: Secrets AZURE_CLIENT_ID, AZURE_TENANT_ID, AZURE_SUBSCRIPTION_ID im Environment
#      "release" und die Repository-Variablen ARTIFACT_SIGNING_ENDPOINT,
#      ARTIFACT_SIGNING_ACCOUNT, ARTIFACT_SIGNING_CERT_PROFILE.
#
# Jeder Schritt prüft zuerst, was schon da ist (erneuter Aufruf ändert nichts Doppeltes).
# Werte der Secrets werden nie ausgegeben. Details und der Weg über das Azure-Portal:
# docs/RELEASING.md, Abschnitt "Windows: Signatur und Updates".
#
# Voraussetzungen: az (Azure CLI, `az login` mit Rechten zum Anlegen von App-Registrierungen
# und Rollenzuweisungen am Profil: Owner oder User Access Administrator), gh (angemeldet,
# Admin am Repo). Läuft mit bash 3.2 (macOS). Umgebungsvariablen: LF_RELEASE_REPO,
# LF_RELEASE_ENV, LF_SIGNING_APP_NAME.
set -euo pipefail

REPO=${LF_RELEASE_REPO:-}
ENV_NAME=${LF_RELEASE_ENV:-release}
APP_NAME=${LF_SIGNING_APP_NAME:-lumaflip-github-release}
OIDC_ISSUER="https://token.actions.githubusercontent.com"
OIDC_AUDIENCE="api://AzureADTokenExchange"
EXT=""  # az command group of the signing extension: artifact-signing or trustedsigning
WORK=""

say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
cleanup() { if [ -n "$WORK" ] && [ -d "$WORK" ]; then rm -rf "$WORK"; fi; }

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
    case "$REPLY" in [jJyY]*) return 0 ;; *) return 1 ;; esac
}

# choose "Überschrift" <datei> → REPLY = gewählte Zeile (Spalten durch Tabs getrennt;
# angezeigt werden nur die erste und, in Klammern, die dritte Spalte - IDs nie).
choose() {
    local title=$1 file=$2 n i line
    n=$(grep -c . "$file" || true)
    [ "$n" -gt 0 ] || return 1
    if [ "$n" -eq 1 ]; then
        REPLY=$(sed -n 1p "$file")
        say "$title: $(printf '%s' "$REPLY" | cut -f1)"
        return 0
    fi
    say "$title:"
    i=0
    while IFS= read -r line; do
        i=$((i + 1))
        say "  $i) $(printf '%s' "$line" | awk -F '\t' '{ s = $1; if ($3 != "") s = s " (" $3 ")"; print s }')"
    done < "$file"
    while true; do
        ask "Nummer" 1
        case "$REPLY" in
            '' | *[!0-9]*) say "Bitte eine Zahl von 1 bis $n." ;;
            *) if [ "$REPLY" -ge 1 ] && [ "$REPLY" -le "$n" ]; then
                   REPLY=$(sed -n "${REPLY}p" "$file"); return 0
               fi
               say "Bitte eine Zahl von 1 bis $n." ;;
        esac
    done
}

# --- Schritte ------------------------------------------------------------------
step_tools() {
    command -v az >/dev/null || die "az (Azure CLI) fehlt: brew install azure-cli"
    command -v gh >/dev/null || die "gh (GitHub CLI) fehlt: brew install gh"
    gh auth status >/dev/null 2>&1 || die "gh ist nicht angemeldet: gh auth login"
    if ! az account show >/dev/null 2>&1; then
        say "Azure CLI ist nicht angemeldet - starte az login (Browser) ..."
        az login >/dev/null || die "az login fehlgeschlagen."
    fi
}

step_repo() {
    if [ -z "$REPO" ]; then
        REPO=$(gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null || echo brokoskokoli/lumaflip)
    fi
    ask "Repository" "$REPO"; REPO=$REPLY
    case "$REPO" in */*) ;; *) die "Repository bitte als <owner>/<name> angeben." ;; esac
    if ! gh api "repos/$REPO/environments/$ENV_NAME" >/dev/null 2>&1; then
        confirm "GitHub-Environment \"$ENV_NAME\" existiert nicht. Anlegen?" j || die "Abgebrochen."
        gh api -X PUT "repos/$REPO/environments/$ENV_NAME" >/dev/null
        say "  Environment angelegt (Schutzregeln: tools/setup_release_secrets.sh)."
    fi
}

step_subscription() {
    say ""
    say "== Azure-Abonnement mit dem Signaturkonto"
    az account list --query "[].[name, id, isDefault && 'aktuell' || '', tenantId]" -o tsv > "$WORK/subs"
    choose "Abonnement" "$WORK/subs" || die "Keine Azure-Abonnements sichtbar (az login mit dem richtigen Konto?)."
    SUB_NAME=$(printf '%s' "$REPLY" | cut -f1)
    SUB_ID=$(printf '%s' "$REPLY" | cut -f2)
    TENANT_ID=$(printf '%s' "$REPLY" | cut -f4)
    az account set --subscription "$SUB_ID" >/dev/null
    say "  Abonnement: $SUB_NAME"
}

step_extension() {
    local e
    for e in artifact-signing trustedsigning; do
        if az extension show --name "$e" >/dev/null 2>&1; then EXT=$e; return 0; fi
    done
    say "  Installiere die az-Erweiterung \"artifact-signing\" ..."
    if az extension add --name artifact-signing --upgrade --yes >/dev/null 2>&1; then
        EXT=artifact-signing
    elif az extension add --name trustedsigning --upgrade --yes >/dev/null 2>&1; then
        EXT=trustedsigning
    else
        die "Weder die az-Erweiterung \"artifact-signing\" noch \"trustedsigning\" ließ sich installieren."
    fi
}

step_account() {
    say ""
    say "== Signaturkonto und Zertifikatprofil"
    step_extension
    az "$EXT" list --query "[].[name, id, accountUri || properties.accountUri, location]" -o tsv \
        > "$WORK/accounts" 2>/dev/null || true
    choose "Signaturkonto" "$WORK/accounts" \
        || die "Kein Artifact-Signing-Konto im Abonnement \"$SUB_NAME\" (anderes Abonnement wählen oder im Portal anlegen)."
    ACCOUNT=$(printf '%s' "$REPLY" | cut -f1)
    ACCOUNT_ID=$(printf '%s' "$REPLY" | cut -f2)
    ENDPOINT=$(printf '%s' "$REPLY" | cut -f3)
    RG=$(printf '%s' "$ACCOUNT_ID" | sed -nE 's#.*/[rR]esource[gG]roups/([^/]+)/.*#\1#p')
    [ -n "$RG" ] || die "Ressourcengruppe nicht aus $ACCOUNT_ID ablesbar."
    if [ -z "$ENDPOINT" ]; then
        ENDPOINT=$(az resource show --ids "$ACCOUNT_ID" --query properties.accountUri -o tsv 2>/dev/null || true)
    fi
    case "$ENDPOINT" in
        https://*.codesigning.azure.net*) ;;
        *) say "  Endpunkt des Kontos nicht lesbar. Er steht im Portal unter Übersicht → \"Account URI\"."
           ask "Endpunkt (z. B. https://weu.codesigning.azure.net/)"; ENDPOINT=$REPLY ;;
    esac
    case "$ENDPOINT" in */) ;; *) ENDPOINT="$ENDPOINT/" ;; esac
    printf '%s' "$ENDPOINT" | grep -Eq '^https://[a-z0-9-]+\.codesigning\.azure\.net/$' \
        || die "Ungültiger Endpunkt: $ENDPOINT"

    az "$EXT" certificate-profile list --account-name "$ACCOUNT" --resource-group "$RG" \
        --query "[].[name, id, profileType || properties.profileType, status || properties.status]" -o tsv \
        > "$WORK/profiles" 2>/dev/null || true
    choose "Zertifikatprofil" "$WORK/profiles" || die "Das Konto \"$ACCOUNT\" hat kein Zertifikatprofil."
    PROFILE=$(printf '%s' "$REPLY" | cut -f1)
    PROFILE_ID=$(printf '%s' "$REPLY" | cut -f2)
    local type status
    type=$(printf '%s' "$REPLY" | cut -f3)
    status=$(printf '%s' "$REPLY" | cut -f4)
    if [ "$type" != "PublicTrust" ]; then
        say "warning: Profiltyp \"$type\": Für die Verteilung an Nutzer braucht es \"PublicTrust\""
        say "         (sonst warnt Windows bei jedem Start)."
        confirm "Trotzdem dieses Profil nehmen?" n || die "Abgebrochen."
    fi
    if [ -n "$status" ] && [ "$status" != "Active" ]; then
        say "warning: Profilstatus \"$status\" (erwartet: Active)."
    fi
    say "  Konto $ACCOUNT ($RG), Profil $PROFILE, Endpunkt $ENDPOINT"
}

step_app() {
    say ""
    say "== Entra-App-Registrierung \"$APP_NAME\" (Anmeldung von GitHub Actions per OIDC)"
    APP_ID=$(az ad app list --display-name "$APP_NAME" --query "[0].appId" -o tsv 2>/dev/null || true)
    if [ -n "$APP_ID" ]; then
        say "  Vorhanden - wird wiederverwendet."
    else
        confirm "App-Registrierung \"$APP_NAME\" anlegen?" j || die "Abgebrochen."
        APP_ID=$(az ad app create --display-name "$APP_NAME" --sign-in-audience AzureADMyOrg \
            --query appId -o tsv) || die "App-Registrierung konnte nicht angelegt werden (Rechte im Tenant?)."
        say "  Angelegt."
    fi
    SP_ID=$(az ad sp show --id "$APP_ID" --query id -o tsv 2>/dev/null || true)
    if [ -z "$SP_ID" ]; then
        SP_ID=$(az ad sp create --id "$APP_ID" --query id -o tsv) || die "Dienstprinzipal nicht anlegbar."
        say "  Dienstprinzipal angelegt."
    fi

    local subject="repo:$REPO:environment:$ENV_NAME" existing
    existing=$(az ad app federated-credential list --id "$APP_ID" \
        --query "[?subject=='$subject' && issuer=='$OIDC_ISSUER'].name" -o tsv 2>/dev/null || true)
    if [ -n "$existing" ]; then
        say "  Föderierte Anmeldeinformation für $subject vorhanden."
    else
        local name
        name="github-$(printf '%s' "$REPO" | tr '/A-Z' '-a-z' | tr -cd 'a-z0-9-')-$ENV_NAME"
        cat > "$WORK/fic.json" <<JSON
{
  "name": "$name",
  "issuer": "$OIDC_ISSUER",
  "subject": "$subject",
  "audiences": ["$OIDC_AUDIENCE"],
  "description": "GitHub Actions, $REPO, environment $ENV_NAME (release.yml, Windows-Signatur)"
}
JSON
        az ad app federated-credential create --id "$APP_ID" --parameters "@$WORK/fic.json" >/dev/null \
            || die "Föderierte Anmeldeinformation konnte nicht angelegt werden."
        say "  Föderierte Anmeldeinformation angelegt: $subject"
    fi
}

step_role() {
    say ""
    say "== Rolle \"Certificate Profile Signer\" auf das Profil $PROFILE"
    local role role_name
    # Der Name hängt vom Stand des Tenants ab (Umbenennung Trusted → Artifact Signing).
    az role definition list --query "[?contains(roleName, 'Certificate Profile Signer')].[name, roleName]" \
        -o tsv > "$WORK/roles" 2>/dev/null || true
    role=$(grep -i 'Artifact Signing' "$WORK/roles" | head -n1 || true)
    [ -n "$role" ] || role=$(head -n1 "$WORK/roles")
    [ -n "$role" ] || die "Rolle \"Artifact/Trusted Signing Certificate Profile Signer\" nicht gefunden."
    role_name=$(printf '%s' "$role" | cut -f2)
    role=$(printf '%s' "$role" | cut -f1)
    if [ -n "$(az role assignment list --assignee "$SP_ID" --scope "$PROFILE_ID" \
            --query "[?roleDefinitionId!=null && ends_with(roleDefinitionId, '$role')].id" -o tsv 2>/dev/null)" ]; then
        say "  \"$role_name\" ist schon zugewiesen."
        return 0
    fi
    # Ein frisch angelegter Dienstprinzipal ist evtl. noch nicht überall repliziert: bis zu 6 Versuche.
    local attempt
    for attempt in 1 2 3 4 5 6; do
        if az role assignment create --assignee-object-id "$SP_ID" --assignee-principal-type ServicePrincipal \
                --role "$role" --scope "$PROFILE_ID" >/dev/null 2>&1; then
            say "  \"$role_name\" zugewiesen (nur dieses Profil)."
            return 0
        fi
        say "  Zuweisung noch nicht möglich (Versuch $attempt), warte 10 s ..."
        sleep 10
    done
    die "Rollenzuweisung fehlgeschlagen (Owner oder User Access Administrator am Profil nötig)."
}

step_github() {
    say ""
    say "== GitHub: Secrets im Environment \"$ENV_NAME\" und Repository-Variablen ($REPO)"
    say "   Secrets: AZURE_CLIENT_ID, AZURE_TENANT_ID, AZURE_SUBSCRIPTION_ID"
    say "   Variablen: ARTIFACT_SIGNING_ENDPOINT=$ENDPOINT"
    say "              ARTIFACT_SIGNING_ACCOUNT=$ACCOUNT"
    say "              ARTIFACT_SIGNING_CERT_PROFILE=$PROFILE"
    confirm "Jetzt setzen (vorhandene werden überschrieben)?" j || die "Abgebrochen, nichts gesetzt."
    printf '%s' "$APP_ID" | gh secret set AZURE_CLIENT_ID --repo "$REPO" --env "$ENV_NAME" >/dev/null
    printf '%s' "$TENANT_ID" | gh secret set AZURE_TENANT_ID --repo "$REPO" --env "$ENV_NAME" >/dev/null
    printf '%s' "$SUB_ID" | gh secret set AZURE_SUBSCRIPTION_ID --repo "$REPO" --env "$ENV_NAME" >/dev/null
    say "  Secrets gesetzt"
    gh variable set ARTIFACT_SIGNING_ENDPOINT --repo "$REPO" --body "$ENDPOINT" >/dev/null
    gh variable set ARTIFACT_SIGNING_ACCOUNT --repo "$REPO" --body "$ACCOUNT" >/dev/null
    gh variable set ARTIFACT_SIGNING_CERT_PROFILE --repo "$REPO" --body "$PROFILE" >/dev/null
    say "  Variablen gesetzt"
    if ! gh variable get SPARKLE_PUBLIC_ED_KEY --repo "$REPO" >/dev/null 2>&1; then
        say "warning: Die Variable SPARKLE_PUBLIC_ED_KEY fehlt noch (Updates): tools/setup_release_secrets.sh"
    fi
}

main() {
    case "${1:-}" in
        "") ;;
        -h|--help) sed -n '3,36p' "$0" | sed 's/^# \{0,1\}//'; return 0 ;;
        *) die "Unbekannte Option: $1" ;;
    esac
    [ -t 0 ] || die "Interaktives Skript: bitte im Terminal starten."
    cd "$(dirname "$0")/.."
    WORK=$(mktemp -d "${TMPDIR:-/tmp}/lumaflip-signing.XXXXXX")
    trap cleanup EXIT
    trap 'exit 130' INT TERM

    say "Lumaflip: Windows-Codesignatur (Azure Artifact Signing) für release.yml einrichten"
    step_tools
    step_repo
    step_subscription
    step_account
    step_app
    step_role
    step_github

    cat <<MSG

Fertig. Nächste Schritte:
  - Probelauf (baut und signiert, veröffentlicht nichts):
      gh workflow run release.yml --repo $REPO --ref main
    Im Job "Windows build, sign" müssen "Azure login" und beide Signaturschritte grün sein;
    "signtool verify /pa" prüft Signatur und Zeitstempel.
  - Neue Rollenzuweisungen brauchen in Azure bis zu einige Minuten, bis sie greifen.
  - Die föderierte Anmeldeinformation gilt nur für Jobs im Environment "$ENV_NAME" von $REPO.
MSG
}

# Nur beim direkten Aufruf ausführen (beim Einbinden mit `source` nur die Funktionen).
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    main "$@"
fi
