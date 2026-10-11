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
| `tools/fetch_sparkle.sh` | Lädt Sparkle 2 (MIT) in der festgelegten Version, prüft die SHA-256 und entpackt nach `third_party/Sparkle/` (in `.gitignore`): Framework, `bin/generate_appcast`, `bin/sign_update`, `bin/generate_keys`, `LICENSE` |
| `tools/sparkle_key_check.sh` | Prüft, ob ein exportierter privater Sparkle-Schlüssel zum öffentlichen Schlüssel der App passt: Der öffentliche Schlüssel wird immer aus dem privaten **abgeleitet** (auch beim alten 96-Byte-Format, dessen gespeichertem Anhang nicht vertraut wird). `--print-public` gibt ihn aus, `--self-test` prüft das Skript mit Wegwerf-Schlüsseln aus OpenSSL (läuft in `tools/check.sh`) |
| `tools/appcast_verify.py` | Vergleicht neuen und alten Appcast vor dem Veröffentlichen: alle alten Einträge mit unveränderter Download-URL vorhanden, genau ein neuer Eintrag mit EdDSA-Signatur und passender Länge |
| `.github/workflows/release.yml` | Läuft bei Tag `v*` im Hauptrepo: Versionsabgleich → Prüfungen (`checks.yml`) + Schlüsselprüfung → Paket/Signatur/Notarisierung → Release als Entwurf → Appcast → Release veröffentlichen |
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
  aus dem Artefakt, dessen Prüfsumme vorher kontrolliert wird).
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
   Arbeitsverzeichnis, Release-Text des Entwurfs als HTML (GitHub-Markdown-API) daneben,
   `generate_appcast --ed-key-file - --maximum-versions 0 --maximum-deltas 0` – der private Schlüssel
   kommt nur über stdin (`printf` ist ein Shell-Builtin, erscheint also in keiner Argumentliste und wird
   nie ausgegeben). Alle vorhandenen Einträge bleiben erhalten, keine Delta-Updates.
5. **Prüfung vor dem Push:** `tools/appcast_verify.py` vergleicht alten und neuen Appcast (jeder alte
   Eintrag mit gleicher Version und gleicher Download-URL noch da; genau ein Eintrag mehr bzw. gleich
   viele bei einem erneuten Lauf; neuer Eintrag mit erwarteter URL, `sparkle:edSignature` und der Länge
   der DMG), `sign_update --verify` prüft die Feed-Signatur.
6. `appcast.xml` und `notes/<version>.html` werden als Commit auf `gh-pages` gepusht (`GITHUB_TOKEN` über
   `gh auth setup-git`, kein Token in einer URL). Schlägt der Push fehl (anderer Push dazwischen), wird bis
   zu fünfmal geholt, neu aufgesetzt (`rebase`) und erneut gepusht; hat sich dabei `appcast.xml` selbst
   geändert, bricht der Job ab (Appcast müsste neu erzeugt werden → Job erneut starten). Ein
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

## Homebrew Cask

Zunächst im eigenen Tap (`brokoskokoli/homebrew-tap`), später Antrag für `homebrew/cask` (SPEC 10.1):

1. `packaging/homebrew/lumaflip.rb` kopieren nach `Casks/lumaflip.rb` im Tap.
2. `version` setzen, `@SHA256_OF_DMG@` durch den Wert aus `Lumaflip-<version>.dmg.sha256` ersetzen.
3. Prüfen: `brew audit --cask --new lumaflip` und `brew install --cask brokoskokoli/tap/lumaflip`.

Installation für Nutzer danach: `brew install --cask brokoskokoli/tap/lumaflip`.

## Nach dem Release prüfen

Die nicht automatisierbaren Prüfungen (Gatekeeper beim echten Download, Offline-Start, TCC-Texte,
Lizenzfenster) stehen in [MANUAL_CHECKS.md](MANUAL_CHECKS.md) im Abschnitt „Auslieferung“.

## Release-Notizen

Der Workflow legt den Entwurf mit `--generate-notes` an; vor dem Veröffentlichen den Text des
Entwurfs durch die Notizen der Version ersetzen (Sparkle zeigt ihn als HTML im Update-Fenster).
Deutsch zuerst, Englisch darunter.

### 1.1.0 (Meilenstein M3)

**Neu**
- 18 Sprachen (folgt der Systemsprache, umschaltbar ohne Neustart; Übersetzungen noch ungeprüft).
- Aussortiermodus (⌘K) mit Sternen, Pick/Reject und automatischem Weiterblättern; Bewertungen als
  XMP-Zusatzdatei, im Bild oder als Finder-Tags; Filter nach Bewertung; Sammelaktion für verworfene Bilder.
- Schnellablage (`M`/`C` + `1`–`9`) mit Namenskonflikt-Abfrage und Rückgängig.
- Rasteransicht, Vergleich von 2–4 Bildern, Lupe, gekoppelte Fenster.
- HDR/EDR-Darstellung mit SDR-Vorschau und Belichtungsregler.
- Unterordner, Nachbarordner (⌥← / ⌥→), „Gehe zu Bild“, zufällige Reihenfolge, Schnellfilter (⌘F).
- RAW+JPEG-Paare als ein Bild (optional), GPS mit Karte nach Klick, Fenster mit allen Metadaten.
- Ordnerkarte (Darstellung → Karte, ⇧⌘M): alle Bilder mit Aufnahmeort auf einer Karte, zusammengefasst,
  aktuelles Bild folgt beim Blättern, Klick zeigt das Bild.
- SVG vollständig über resvg (CSS, Filter, Text), Qt SVG als Rückfall.
- Neue Formate: GIMP (XCF), Photoshop Large Document (PSB), QOI, PCX, PBM/PGM/PPM/PFM, SGI, OpenRaster,
  Krita; Decoder im Hilfsprozess.

**Hinweise**
- Bei RAW+JPEG gehört `IMG_1.xmp` der RAW-Datei (Lightroom); das JPEG nutzt `IMG_1.JPG.xmp`.
- Kartenausschnitte werden erst nach „Karte anzeigen“ von Apple geladen und nur bei sichtbarem
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
