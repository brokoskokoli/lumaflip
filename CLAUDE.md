# Lumaflip – Hinweise für die Entwicklung

Bildbetrachter für macOS (später Windows/Linux). Die verbindliche Spezifikation ist
[SPEC.md](SPEC.md). Jede Anforderung hat eine ID (z. B. `F-SORT-01`, `P-03`).

## Bauen und prüfen

**Wichtig: Der Rechner ist der Arbeitslaptop des Nutzers.** Integrationstests (`tst_*`), Benchmarks (`lf_bench`),
Sanitizer-Läufe und Lasttests öffnen echte Fenster bzw. belasten die Maschine – **nur nach ausdrücklicher Rückfrage**
ausführen. Ohne Rückfrage erlaubt: Bauen und `tools/check.sh --quick --no-gui` (nur Unit-Tests).

```sh
tools/run.sh [bild]             # nur die App bauen und starten (keine Tests) – zum Ausprobieren
tools/check.sh --quick --no-gui # Build + Unit-Tests ohne Fenster
```

Vollständige Prüfungen (nach Rückfrage):

```sh
tools/check.sh --quick          # Build + alle Tests (schnell, vor jedem Commit)
tools/check.sh --stage M4       # alle Pflichtprüfungen inkl. Format, SPDX, Spec-Abdeckung, Benchmarks
                                # (Stufe: Standard M4 bzw. $LF_STAGE; CI: LF_STAGE in checks.yml)
tools/check.sh --quick --sanitize   # ASan/UBSan
tools/check.sh --format-all     # clang-format für alle Dateien (Standard: nur geänderte Zeilen ggü. main)
tools/check.sh --quick --tidy   # zusätzlich clang-tidy (.clang-tidy) auf geänderten Dateien, falls installiert
python3 tools/spec_coverage.py --verbose   # wo welche Anforderungs-ID getestet wird
cmake -B build/generic -DLF_FORCE_GENERIC_PLATFORM=ON …  # Core mit src/platform/generic (wie Linux-CI)
open build/check/src/Lumaflip.app --args <bild>   # App starten
```

Abhängigkeiten: `brew install cmake ninja qt jpeg-turbo googletest clang-format` (Qt 6.11 via Homebrew).
Optional `tools/fetch_sparkle.sh` (Sparkle 2 nach `third_party/`, automatische Updates F-MAC-08;
ohne baut die App mit abgeschaltetem Updater, siehe `docs/RELEASING.md`).
Bauverzeichnisse liegen unter `build/` – nicht löschen, `cmake --fresh` verwenden, falls nötig.

- **Aktuelle Stufe:** `LF_STAGE` in `.github/workflows/checks.yml` (derzeit `M4`) bestimmt, bis zu
  welcher Stufe die Spec-Abdeckung vollständig sein muss; beim Start der nächsten Stufe nur dort
  (und als Standard in `tools/check.sh`) anheben.
- **Benchmarks unter Last:** Vor `lf_bench` wartet `check.sh` höchstens `LF_BENCH_LOAD_WAIT`
  Sekunden (Standard 300, `0` = nicht warten), bis die 1-Minuten-Last ≤ Anzahl der Kerne ist
  (parallele Builds verfälschen die Zeiten). Überschreitet ein Benchmark sein Limit, wird nur
  dieser einmal wiederholt (`LF_BENCH_ONLY=name1,name2 build/check/tests/lf_bench`); beide
  Ergebnisse stehen in der Ausgabe (`build/check/bench.log`, `bench-rerun.log`). Fehlschlag nur,
  wenn auch die Wiederholung scheitert.
- **Finder-Fixtures:** `LF_REQUIRE_FINDER_FIXTURES=1` lässt den Fixture-Test fehlschlagen statt
  überspringen; die CI setzt es automatisch, sobald `tests/data/finder/` existiert
  (`tools/make_finder_fixtures.sh`, docs/TESTING-M1.md §4).
- **Release-Werkzeuge:** `tools/package.sh` (verlangt das statische `libturbojpeg.a`,
  `brew install jpeg-turbo`; Sparkle lokal optional), `tools/sparkle_key_check.sh --self-test`
  (läuft in `check.sh`), `tools/appcast_verify.py` – Details in `docs/RELEASING.md`.
