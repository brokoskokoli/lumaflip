# Übersetzungen (SPEC 4.16)

Lumaflips Oberfläche ist in **Deutsch** geschrieben (Ausgangssprache der Quelltexte) und in
17 weitere Sprachen übersetzt. Pro Sprache gibt es hier eine Qt-Linguist-Datei
`lumaflip_<sprache>.ts`:

| Datei | Sprache | Datei | Sprache |
|---|---|---|---|
| `lumaflip_en.ts` | Englisch | `lumaflip_da.ts` | Dänisch |
| `lumaflip_fr.ts` | Französisch | `lumaflip_nb.ts` | Norwegisch (Bokmål) |
| `lumaflip_es.ts` | Spanisch | `lumaflip_fi.ts` | Finnisch |
| `lumaflip_it.ts` | Italienisch | `lumaflip_cs.ts` | Tschechisch |
| `lumaflip_pt_BR.ts` | Portugiesisch (Brasilien) | `lumaflip_ja.ts` | Japanisch |
| `lumaflip_pt_PT.ts` | Portugiesisch (Portugal) | `lumaflip_zh_CN.ts` | Chinesisch (vereinfacht) |
| `lumaflip_nl.ts` | Niederländisch | `lumaflip_zh_TW.ts` | Chinesisch (traditionell) |
| `lumaflip_pl.ts` | Polnisch | `lumaflip_ko.ts` | Koreanisch |
| `lumaflip_sv.ts` | Schwedisch | `lumaflip_de.ts` | Deutsch: nur Pluralformen |

Der Stand der Prüfung je Sprache steht in [REVIEW_STATUS.md](REVIEW_STATUS.md). Die
Erstübersetzungen sind maschinell entstanden und **ungeprüft** – Korrekturen von
Muttersprachler:innen sind sehr willkommen.

## Wie es zusammenhängt

- Im Code laufen alle sichtbaren Texte über `tr()` / `QCoreApplication::translate()`
  (Objective-C++: `lf::mac::translate()`, `src/platform/mac/MacTranslate.h`).
  Code außerhalb von QObject-Klassen benutzt eine kleine `struct Tr { Q_DECLARE_TR_FUNCTIONS(lf::…) };`,
  damit `lupdate` den richtigen Kontext findet. Plural: `tr("%n Bilder", nullptr, n)`.
- `lupdate` liest die Quellen und aktualisiert die `.ts`-Dateien (nur auf Anforderung).
- `lrelease` erzeugt bei jedem Build die `.qm`-Dateien (`build/…/translations/`); sie landen
  im App-Paket unter `Contents/Resources/translations/`, zusammen mit Qts eigenen
  Übersetzungen (`qtbase_<sprache>.qm`, kopiert von `tools/macos_deploy.py`).
- Die Sprache folgt der Einstellung *Allgemein → Sprache*, sonst der Sprache pro App in den
  macOS-Systemeinstellungen, sonst den bevorzugten Systemsprachen, sonst Englisch
  (`src/core/Language.cpp`). Ein Wechsel wirkt sofort (Menüs, Einstellungen, Fenster).
- Zahlen, Größen und Datumsangaben richten sich nach der UI-Sprache in der Region des
  Systems (`src/core/LocaleFormat.cpp`); die Finder-Sortierung bleibt beim System-Gebietsschema.
- macOS-Texte aus der `Info.plist` (Berechtigungsabfragen, Dokumenttypen, UTI-Beschreibungen,
  Copyright) stehen in `packaging/macos/lproj/<sprache>.lproj/InfoPlist.strings` und werden
  dort **von Hand** gepflegt (nicht aus den `.ts`-Dateien erzeugt). Neuer Text in
  `Info.plist.in` (z. B. ein `UTTypeDescription`) → in allen 18 Dateien ergänzen;
  `tools/i18n_check.py` meldet fehlende und veraltete Einträge.
