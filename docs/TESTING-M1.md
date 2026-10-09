<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Manuelle Abnahme M1 (MVP)

Diese Anleitung führt Schritt für Schritt durch die Prüfungen, die nur ein Mensch sinnvoll
machen kann: echtes Öffnen aus dem Finder, Finder-Sortierung in eigenen Ordnern,
Datenschutzabfragen, Gefühl beim Blättern, Farbeindruck. Alles andere prüfen die
automatischen Tests (`tools/check.sh`). Grundlage sind SPEC.md Abschnitt 3
(Nutzerszenarien) und 12.1 (Abnahmekriterien M1). Die einzelnen nicht automatisierbaren
Anforderungen stehen zusätzlich in `docs/MANUAL_CHECKS.md`.

Hake jeden Punkt ab. Wenn etwas nicht passt: Schritt, erwartetes und tatsächliches
Ergebnis notieren (siehe Abschnitt 8).

---

## 1. Bauen und starten

```sh
brew install cmake ninja qt googletest clang-format   # einmalig
tools/check.sh --quick            # Build + alle Tests (ca. 30 s)
tools/check.sh --stage M1         # zusätzlich Format, SPDX, Spec-Abdeckung, Benchmarks
tests/corpus/make_corpus.sh       # Testbilder nach tests/corpus/generated/ (macht check.sh automatisch)
```

App direkt starten (mit einem Bild als Argument):

```sh
open build/check/src/Lumaflip.app --args "$PWD/tests/corpus/generated/srgb.png"
```

Damit der Finder Lumaflip unter „Öffnen mit“ anbietet, die App einmal an einen festen Ort
kopieren und von dort starten:

```sh
mkdir -p ~/Applications
ditto build/check/src/Lumaflip.app ~/Applications/Lumaflip.app
open ~/Applications/Lumaflip.app
```

- [ ] Die App startet ohne Bild mit einem leeren Fenster: „Bild oder Ordner hierher ziehen“
      und Knopf „Öffnen…“.
- [ ] Im Dock erscheint das Lumaflip-Symbol, die Menüleiste zeigt „Lumaflip, Ablage,
      Bearbeiten, Darstellung, Navigation, Bild, Fenster, Hilfe“.

## 2. Lumaflip als Standard-App für JPEG und HEIC festlegen

Über den Finder (ändert nur die Zuordnung für diesen Dateityp, jederzeit umkehrbar):

1. Im Finder eine **JPEG**-Datei auswählen, **⌘I** (Ablage → Informationen).
2. Bereich **„Öffnen mit:“** aufklappen, im Menü **Lumaflip** wählen (ggf. „Andere…“ →
   `~/Applications/Lumaflip.app`).
3. **„Alle ändern…“** klicken und mit „Fortfahren“ bestätigen.
4. Dasselbe für eine **HEIC**-Datei wiederholen (optional PNG, WebP, RAW wie CR3/NEF/ARW/DNG).

Rückgängig: dieselben Schritte mit „Vorschau“ statt Lumaflip.
(Alternativ: Ablage → „Als Standard-App festlegen…“ bzw. Einstellungen → Formate.
Das setzt die Zuordnung für alle gängigen Bildtypen auf einmal.)

- [ ] Doppelklick auf eine JPEG- und eine HEIC-Datei öffnet Lumaflip.

## 3. Kernfall: Doppelklick → Ordner blättern (SPEC 3.1, 12.1 Nr. 1, 2, 5)

Testordner: eigener Fotoordner mit gemischtem Inhalt (Bilder, PDFs, Textdateien,
Unterordner) oder `tests/corpus/generated/`.

- [ ] Doppelklick auf ein Bild in der Mitte des Ordners: **genau dieses Bild** erscheint
      sofort (gefühlt ohne Wartezeit, P-01: deutlich unter 0,5 s bei einem 24-MP-JPEG,
      auch wenn Lumaflip vorher nicht lief).
- [ ] Titelleiste: `Dateiname — 23 / 412`. Die Position erscheint, sobald der Ordner
      gelesen ist (bei großen Ordnern evtl. einen Augenblick später).
