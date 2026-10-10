<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Releases erstellen

Lumaflip wird direkt über GitHub vertrieben: signiert mit **Developer ID**, mit Hardened
Runtime, **notarisiert** und „gestapelt“, ohne Sandbox (SPEC 10.1). Jedes Release enthält
`Lumaflip-<version>.dmg`, `Lumaflip-<version>.zip` und je eine `.sha256`-Datei.

## Überblick

| Datei | Aufgabe |
|---|---|
| `tools/package.sh` | Release-Build (`build/release`), Qt einbetten, Lizenztexte beilegen, Homebrew-Verweise prüfen, signieren, DMG + ZIP + SHA-256 nach `dist/` |
| `tools/macos_deploy.py` | Wird von `package.sh` aufgerufen: kopiert Qt-Frameworks, -Plugins, Übersetzungen und alle abhängigen Bibliotheken ins Bundle, schreibt `qt.conf` und `Contents/Resources/licenses/` (inkl. `index.json` für das Über-Fenster); `--verify` prüft mit `otool`, dass nichts mehr auf `/opt/homebrew` zeigt |
| `tools/notarize.sh` | Lädt App oder DMG zum Apple-Notardienst hoch (`notarytool submit --wait`), heftet das Ticket an (`stapler staple`); `--dry-run` zeigt nur die Befehle |
| `.github/workflows/release.yml` | Läuft bei Tag `v*` im Hauptrepo: Versionsabgleich → Prüfungen (`checks.yml`) → Paket → Signatur → Notarisierung → GitHub Release |
| `.github/workflows/checks.yml` | Wiederverwendbarer Workflow mit allen Pflichtprüfungen (SPEC 15.1); `ci.yml` (Push/PR) und `release.yml` rufen ihn unverändert auf |
| `packaging/macos/Lumaflip.entitlements` | Einzige Berechtigung: `com.apple.security.automation.apple-events` (Opt-in F-SORT-04). Keine Sandbox |
| `packaging/macos/Info.plist.in` | Dateitypen, TCC-Texte, Symbol, Copyright |
| `packaging/macos/icon/` | `lumaflip.svg` (Quelle) und `Lumaflip.icns` (erzeugt mit `tools/make_icon.sh`, braucht `rsvg-convert`) |
| `packaging/licenses/` | Lizenztexte, die Homebrew nicht mitliefert (LGPL-3.0 und Hinweis für Qt, LGPL-2.1 für libintl) |
| `packaging/homebrew/lumaflip.rb` | Vorlage für den Homebrew Cask |

## Lokal bauen

```sh
tools/package.sh                       # ad-hoc signiert, nur für diesen Mac
LF_SIGN_IDENTITY="Developer ID Application: Name (TEAMID)" tools/package.sh
LF_SIGN_IDENTITY="…" tools/package.sh --notarize-dry-run   # zeigt die Notarisierungsbefehle
```

- **Ad-hoc** (Standard, `LF_SIGN_IDENTITY` nicht gesetzt): ohne Hardened Runtime, denn deren
  Bibliotheksprüfung verlangt dieselbe Team-ID für alle Bibliotheken, die ad-hoc-Signaturen nicht
  haben. Läuft auf dem eigenen Mac; Gatekeeper weist es auf anderen Macs ab.
- **Developer ID**: Hardened Runtime (`--options runtime`), Zeitstempel (`--timestamp`, braucht
  Netz), Signatur von innen nach außen (Bibliotheken → Plugins → Frameworks → App, kein `--deep`).
  Vorhandene Identitäten: `security find-identity -v -p codesigning`.
- `spctl -a -vvv -t exec` meldet vor der Notarisierung `rejected (Unnotarized Developer ID)` – erwartet.
- Nur arm64: Homebrew-Qt gibt es nur für die Architektur des Rechners. Universal-Builds bräuchten ein
  Universal-Qt (z. B. Qt-Online-Installer); `tools/macos_deploy.py` funktioniert damit unverändert.
- Die Arbeitskopie liegt in einem neuen Temp-Verzeichnis und wird am Ende (auch bei Fehlern)
  gelöscht; `--keep-work` behält sie zum Nachsehen (Pfad steht am Ende der Ausgabe).
  `dist/` und `build/release/` sind in `.gitignore`.