- Eine fest gewählte Sprache (*Allgemein → Sprache*) schreibt Lumaflip zusätzlich als
  `AppleLanguages` in die eigenen Einstellungen, damit auch die Texte, die macOS selbst
  baut („Vollbild aktivieren“, „Dienste“, Öffnen-Dialog, Berechtigungsabfragen), nach einem
  Neustart folgen; „Systemsprache“ entfernt den Eintrag wieder.

## Texte aktualisieren (Entwickler:innen)

Nach dem Ändern oder Hinzufügen von Texten im Code:

```sh
cmake --build build/check --target lf_lupdate   # .ts-Dateien aus den Quellen aktualisieren
cmake --build build/check                       # .qm-Dateien neu erzeugen (lrelease)
python3 tools/i18n_check.py --lupdate "$(brew --prefix qt)/bin/lupdate"
```

`lupdate` läuft mit `-locations none -no-obsolete` (keine Zeilennummern, kein Ballast, kleine
Diffs). Neue Texte sind danach in allen Sprachen „unfinished“ und erscheinen bis zur
Übersetzung auf Deutsch. `tools/check.sh` (auch `--quick --no-gui`) prüft

- Texte ohne `tr()` (Heuristik; Ausnahmen in `tools/i18n_allowlist.txt` mit Begründung),
- ob die `.ts`-Dateien zum Code passen (fehlende/veraltete Einträge, falsche Kontexte,
  Plural-Attribut),
- ob jede Übersetzung die Platzhalter der Quelle behält (`%1`, `%2` … in gleicher Anzahl;
  `%n` in mindestens einer Pluralform),
- ob jede Sprache zu mindestens 99 % übersetzt ist,
- ob jede `InfoPlist.strings` alle übersetzbaren Texte der `Info.plist.in` enthält.

Die Unit-Tests `I18n.F_I18N_*` prüfen außerdem Platzhalter (`%1`, `%n`), Pluralformen,
HTML, Tastenkürzel-Symbole und Leerzeichen in jeder Übersetzung.

## Eine Sprache prüfen oder verbessern (Übersetzer:innen)

1. Datei mit **Qt Linguist** öffnen (`brew install qt`, dann `open -a Linguist
   translations/lumaflip_fr.ts`) oder mit einem Texteditor bearbeiten.
2. Hinweise beachten: der *Kontext* (z. B. `lf::MainMenu` = Menüleiste) und die
   Entwicklerkommentare. Platzhalter `%1`, `%2`, `%n` müssen erhalten bleiben,
   Tastensymbole (⌘ ⌥ ⇧) ebenso. Die Begriffe der macOS-Oberfläche in der jeweiligen Sprache
   verwenden (Menünamen, „Papierkorb“, „Systemeinstellungen“ …). „Lumaflip“ wird nicht übersetzt.
3. Übersetzungen nicht als „unfinished“ markieren – Qt lädt solche Einträge nicht.
4. Bauen und `tools/check.sh --quick --no-gui` laufen lassen; ein Bildschirmfoto der
   Einstellungen in der Sprache hilft beim Review (*Allgemein → Sprache*).
5. In [REVIEW_STATUS.md](REVIEW_STATUS.md) die Sprache auf „geprüft“ setzen (mit Name/Handle
   und Datum) und einen Pull Request auf GitHub öffnen.

## Neue Sprache hinzufügen

Sprachcode in `src/core/Language.cpp` (mit Namen in der eigenen Sprache),
`src/CMakeLists.txt` (`LF_UI_LANGUAGES`), `tools/macos_deploy.py` (`UI_LANGUAGES`) und
`CFBundleLocalizations` in `packaging/macos/Info.plist.in` eintragen, CMake neu
konfigurieren (legt die `.ts`-Datei an), `lf_lupdate` ausführen, übersetzen,
`packaging/macos/lproj/<code>.lproj/InfoPlist.strings` anlegen, REVIEW_STATUS.md ergänzen.