- [ ] Statuszeile unten: Dateiname · Position · Pixelmaße · Dateigröße · Zoom in % ·
      rechts die Sortierung (z. B. „Wie Finder: Name ↑“).
- [ ] → zeigt das nächste, ← das vorherige Bild – ohne spürbare Verzögerung (P-03).
- [ ] Nicht-Bilder (PDF, TXT, Unterordner, versteckte Dateien, `._*`) werden übersprungen.
- [ ] Am Ende hält das Blättern an und zeigt kurz „Letztes Bild“ (am Anfang „Erstes Bild“).
- [ ] Pos1/Ende sowie ⌘↑/⌘↓ springen zum ersten/letzten Bild; Bild↑/Bild↓, Leertaste/
      ⇧Leertaste blättern ebenfalls.
- [ ] → **gedrückt halten** in einem Ordner mit vielen großen Fotos: die Bilder laufen
      flüssig durch (ggf. kurz als Vorschau), kein Hängen (P-06).
- [ ] Formate (12.1 Nr. 1) – je eine Datei öffnen, Bild korrekt und **richtig herum**:
      JPEG, HEIC, PNG, WebP, AVIF, JXL, GIF und RAW (CR3/NEF/ARW/DNG, falls vorhanden;
      `tests/corpus/download_raw.sh` lädt Beispiele). Die Dateien
      `tests/corpus/generated/orient_1.jpg` … `orient_8.jpg` sehen alle gleich aus.
- [ ] Farbe: `p3.png` und `srgb.png` neben Vorschau.app vergleichen – gleicher Eindruck
      (F-COL-01, Details in `MANUAL_CHECKS.md`).
- [ ] Großer Ordner (P-05): z. B. 10 000 Kopien eines kleinen Bildes erzeugen
      ```sh
      mkdir -p /tmp/lf-10k && for i in $(seq -w 1 10000); do cp tests/corpus/generated/srgb.png /tmp/lf-10k/img_$i.png; done
      ```
      und eine Datei daraus öffnen: das Bild erscheint sofort, die Position folgt nach
      weniger als einer halben Sekunde, das Fenster bleibt währenddessen bedienbar.

## 4. Sortierung wie im Finder (SPEC 6, 12.1 Nr. 3, 4)

Hinweis: Der Finder speichert seine Ansichtseinstellungen (`.DS_Store`) verzögert. Nach
einer Änderung der Sortierung das Finder-Fenster **schließen** (oder kurz warten) und
erst dann in Lumaflip öffnen.

Listenansicht (Darstellung → Als Liste):

- [ ] Spalte **Änderungsdatum** anklicken, bis ↓ (neueste oben) gilt. Finder-Fenster
      schließen, Ordner wieder öffnen, Doppelklick auf das **oberste** Bild. In Lumaflip
      mit → durchblättern: Reihenfolge identisch mit der Finder-Liste; Statuszeile
      „Wie Finder: Änderungsdatum ↓“.
- [ ] Dasselbe für **Name ↑**, **Größe** (beide Richtungen) und **Hinzugefügt am**.
- [ ] Namen mit Zahlen: `IMG_2.jpg` steht vor `IMG_10.jpg`, `IMG_2 (1).jpg` vor
      `IMG_2.jpg`, Umlaute wie im Finder.

Symbolansicht (Darstellung → Als Symbole):

- [ ] Darstellung → Sortieren nach → **Name**: gleiche Reihenfolge wie im Finder.
- [ ] „Sortieren nach: Ohne“ und Symbole von Hand umstellen: Lumaflip folgt den Zeilen von
      links oben nach rechts unten (Statuszeile „Wie Finder: Symbolposition“).

Ohne Finder-Information:

- [ ] Neuer Ordner, der noch nie im Finder geöffnet wurde (z. B. im Terminal mit `mkdir`
      und `cp` angelegt): Statuszeile „Name ↑ (Finder-Info nicht gefunden)“,
      Reihenfolge wie die Finder-Namenssortierung.

Manuelle Sortierung (Darstellung → Sortieren nach):

- [ ] „Größe“ wählen: Reihenfolge ändert sich, das aktuelle Bild bleibt, Position im
      Titel passt. „Absteigend“ dreht um. „Wie Finder“ stellt zurück.

