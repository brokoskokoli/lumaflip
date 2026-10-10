<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Lumaflip

Schneller, freier Bildbetrachter für macOS. Doppelklick auf ein Bild öffnet den ganzen Ordner –
in **derselben Reihenfolge wie im Finder** –, und mit ← / → blätterst du ohne Wartezeit durch
alle Bilder.

> Status: **Version 1.1 (Meilenstein M3) in der Abnahme.** Die Spezifikation steht in [SPEC.md](SPEC.md),
> die manuelle Abnahme in [docs/TESTING-M3.md](docs/TESTING-M3.md) (Version 1.0: [docs/TESTING-M2.md](docs/TESTING-M2.md)).

## Neu in Version 1.1

- **18 Sprachen:** Deutsch, Englisch, Französisch, Spanisch, Italienisch, Portugiesisch (BR/PT),
  Niederländisch, Polnisch, Schwedisch, Dänisch, Norwegisch, Finnisch, Tschechisch, Japanisch,
  Chinesisch (vereinfacht/traditionell), Koreanisch – folgt der Systemsprache, umschaltbar ohne Neustart
  (Übersetzungen noch nicht muttersprachlich geprüft).
- **Aussortieren und Bewerten:** Aussortiermodus (⌘K) mit 0–5 Sternen, Pick/Reject und automatischem
  Weiterblättern; Bewertungen als XMP-Zusatzdatei (Lightroom, Capture One, darktable), im Bild oder als
  Finder-Tags; Filter nach Bewertung; „Alle verworfenen in den Papierkorb / nach _Rejected“.
- **Schnellablage:** `M`/`C` + `1`–`9` verschiebt bzw. kopiert in feste Zielordner, mit Rückgängig und
  Namenskonflikt-Abfrage.
- **Raster, Vergleich, Lupe:** Rasteransicht mit Mehrfachauswahl, 2–4 Bilder synchron vergleichen,
  Lupe am Cursor; gekoppelte Fenster (blättern und zoomen gemeinsam).
- **HDR/EDR:** HDR-Fotos (iPhone-HEIC, Ultra-HDR-JPEG, AVIF, OpenEXR) mit echter Spitzlicht-Helligkeit
  auf XDR-Displays, SDR-Vorschau, Belichtungsregler.
- **Navigation:** Unterordner einbeziehen, Nachbarordner (⌥← / ⌥→), „Gehe zu Bild“ (⌘G), zufällige
  Reihenfolge, Schnellfilter nach Dateiname (⌘F).
- **RAW+JPEG-Paare** wahlweise als ein Bild (Bewerten, Verschieben, Löschen wirken auf beide).
- **GPS und Metadaten:** Koordinaten im Infopanel, Mini-Karte erst nach Klick, „In Karten öffnen“;
  Fenster mit allen Rohmetadaten (Suche, Kopieren).
- **Weitere Formate:** QOI, PCX, PBM/PGM/PPM/PFM, SGI, OpenRaster, Krita; Decoder in einem
  abgeschotteten Hilfsprozess (defekte Dateien bringen die App nicht zum Absturz).

## Funktionen (Version 1.0)

**Öffnen und Blättern**
- **Ganzer Ordner per Doppelklick:** Bild öffnen, mit ← / →, Trackpad-Wischen, Mausrad oder Leertaste
  blättern; Nicht-Bilder werden übersprungen. Startbildschirm mit „Zuletzt geöffnet“.
- **Finder-Reihenfolge:** übernimmt die Sortierung des Finder-Fensters (Name, Änderungsdatum, Größe,
  „Hinzugefügt am“, Symbolpositionen …), auf Wunsch live vom offenen Finder-Fenster; eigene Sortierung
  wird pro Ordner gemerkt.
- **Ordner bleibt aktuell:** neue, gelöschte und umbenannte Dateien erscheinen sofort, das aktuelle Bild bleibt.
- **Schnell:** Vorausladen der Nachbarbilder, Darstellung über Metal, flüssig auch auf ProMotion-Displays.

**Formate und Farbe**
- JPEG, PNG, GIF, BMP, TIFF, WebP, ICO, ICNS, HEIC/HEIF, AVIF, JPEG XL, RAW (CR2, CR3, NEF, ARW, DNG,
  RAF, ORF, RW2 …), PSD, OpenEXR, Radiance HDR, JPEG 2000, TGA, DDS, KTX und SVG (SVG Tiny 1.2) – ohne
  Nachinstallation.
