# Lumaflip – Hinweise für die Entwicklung

Bildbetrachter für macOS (später Windows/Linux). Die verbindliche Spezifikation ist
[SPEC.md](SPEC.md). Jede Anforderung hat eine ID (z. B. `F-SORT-01`, `P-03`).

## Bauen und prüfen

```sh
tools/check.sh --quick          # Build + alle Tests (schnell, vor jedem Commit)
tools/check.sh --stage M1       # alle Pflichtprüfungen inkl. Format, SPDX, Spec-Abdeckung, Benchmarks
tools/check.sh --quick --sanitize   # ASan/UBSan
open build/check/src/Lumaflip.app --args <bild>   # App starten
```

Abhängigkeiten: `brew install cmake ninja qt googletest clang-format` (Qt 6.11 via Homebrew).
Bauverzeichnisse liegen unter `build/` – nicht löschen, `cmake --fresh` verwenden, falls nötig.

## Aufbau

| Pfad | Inhalt |
|---|---|
| `src/core/` | Plattformneutraler Kern (C++20, nur QtCore/QtGui): Collection, Sortierung, Decoder-Registry, ImageProvider (Prefetch/Cache), FileOps, Settings, InputBindings, .DS_Store-Parser |
| `src/core/Platform.h` | Plattform-Schnittstellen + Fabrikfunktionen |
| `src/platform/mac/` | macOS-Implementierungen in Objective-C++ (`.mm`, ARC): ImageIO-Decoder, Finder-Sortierung, Namensvergleich, Papierkorb, FSEvents, Shell |
| `src/platform/generic/` | Portable Fallbacks (für spätere Windows/Linux-Builds; werden auf macOS nicht gebaut) |
| `src/app/` | Qt-Widgets-UI: Application, ViewerWindow, ImageView (QRhi), Panels, Einstellungsdialog. `main.cpp` nur Einstieg |
| `src/app/shaders/` | GLSL-Shader (`qt_add_shaders`, Präfix `/shaders`) |
| `tests/unit/` | GoogleTest, linkt `lf_core` (ein Binary) |
| `tests/integration/tst_*.cpp` | Qt Test, linkt `lf_ui` (je Datei ein Binary) |
| `tests/bench/` | Benchmarks für P-xx (`lf_bench`) |
| `tests/corpus/` | Formatkorpus; `tests/corpus/make_corpus.sh` erzeugt Dateien nach `tests/corpus/generated/` |
| `tests/data/` | Kleine Testdaten (z. B. synthetische `.DS_Store`-Dateien) |

Neue Quelldateien werden per Glob automatisch gebaut (CMake `CONFIGURE_DEPENDS`).

## Konventionen

- Namespace `lf`. C++20. Qt-Typen bevorzugen (QString, QVector …). Keine Exceptions über Modulgrenzen.
- Jede Datei beginnt mit `// SPDX-License-Identifier: GPL-3.0-or-later`.
- UI-Texte auf Deutsch (später übersetzbar: `tr()` verwenden).
- **Rückverfolgbarkeit:** Tests nennen die Anforderungs-ID im Namen, z. B.
  `TEST(SortEngine, F_SORT_01_FinderNameOrder)` oder `void F_NAV_01_arrowKeys();`.
  Nicht automatisch testbare Anforderungen kommen mit Begründung in `docs/MANUAL_CHECKS.md`.
- Die GUI blockiert nie: Dateisystem, Dekodierung und Metadaten nur in Worker-Threads (QThreadPool/QtConcurrent).
- Fehler sichtbar machen statt verschlucken (Fehlerkarte, Statusmeldung).
- Keine privaten Nutzerdaten in Testdaten (keine echten `.DS_Store`-Dateien aus dem Home-Verzeichnis einchecken – synthetisch erzeugen).
- Öffentliche Header in `src/core/*.h` sind Schnittstellen zwischen Modulen: Änderungen klein halten und im Commit begründen.
- Eigene Quellen kompilieren mit `-Werror`.