## 5. Dateien: Papierkorb, Finder, Zwischenablage, Drehen (12.1 Nr. 6)

Mit Kopien arbeiten (z. B. `cp -R tests/corpus/generated /tmp/lf-test`).

- [ ] **⌘⌫** legt das aktuelle Bild in den Papierkorb (ohne Rückfrage), das nächste Bild
      erscheint; im Finder-Papierkorb liegt die Datei.
- [ ] **⌘Z** (Bearbeiten → „… widerrufen“) holt sie zurück, sie wird wieder angezeigt.
      **⇧⌘Z** legt sie erneut in den Papierkorb.
- [ ] Die Taste **Entf** (⌦, auf kleinen Tastaturen fn+⌫) wirkt wie ⌘⌫.
- [ ] Zwei Fenster mit demselben Ordner: Löschen in einem Fenster aktualisiert beide.
- [ ] **⇧⌘R** öffnet ein Finder-Fenster mit markierter Datei.
- [ ] **⌘C** und in Vorschau.app „Ablage → Neu aus Zwischenablage“: das Bild erscheint;
      in einem Finder-Fenster „Einsetzen“ legt eine Kopie der Datei ab.
- [ ] **⌥⌘C** und in TextEdit einsetzen: der vollständige Pfad.
- [ ] **⌘L/⌘R** drehen, **H** / **⇧H** spiegeln – nur die Anzeige: die Vorschau im Finder
      bleibt unverändert; beim nächsten Bild ist die Drehung wieder weg.
- [ ] Ordner im Finder ändern, während Lumaflip ihn zeigt (Datei hineinkopieren,
      löschen, umbenennen): Position/Anzahl im Titel aktualisieren sich nach etwa einer
      Sekunde, das aktuelle Bild bleibt.

## 6. Ansicht, Zoom, Vollbild (12.1 Nr. 7)

- [ ] ⌘9 bzw. `1` = Einpassen, ⌘0 bzw. `3` = 100 % (1 Bildpixel = 1 Bildschirmpixel),
      `2` = Breite füllen, ⌘+/⌘− zoomen, alle weiteren Modi unter Darstellung.
- [ ] Trackpad: Zusammenziehen/Aufziehen zoomt, Doppeltippen mit zwei Fingern wechselt
      Einpassen ↔ 100 %, Ziehen bzw. Zweifinger-Wischen verschiebt den gezoomten Ausschnitt.
- [ ] Mausrad (Maus anschließen): Bild wechseln; Einstellungen → Maus & Tastatur →
      Mausrad „Zoomen“: zoomt am Mauszeiger; ⌘+Mausrad zoomt immer.
- [ ] Vollbild mit `F`, ⌃⌘F oder Doppelklick: nur das Bild. Maus an den unteren Rand →
      Statuszeile erscheint, nach 1,5 s ohne Bewegung verschwindet sie wieder. Esc beendet.
- [ ] Darstellung → Hintergrund: Dunkel, Hell, Wie System, Schachbrett (bei
      transparenten PNGs sichtbar), Eigene Farbe.
- [ ] Defekte Datei (`tests/corpus/generated/garbage.jpg`, `empty.jpg`): statt eines
      leeren Fensters erscheint eine Fehlerkarte mit Dateiname, Grund und „Im Finder
      zeigen“; → / ← blättern weiter (F-FMT-02).

## 7. Mehrere Fenster, Menüs, Einstellungen, Datenschutz

Fenster (SPEC 4.15):

- [ ] Zwei Bilder aus verschiedenen Ordnern nacheinander per Doppelklick: zwei Fenster,
      versetzt (nie exakt übereinander), jedes blättert/zoomt/sortiert unabhängig.
- [ ] Im Finder **5 Dateien markieren** und gemeinsam öffnen (⌘O im Finder): ein Fenster,
      Titel „… — 1 / 5“, nur diese fünf Bilder.