- `package.sh` baut immer `CMAKE_BUILD_TYPE=Release` ohne Sanitizer und prüft anhand von
  `build/release/compile_commands.json`, dass alle eigenen Quellen mit `-O2`/`-O3 -DNDEBUG`
  übersetzt wurden (schützt `--skip-build` vor einem Debug-Cache).
- `CFBundleDevelopmentRegion` ist `de` (Qt würde sonst seine Quellsprache `en` eintragen:
  `QT_I18N_SOURCE_LANGUAGE` in `src/CMakeLists.txt`; `package.sh` erzwingt `de` zusätzlich).

## Notarisieren (lokal)

Benötigt `notarytool` und `stapler` – beide sind in Xcode 13+ **und** in den Command Line Tools enthalten
(`xcrun --find notarytool`). Xcode selbst ist nicht nötig.

Zugangsdaten, eine der beiden Varianten:

1. **Schlüsselbund-Profil** (empfohlen lokal), einmalig:
   ```sh
   xcrun notarytool store-credentials lumaflip \
     --apple-id <apple-id> --team-id <TEAMID>     # fragt nach einem app-spezifischen Passwort
   LF_NOTARY_PROFILE=lumaflip LF_SIGN_IDENTITY="Developer ID Application: …" tools/package.sh --notarize
   ```
2. **App-Store-Connect-API-Schlüssel** (so arbeitet die CI):
   `LF_NOTARY_KEY_ID`, `LF_NOTARY_ISSUER`, `LF_NOTARY_KEY_PATH` (Pfad zur `.p8`-Datei).

Ablauf von `package.sh --notarize`: App signieren → App (als ZIP) notarisieren → Ticket an die App
heften → ZIP und DMG aus der gehefteten App bauen → DMG signieren → DMG notarisieren → Ticket an die
DMG heften → SHA-256. So hat sowohl die App im ZIP als auch die DMG ein angeheftetes Ticket und
startet auch ohne Netz ohne Gatekeeper-Warnung.

Fehler: `notarize.sh` zeigt bei einem anderen Status als `Accepted` (z. B. `Invalid`), bei
Zeitüberschreitung und bei Upload-Fehlern die Ausgabe von `notarytool`, den Exit-Code und – sofern
eine Submission-ID bekannt ist – das Protokoll (`xcrun notarytool log <id>`), dann bricht es ab.
Sein Temp-Verzeichnis (ZIP der App) räumt es immer auf. Testen ohne Netz: `--dry-run`.

## Release über GitHub Actions

1. Version in `CMakeLists.txt` (`project(Lumaflip VERSION x.y.z …)`) erhöhen, committen.
2. Tag setzen und pushen: `git tag vX.Y.Z && git push origin vX.Y.Z`.
   Der Workflow bricht ab, wenn Tag und Version nicht übereinstimmen.
3. Der Workflow `release.yml` läuft nur für Tags im Repo `brokoskokoli/lumaflip`, nie für Forks
   oder Pull Requests. Er nutzt die GitHub-Umgebung (Environment) **`release`** – dort lassen sich
   erforderliche Freigaben einstellen (Settings → Environments → release → Required reviewers).

### Benötigte Secrets

Unter Settings → Secrets and variables → Actions (oder als Secrets der Umgebung `release`):

| Secret | Inhalt | Woher |
|---|---|---|
| `MACOS_CERT_P12_BASE64` | Zertifikat **Developer ID Application** samt privatem Schlüssel als `.p12`, Base64-kodiert: `base64 -i DeveloperID.p12 \| pbcopy` | Schlüsselbundverwaltung → Zertifikat + Schlüssel auswählen → Exportieren (.p12) |
| `MACOS_CERT_PASSWORD` | Passwort der `.p12`-Datei | beim Export vergeben |
| `NOTARY_API_KEY_ID` | Key-ID des API-Schlüssels (z. B. `ABC123DEFG`) | App Store Connect → Benutzer und Zugriff → Integrationen → App Store Connect API |
| `NOTARY_API_ISSUER_ID` | Issuer-ID (UUID) | dieselbe Seite, oben |
| `NOTARY_API_KEY_P8_BASE64` | Inhalt der `AuthKey_<id>.p8`, Base64-kodiert: `base64 -i AuthKey_ABC123DEFG.p8 \| pbcopy` | wird nur einmal zum Download angeboten; Rolle „Developer“ genügt |