- **Animationen** (GIF, APNG, WebP, AVIF/HEICS, JXL) mit Pause, Einzelbild und Tempo; **mehrseitige**
  Dateien (TIFF, ICO, HEIF) mit Seitennavigation; RAW zuerst als eingebettete Vorschau.
- **Farbrichtig:** ICC-Profile, Display P3, CMYK; Profilwechsel beim Verschieben auf einen anderen Monitor.

**Ansehen und Ordnen**
- Zoom-Modi (Einpassen, 100 %, Breite/Höhe füllen …), Zoom am Cursor, „Zoom fixieren“ für Serienaufnahmen,
  Vollbild, Diashow mit Überblendung.
- **Infopanel** mit Datei-, Bild- und Kameradaten, Histogramm und Finder-Tags; **Filmstreifen** mit Vorschaubildern.
- **Dateien:** Papierkorb mit Rückgängig, Umbenennen, „Öffnen mit“, Teilen, Bild in andere Apps ziehen,
  Drehung verlustfrei speichern (JPEG), Kopieren von Bild und Pfad.

**Bedienung und Mac-Integration**
- **Mehrere Fenster** mit Anordnen, Tabs und Wiederherstellung; natives Menü, Hell/Dunkel.
- **Eingaben frei belegbar:** Mausrad, Klick, Trackpad, alle Tastenkürzel (mit Konfliktprüfung),
  Profilvorlagen („Mac-Standard“, „Windows-Viewer-Gewohnheiten“, „Fotograf:in“); Einstellungen
  exportieren und importieren.
- **Standard-App** für Bilder per Knopf (Dateitypen einzeln abwählbar).
- **Automatische Updates** (Sparkle, signiert) – abschaltbar.
- **Datenschutz:** keine Telemetrie, Netzwerkverkehr nur für die abschaltbare Update-Prüfung und – erst
  nach einem Klick auf „Karte anzeigen“ – für Kartenausschnitte; kein Festplattenvollzugriff nötig.

## Installation

- **DMG:** Unter [Releases](https://github.com/brokoskokoli/lumaflip/releases) die Datei
  `Lumaflip-<version>.dmg` laden, öffnen und Lumaflip in den Ordner „Programme“ ziehen. Die App ist mit
  Developer ID signiert und von Apple notarisiert. Prüfsumme: `shasum -a 256 -c Lumaflip-<version>.dmg.sha256`.
- **Homebrew:** `brew install --cask brokoskokoli/tap/lumaflip` (eigener Tap; Updates danach über
  Sparkle in der App oder `brew upgrade --cask lumaflip`).

Voraussetzung: macOS 15 (Sequoia) oder neuer, Apple-Silicon-Mac.

Beim ersten Öffnen eines Bildes aus Schreibtisch, Dokumenten, Downloads, von einem USB-Stick oder einem
Netzlaufwerk fragt macOS einmal nach Zugriff: Lumaflip liest den Ordner des geöffneten Bildes, damit du
blättern kannst.

## Aus dem Quellcode bauen

```sh
brew install cmake ninja qt jpeg-turbo googletest clang-format
tools/fetch_sparkle.sh                              # optional: Sparkle für automatische Updates
tools/check.sh --quick                              # bauen + alle Tests
open build/check/src/Lumaflip.app --args <bild>     # starten
tools/package.sh                                    # eigenständiges App-Paket + DMG/ZIP in dist/ (ad-hoc signiert)
```

Details zu Signatur, Notarisierung und Releases: [docs/RELEASING.md](docs/RELEASING.md).
Hinweise für Mitwirkende: [CLAUDE.md](CLAUDE.md). Beiträge laufen über das DCO (`git commit -s`).

## Lizenz

Lumaflip ist freie Software unter der **GNU General Public License v3.0 oder später**
(GPL-3.0-or-later), siehe [LICENSE](LICENSE). Lumaflip verwendet Qt unter der LGPLv3 (dynamisch
gelinkt), Sparkle (MIT) für Updates und libjpeg-turbo (BSD-artig, statisch gelinkt) für das
verlustfreie Drehen; die Lizenzen aller mitgelieferten Komponenten zeigt das Fenster
*Über Lumaflip → Lizenzen von Drittanbietern …*, die Texte liegen im App-Paket unter
`Contents/Resources/licenses`.