- [ ] Einen **Ordner auf das Dock-Symbol** ziehen: das erste Bild in Finder-Reihenfolge.
- [ ] Bild in ein offenes Lumaflip-Fenster ziehen: es erscheint in diesem Fenster.
- [ ] ⌘N: leeres Fenster; ⌥⌘N: Kopie des Fensters mit gleichem Ordner, Bild und Zoom;
      ⌘W schließt eins, ⌥⌘W alle. Ohne Fenster bleibt die Menüleiste nutzbar (⌘O, ⌘N).
- [ ] Menü „Fenster“ und Rechtsklick auf das Dock-Symbol listen alle Fenster; Auswahl holt
      das Fenster nach vorn.
- [ ] Einstellungen (⌘,) → Allgemein → „Öffnen aus dem Finder“: „Im vordersten Fenster
      ersetzen“ öffnet im selben Fenster; „Neues Fenster, außer die Datei ist schon offen“
      holt das Fenster mit dieser Datei nach vorn.
- [ ] Fenster verschieben/vergrößern, Zoom auf 100 % stellen, alle Fenster schließen,
      Bild erneut öffnen: Position, Größe und Zoommodus sind gemerkt (F-MAC-05).
- [ ] Einstellungen → Fenster → „Fenstergröße an Bild anpassen“: neue Fenster passen sich
      der Bildgröße an (große Bilder höchstens 90 % des Bildschirms).

Menüs und Erscheinungsbild:

- [ ] Alle Befehle stehen im Menü mit ihrem Tastenkürzel; Hilfe → „Tastenkürzel“ (`?`)
      zeigt die Übersicht. Ein in den Einstellungen geändertes Kürzel erscheint sofort
      im Menü.
- [ ] Systemeinstellungen → Erscheinungsbild Hell/Dunkel umschalten: Lumaflip folgt.

Datenschutz (SPEC 10.2, 12.1 Nr. 8):

1. Berechtigungen von Lumaflip zurücksetzen (betrifft nur Lumaflip):
   ```sh
   tccutil reset SystemPolicyDesktopFolder io.github.brokoskokoli.lumaflip
   tccutil reset SystemPolicyDocumentsFolder io.github.brokoskokoli.lumaflip
   ```
2. Ein Bild auf dem **Schreibtisch** per Doppelklick öffnen.

- [ ] macOS fragt einmal, ob Lumaflip auf den Ordner „Schreibtisch“ zugreifen darf.
- [ ] Bei **„Nicht erlauben“**: Das Bild wird trotzdem angezeigt (Titel „… — 1 / 1“),
      darüber eine gelbe Leiste mit Erklärung und „Zugriff erlauben…“. Kein Fehlerzustand.
- [ ] „Zugriff erlauben…“ öffnet Systemeinstellungen → Datenschutz & Sicherheit →
      **Dateien und Ordner**. Lumaflip verlangt nirgends Festplattenvollzugriff.
- [ ] Zugriff dort erlauben, Bild erneut öffnen: der ganze Ordner ist verfügbar.

## 8. Bekannte Grenzen von M1

- Der Finder schreibt Sortieränderungen verzögert; direkt nach einer Änderung kann
  Lumaflip noch die alte Reihenfolge zeigen (Abhilfe ab M2: Live-Abfrage, Opt-in).
- Spaltenansicht, Galerie, „Gruppen verwenden“ und Suchergebnisfenster lassen sich nicht
  exakt nachbilden (SPEC 6.3).
- Ein Klick auf das Dock-Symbol ohne offenes Fenster öffnet noch kein neues Fenster
  (⌘N bzw. Ablage → Öffnen verwenden).
- Noch nicht enthalten (M2 und später): Fensterwiederherstellung beim Start, Infopanel,
  Filmstreifen, Diashow, Umbenennen, Teilen, Drag-out, Fenster anordnen, Merken der
  manuellen Sortierung pro Ordner.

## 9. Fehler melden

Bitte angeben: macOS-Version, Mac-Modell, Schritt aus dieser Liste, erwartetes und
tatsächliches Ergebnis, betroffene Datei (falls teilbar). Hilfreich ist das Protokoll:

```sh
log stream --style compact --predicate 'process == "Lumaflip"'
```

Absturzberichte liegen unter `~/Library/Logs/DiagnosticReports/Lumaflip-*.ips`.