Der Workflow legt einen temporären Schlüsselbund an (zufälliges Passwort), importiert das Zertifikat,
ermittelt die Identität `Developer ID Application: …` daraus und löscht Schlüsselbund und Schlüsseldatei
am Ende (auch bei Fehlern). `GITHUB_TOKEN` reicht für `gh release create`.

Ablauf und Absicherung von `release.yml`:

- **Gleiche Prüfungen wie CI:** Der Job `release` hängt von `checks` ab, der denselben
  wiederverwendbaren Workflow `checks.yml` wie die CI für den getaggten Commit ausführt
  (Benchmark-Toleranz 2,0, `webp`/`jpeg-xl` für den Formatkorpus, RAW-Beispiele mit Cache und
  `LF_REQUIRE_RAW=1`, clang-tidy, Sanitizer, Linux-Kern, REUSE). `checks` bekommt keine Secrets.
- **Actions per Commit-SHA gepinnt** (Kommentar = Tag). Aktualisieren:
  `gh api repos/<owner>/<repo>/git/ref/tags/<tag>` (bei annotierten Tags zusätzlich
  `gh api repos/<owner>/<repo>/git/tags/<sha>` → `object.sha`).
- **Passwörter nie auf der Kommandozeile:** `security` nimmt Passwörter nur als Argument (`-P`, in
  `ps` sichtbar) oder per GUI. Der Workflow füttert deshalb `security -i` (interaktiver Modus) über
  stdin mit `printf` (Shell-Builtin) – das gilt für das `.p12`-Passwort und das Schlüsselbund-Passwort.
  Fertige Actions wie `apple-actions/import-codesign-certs` übergeben `-P` als Argument und bringen
  daher keinen Vorteil. Das `.p12`-Passwort darf keinen Zeilenumbruch enthalten.
- `.p12` und `.p8` werden mit `umask 077` in ein eigenes Verzeichnis (`0700`) unter `$RUNNER_TEMP`
  geschrieben und im letzten Schritt (`if: always()`) gelöscht; kein `set -x`, `persist-credentials: false`.

## Homebrew Cask

Zunächst im eigenen Tap (`brokoskokoli/homebrew-tap`), später Antrag für `homebrew/cask` (SPEC 10.1):

1. `packaging/homebrew/lumaflip.rb` kopieren nach `Casks/lumaflip.rb` im Tap.
2. `version` setzen, `@SHA256_OF_DMG@` durch den Wert aus `Lumaflip-<version>.dmg.sha256` ersetzen.
3. Prüfen: `brew audit --cask --new lumaflip` und `brew install --cask brokoskokoli/tap/lumaflip`.

Installation für Nutzer danach: `brew install --cask brokoskokoli/tap/lumaflip`.

## Nach dem Release prüfen

Die nicht automatisierbaren Prüfungen (Gatekeeper beim echten Download, Offline-Start, TCC-Texte,
Lizenzfenster) stehen in [MANUAL_CHECKS.md](MANUAL_CHECKS.md) im Abschnitt „Auslieferung“.

## Lizenzen im Paket (SPEC 11)

- Qt bleibt dynamisch gelinkt (Frameworks unter `Contents/Frameworks`) und ist damit austauschbar (LGPLv3).
- Mitgeliefert werden nur Qt-Plugins, die Lumaflip braucht (`PLUGINS` in `tools/macos_deploy.py`):
  Cocoa-Plattform, macOS-Stil, SVG-Symbole und die Bildformate GIF, ICNS, ICO, JPEG, HEIF (macOS), SVG,
  TGA, TIFF, WBMP, WebP. Bewusst weggelassen: PDF (zieht Qt PDF nach), MNG (libmng + Little CMS),
  JPEG 2000 (JasPer; dekodiert ImageIO), virtuelle Tastatur (zieht Qt Quick nach).
- Für jede eingebettete Bibliothek ermittelt `macos_deploy.py` das Homebrew-Paket, kopiert dessen
  Lizenzdateien nach `Contents/Resources/licenses/<paket>/` und schreibt Version, SPDX-Lizenz
  (`brew info --json`) und die zugehörigen Binärdateien in `index.json`. Findet es für ein Paket keinen
  Lizenztext, bricht das Paketieren ab.
- Little CMS, resvg und KImageFormats werden derzeit **nicht** verwendet und deshalb nicht aufgeführt.
