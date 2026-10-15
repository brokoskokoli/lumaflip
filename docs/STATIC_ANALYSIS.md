<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Statische Code-Analyse – Bericht vom 15.10.2026

Stand: `main` @ `a185709` (Version 1.2.2), macOS, Build `build/check`.

## Werkzeuge

| Werkzeug | Version | Umfang | Aufruf | Laufzeit |
|---|---|---|---|---|
| clang-tidy (inkl. Clang Static Analyzer) | LLVM 22.1.8 (Homebrew `llvm@22`) | 280 eigene `.cpp`/`.mm` (alles, was der Build übersetzt) | `tools/check.sh --tidy-all` | ca. 7,5 min (6 Prozesse) |
| Cppcheck | 2.22.0 | 234 `.cpp` (ohne `.mm`, Cppcheck kann kein Objective-C++) | `tools/cppcheck.sh` bzw. `tools/check.sh --cppcheck` | ca. 6 s |

**PC-lint Plus** (Vector) kostet Geld. Es gibt eine 14-Tage-Testversion für macOS (nur Kommandozeile),
die Lizenz muss man aber über ein Formular anfordern, sie kommt per E-Mail
([pclintplus.com/downloads](https://pclintplus.com/downloads/),
[Vector](https://www.vector.com/en/product/pc-lint-plus/pc-lint-plus-downloads/)). Deshalb sind hier
die beiden freien Werkzeuge verwendet. Sobald eine Testlizenz da ist, lässt sich PC-lint Plus
mit derselben `compile_commands.json` dazunehmen.

clang-tidy war schon eingerichtet (`.clang-tidy`, CI: `--tidy`), prüft dort aber nur die
**geänderten** Dateien. Neu ist Cppcheck als zweites, unabhängiges Werkzeug:
`tools/cppcheck.sh` (Unterdrückungen in `tools/cppcheck-suppressions.txt`), das sich über
`tools/check.sh --cppcheck` aufrufen lässt. Es meldet seine Funde nur und lässt den Lauf nicht
scheitern (siehe Empfehlung).

## Ergebnis in Kürze

- **Keine bestätigten Fehler** (Absturz, Speicherfehler, falsches Verhalten). Alle Treffer der
  Klassen „error“ und „warning“ wurden von Hand geprüft: Es sind Fehlalarme oder bewusste
  Konstruktionen.
- clang-tidy: **81 Meldungen** in Dateien, die die CI bisher nicht als Ganzes geprüft hat. Größtenteils
  Performance- und Modernisierungs-Hinweise, dazu eine kleine echte Unsauberkeit (Nullable-Pointer
  an ObjC-API).
- Cppcheck: **524 Meldungen**, davon 435 reiner Stil (`useStlAlgorithm`, `shadowFunction`,
  `const`-Hinweise). Ohne die Qt-Header (die Cppcheck nicht liest) entstehen viele Fehlalarme.

## clang-tidy (81)

| Check | Anzahl | Bewertung |
|---|---:|---|
| `bugprone-throwing-static-initialization` | 27 | Statische `QString`/`QStringList`/`QHash`-Konstanten. Wirft nur bei Speichermangel beim Programmstart – unkritisch. **Vorschlag:** Check abschalten (Qt-Stil). |
| `performance-no-automatic-move` | 15 | `const` lokale Variable wird zurückgegeben → Kopie statt Move (9× `src/core/FileBatch.cpp`). Billig bei implizit geteilten Qt-Typen. Leicht zu beheben (`const` entfernen). |
| `modernize-loop-convert` | 10 | Index-Schleifen → range-for (v. a. `tests/bench`, `tests/unit/test_resvg.cpp`, `src/core/Xmp.cpp`). |
| `performance-unnecessary-value-param` | 8 | `std::function`/`shared_ptr` per Wert: `EdrDisplay_mac.mm:55`, `ScreenProfileWatcher_mac.mm:26`, `FileBatch.cpp:544`, `Platform.cpp:73-74`, 3× Tests. |
| `bugprone-assignment-in-if-condition` | 6 | Gewollt (`if ((tier1 = …))`, `FinderViewSettings.cpp:319`, 5× `tst_culling.cpp`). |
| `performance-implicit-conversion-in-loop` | 3 | `QJsonValueRef` in Tests. |
| `clang-analyzer-nullability.NullablePassedToNonnull` | 3 | **`src/app/OpenWith_mac.mm:21`**: `appUrl.path` kann `nil` sein und geht an `displayNameAtPath:` – sollte abgefangen werden. 2× in `test_m4_export_mac.mm`. |
| `bugprone-integer-division` | 2 | `tests/unit/test_m4_export_mac.mm:176` – Ganzzahldivision im Gleitkomma-Kontext prüfen. |
| `bugprone-casting-through-void` | 2 | `(__bridge void*)` → `WId`: so bei ARC üblich; Fehlalarm. |
| `clang-analyzer-core.CallAndMessage` | 1 | `tst_m3_collection_info.cpp:224` – Fehlalarm (`QVERIFY(next && prev)` steht davor). |
| `bugprone-random-generator-seed` | 1 | `SlideshowController.cpp:11` – Fehlalarm, `m_rng.seed(options.seed)` folgt beim Start. |
| sonstige (`no-int-to-ptr`, `unnecessary-copy-initialization`, `inefficient-vector-operation`) | 3 | Tests, unkritisch. |

Verteilung: `src/core` 22, `src/app` 9, `tests` 50. In `src/platform/mac` keine Meldungen.

Hinweis: In der CI läuft das aktuelle `llvm` (neuer als lokal 22), dort können weitere Checks
dazukommen.

## Cppcheck (524)

| Schwere | Anzahl | Bewertung |
|---|---:|---|
| error | 4 | Alle Fehlalarme (siehe unten). |
| warning | 34 | `uninitMemberVarNoCtor` bei Aggregaten, die immer per `{…}` initialisiert werden; 3× `nullPointerRedundantCheck` nach `QFAIL` (Cppcheck kennt das Makro-`return` nicht); 1× `noDestructor` (Singleton `OpenWithService`, lebt bis Programmende). |
| performance | 38 | Fast nur `returnByReference`-Hinweise; 2× `passedByValue` (`LosslessTransform.cpp:829`, `QuickMoveController.cpp:111`). |
| portability | 13 | `CastIntegerToAddressAtReturn` bei Gettern für Qt-Zeiger – Fehlalarm, weil die Qt-Typen unbekannt sind. |
| style | 435 | `useStlAlgorithm` 123, `shadowFunction` 108, `const`-Hinweise ~135, `functionStatic` 29 … |

Geprüfte „error“-Treffer:

- `LosslessTransform.cpp:64` `containerOutOfBounds` – `kOrient` hat 9 Einträge (Index 0 ungenutzt),
  Cppcheck hält `Mat` (= `std::array<int,4>`) für das Array. **Fehlalarm.**
- `test_review3.cpp:345` `containerOutOfBounds` – `waitUntil(…!screens.isEmpty())` steht davor. **Fehlalarm.**
- `tst_review3.cpp:463` `danglingLifetime` – `qScopeGuard` setzt `trash->gate` vor Ende des Gültigkeitsbereichs zurück. **Fehlalarm.**
- `UndoBackupStore.cpp:55` `unknownMacro` – `Q_COREAPP_STARTUP_FUNCTION` (Konfigurationssache).

Kleine echte Aufräumpunkte im Produktcode:

- `src/app/ViewerWindow.cpp:493/498` – zweimal hintereinander `if (visible)` (zusammenfassen).
- `src/app/Filmstrip.cpp:194`, `src/app/GridView.cpp:88`, `src/core/LibArchive.cpp:216` – überflüssige Initialisierung.
- `src/core/Xmp.cpp:107` – `xml[end - 1] == u'>'` ist durch die Prüfung zwei Zeilen davor immer wahr.
- `src/app/ExportDialog.h:35` – Ein-Argument-Konstruktor ohne `explicit`.
- `src/core/FolderMap.cpp:272/276`, `src/core/SimpleDecoders.cpp:130/131` – Member ohne Default-Initialisierung (heute immer gesetzt, defensiv `= 0`/`= nullptr`).

Die übrigen `knownConditionTrueFalse`-Treffer in den Integrationstests (`QTRY_*`, Ereignisse
ändern den Zustand zwischen zwei Aufrufen) sind Fehlalarme.

## Empfehlung

1. **Sofort (klein):** `OpenWith_mac.mm:21` gegen `nil` absichern; die Aufräumpunkte oben und die
   8 `unnecessary-value-param`/15 `no-automatic-move` beheben – danach ist `--tidy-all` auf `src/`
   bis auf `throwing-static-initialization` sauber.
2. **`.clang-tidy`:** `bugprone-throwing-static-initialization` abschalten (wie schon
   `exception-escape`); `assignment-in-if-condition` abschalten oder die Stellen umschreiben.
   Dann `--tidy-all` gelegentlich (z. B. vor Releases) laufen lassen, damit sich in unveränderten
   Dateien nichts ansammelt.
3. **Cppcheck** als Zusatzwerkzeug behalten (schnell, andere Analyse), aber nicht als harte
   CI-Hürde: Ohne Qt-Header ist die Fehlalarmquote hoch. Falls doch gewünscht: zuerst die
   Stil-Klassen `useStlAlgorithm`, `shadowFunction`, `functionStatic`, `returnByReference` in
   `tools/cppcheck-suppressions.txt` abschalten, dann nur `error`/`warning` als Hürde nehmen.
4. **PC-lint Plus:** Testlizenz anfordern, wenn ein Vergleich gewünscht ist (MISRA/AUTOSAR-artige
   Regeln, die clang-tidy nicht hat).

Rohdaten: `build/cppcheck/report.txt` (Cppcheck); clang-tidy-Ausgabe über `tools/check.sh --tidy-all`.

## Umgesetzt (15.10.2026)

- `OpenWith_mac.mm`: `nil`-Pfad abgefangen; `ViewerWindow::setVisible` – doppelte `if (visible)`
  zusammengefasst; `Xmp.cpp` – überflüssige Prüfung entfernt; `Filmstrip`/`GridView` – überflüssige
  Initialisierung; `ExportDialog` – `explicit`; Startwerte für `ScanItem`, `PnmCursor`,
  `FileOps::Private::stack`.
- Performance: `const` bei zurückgegebenen lokalen Variablen entfernt (15 Stellen),
  `runBatchAsync` nimmt `const std::shared_ptr<FileBatch>&` (öffentlicher Header
  `src/core/FileBatch.h`, nur der Parametertyp), `Shell::makeDefaultViewerAsync` verschiebt `done`.
- Tests: `nil`-Prüfung in `test_m4_export_mac.mm`, `QJsonValue`-Schleifen, `reserve()`.
- `.clang-tidy`: `bugprone-throwing-static-initialization` und `bugprone-assignment-in-if-condition`
  aus (Begründung in der Datei); `modernize-loop-convert` schreibt Rückwärts-Schleifen nicht mehr um
  (`std::views::reverse` würde Qt-Container ablösen).
- Mit `NOLINT` und Begründung belassen: `__bridge`-Umwandlungen (ARC), der Seed der Diashow,
  ein Fehlalarm nach `QVERIFY`, `std::function` per Wert, wo ein Objective-C-Block oder eine
  virtuelle Signatur es verlangt. **Achtung:** In `test_review3_mac.mm` war die Empfehlung von
  clang-tidy (`const&`) falsch – ein Block, der später läuft, erfasst eine Referenz als Referenz;
  die Tests stürzten ab. Dort bleibt die Kopie.
- Nicht geändert: `LibArchive.cpp:216` (defensive Initialisierung), `LosslessTransform.cpp:829` und
  `QuickMoveController.cpp:111` (`QString`/`QVector` per Wert sind implizit geteilt, wie in
  `.clang-tidy` erlaubt).

Danach: `tools/check.sh --tidy-all` ohne Meldungen (LLVM 22), Cppcheck 514 statt 524 Meldungen
(nur noch Stil und Fehlalarme).

## PC-lint Plus (15.10.2026)

PC-lint Plus 2025 SP1 mit Testlizenz (bis 29.10.2026), macOS-Binary (x86_64, läuft über Rosetta 2).
Nicht im Repository: Distribution nach `third_party/pclp/` entpacken (git-ignoriert), Lizenzdatei
daneben legen, dann `tools/pclint.sh --report` (ca. 9 min, 234 `.cpp`, Bericht
`build/pclp/report.txt`; einzelne Dateien mit `LF_PCLP_ONLY=src/core/Xmp`).

Einrichtung:

- `tools/pclint.sh` erzeugt mit dem mitgelieferten `pclp_config.py` (Python-Module in
  `build/pclp/venv`) die Compiler-Konfiguration (Apple Clang, C++20, SDK) und die Projekt-
  Konfiguration aus `compile_commands.json` (gleiche Dateiauswahl wie Cppcheck). Qt-Frameworks
  werden über einen Ordner mit Verweisen (`build/pclp/frameworks/QtCore` → `…/QtCore.framework/Headers`)
  gefunden, weil PC-lint keine Framework-Suche kennt.
- `tools/pclint_builtins.h`: Ersatz für Builtins der libc++ von Clang 21 (`__builtin_clzg` …),
  die PC-lint 2025 nicht kennt, und `#undef __ARM_NEON` (sonst ca. 11 000 Fehler aus
  `arm_neon.h`/`__fp16`).
- `tools/pclint.lnt`: Profil wie `.clang-tidy` – Fehler und Warnungen, dazu `const&`-Hinweise mit
  denselben Qt-Ausnahmen; aus wie dort: Qt-Eltern-Besitz („Leck“), Zahl-Umwandlungen, werfende
  statische Initialisierung. Zusätzlich aus, weil PC-lint moc-Code, `.mm`-Dateien und
  `QMutexLocker` nicht sieht (Begründungen in der Datei).

Ergebnis: 1 513 Meldungen, kein Fehler. Umgesetzt:

- `ImageCanvas`: Ergebnis von `QRhi…::create()` wird geprüft (14 Stellen). Fehlschlag wird
  geloggt; die Kachel bzw. das Overlay wird weggelassen, eine LUT fällt auf die Dummy-LUT zurück,
  fehlende Grundressourcen → nur Hintergrund und neuer Versuch beim nächsten `initialize()`.
- Verschattete Variablen umbenannt (`SimpleDecoders.cpp` `needBytes`, `ImageProvider.cpp` `failedStage`);
  `192ll` → `192LL` (+ 3 in Tests).

Nicht umgesetzt (Fehlalarme oder Absicht): ignorierte Rückgabewerte (1 037, Menüaufbau, Aufräumen,
klebrige Lesefehler), „nicht initialisiert“ bei `std::vector`/`unique_ptr` (96), Null-Zeiger, die
der Ablauf ausschließt (41), „immer wahr/falsch“ wegen `(c ? a : b) << x` und Build-Schaltern (89),
Rechnungen in Decodern, die die Größengrenzen (F-FMT-02) beschränken (36), 32-Bit-Prüfungen in
`DecoderIpc.cpp`, `render()` (von `QRhiWidget` vorgegeben), `return` nach vollständigem `switch`,
Qt-Test- und Benchmark-Makros.
