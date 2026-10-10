<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
[English](README.md) · **Deutsch**

<div align="center">

# Lumaflip

**Schneller, freier Bildbetrachter für macOS – Doppelklick auf ein Foto, dann mit ← / → ohne Wartezeit durch den ganzen Ordner, in Finder-Reihenfolge.**

[![CI](https://github.com/brokoskokoli/lumaflip/actions/workflows/ci.yml/badge.svg)](https://github.com/brokoskokoli/lumaflip/actions/workflows/ci.yml)
[![Lizenz: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE)
[![Plattform: macOS 15+](https://img.shields.io/badge/platform-macOS%2015%2B%20%C2%B7%20Apple%20Silicon-lightgrey.svg)](#installation)
[![Neueste Version](https://img.shields.io/github/v/release/brokoskokoli/lumaflip?include_prereleases&sort=semver)](https://github.com/brokoskokoli/lumaflip/releases)

</div>

> **Status:** Version 1.1 (Meilenstein M3) in der Abnahme – siehe [CHANGELOG.md](CHANGELOG.md).
> Die ausführliche Dokumentation ist auf Englisch ([README.md](README.md)); die Spezifikation
> [SPEC.md](SPEC.md) ist auf Deutsch.

## Warum Lumaflip?

Vorschau.app blättert nach einem Doppelklick **nicht** mit den Pfeiltasten durch die anderen Bilder
des Ordners. Lumaflip zeigt das Bild sofort, lädt den ganzen Ordner im Hintergrund, sortiert ihn
**wie das Finder-Fenster** und lädt die Nachbarbilder vor. Ohne Abo, ohne Konto, ohne Telemetrie.

## Funktionen (Auswahl)

- **Wie im Finder blättern:** Ordner per Doppelklick, Finder-Sortierung (auch live vom
  Finder-Fenster, Opt-in), Ordner bleibt aktuell, Unterordner, Nachbarordner (⌥← / ⌥→),
  „Gehe zu Bild“ (⌘G), Schnellfilter (⌘F), mehrere und gekoppelte Fenster.
- **Formate ohne Nachinstallation:** JPEG, PNG, GIF, WebP, TIFF, HEIC/HEIF, AVIF, JPEG XL, RAW,
  PSD/PSB, XCF, OpenEXR, Radiance HDR, JPEG 2000, SVG (resvg), QOI, PCX, PNM, SGI, OpenRaster,
  Krita u. v. m.; Animationen und mehrseitige Dateien.
- **Farbe und HDR/EDR:** ICC-Profile, Display P3, CMYK; HDR-Fotos mit echter Spitzlicht-Helligkeit
  auf XDR-Displays, SDR-Vorschau, Belichtungsregler.
- **Aussortieren und Bewerten:** Aussortiermodus (⌘K), Sterne, Pick/Reject, Bewertungen als
  Lightroom-kompatible XMP-Zusatzdatei, im Bild oder als Finder-Tags; Schnellablage
  `M`/`C` + `1`–`9`.
- **Raster, Vergleich (2–4 Bilder synchron), Lupe, Ordnerkarte (⇧⌘M), Infopanel mit Histogramm.**
- **18 Sprachen**, Wechsel ohne Neustart (maschinell übersetzt, Korrekturen willkommen).
- **Datenschutz:** keine Telemetrie, kein Festplattenvollzugriff; seltene Formate werden in einem
  abgeschotteten Hilfsprozess dekodiert.

Alle Funktionen und Tastenkürzel: [README.md](README.md#features) (Englisch).

## Installation

Voraussetzung: macOS 15 (Sequoia) oder neuer, Apple-Silicon-Mac.

- **DMG:** unter [Releases](https://github.com/brokoskokoli/lumaflip/releases) laden, öffnen,
  Lumaflip nach „Programme“ ziehen (signiert und notarisiert).
- **Homebrew** *(geplant)*: `brew install --cask brokoskokoli/tap/lumaflip`, nach Aufnahme in
  `homebrew/cask` auch `brew install --cask lumaflip`.

### Linux (Vorschau)

Für x86_64-Linux (glibc 2.39+, z. B. Ubuntu 24.04 oder neuer) baut die CI ein **AppImage**
(Artefakt `Lumaflip-linux-x86_64-AppImage` des Jobs „Linux app“): `chmod +x
Lumaflip-<version>-x86_64.AppImage`, dann starten. *Ablage → Als Standard-App festlegen …* macht
es zum Bildbetrachter. HEIC braucht einen separat installierten HEVC-Decoder; Live Text, Quick
Look, Karten und HDR (EDR) gibt es nur auf dem Mac. Details, Bauen und Sandbox:
[docs/LINUX.md](docs/LINUX.md) (Englisch).

## Aus dem Quellcode bauen

```sh
brew install cmake ninja qt jpeg-turbo googletest clang-format resvg
tools/run.sh [bild]                    # nur die App bauen und starten
tools/check.sh --quick --no-gui        # Build + Unit-Tests ohne Fenster
```

Linux: `cmake -S . -B build/linux -G Ninja && cmake --build build/linux`, AppImage mit
`tools/linux_appimage.sh build/linux dist` (Abhängigkeiten in [docs/LINUX.md](docs/LINUX.md)).

## Mitmachen

Siehe [CONTRIBUTING.md](CONTRIBUTING.md), [Verhaltenskodex](CODE_OF_CONDUCT.md),
[Sicherheit](SECURITY.md) und [Übersetzungen](translations/README.md).

## Lizenz

GPL-3.0-or-later, siehe [LICENSE](LICENSE). Drittanbieter-Lizenzen: *Über Lumaflip → Lizenzen von
Drittanbietern …* bzw. `Lumaflip.app/Contents/Resources/licenses/`.
