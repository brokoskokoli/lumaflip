<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Lumaflip

Schneller, freier Bildbetrachter für macOS. Doppelklick auf ein Bild öffnet den ganzen Ordner –
in **derselben Reihenfolge wie im Finder** –, und mit ← / → blätterst du ohne Wartezeit durch
alle Bilder.

> Status: in Entwicklung (Meilenstein M1). Die Spezifikation steht in [SPEC.md](SPEC.md).

## Funktionen (M1)

- **Ganzer Ordner per Doppelklick:** Bild öffnen, mit den Pfeiltasten blättern; Nicht-Bilder werden übersprungen.
- **Finder-Reihenfolge:** übernimmt die Sortierung des Finder-Fensters (Name, Änderungsdatum, Größe,
  „Hinzugefügt am“ …); ohne Finder-Information wie die Finder-Namenssortierung.
- **Schnell:** Vorausladen der Nachbarbilder, Darstellung über Metal.
- **Viele Formate ohne Nachinstallation:** JPEG, PNG, GIF, BMP, TIFF, WebP, ICO, ICNS, HEIC/HEIF,
  AVIF, JPEG XL und RAW-Formate (CR2, CR3, NEF, ARW, DNG, RAF, ORF, RW2 …) über macOS ImageIO, dazu SVG.
- **Farbrichtig:** ICC-Profile, Display P3.
- **Zoom:** Einpassen, 100 %, Trackpad-Gesten und Tastatur.
- **Papierkorb mit Rückgängig:** ⌘⌫ legt die Datei in den Papierkorb, ⌘Z holt sie zurück.
- **Mehrere Fenster**, Einstellungen für Maus und Tastatur, helles und dunkles Erscheinungsbild.
- **Datenschutz:** keine Telemetrie, kein Netzwerkverkehr, kein Festplattenvollzugriff nötig.

## Installation

- **DMG:** Unter [Releases](https://github.com/brokoskokoli/lumaflip/releases) die Datei
  `Lumaflip-<version>.dmg` laden, öffnen und Lumaflip in den Ordner „Programme“ ziehen. Die App ist mit
  Developer ID signiert und von Apple notarisiert. Prüfsumme: `shasum -a 256 -c Lumaflip-<version>.dmg.sha256`.
- **Homebrew** (folgt): `brew install --cask brokoskokoli/tap/lumaflip`

Voraussetzung: macOS 15 (Sequoia) oder neuer, Apple-Silicon-Mac.

Beim ersten Öffnen eines Bildes aus Schreibtisch, Dokumenten, Downloads, von einem USB-Stick oder einem
Netzlaufwerk fragt macOS einmal nach Zugriff: Lumaflip liest den Ordner des geöffneten Bildes, damit du
blättern kannst.

## Aus dem Quellcode bauen

```sh
brew install cmake ninja qt googletest clang-format
tools/check.sh --quick                              # bauen + alle Tests
open build/check/src/Lumaflip.app --args <bild>     # starten
tools/package.sh                                    # eigenständiges App-Paket + DMG/ZIP in dist/ (ad-hoc signiert)
```

Details zu Signatur, Notarisierung und Releases: [docs/RELEASING.md](docs/RELEASING.md).
Hinweise für Mitwirkende: [CLAUDE.md](CLAUDE.md). Beiträge laufen über das DCO (`git commit -s`).

## Lizenz

Lumaflip ist freie Software unter der **GNU General Public License v3.0 oder später**
(GPL-3.0-or-later), siehe [LICENSE](LICENSE). Lumaflip verwendet Qt unter der LGPLv3 (dynamisch
gelinkt); die Lizenzen aller mitgelieferten Komponenten zeigt das Fenster
*Über Lumaflip → Lizenzen von Drittanbietern…*, die Texte liegen im App-Paket unter
`Contents/Resources/licenses`.