- **Manuelle Abnahme:** `docs/TESTING-M3.md` (v1.1), `docs/TESTING-M2.md` (v1.0), `docs/TESTING-M1.md`, `docs/MANUAL_CHECKS.md`.

## Aufbau

| Pfad | Inhalt |
|---|---|
| `src/core/` | Plattformneutraler Kern (C++20, nur QtCore/QtGui): Collection, Sortierung, Decoder-Registry, ImageProvider (Prefetch/Cache), FileOps, Settings, InputBindings, .DS_Store-Parser |
| `src/core/Platform.h` | Plattform-Schnittstellen + Fabrikfunktionen |
| `src/platform/mac/` | macOS-Implementierungen in Objective-C++ (`.mm`, ARC): ImageIO-Decoder, Finder-Sortierung, Namensvergleich, Papierkorb, FSEvents, Shell |
| `src/platform/generic/` | Portable Fallbacks (Linux-CI baut damit den Core ohne App, `-DLF_BUILD_UI=OFF`; auf macOS nur mit `-DLF_FORCE_GENERIC_PLATFORM=ON`) |
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
<!-- REUSE-IgnoreStart -->
- Jede Datei beginnt mit `// SPDX-License-Identifier: GPL-3.0-or-later`.
<!-- REUSE-IgnoreEnd -->
- UI-Texte auf Deutsch als Ausgangssprache, immer über `tr()` (Mehrsprachigkeit, SPEC 4.16):
  ganze Sätze mit `%1` statt Verkettung, Plural mit `tr("%n …", nullptr, n)`. Außerhalb von
  QObject-Klassen eine `struct Tr { Q_DECLARE_TR_FUNCTIONS(lf::Kontext) };` (sonst falscher
  lupdate-Kontext); in `.mm`-Dateien `QCoreApplication::translate("lf::…", …)` bzw.
  `lf::mac::translate()` für NSString. Tabellen mit `QT_TRANSLATE_NOOP`, übersetzt erst beim
  Anzeigen (Sprachwechsel zur Laufzeit). Zahlen/Größen/Daten über `core/LocaleFormat.h`.
  Nach Textänderungen `cmake --build build/check --target lf_lupdate` (aktualisiert
  `translations/*.ts`); `tools/i18n_check.py` (läuft in `check.sh`) meldet Texte ohne `tr()`
  und veraltete Kataloge. Details: [translations/README.md](translations/README.md).
- **Rückverfolgbarkeit:** Tests nennen die Anforderungs-ID im Namen, z. B.
  `TEST(SortEngine, F_SORT_01_FinderNameOrder)`, `void F_NAV_01_arrowKeys();` oder
  `LF_BENCH(P_05_…)`. Nur Testnamen zählen, keine Kommentare.
  Nicht automatisch testbare Anforderungen kommen mit Begründung in `docs/MANUAL_CHECKS.md`.
- Die GUI blockiert nie: Dateisystem, Dekodierung und Metadaten nur in Worker-Threads (QThreadPool/QtConcurrent).
- Fehler sichtbar machen statt verschlucken (Fehlerkarte, Statusmeldung).
- Keine privaten Nutzerdaten in Testdaten (keine echten `.DS_Store`-Dateien aus dem Home-Verzeichnis einchecken – synthetisch erzeugen).
- Öffentliche Header in `src/core/*.h` sind Schnittstellen zwischen Modulen: Änderungen klein halten und im Commit begründen.
- Eigene Quellen kompilieren mit `-Werror`.

## Commits und Veröffentlichung

- Autor/Committer ist immer `Stefan Richter <16724498+brokoskokoli@users.noreply.github.com>` (in der
  repo-lokalen git-Konfiguration gesetzt – nie die globale Konfiguration verwenden).
- Commit-Nachrichten enden mit `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`; sonst keine E-Mail-Adressen.
- Keine Firmennamen oder Firmen-E-Mail-Adressen in Dateien oder Historie. Vor jedem Push prüfen:
  `git log --format='%an %ae %cn %ce %B'` und `git log -p --format=` auf fremde E-Mail-Adressen.
- Repository: `github.com/brokoskokoli/lumaflip`.
