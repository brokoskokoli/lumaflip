<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Releases erstellen

Lumaflip wird direkt über GitHub vertrieben: signiert mit **Developer ID**, mit Hardened
Runtime, **notarisiert** und „gestapelt“, ohne Sandbox (SPEC 10.1). Jedes Release enthält
`Lumaflip-<version>.dmg`, `Lumaflip-<version>.zip` und je eine `.sha256`-Datei, dazu für Windows
`Lumaflip-<version>-windows-x64-setup.exe` und `Lumaflip-<version>-windows-x64.zip` (mit **Azure Artifact
Signing** signiert, siehe [Windows: Signatur und Updates](#windows-signatur-und-updates)) samt `.sha256`.

## Überblick

| Datei | Aufgabe |
|---|---|
| `tools/package.sh` | Release-Build (`build/release`), Qt einbetten, Lizenztexte beilegen, Homebrew-Verweise prüfen, signieren, DMG + ZIP + SHA-256 nach `dist/` |
| `tools/macos_deploy.py` | Wird von `package.sh` aufgerufen: kopiert Qt-Frameworks, -Plugins, Übersetzungen und alle abhängigen Bibliotheken ins Bundle, schreibt `qt.conf` und `Contents/Resources/licenses/` (inkl. `index.json` für das Über-Fenster); `--verify` prüft mit `otool`, dass nichts mehr auf `/opt/homebrew` zeigt |
| `tools/notarize.sh` | Lädt App oder DMG zum Apple-Notardienst hoch (`notarytool submit --wait`), heftet das Ticket an (`stapler staple`); `--dry-run` zeigt nur die Befehle |
| `tools/fetch_sparkle.sh` | Lädt Sparkle 2 (MIT) in der festgelegten Version, prüft die SHA-256 und entpackt nach `third_party/Sparkle/` (in `.gitignore`): Framework, `bin/generate_appcast`, `bin/sign_update`, `bin/generate_keys`, `LICENSE` |
| `tools/sparkle_key_check.sh` | Prüft, ob ein exportierter privater Sparkle-Schlüssel zum öffentlichen Schlüssel der App passt: Der öffentliche Schlüssel wird immer aus dem privaten **abgeleitet** (auch beim alten 96-Byte-Format, dessen gespeichertem Anhang nicht vertraut wird). `--print-public` gibt ihn aus, `--self-test` prüft das Skript mit Wegwerf-Schlüsseln aus OpenSSL (läuft in `tools/check.sh`) |
| `tools/appcast_verify.py` | Vergleicht neuen und alten Appcast vor dem Veröffentlichen: alle alten Einträge mit unveränderter Download-URL vorhanden, genau ein neuer Eintrag mit EdDSA-Signatur und passender Länge; die Feeds bleiben getrennt (`--os windows`: nur Windows-Einträge mit Installer-Argumenten, ohne: kein Windows-Eintrag im macOS-Feed). Unit-Tests: `tests/tools/test_appcast.py` (ctest `tools_appcast`) |
| `.github/workflows/release.yml` | Läuft bei Tag `v*` im Hauptrepo: Versionsabgleich → Prüfungen (`checks.yml`) + Schlüsselprüfung → Paket/Signatur/Notarisierung (macOS) und Build/Signatur (Windows) → Release als Entwurf → Appcasts → Release veröffentlichen |
| `.github/actions/windows-build/` | Gemeinsamer Windows-Build (MSVC, Qt, vcpkg, WinSparkle) für den CI-Job „Windows app“ und den Release-Job `build-windows` |
| `tools/fetch_winsparkle.py` | Lädt WinSparkle (MIT) in der festgelegten Version, prüft die SHA-256, entpackt `x64/WinSparkle.dll`, Header und `COPYING` nach `third_party/WinSparkle/` |
| `tools/windows_package.py` | ZIP, Inno-Setup-Installer und `.sha256` aus dem bereitgestellten Ordner (`tools/windows_deploy.py`) |
| `tools/windows_installer_check.py` | Installiert den Installer still, aktualisiert ihn wie WinSparkle (Appcast-Argumente, laufende App, Neustart) und deinstalliert ihn (CI und Release) |
| `tools/appcast_windows.py` | Schreibt `appcast-windows.xml` (WinSparkle): alte Einträge bleiben, neuer Eintrag mit EdDSA-Signatur, `sparkle:os="windows"`, Installer-Argumenten und Release-Notizen |
| `tools/setup_windows_signing.sh` | Richtet die Windows-Signatur ein: Entra-App mit föderierter Anmeldeinformation (OIDC), Rolle am Zertifikatprofil, GitHub-Secrets und -Variablen |
| `.github/workflows/checks.yml` | Wiederverwendbarer Workflow mit allen Pflichtprüfungen (SPEC 15.1); `ci.yml` (Push/PR) und `release.yml` rufen ihn unverändert auf |
| `packaging/macos/Lumaflip.entitlements` | Einzige Berechtigung: `com.apple.security.automation.apple-events` (Opt-in F-SORT-04). Keine Sandbox |
| `packaging/macos/Info.plist.in` | Dateitypen, TCC-Texte, Symbol, Copyright |
| `packaging/macos/icon/` | `lumaflip.svg` (Quelle des App-Symbols „Sun Chevron“ für alle Plattformen) und `Lumaflip.icns` (erzeugt mit `tools/make_icon.sh`, braucht `rsvg-convert`). Windows rendert die `.ico` beim Bauen daraus (`lf_make_ico`), Linux die hicolor-PNGs (`rsvg-convert`) |
| `packaging/brand/` | `lumaflip-mono.svg` (einfarbige Glyphe des Symbols) und `social-preview.svg` (Vorlage für das Linkvorschaubild). `tools/make_website_images.sh` (braucht `resvg`) erzeugt daraus und aus dem Symbol `website/favicon.png`, `website/apple-touch-icon.png`, `website/images/icon.png` und `website/images/social-preview.png` – nach jeder Änderung am Symbol zusammen mit `tools/make_icon.sh` ausführen |
| `packaging/licenses/` | Lizenztexte, die Homebrew nicht mitliefert (LGPL-3.0 und Hinweis für Qt, LGPL-2.1 für libintl) |
| `packaging/homebrew/lumaflip.rb` | Homebrew Cask, Spiegel von `Casks/lumaflip.rb` im Tap [`brokoskokoli/homebrew-lumaflip`](https://github.com/brokoskokoli/homebrew-lumaflip) (siehe [Homebrew Cask](#homebrew-cask)) |
| `packaging/winget/manifests/…/1.2.1/` | winget-Manifeste (`Brokoskokoli.Lumaflip`) für die erste Einreichung bei `microsoft/winget-pkgs` (siehe [winget](#winget)) |
| `tools/setup_distribution_secrets.sh` | Setzt `HOMEBREW_TAP_TOKEN` und `WINGET_TOKEN` (Environment `release`) für die Jobs `homebrew-tap` und `winget` |

## Lokal bauen

```sh
tools/package.sh                       # ad-hoc signiert, nur für diesen Mac
LF_SIGN_IDENTITY="Developer ID Application: Name (TEAMID)" tools/package.sh
LF_SIGN_IDENTITY="…" tools/package.sh --notarize-dry-run   # zeigt die Notarisierungsbefehle
```

- **libjpeg-turbo ist Pflicht:** `package.sh` konfiguriert mit `-DLF_REQUIRE_TURBOJPEG=ON` und prüft
  im `CMakeCache.txt`, dass das statische `libturbojpeg.a` gelinkt ist (verlustfreies JPEG-Drehen,
  F-EDIT-02; `brew install jpeg-turbo`, kommt mit Qt ohnehin mit). Nur für Experimente:
  `LF_REQUIRE_TURBOJPEG=OFF tools/package.sh`.
- **Sparkle ist lokal optional:** ohne `tools/fetch_sparkle.sh` entsteht ein Paket ohne Updater; mit
  Framework, aber ohne `LF_SPARKLE_PUBLIC_KEY` bleibt der Updater aus. Nur der Release verlangt
  beides (`LF_REQUIRE_SPARKLE=1`).

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

**Am einfachsten:** `tools/setup_release_secrets.sh` (interaktiv, im Terminal auf dem eigenen Mac). Das
Skript exportiert das Developer-ID-Zertifikat aus dem Schlüsselbund (reduziert auf genau diese Identität,
neues Passwort), fragt den App-Store-Connect-API-Key ab und prüft ihn bei Apple, erzeugt bzw. liest das
Sparkle-Schlüsselpaar (Schlüsselbund-Konto `lumaflip`), setzt alle Secrets in der Umgebung `release`
(auf Wunsch mit Pflicht-Freigabe und Tag-/Branch-Regeln) und die Variable `SPARKLE_PUBLIC_ED_KEY` und
schaltet GitHub Pages (Branch `gh-pages`) ein. Nichts davon landet im Repo oder in der Shell-History.

**Probelauf:** Actions → Release → *Run workflow* (Branch `main`) bzw.
`gh workflow run release.yml --ref main` baut, signiert, notarisiert und heftet die Tickets an wie ein
Release (Artefakt `release-dist`), legt aber kein GitHub-Release an und ändert den Update-Feed nicht. So
lassen sich die Secrets vor dem ersten Tag prüfen.

Von Hand: unter Settings → Secrets and variables → Actions (oder als Secrets der Umgebung `release`):

| Secret | Inhalt | Woher |
|---|---|---|
| `MACOS_CERT_P12_BASE64` | Zertifikat **Developer ID Application** samt privatem Schlüssel als `.p12`, Base64-kodiert: `base64 -i DeveloperID.p12 \| pbcopy` | Schlüsselbundverwaltung → Zertifikat + Schlüssel auswählen → Exportieren (.p12) |
| `MACOS_CERT_PASSWORD` | Passwort der `.p12`-Datei | beim Export vergeben |
| `NOTARY_API_KEY_ID` | Key-ID des API-Schlüssels (z. B. `ABC123DEFG`) | App Store Connect → Benutzer und Zugriff → Integrationen → App Store Connect API |
| `NOTARY_API_ISSUER_ID` | Issuer-ID (UUID) | dieselbe Seite, oben |
| `NOTARY_API_KEY_P8_BASE64` | Inhalt der `AuthKey_<id>.p8`, Base64-kodiert: `base64 -i AuthKey_ABC123DEFG.p8 \| pbcopy` | wird nur einmal zum Download angeboten; Rolle „Developer“ genügt |
| `SPARKLE_ED_PRIVATE_KEY` | Privater EdDSA-Schlüssel für Sparkle, Inhalt der mit `generate_keys -x` exportierten Datei (eine Zeile Base64) | siehe „Automatische Updates (Sparkle)“ unten |

Optional (Environment `release`, gesetzt mit `tools/setup_distribution_secrets.sh`; fehlen sie, überspringen
die Jobs `homebrew-tap` bzw. `winget` mit einem Hinweis, das Release ist davon nie betroffen):

| Secret | Inhalt | Woher |
|---|---|---|
| `HOMEBREW_TAP_TOKEN` | Fine-grained PAT, *Repository access* nur `brokoskokoli/homebrew-lumaflip`, *Permissions → Contents: Read and write* | github.com → Settings → Developer settings → Fine-grained tokens |
| `WINGET_TOKEN` | Classic PAT mit genau dem Scope `public_repo` (Fork von `winget-pkgs` + Pull Request; fine-grained PATs können keine PRs an fremde Repos öffnen) | github.com → Settings → Developer settings → Tokens (classic) |

Beide Tokens laufen ab (beim Anlegen z. B. ein Jahr wählen); danach das Skript erneut ausführen.

Dazu eine **Variable** (kein Secret, der Schlüssel ist öffentlich) unter Settings → Secrets and variables →
Actions → Variables: `SPARKLE_PUBLIC_ED_KEY` = öffentlicher EdDSA-Schlüssel (44 Zeichen Base64).

Der Workflow legt einen temporären Schlüsselbund an (zufälliges Passwort), importiert das Zertifikat,
ermittelt die Identität `Developer ID Application: …` daraus und löscht Schlüsselbund und Schlüsseldatei
am Ende (auch bei Fehlern). `GITHUB_TOKEN` reicht für `gh release create` und den Push nach `gh-pages`.

Ablauf und Absicherung von `release.yml`:

- **Jobs und Secrets getrennt:** `sparkle-keys` (nur Sparkle-Schlüssel) → `build` (nur `.p12` und
  Notar-Schlüssel, **nicht** der Sparkle-Schlüssel; Ergebnis als Artefakt `release-dist`) → `draft`
  (nur `GITHUB_TOKEN`) → `appcast` (nur Sparkle-Schlüssel und `GITHUB_TOKEN`; kein `brew`, kein Build:
  Sparkles Werkzeuge kommen aus dem per SHA-256 geprüften Archiv von `tools/fetch_sparkle.sh`, die DMG
  aus dem Artefakt, dessen Prüfsumme vorher kontrolliert wird). Erst nach dem Veröffentlichen:
  `homebrew-tap` (nur `HOMEBREW_TAP_TOKEN`) und `winget` (nur `WINGET_TOKEN`), siehe
  [Homebrew Cask](#homebrew-cask) und [winget](#winget).
- **Gleiche Prüfungen wie CI:** Der Job `build` hängt von `checks` ab, der denselben
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

## Automatische Updates (Sparkle)

F-MAC-08: Lumaflip bringt [Sparkle 2](https://sparkle-project.org) (MIT) mit. Die App liest den Appcast
`https://brokoskokoli.github.io/lumaflip/appcast.xml` (GitHub Pages, Branch `gh-pages`), lädt Updates aus
den GitHub Releases (die notarisierte DMG) und installiert sie nur, wenn ihre **EdDSA-Signatur** zum
öffentlichen Schlüssel in der App passt. Der Appcast selbst ist ebenfalls signiert (`SURequireSignedFeed`),
Updates werden vor dem Entpacken geprüft (`SUVerifyUpdateBeforeExtraction`).

**Datenschutz (SPEC 10.3):** Die Update-Prüfung ist der einzige Netzwerkverkehr. `SUEnableAutomaticChecks`
steht bewusst **nicht** in der `Info.plist`: Beim ersten Start passiert nichts, beim zweiten fragt Sparkle,
ob automatisch gesucht werden soll. Danach ist es in Einstellungen → Allgemein umschaltbar
(„Automatisch nach Updates suchen“, „Updates automatisch installieren“); „Nach Updates suchen …“ im
Programmmenü prüft sofort. Sparkle prüft höchstens einmal am Tag und sendet kein Systemprofil.

### Bauen mit und ohne Sparkle

```sh
tools/fetch_sparkle.sh                 # einmalig: Sparkle 2.10.0 nach third_party/Sparkle
cmake … -DLF_ENABLE_SPARKLE=AUTO       # Standard: an, wenn third_party/Sparkle existiert
cmake … -DLF_ENABLE_SPARKLE=OFF        # ohne Sparkle: kein Menüpunkt, keine Einstellungen
cmake … -DLF_SPARKLE_PUBLIC_KEY=<base64>   # Feed + Schlüssel in die Info.plist (sonst Updater aus)
LF_SPARKLE_PUBLIC_KEY=<base64> LF_REQUIRE_SPARKLE=1 tools/package.sh   # wie im Release
```

- Ohne `LF_SPARKLE_PUBLIC_KEY` (lokale Builds, CI-Prüfungen) wird Sparkle zwar eingebettet, die
  `Info.plist` bekommt aber weder `SUFeedURL` noch `SUPublicEDKey`: Der Updater bleibt aus (Hinweis im
  Log `lumaflip.updater`), Menüpunkt und Einstellungen sind ausgeblendet. **Nie ohne Schlüssel ausliefern:**
  Der Release-Workflow setzt `LF_REQUIRE_SPARKLE=1`, `package.sh` bricht dann ohne Framework, Feed oder
  Schlüssel ab.
- `tools/package.sh` kopiert `Sparkle.framework` vollständig nach `Contents/Frameworks` (mit `Autoupdate`,
  `Updater.app`, `XPCServices/Installer.xpc`, `XPCServices/Downloader.xpc`) und signiert es nach Sparkles
  Anleitung von innen nach außen, immer mit Hardened Runtime: XPC-Dienste (Downloader mit
  `--preserve-metadata=entitlements`) → `Autoupdate` → `Updater.app` → Framework, danach Qt und die App.
  Sparkles `LICENSE` steht in `Contents/Resources/licenses/sparkle/` und im Über-Fenster.
- Sparkle aktualisieren: Version und SHA-256 in `tools/fetch_sparkle.sh` anpassen (Wert = `digest` des
  Release-Assets, `gh api repos/sparkle-project/Sparkle/releases/tags/<version>`), `tools/fetch_sparkle.sh`
  ausführen, Update-Test aus [MANUAL_CHECKS.md](MANUAL_CHECKS.md) wiederholen.

### Schlüsselpaar einmalig erzeugen

Nur einmal und nur auf dem eigenen Mac (nicht in der CI). `generate_keys` legt den privaten Schlüssel im
**Anmelde-Schlüsselbund** ab (Eintrag „Private key for signing Sparkle updates“) und gibt den öffentlichen aus:

```sh
tools/fetch_sparkle.sh
third_party/Sparkle/bin/generate_keys              # erzeugt das Paar (oder zeigt ein vorhandenes)
third_party/Sparkle/bin/generate_keys -p           # nur den öffentlichen Schlüssel ausgeben
third_party/Sparkle/bin/generate_keys -x sparkle_private.key   # privaten Schlüssel exportieren
tools/sparkle_key_check.sh sparkle_private.key "<öffentlicher Schlüssel>"   # passt das Paar?
```

1. **Öffentlicher Schlüssel** → Repo-Variable `SPARKLE_PUBLIC_ED_KEY` (Settings → Secrets and variables →
   Actions → Variables). Für lokale Testbuilds: `-DLF_SPARKLE_PUBLIC_KEY=…` bzw.
   `LF_SPARKLE_PUBLIC_KEY=… tools/package.sh`.
2. **Privater Schlüssel** → Secret `SPARKLE_ED_PRIVATE_KEY` (am besten in der Umgebung `release`): Inhalt von
   `sparkle_private.key` einfügen, z. B. `gh secret set SPARKLE_ED_PRIVATE_KEY --env release < sparkle_private.key`.
   Danach `sparkle_private.key` löschen (`rm -P` bzw. sicher entsorgen). Zusätzlich offline sichern
   (Passwortmanager): Geht der Schlüssel verloren, ist ein Wechsel nur über die Developer-ID-Signatur möglich
   (siehe unten).
3. **GitHub Pages** einschalten: Settings → Pages → Source „Deploy from a branch“, Branch `gh-pages`,
   Ordner `/ (root)`. Den Branch legt der erste Lauf des Workflows `pages.yml` (Homepage aus `website/`)
   oder der erste Release-Lauf an. Auf `gh-pages` liegen nebeneinander die Homepage (`pages.yml`, ersetzt
   alles außer `appcast.xml` und `notes/`) und der Update-Feed (`release.yml`).

### Was der Release-Workflow macht

1. **`sparkle-keys`:** Fehlen Variable oder Secret oder passen sie nicht zusammen
   (`tools/sparkle_key_check.sh`), bricht der Workflow **vor** dem Bauen ab.
2. **`build`:** `tools/fetch_sparkle.sh`, `package.sh --notarize` mit `LF_ENABLE_SPARKLE=ON`,
   `LF_SPARKLE_PUBLIC_KEY`, `LF_REQUIRE_SPARKLE=1` (und `LF_REQUIRE_TURBOJPEG=ON`), Tickets prüfen,
   DMG/ZIP/SHA-256 als Artefakt `release-dist` hochladen.
3. **`draft`:** Prüfsummen kontrollieren; `gh release view` → gibt es das Release noch nicht, wird es als
   **Entwurf** angelegt (`gh release create --draft --generate-notes`), ein vorhandener Entwurf bekommt die
   neuen Dateien (`gh release upload --clobber`). Ein bereits **veröffentlichtes** Release wird nie
   überschrieben (Abbruch: neue Version taggen).
4. **`appcast`** (globale Concurrency-Gruppe `release-appcast`, `cancel-in-progress: false`: nie zwei
   Appcast-Läufe gleichzeitig, auch nicht für verschiedene Tags): Sparkle-Werkzeuge per
   `tools/fetch_sparkle.sh`, DMG aus dem Artefakt (Prüfsumme), Schlüsselprüfung. Dann `gh-pages`
   **vollständig** klonen (bzw. neu anlegen), vorhandene `appcast.xml` und die DMG in ein
   Arbeitsverzeichnis, Release-Text des Entwurfs als HTML (GitHub-Markdown-API, `mode=markdown`: harte
   Zeilenumbrüche aus `CHANGELOG.md` werden kein `<br>`) daneben,
   `generate_appcast --ed-key-file - --maximum-versions 0 --maximum-deltas 0` – der private Schlüssel
   kommt nur über stdin (`printf` ist ein Shell-Builtin, erscheint also in keiner Argumentliste und wird
   nie ausgegeben). Alle vorhandenen Einträge bleiben erhalten, keine Delta-Updates.
5. **Prüfung vor dem Push:** `tools/appcast_verify.py` vergleicht alten und neuen Appcast (jeder alte
   Eintrag mit gleicher Version und gleicher Download-URL noch da; genau ein Eintrag mehr bzw. gleich
   viele bei einem erneuten Lauf; neuer Eintrag mit erwarteter URL, `sparkle:edSignature` und der Länge
   der DMG), `sign_update --verify` prüft die Feed-Signatur.
   **Windows:** `sign_update -p` signiert den (Authenticode-signierten) Installer mit demselben
   EdDSA-Schlüssel, `sign_update --verify` prüft die Signatur, `tools/appcast_windows.py` ergänzt
   `appcast-windows.xml` um den neuen Eintrag (gleiche Release-Notizen) und
   `tools/appcast_verify.py --os windows --archive <setup.exe>` prüft ihn. `appcast.xml` bekommt nie
   Windows-Einträge – Sparkle auf dem Mac sieht den Windows-Installer nicht.
6. `appcast.xml`, `appcast-windows.xml` und `notes/<version>.html` werden als Commit auf `gh-pages` gepusht (`GITHUB_TOKEN` über
   `gh auth setup-git`, kein Token in einer URL). Schlägt der Push fehl (anderer Push dazwischen), wird bis
   zu fünfmal geholt, neu aufgesetzt (`rebase`) und erneut gepusht; hat sich dabei einer der Appcasts
   selbst geändert, bricht der Job ab (Appcast müsste neu erzeugt werden → Job erneut starten). Ein
   unveränderter Appcast (erneuter Lauf) wird nicht noch einmal committet.
7. **Erst danach** wird das Release veröffentlicht (`gh release edit --draft=false`). Bis dahin zeigt der
   Appcast evtl. schon die neue Version, deren Download noch nicht öffentlich ist; GitHub Pages braucht
   für die Veröffentlichung ohnehin etwa eine Minute, das Release ist dann in der Regel schon öffentlich.

**Erneut ausführen:** Jeder Job lässt sich wiederholen („Re-run failed jobs“). Bricht der Appcast-Schritt
ab, bleibt das Release ein unveröffentlichter Entwurf; ein erneuter Lauf erzeugt den Appcast neu und
veröffentlicht danach. Hinweis zur Concurrency-Gruppe: GitHub hält je Gruppe nur **einen wartenden** Job;
werden drei Tags schnell hintereinander gepusht, bricht der dritte den wartenden zweiten ab – dann den
abgebrochenen Lauf neu starten.

Notfall (Workflow nicht nutzbar): DMG aus dem Release und die aktuelle `appcast.xml` von `gh-pages` in ein
leeres Verzeichnis legen, `third_party/Sparkle/bin/generate_appcast --download-url-prefix
https://github.com/brokoskokoli/lumaflip/releases/download/vX.Y.Z/ --maximum-deltas 0 --maximum-versions 0 <dir>`
(Schlüssel aus dem Schlüsselbund), mit `tools/appcast_verify.py --old <alt> --new <dir>/appcast.xml --version
X.Y.Z --url <URL der DMG>` prüfen und `appcast.xml` auf `gh-pages` committen.

### Schlüssel fehlt, ist falsch oder kompromittiert

- **Variable oder Secret fehlt / passen nicht:** Kein Release wird gebaut (Job `sparkle-keys`).
- **Altes Schlüsselformat (96 Byte):** frühere `generate_keys`-Versionen speichern den privaten Schlüssel
  als 64-Byte-orlp-Schlüssel plus 32-Byte-öffentlichen Schlüssel. `sparkle_key_check.sh` leitet den
  öffentlichen Schlüssel aus dem privaten Skalar ab und lehnt Dateien ab, deren Anhang nicht dazu passt.
- **Lokaler Build ohne Schlüssel:** Updater aus, nichts wird veröffentlicht.
- **Schlüssel wechseln (Rotation):** Sparkle erlaubt pro Update nur *eine* Änderung – entweder den
  EdDSA-Schlüssel oder das Developer-ID-Zertifikat, nie beide zugleich. Wegen
  `SUVerifyUpdateBeforeExtraction` geht ein Schlüsselwechsel nur mit einer Developer-ID-signierten **DMG**
  als Update (das liefert der Workflow). Ablauf: neues Paar mit
  `generate_keys --account lumaflip-2` erzeugen, Variable `SPARKLE_PUBLIC_ED_KEY` auf den neuen öffentlichen,
  Secret `SPARKLE_ED_PRIVATE_KEY` auf den neuen privaten Schlüssel setzen, Release taggen. Bestehende
  Installationen akzeptieren dieses Update, weil die DMG mit demselben Developer-ID-Zertifikat signiert
  ist; der Appcast wird dabei neu (mit dem neuen Schlüssel) signiert. Das Zertifikat erst in einem späteren
  Release wechseln.
- **Privater Schlüssel kompromittiert:** sofort rotieren (wie oben) und das Secret ersetzen; zusätzlich alte
  Exporte löschen. Ausliefern könnte ein Angreifer ein so signiertes Update nur über die Feed-URL
  (`gh-pages`) – trotzdem nicht warten.

## Windows: Signatur und Updates

Die Windows-Ausgabe bekommt im Release-Job **`build-windows`** (Runner `windows-2025`, Environment
`release`) eine Authenticode-Signatur von **Azure Artifact Signing** (früher „Trusted Signing“) und
automatische Updates über **WinSparkle** (Gegenstück zu Sparkle, MIT) mit einem eigenen Feed
`https://brokoskokoli.github.io/lumaflip/appcast-windows.xml`. Bedienung und Datenschutz wie auf dem Mac
(siehe [docs/WINDOWS.md](WINDOWS.md#automatic-updates)).

### Ablauf im Release-Job

1. **Signatur-Konfiguration prüfen:** Variablen `ARTIFACT_SIGNING_ENDPOINT`, `ARTIFACT_SIGNING_ACCOUNT`,
   `ARTIFACT_SIGNING_CERT_PROFILE` und die Secrets `AZURE_CLIENT_ID`, `AZURE_TENANT_ID`,
   `AZURE_SUBSCRIPTION_ID` (Environment `release`) müssen gesetzt sein, dazu `SPARKLE_PUBLIC_ED_KEY`.
   **Bei einem Tag bricht der Job sonst ab – ein Release wird nie unsigniert veröffentlicht.** Der
   Probelauf (*Run workflow*) baut ohne Signatur-Konfiguration weiter, unsigniert und mit Warnung.
2. **Bauen** mit derselben Composite-Action wie die CI (`.github/actions/windows-build`), zusätzlich
   `-DLF_SPARKLE_PUBLIC_KEY=<SPARKLE_PUBLIC_ED_KEY>`: derselbe öffentliche EdDSA-Schlüssel wie auf dem
   Mac wird in `Lumaflip.exe` einkompiliert. `tools/windows_deploy.py --require-winsparkle`, Rauchtest.
3. **Ordner signieren:** `Lumaflip.exe`, `lumaflip-decoder.exe` und jede weitere `.exe`/`.dll` ohne gültige
   Signatur (u. a. `WinSparkle.dll`, die upstream unsigniert ist; Qt- und Microsoft-DLLs behalten ihre
   Signatur). Anmeldung per **OIDC** (`azure/login` mit Client-, Tenant- und Abonnement-ID, **kein
   Client-Secret**), signiert wird mit `azure/artifact-signing-action` (SHA-256, RFC-3161-Zeitstempel von
   `http://timestamp.acs.microsoft.com`). Danach `signtool verify /pa` für alle `.exe`/`.dll` und ein
   Zeitstempel-Check.
4. **ZIP und Installer** aus dem signierten Ordner (`tools/windows_package.py`), dann den **Installer
   signieren** und prüfen (`signtool verify /pa /v`).
5. **Installer testen** wie WinSparkle ihn nutzt (`tools/windows_installer_check.py`: still installieren,
   bei laufender App mit den Appcast-Argumenten aktualisieren, Neustart, deinstallieren), `.sha256`
   schreiben, Artefakt `release-windows`.
6. `draft` hängt Installer, ZIP und `.sha256` an das Release, `appcast` schreibt `appcast-windows.xml`
   (siehe oben, Schritt 5 von „Was der Release-Workflow macht“).

Nicht signiert wird das in den Installer eingebettete Deinstallationsprogramm (`unins000.exe`): Dafür
müsste Inno Setup während des Kompilierens selbst signieren (`SignTool=`), was mit der GitHub-Action
nicht geht. Es wird nicht heruntergeladen (kein „Mark of the Web“), SmartScreen prüft es daher nicht.

### Einrichten mit dem Skript

Voraussetzung: ein Artifact-Signing-Konto mit abgeschlossener Identitätsprüfung und einem
Zertifikatprofil vom Typ **Public Trust** (ein vorhandenes eines anderen Projekts genügt; der Name im
Zertifikat ist der geprüfte Name, nicht „Lumaflip“). Dann auf dem eigenen Mac:

```sh
brew install azure-cli gh
az login                                  # Konto mit Owner/User Access Administrator am Profil
tools/setup_windows_signing.sh
```

Das Skript (interaktiv, bash 3.2, gibt keine Secret-Werte aus):

1. fragt nach dem Repository und legt bei Bedarf das Environment `release` an,
2. findet alles selbst, gefragt wird nur nach der Auswahl: Abonnement (bei mehreren), dann die
   **Signaturkonten** des Abonnements (`az resource list --resource-type
   Microsoft.CodeSigning/codeSigningAccounts`; Name, Ressourcengruppe, Region – der Endpunkt folgt aus
   der Region nach Microsofts Tabelle, z. B. `westeurope` → `https://weu.codesigning.azure.net/`), dann die
   **Zertifikatprofile** des Kontos mit Typ, Status und Identität (CN des Zertifikats; az-Erweiterung
   `artifact-signing`, ersatzweise das ältere `trustedsigning` – fehlt sie, fragt das Skript, ob es sie
   installieren soll). Public-Trust-Profile stehen oben, das erste ist die Vorgabe; das Profil eines
   anderen Projekts mitzubenutzen ist in Ordnung. Fehlt `az` oder ist es nicht angemeldet, bricht das
   Skript mit dem Hinweis `brew install azure-cli` bzw. `az login` ab,
3. legt die Entra-App-Registrierung **`lumaflip-github-release`** samt Dienstprinzipal an (oder nutzt die
   vorhandene) und eine **föderierte Anmeldeinformation**: Aussteller
   `https://token.actions.githubusercontent.com`, Subjekt `repo:brokoskokoli/lumaflip:environment:release` und (neueres GitHub-Format mit IDs) `repo:brokoskokoli@16724498/lumaflip@1413300059:environment:release`,
   Zielgruppe `api://AzureADTokenExchange`,
4. weist dem Dienstprinzipal die Rolle **„Artifact Signing Certificate Profile Signer“** (in älteren
   Tenants „Trusted Signing Certificate Profile Signer“) zu – **nur auf das gewählte Profil**,
5. setzt mit `gh` die Secrets `AZURE_CLIENT_ID`, `AZURE_TENANT_ID`, `AZURE_SUBSCRIPTION_ID` (Environment
   `release`) und die Repository-Variablen `ARTIFACT_SIGNING_ENDPOINT`, `ARTIFACT_SIGNING_ACCOUNT`,
   `ARTIFACT_SIGNING_CERT_PROFILE`.

Ein erneuter Aufruf legt nichts doppelt an. Danach den Probelauf starten
(`gh workflow run release.yml --ref main`): Im Job „Windows build, sign“ müssen „Azure login“, beide
Signaturschritte und die `signtool`-Prüfungen grün sein. Rollenzuweisungen brauchen in Azure einige Minuten.

| Name | Art | Inhalt |
|---|---|---|
| `ARTIFACT_SIGNING_ENDPOINT` | Variable | Endpunkt der Region des Kontos, z. B. `https://weu.codesigning.azure.net/` |
| `ARTIFACT_SIGNING_ACCOUNT` | Variable | Name des Artifact-Signing-Kontos |
| `ARTIFACT_SIGNING_CERT_PROFILE` | Variable | Name des Zertifikatprofils (Public Trust) |
| `AZURE_CLIENT_ID` | Secret (Environment `release`) | Anwendungs-ID (Client-ID) der App-Registrierung `lumaflip-github-release` |
| `AZURE_TENANT_ID` | Secret (Environment `release`) | Verzeichnis-ID (Tenant) |
| `AZURE_SUBSCRIPTION_ID` | Secret (Environment `release`) | Abonnement-ID mit dem Signaturkonto |

Die IDs sind keine Zugangsdaten (ohne die föderierte Anmeldeinformation nützen sie nichts), liegen aber
trotzdem als Secrets im Environment, damit nur Jobs mit Freigabe sie sehen.

### Einrichten von Hand (Azure-Portal)

1. **Microsoft Entra ID → App-Registrierungen → Neue Registrierung:** Name `lumaflip-github-release`,
   „Nur Konten in diesem Organisationsverzeichnis“, keine Umleitungs-URI. Auf der Übersicht
   **Anwendungs-ID (Client)** und **Verzeichnis-ID (Mandant)** notieren.
2. In der App: **Zertifikate & Geheimnisse → Verbundanmeldeinformationen → Anmeldeinformationen
   hinzufügen**, Szenario „GitHub Actions, die Azure-Ressourcen bereitstellen“: Organisation
   `brokoskokoli`, Repository `lumaflip`, Entitätstyp **Umgebung**, Name `release`. Das ergibt das
   Subjekt `repo:brokoskokoli/lumaflip:environment:release` und (neueres GitHub-Format mit IDs) `repo:brokoskokoli@16724498/lumaflip@1413300059:environment:release` mit Aussteller
   `https://token.actions.githubusercontent.com` und Zielgruppe `api://AzureADTokenExchange`.
   **Kein** Client-Secret anlegen.
3. **Artifact-Signing-Konto → Zertifikatprofile →** das Public-Trust-Profil öffnen → **Zugriffssteuerung
   (IAM) → Hinzufügen → Rollenzuweisung:** Rolle „Artifact Signing Certificate Profile Signer“ (bzw.
   „Trusted Signing …“), Mitglied: Benutzer, Gruppe oder Dienstprinzipal → `lumaflip-github-release`.
4. Auf der Übersicht des Kontos die **Konto-URI** (Endpunkt) und die Namen von Konto und Profil
   ablesen, im Abonnement die **Abonnement-ID**.
5. GitHub → Settings → Environments → `release` → Environment secrets: `AZURE_CLIENT_ID`,
   `AZURE_TENANT_ID`, `AZURE_SUBSCRIPTION_ID`; Settings → Secrets and variables → Actions → Variables:
   `ARTIFACT_SIGNING_ENDPOINT`, `ARTIFACT_SIGNING_ACCOUNT`, `ARTIFACT_SIGNING_CERT_PROFILE`.

### WinSparkle und der Windows-Feed

- `tools/fetch_winsparkle.py` lädt WinSparkle 0.9.4 (EdDSA ab 0.9), geprüft per SHA-256; die CMake-Option
  `LF_ENABLE_WINSPARKLE` (`AUTO`/`ON`/`OFF`) kopiert `WinSparkle.dll` neben `Lumaflip.exe`. Gelinkt wird
  nichts: Lumaflip lädt die DLL zur Laufzeit nur aus dem eigenen Ordner (`LoadLibraryEx`, keine Suche in
  `PATH`). Ohne DLL oder ohne `LF_SPARKLE_PUBLIC_KEY` ist der Updater aus (Menüpunkt und Einstellungen
  ausgeblendet, Grund im Log `lumaflip.updater`).
- Der Schlüssel ist derselbe wie für Sparkle (Variable `SPARKLE_PUBLIC_ED_KEY`, Secret
  `SPARKLE_ED_PRIVATE_KEY`); eine Schlüsselrotation betrifft beide Plattformen. WinSparkle prüft die
  EdDSA-Signatur des heruntergeladenen Installers, bevor es ihn startet; Windows prüft zusätzlich die
  Authenticode-Signatur.
- Feed-Eintrag (`tools/appcast_windows.py`): `sparkle:version` = Projektversion, `sparkle:os="windows"`,
  `sparkle:minimumSystemVersion` 10.0.17763, Release-Notizen inline als `<description>`, Installer mit
  `sparkle:installerArguments="/SILENT /SP- /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /RELAUNCH=1"`.
  Der Installer aktualisiert an Ort und Stelle (gleiche `AppId`, vorheriger Ordner und Modus „nur für
  mich“/„für alle“), schließt laufende Kopien und startet Lumaflip danach neu (`/RELAUNCH=1`).
- Notfall: Installer aus dem Release, `third_party/Sparkle/bin/sign_update -p <setup.exe>` (Schlüssel aus
  dem Schlüsselbund), `tools/appcast_windows.py --old <appcast-windows.xml von gh-pages> …`, mit
  `tools/appcast_verify.py --os windows …` prüfen und auf `gh-pages` committen.

## Homebrew Cask

Eigener Tap [`brokoskokoli/homebrew-lumaflip`](https://github.com/brokoskokoli/homebrew-lumaflip), später Antrag
für `homebrew/cask` (SPEC 10.1). Installation für Nutzer:
`brew install --cask brokoskokoli/lumaflip/lumaflip`.

- **Datei:** `Casks/lumaflip.rb` im Tap; `packaging/homebrew/lumaflip.rb` hier ist derselbe Cask mit
  Kommentarkopf. Ändert sich ein Abschnitt (zap-Pfade, Voraussetzungen), beide anpassen.
  Der Cask hat `auto_updates true` (Sparkle aktualisiert die App selbst; `brew upgrade` lässt sie in
  Ruhe, `brew upgrade --greedy` nicht), `depends_on arch: :arm64`, `depends_on macos: :sequoia`,
  `livecheck` über den Sparkle-Appcast und `zap` für Einstellungen, Caches und Fensterzustand.
- **Nach jedem Release automatisch:** Der Job `homebrew-tap` (nach `appcast`, also erst wenn das
  Release öffentlich ist) lädt `Lumaflip-<version>.dmg` und `.dmg.sha256` aus dem Release, prüft die
  Summe, setzt `version` und `sha256` im Tap und pusht (`github-actions[bot]`). Er ist idempotent und
  setzt den Tap nie auf eine ältere Version zurück. Die CI des Taps (`.github/workflows/tests.yml`)
  führt danach `brew style`, `brew audit --cask --online --strict`, `brew livecheck` und
  `brew fetch` aus. Ohne `HOMEBREW_TAP_TOKEN` endet der Job mit einem Hinweis.
- **Von Hand** (z. B. ohne Token): im Tap `version` und `sha256` (Inhalt von
  `Lumaflip-<version>.dmg.sha256`) setzen, dann
  `brew style --cask brokoskokoli/lumaflip/lumaflip && brew audit --cask --online --strict brokoskokoli/lumaflip/lumaflip`.
- **Später `homebrew/cask`:** `brew audit --cask --new` verlangt ein „bekanntes“ Repository
  (mindestens 30 Forks, 30 Watcher oder 75 Sterne); bis dahin bleibt es beim eigenen Tap.

## winget

Paket `Brokoskokoli.Lumaflip` (Installer-Typ `inno`, Scope `user` mit `/CURRENTUSER` als Standard,
`machine` mit `/ALLUSERS`; winget startet Inno Setup still mit `/SP- /VERYSILENT /SUPPRESSMSGBOXES
/NORESTART`). Die Daten unter „Apps & Features“ stammen aus `packaging/windows/Lumaflip.iss`:
`ProductCode` = `{6E0D5D8A-6A43-4C6B-9C6F-4C1A5B7E1F2D}_is1` (AppId + `_is1`), `DisplayName`
`Lumaflip`, `Publisher` `Stefan Richter` (ab 1.2.2; 1.2.1 schrieb noch `The Lumaflip contributors`, so steht es im
1.2.1-Manifest). Ändert sich `AppId` oder `AppPublisher`, müssen die Manifeste mitziehen. Die erste winget-Einreichung
am besten mit 1.2.2, dann passen Herausgeber im Manifest und unter „Apps & Features“ zusammen.

**Erste Einreichung (einmalig, von Hand):** Microsoft prüft neue Pakete manuell; erst nach dem
Merge kann der Release-Job Updates einreichen. Die Manifeste für 1.2.1 liegen unter
`packaging/winget/manifests/b/Brokoskokoli/Lumaflip/1.2.1/` (gegen das Schema 1.12.0 geprüft):

```sh
brew install komac
gh repo fork microsoft/winget-pkgs --clone=false          # einmalig: Fork im eigenen Konto
komac token update                                         # PAT mit public_repo (classic), fragt verdeckt
komac submit packaging/winget/manifests/b/Brokoskokoli/Lumaflip/1.2.1
```

Alternativ unter Windows: `winget validate --manifest <ordner>`, `winget install --manifest <ordner>`
zum Ausprobieren, dann `wingetcreate submit --token <PAT> <ordner>`. Nach dem Merge:
`winget install Brokoskokoli.Lumaflip` (bzw. `winget install lumaflip`).

**Danach automatisch:** Der Job `winget` (nach `appcast`) prüft, ob das Paket schon in
`microsoft/winget-pkgs` steht (sonst Hinweis und Ende), lädt [komac](https://github.com/russellbanks/Komac)
in der festgelegten Version (SHA-256 im Workflow) und ruft
`komac update Brokoskokoli.Lumaflip --version <v> --urls <setup.exe-URL> --submit` auf: komac liest
Hash, `ProductCode` und ARP-Daten aus dem signierten Installer, übernimmt die übrigen Felder aus der
vorigen Version und öffnet den Pull Request über den Fork. Der Token geht nur über die Umgebung an
komac. Ohne `WINGET_TOKEN` endet der Job mit einem Hinweis. `vedantmgoyal9/winget-releaser` nutzt
dasselbe Werkzeug, installiert es aber ungepinnt (`cargo-binstall@main`) – deshalb komac direkt.
komac aktualisieren: Version und SHA-256 aus `SHA256SUMS` des Releases in `release.yml` eintragen.

## Screenshots und Demo-Animation

Die Bilder in README und Website entstehen automatisch aus echten Fenstern:

- **Beispielfotos:** `tools/fetch_sample_photos.py` lädt 16 Fotos (CC0 1.0, Wikimedia Commons, Autor
  meist *Jebulon*) laut `tools/screenshots/sample-photos.json`, jeweils mit SHA-256 gepinnt, nach
  `build/sample-photos/` (Originale im Cache `.originals/`, nichts davon wird eingecheckt). Das Skript
  verkleinert sie auf 3000 px (`sips`, EXIF und ICC-Profil bleiben) und schreibt die Position der
  Commons-Seite als EXIF-GPS hinein, damit die Ordnerkarte etwas zeigt. Neues Foto: Eintrag mit
  URL, SHA-256, Quelle, Autor, Lizenz und Koordinaten ergänzen; der Unit-Test
  `tests/tools/test_sample_photos.py` prüft das Manifest und dass jeder Autor in README und Website
  genannt ist.
- **Werkzeug:** `lf_screenshots` (`tools/screenshots/`, nur macOS, wird mit der App gebaut) öffnet die
  Fotos in einer temporären Kopie mit den Klassen der App, stellt die Zustände her (Bild mit
  Titelleiste, Infopanel + Filmstreifen, Culling, Raster mit Auswahl, Vergleich, Ordnerkarte,
  Einstellungen → Maus & Tastatur, Demo) und speichert sie per `QWidget::grab` (die QRhi-Leinwand
  rendert in den Grab) mit 2× Pixeldichte. Ist der Bildschirm 1×, startet es sich mit
  `QT_SCALE_FACTOR` neu. Rahmen, Schatten und Fensterknöpfe zeichnet `ShotFrame.cpp` (Unit-Test
  `tests/unit/test_screenshot_frame.cpp`). Die Ordnerkarte (`--shots map`, MapKit) ist nicht im
  Grab und kommt nur per `screencapture -l` in der 1×-Auflösung des Bildschirms dazu; sie wird
  deshalb standardmäßig nicht aufgenommen.
- **Ablauf:** `tools/screenshots/make_screenshots.sh` nimmt alles in Englisch und Deutsch, hell und
  dunkel auf (Master-PNGs in `build/screenshots/masters/`), `tools/screenshots/export_images.sh`
  erzeugt daraus `docs/images/*.webp` (README) und `website/images/shots/` (WebP plus PNG-Fallback,
  Demo als animiertes WebP). **Öffnet minutenlang Fenster** – nicht auf einem Arbeitsrechner starten.
- **CI:** `.github/workflows/screenshots.yml` (manuell, bei Release-Tags und bei Pull Requests, die das
  Werkzeug ändern) baut, lädt die Fotos, nimmt auf und lädt das Artefakt `screenshots` hoch. Es
  committet nichts: Bilder ansehen, dann `docs/images/` und `website/images/shots/` aus dem Artefakt
  übernehmen und committen (`gh run download <run> -n screenshots`).

## Nach dem Release prüfen

Die nicht automatisierbaren Prüfungen (Gatekeeper beim echten Download, Offline-Start, TCC-Texte,
Lizenzfenster) stehen in [MANUAL_CHECKS.md](MANUAL_CHECKS.md) im Abschnitt „Auslieferung“.

## Release-Notizen

Der Workflow nimmt den Text des Releases aus `CHANGELOG.md`: den Abschnitt `## [X.Y.Z] …` bis zum
nächsten `## [`. Fehlt der Abschnitt, bricht der Job `draft` ab. Der Text erscheint auf der Release-Seite
und – als HTML – im Update-Fenster von Sparkle bzw. WinSparkle. Vor dem Tag also den Abschnitt `[Unreleased]` in
`[X.Y.Z] - Datum` umbenennen (mit kurzer Einleitung für Nutzer) und die Links am Ende anpassen.
Die älteren deutschen Notizen unten sind Entwürfe aus der Abnahme.

### 1.1.0 (Meilenstein M3)

**Neu**
- 18 Sprachen (folgt der Systemsprache, umschaltbar ohne Neustart; Übersetzungen noch ungeprüft).
- Aussortiermodus (⌘K) mit Sternen, Markieren/Ablehnen und automatischem Weiterblättern; Bewertungen als
  XMP-Zusatzdatei, im Bild oder als Finder-Tags; Filter nach Bewertung; Sammelaktion für abgelehnte Bilder.
- Schnellablage (`M`/`C` + `1`–`9`) mit Namenskonflikt-Abfrage und Widerrufen.
- Rasteransicht, Vergleich von 2–4 Bildern, Lupe, gekoppelte Fenster.
- HDR/EDR-Darstellung mit SDR-Vorschau und Belichtungsregler.
- Unterordner, Nachbarordner (⌥← / ⌥→), „Gehe zu Bild“, zufällige Reihenfolge, Schnellfilter (⌘F).
- RAW+JPEG-Paare als ein Bild (optional), GPS mit Karte nach Klick, Fenster mit allen Metadaten.
- Ordnerkarte (Darstellung → Ordnerkarte, ⇧⌘M): alle Bilder mit Aufnahmeort auf einer Karte, zusammengefasst,
  aktuelles Bild folgt beim Blättern, Klick zeigt das Bild.
- SVG vollständig über resvg (CSS, Filter, Text), Qt SVG als Rückfall.
- Neue Formate: GIMP (XCF), Photoshop Large Document (PSB), QOI, PCX, PBM/PGM/PPM/PFM, SGI, OpenRaster,
  Krita; Decoder im Hilfsprozess.

**Hinweise**
- Bei RAW+JPEG gehört `IMG_1.xmp` der RAW-Datei (Lightroom); das JPEG nutzt `IMG_1.JPG.xmp`.
- Kartenausschnitte werden erst nach „Karte einblenden“ von Apple geladen und nur bei sichtbarem
  Infopanel; „Karte ausblenden“ nimmt die Zustimmung zurück.
- Auch die Ordnerkarte lädt Kartendaten erst, wenn sie geöffnet wird; Schließen beendet das Laden.
- Bekannte Grenzen: XCF-Ebenenmodi ohne Gegenstück (HSV/LCH …) und GIMP-3-Effekte werden vereinfacht
  dargestellt.

**New (English)**
- 18 languages (follows the system language, switchable without restart; translations not yet reviewed).
- Culling mode (⌘K) with stars, pick/reject and auto-advance; ratings in XMP sidecars, embedded or as
  Finder tags; rating filter; bulk action for rejected images.
- Quick move/copy (`M`/`C` + `1`–`9`) with name-conflict handling and undo.
- Grid view, compare 2–4 images, loupe, coupled windows.
- HDR/EDR display with SDR preview and exposure control.
- Subfolders, sibling folders (⌥← / ⌥→), “Go to Image”, shuffle, quick filter (⌘F).
- RAW+JPEG pairs as one image (optional), GPS with a map after a click, full metadata window.
- Folder map (View → Map, ⇧⌘M): every image with a location on a map, clustered; the current image
  follows as you flip, a click shows the image. Map data is loaded only after you open it.
- Full SVG via resvg (CSS, filters, text), Qt SVG as the fallback.
- New formats: GIMP (XCF), Photoshop Large Document (PSB), QOI, PCX, PBM/PGM/PPM/PFM, SGI, OpenRaster,
  Krita; decoders in a helper process.

## Lizenzen im Paket (SPEC 11)

- Qt bleibt dynamisch gelinkt (Frameworks unter `Contents/Frameworks`) und ist damit austauschbar (LGPLv3).
- Mitgeliefert werden nur Qt-Plugins, die Lumaflip braucht (`PLUGINS` in `tools/macos_deploy.py`):
  Cocoa-Plattform, Offscreen-Plattform (nur für den Decoder-Hilfsprozess), macOS-Stil, SVG-Symbole und
  die Bildformate GIF, ICNS, ICO, JPEG, HEIF (macOS), SVG, TGA, TIFF, WBMP, WebP. Bewusst weggelassen: PDF (zieht Qt PDF nach), MNG (libmng + Little CMS),
  JPEG 2000 (JasPer; dekodiert ImageIO), virtuelle Tastatur (zieht Qt Quick nach).
- Der Decoder-Hilfsprozess `Contents/MacOS/lumaflip-decoder` (SPEC 8.5: Qt-Bildformat-Plugins und SVG
  laufen außerhalb der App, in einer Sandbox ohne Netz und ohne Schreibzugriff) wird wie die App
  eingebettet (`@executable_path/../Frameworks`) und von `package.sh` vor dem Bundle signiert (Hardened
  Runtime, ohne Entitlements).
- Für jede eingebettete Bibliothek ermittelt `macos_deploy.py` das Homebrew-Paket, kopiert dessen
  Lizenzdateien nach `Contents/Resources/licenses/<paket>/` und schreibt Version, SPDX-Lizenz
  (`brew info --json`) und die zugehörigen Binärdateien in `index.json`. Findet es für ein Paket keinen
  Lizenztext, bricht das Paketieren ab.
- resvg (Apache-2.0/MIT, SVG im Decoder-Hilfsprozess) wird als `libresvg.0.48.dylib` aus Homebrews Paket
  `resvg` eingebettet; seine Lizenztexte (`LICENSE-APACHE`, `LICENSE-MIT`) kommen wie bei den anderen
  Paketen aus dem Keg. Ohne `brew install resvg` baut die App mit Qt SVG allein (`LF_ENABLE_RESVG=AUTO`);
  für Releases `-DLF_ENABLE_RESVG=ON` setzen. Wird `libresvg.a` statisch gelinkt, steht resvg in
  `static-components.json` (Texte in `packaging/licenses/resvg-*`).
- Little CMS und KImageFormats werden derzeit **nicht** verwendet und deshalb nicht aufgeführt.
- Sparkle (MIT) kommt nicht aus Homebrew: `macos_deploy.py` übernimmt `third_party/Sparkle/LICENSE` und die
  Version aus der `Info.plist` des Frameworks in `index.json`.
