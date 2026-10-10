<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Manuelle Abnahme M3 (Version 1.1)

Diese Anleitung führt durch die Prüfungen für Version 1.1, die nur ein Mensch sinnvoll machen
kann (SPEC 15.3): echte Geräte (EDR-Display, zweiter Monitor, Trackpad), fremde Programme
(Lightroom, darktable), Netzwerkverkehr, Sprachgefühl und optischer Eindruck. Alles andere prüfen
die automatischen Tests. Die einzelnen nicht automatisierbaren Anforderungen mit Begründung stehen in
[MANUAL_CHECKS.md](MANUAL_CHECKS.md); die Abnahmen von Version 1.0 ([TESTING-M2.md](TESTING-M2.md))
und M1 ([TESTING-M1.md](TESTING-M1.md)) gelten weiter (Stichproben in §11).

Aufbau jedes Schritts: **Tun** → **Erwartet** (Checkbox) · in Klammern die Anforderungs-IDs.

---

## 0. Vorbereitung

1. **Build/Paket 1.1.0:** zum schnellen Ausprobieren `tools/run.sh [bild]` (baut nur die App und
   startet sie, keine Tests); für die eigentliche Abnahme das Paket:
   ```sh
   tools/package.sh                       # dist/Lumaflip-1.1.0.dmg + .zip + .sha256 (ad-hoc signiert)
   ```
   DMG öffnen, Lumaflip nach „Programme“ ziehen (nicht aus `build/` starten – sonst stimmen
   Dateizuordnung, Sprache pro App und Berechtigungstexte nicht).
   - [ ] Über Lumaflip zeigt **Version 1.1.0**.
2. **Geräte:** ein Mac mit **EDR-Display** (MacBook Pro mit XDR bzw. Pro Display XDR/Studio
   Display mit HDR) und ein **SDR-Monitor** (sRGB) am selben Mac; Trackpad.
3. **Testordner** (Kopien, nie Originale – Lumaflip schreibt Bewertungen und verschiebt Dateien):
   - `RAW+JPEG/`: Kameraordner mit Paaren (`IMG_0001.CR3` + `IMG_0001.JPG` …), einige davon in
     Lightroom bewertet (Adobe-Zusatzdateien `IMG_0001.xmp`), ein Name mit Klammern
     (`Shot [a].CR3` + `Shot [a].JPG`).
   - `Viele/`: 1000+ Fotos (gemischt Quer/Hoch, einige RAW).
   - `Baum/`: Ordner mit Unterordnern (3 Ebenen, ein leerer und ein versteckter Unterordner) und
     zwei Geschwisterordnern daneben.
   - `GPS/`: Fotos mit GPS-Daten (iPhone), dazu eines ohne.
   - `HDR/`: iPhone-HEIC mit Gain-Map, Ultra-HDR-JPEG (z. B. Pixel), PQ-AVIF,
     `tests/corpus/generated/hdr.exr` und `hdr.pfm`, dazu zwei normale SDR-JPEGs.
   - `Rest/`: QOI, PCX, PBM/PGM/PPM/PFM, SGI, ORA, KRA (aus Krita/GIMP exportiert), ein SVG und
     eine **defekte** Datei (`kaputt.svg`: die ersten 200 Bytes eines SVG; `kaputt.qoi`: abgeschnitten).
   Formatkorpus: `tests/corpus/make_corpus.sh` (nach `tests/corpus/generated/`).
4. **Netzwerkmonitor:** Little Snitch, LuLu oder `nettop -p Lumaflip` bereitlegen (§10, §14 aus M2).
5. **Fremdprogramme:** Lightroom Classic (oder Bridge) und darktable, falls vorhanden.
6. **Ergebnisse festhalten:** diese Datei kopieren (`TESTING-M3-<datum>-<mac>.md`), Punkte
   abhaken, Abweichungen direkt darunter notieren (Format siehe §13).

## 1. Automatische Läufe (nur nach Absprache)

Diese Läufe öffnen Fenster bzw. belasten den Rechner stark (CLAUDE.md): **nur nach ausdrücklicher
Absprache** und nicht auf einem Arbeitsrechner während der Arbeit.

```sh
tools/check.sh --stage M3              # Build, alle Tests, Format, SPDX, Spec-Abdeckung M3, Benchmarks
tools/check.sh --quick --sanitize      # ASan/UBSan
```

- [ ] `check.sh --stage M3` endet mit „All checks passed.“ (Integrationstests `tst_*`
      einschließlich `tst_i18n` – jede Sprache, Dialoge ohne abgeschnittene Texte –, `lf_bench`
      inkl. HDR-Benchmarks).
- [ ] Sanitizer-Lauf ohne Befund.
- [ ] Ohne Absprache genügt `tools/check.sh --quick --no-gui` (Build + Unit-Tests, keine Fenster).

## 2. Mehrsprachigkeit (F-I18N-01 … 06)

- [ ] Einstellungen → Allgemein → Sprache: Englisch wählen. Menüs (auch das native Programmmenü
      „Lumaflip“ mit Dienste/Ausblenden/Beenden), offene Fenster, Statuszeile, Infopanel und das
      Einstellungsfenster wechseln **ohne Neustart**. (F-I18N-03)
- [ ] Nacheinander Französisch, Japanisch, Chinesisch (vereinfacht) und Finnisch: keine
      abgeschnittenen Beschriftungen in Einstellungen (jede Seite), Infopanel, Konfliktdialog (§4),
      „Gehe zu Bild“, Tastenkürzel-Übersicht, „Fenster koppeln …“ und Aussortier-Tastenhilfe.
      CJK-Zeichen sauber (keine Kästchen). (F-I18N-06)
- [ ] Französisch: Menü „Sélection“, Modus „Mode sélection“, Overlay „SÉLECTION“, Filter
      „Retenues seulement“ / „Rejetées seulement“. Menüeinträge mit Auslassungspunkten ohne
      Leerzeichen („Aller à l’image…“); Deutsch mit Leerzeichen („Gehe zu Bild …“). (F-I18N-05)
- [ ] Japanisch, GPS-Foto im Infopanel: „北緯 35° 39′ 29″, 東経 139° …“ (Richtung vorn);
      Chinesisch „北纬 35°39′29″“. Zahlen, Größen und Daten im Format der Sprache. (F-I18N-04)
- [ ] „Sprache pro App“: Systemeinstellungen → Allgemein → Sprache & Region → Programme → Lumaflip
      → Italienisch, Sprache in Lumaflip auf „Wie System“: nach Neustart Italienisch. (F-I18N-03)
- [ ] Berechtigungsabfrage (Bild aus „Dokumente“ ohne Berechtigung) in Englisch bzw. Japanisch:
      der Text der Abfrage ist übersetzt (InfoPlist.strings). (F-I18N-02)
- [ ] Zurück auf Deutsch bzw. „Wie System“.

## 3. Bewertung und Aussortieren (F-CULL-01 … 06)

Ordner `Viele/`, Einstellungen → Bewertung: Speicherort „XMP-Zusatzdatei“.

- [ ] ⌘K (Aussortiermodus): Overlay „AUSSORTIEREN“ mit Bewertung, große Tastenhilfe, Zoom bleibt
      beim Blättern. `3` → drei Sterne, `P` → grün hinterlegt, `X` → rot, `U` nimmt die
      Markierung, die Sterne bleiben. Esc verlässt den Modus. (F-CULL-01, F-CULL-06)
- [ ] „Nach Bewertung automatisch weiter“ an: nach jeder Bewertung das nächste Bild. (F-CULL-03)
- [ ] ⌘Z nimmt die letzte Bewertung zurück (Overlay und Datei). (F-CULL-01)
- [ ] Aussortieren → Filter „≥ 3 ★“: Statuszeile „Filter: ≥ 3 ★ — n / m“, nur passende Bilder;
      „Nur Picks“, „Nur unbewertete“, „Nur Rejects“ ebenso. (F-CULL-04)
- [ ] Speicherorte (F-CULL-02):
  - [ ] Zusatzdatei: `IMG.xmp` neben dem Bild; Lightroom „Metadaten aus Datei lesen“ zeigt die
        Sterne, X als abgelehnt; Entwicklungseinstellungen einer vorhandenen Datei bleiben.
  - [ ] „Im Bild“ (JPEG, HEIC, TIFF, **PNG**, DNG): Vorschau.app → Informationen zeigt die
        Bewertung, das Bild sieht unverändert aus, das Änderungsdatum bleibt (Einstellung). RAW:
        Hinweis, dass die Zusatzdatei benutzt wird.
  - [ ] Finder-Tags: „★★★“, „Pick“ (grün), „Reject“ (rot) im Finder; Spotlight `tag:Pick`.
  - [ ] Widerspruch: Zusatzdatei 4 ★, im Bild 2 ★, beide Speicherorte an → Lumaflip zeigt 4 ★
        (feste Reihenfolge Zusatzdatei → im Bild → Finder-Tags).
- [ ] Sammelaktion (F-CULL-05), Ordner `Baum/` mit „Unterordner einbeziehen“, einige Bilder
      verworfen, eines davon schon in einem Unterordner `_Rejected`:
  - [ ] Schnellfilter (⌘F) auf einen Teil der Namen setzen, Aussortieren → „Alle verworfenen nach
        „_Rejected“ verschieben …“: der Dialog nennt nur die **angezeigten** verworfenen Bilder
        (Anzahl prüfen), die Bilder in `_Rejected` sind nicht dabei; es entsteht nie
        `_Rejected/_Rejected`. ⌘Z macht den ganzen Schritt rückgängig.
  - [ ] „Alle verworfenen in den Papierkorb …“: ein Schritt, ⌘Z holt alle samt Zusatzdateien zurück.

## 4. Schnellablage und Konflikte (F-FILE-08, F-FILE-11)

- [ ] Aussortieren → „Ziele der Schnellablage festlegen …“: Ziele 1–3 auf Testordner setzen.
- [ ] `M` `1`: Bild samt Zusatzdatei nach Ziel 1, Statuszeile „Nach „…“ verschoben“; `M` `M`
      wiederholt; `C` `2` kopiert; ⌘Z nimmt jeden Schritt zurück. (F-FILE-08)
- [ ] Ziel enthält schon eine Datei gleichen Namens: Dialog „Ersetzen / Beide behalten /
      Überspringen / Abbrechen“; „Beide behalten“ ergibt „name 2.jpg“; „Ersetzen“ legt die alte
      Datei in den Papierkorb, ⌘Z holt sie zurück; „Für alle n Konflikte“ bei mehreren. (F-FILE-11)
- [ ] **RAW+JPEG-Paar** (Einstellungen → Formate → Paare als ein Bild an), Ziel enthält schon
      `IMG_0001.CR3`: **eine** Frage „IMG_0001.CR3 + IMG_0001.JPG“; „Beide behalten“ →
      `IMG_0001 2.CR3`, `IMG_0001 2.JPG`, `IMG_0001 2.xmp`, `IMG_0001 2.JPG.xmp`; Lightroom zeigt am
      neuen RAW die Sterne. „Überspringen“ verschiebt keine der beiden Dateien. (F-FILE-11, F-FMT-06)
- [ ] Kopieren eines Paares: im Ziel liegen RAW, JPEG, `IMG_0001.xmp` und `IMG_0001.JPG.xmp`.
- [ ] Raster (§5) mit Mehrfachauswahl: `M` `1` verschiebt alle markierten Bilder in einem Schritt.

## 5. Raster, Vergleich, Lupe (F-GRID-02, F-GRID-03, F-VIEW-11)

- [ ] `G` in `Viele/`: Vorschauen erscheinen zügig, Scrollen flüssig, Pinch und ⌘+/⌘− ändern
      die Größe, Sterne/Pick/Reject sichtbar; Enter bzw. ⌘↓ öffnet das Bild. (F-GRID-02)
- [ ] Mehrfachauswahl (⌘-Klick, ⇧-Klick), Bewerten (`3`) bewertet alle markierten Bilder in
      **einem** Rückgängig-Schritt.
- [ ] 2–4 Bilder markieren, Darstellung → „Auswahl vergleichen“: Zoom und Ausschnitt synchron,
      mit ⌥-Ziehen nur eines; Tab wechselt den Fokus. (F-GRID-03)
- [ ] `L` gedrückt halten: Lupe am Cursor (100 %/200 %), scharf auf Retina und externem Monitor,
      auch bei gedrehten Bildern; Loslassen schließt sie. (F-VIEW-11)

## 6. Fenstermodi und Koppeln (F-VIEW-12, F-VIEW-13, F-WIN-07, F-WIN-08)

- [ ] Fenster → „Immer im Vordergrund“: bleibt über anderen Apps, auch nach Vollbild. (F-VIEW-12)
- [ ] Fenster → „Minimaler Fenstermodus“: keine Titelleiste, oben ziehen bewegt das Fenster. (F-VIEW-13)
- [ ] Zwei Fenster (JPEG- und RAW-Variante derselben Aufnahmen), Fenster → „Fenster koppeln …“
      nach Dateiname: Blättern, Zoom und Ausschnitt laufen mit, Statuszeile „Gekoppelt“;
      „Entkoppeln“ löst. (F-WIN-07)
- [ ] Neue Fenster (⌘N mehrfach) liegen nie exakt übereinander. (F-WIN-08)

## 7. Navigation (F-NAV-06, F-NAV-08, F-OPEN-08, F-OPEN-09)

- [ ] ⌘G „Gehe zu Bild …“: `23` springt zu Bild 23, `+5`/`-3` relativ; ungültige Eingabe: Meldung. (F-NAV-06)
- [ ] Navigation → „Zufällige Reihenfolge“: Statuszeile „Zufällig“, jedes Bild genau einmal;
      aus und wieder an: dieselbe Reihenfolge in dieser Sitzung. (F-NAV-06)
- [ ] ⌘F (oder einfach einen Buchstaben tippen): Schnellfilter; „urlaub“ findet „Urlaub_01.jpg“
      und „URLAUB.JPG“, Umlaute/NFD egal; Esc bzw. „Namensfilter aufheben“ zeigt wieder alle. (F-NAV-08)
- [ ] `Baum/`, Darstellung → „Unterordner einbeziehen“: Fortschritt in der Statuszeile, Reihenfolge
      ordnerweise wie im Finder, versteckte Ordner fehlen; ein im Finder neu angelegtes Bild in einem
      Unterordner erscheint ohne Neuladen. (F-OPEN-08)
- [ ] ⌥→ / ⌥← (nicht gezoomt) bzw. Navigation → „Nächster Ordner“ / „Vorheriger Ordner“: springt
      in den Geschwisterordner, leere werden übersprungen; gezoomt verschiebt ⌥→ den Ausschnitt. (F-OPEN-09)

## 8. HDR/EDR (F-COL-03, F-COL-04, F-COL-05)

- [ ] Auf dem EDR-Display: iPhone-HEIC, Ultra-HDR-JPEG, PQ-AVIF und EXR neben Vorschau.app: Spitzlichter
      heller als UI-Weiß, gleicher Eindruck; Statuszeile „HDR“. Displayhelligkeit ändern →
      Spitzlichter folgen. SDR-Bilder unverändert. (F-COL-03)
- [ ] Fenster auf den SDR-Monitor ziehen: SDR-Vorschau (weich abgerollte Spitzlichter), zurück: HDR. (F-COL-03)
- [ ] ⇧⌘H „HDR anzeigen“ aus/an: Wechsel HDR ↔ „HDR (SDR-Vorschau)“. (F-COL-04)
- [ ] EXR/PFM: ⌥⌘E Belichtungsregler, ⌥⌘+/−/0; Datei bleibt unverändert; nächstes Bild bei 0 EV. (F-COL-05)
- [ ] HDR in **Lupe**, **Raster** und **Vergleich** (HDR neben SDR): Lupe/Vergleich mit gleicher
      Helligkeit wie die Einzelansicht, kein Flackern; Raster ruhig. Toast, Umbenennen-Feld,
      Aussortier-Overlay und Fehlerkarte über HDR-Bildern vollständig sichtbar. (F-COL-03)

## 9. RAW+JPEG, Restformate, SVG-Hilfsprozess (F-FMT-06, F-FMT-01, SPEC 8.5)

- [ ] `RAW+JPEG/` mit „Paare als ein Bild“: ein Eintrag je Paar, Statuszeile „RAW+JPEG“;
      Bild → „RAW anzeigen“ wechselt zur RAW-Datei. (F-FMT-06)
- [ ] Bewerten (`3`) eines Paares: `IMG_0001.JPG.xmp` und `IMG_0001.xmp` entstehen, **ein** ⌘Z
      nimmt beide zurück. Ein in Lightroom mit 5 ★ bewertetes RAW behält 5 ★, wenn nur das JPEG
      bewertet wird (Paar-Einstellung aus) – auch bei `Shot [a]`. (F-FMT-06, F-CULL-02)
- [ ] Papierkorb (⌘⌫) eines Paares: beide Dateien und beide Zusatzdateien, ⌘Z holt alles in einem
      Schritt zurück. (F-FMT-06)
- [ ] Das angezeigte JPEG eines Paares umbenennen (Enter) in „Urlaub.JPG“: das RAW erscheint als
      eigenes Bild, die Bewertung des JPEG bleibt (Zusatzdatei heißt jetzt `Urlaub.JPG.xmp`);
      Verschieben/Löschen wirkt danach nur noch auf die jeweilige Datei. (F-FMT-06, F-FILE-04)
- [ ] `Rest/`: QOI, PCX, PBM/PGM/PPM/PFM, SGI, ORA, KRA wie im Ursprungsprogramm; PFM mit Werten
      über 1 auf dem EDR-Display hell. (F-FMT-01)
- [ ] `kaputt.svg` und `kaputt.qoi`: **Fehlerkarte** mit verständlichem Text, die App läuft weiter,
      Blättern geht weiter; Aktivitätsanzeige: Prozess `lumaflip-decoder` wird bei Bedarf neu
      gestartet. (F-FMT-02, SPEC 8.5)

## 10. GPS, Datenschutz, Rohmetadaten (F-INFO-04, F-INFO-05, SPEC 10.3)

Netzwerkmonitor läuft, Update-Prüfung aus.

- [ ] `GPS/`, Infopanel (`I`): Koordinaten in Grad/Minuten/Sekunden, **kein** Netzwerkzugriff.
      Rechtsklick → „Koordinaten kopieren“. (F-INFO-04)
- [ ] „Karte anzeigen“: genau jetzt Verbindungen zu Apple; Mini-Karte mit Markierung. Weiterblättern:
      Karten der weiteren GPS-Bilder laden ohne Klick (nur die tatsächlich angezeigten). (F-INFO-04)
- [ ] Infopanel schließen und weiterblättern: **keine** Kartenanfragen; Panel öffnen: nur die Karte
      des angezeigten Bildes. (SPEC 10.3)
- [ ] „Karte ausblenden“: Karte verschwindet, das nächste GPS-Bild fragt wieder („Karte anzeigen“),
      keine Anfragen. Neues Fenster bzw. Neustart fragen ebenfalls wieder. (SPEC 10.3)
- [ ] „In Karten öffnen“ öffnet Apple Karten an der Stelle; offline: verständliche Meldung statt Karte.
- [ ] „Alle Metadaten …“ (RAW, iPhone-HEIC, JPEG mit IPTC): Gruppen aufklappbar, Suche „ISO“,
      Kopieren/„Alles kopieren“ liefert lesbaren Text; das Fenster folgt beim Blättern. (F-INFO-05)

## 11. Regression M1/M2 (Stichproben)

- [ ] Doppelklick im Finder öffnet den Ordner in Finder-Reihenfolge, ← / → ohne Wartezeit (TESTING-M1 §1–3).
- [ ] Ordnerüberwachung, Umbenennen, Papierkorb mit ⌘Z (TESTING-M2 §3, §8).
- [ ] Infopanel, Histogramm, Finder-Tags, Filmstreifen (TESTING-M2 §7).
- [ ] Diashow, Fenster wiederherstellen, Standard-App, Updates (TESTING-M2 §9, §11, §12, §14).
- [ ] Gefühl: Blättern in 24-MP-Fotos flüssig, keine Hänger mit offenem Infopanel und Raster (TESTING-M2 §15).

## 12. Bekannte Grenzen von Version 1.1

- SVG läuft in diesem Stand weiter über Qt SVG (SVG Tiny 1.2, kein vollständiges CSS/Filter).
  resvg sowie XCF/PSB sind noch nicht enthalten – die SPEC plant sie inzwischen für M3, in dieser
  Abnahme also als „fehlt noch“ notieren, nicht als Fehler; ebenso die Ordnerkarte (F-INFO-08).
- Archive/PDF (F-OPEN-10) folgen in M4.
- Die Übersetzungen sind maschinell entstanden und **noch nicht muttersprachlich geprüft**
  (`translations/REVIEW_STATUS.md`).
- Netzlaufwerke: Änderungen von anderen Rechnern meldet macOS oft nicht (wie in 1.0).

## 13. Fehler melden

Bitte angeben: macOS-Version, Mac-Modell (Display: EDR/extern), Lumaflip-Version (Über Lumaflip),
UI-Sprache, **Abschnitt und Schritt** aus dieser Liste, erwartetes und tatsächliches Ergebnis,
betroffene Datei (falls teilbar; keine privaten Fotos ins Repo). Hilfreich:

```sh
log stream --style compact --predicate 'process == "Lumaflip" || process == "lumaflip-decoder"'
log show --last 10m --predicate 'process == "Lumaflip"' > lumaflip.log
```

Absturzberichte: `~/Library/Logs/DiagnosticReports/Lumaflip-*.ips` bzw. `lumaflip-decoder-*.ips`.
Ergebnis der Abnahme als GitHub-Issue „Abnahme M3: <Mac>, macOS <Version>“ mit der ausgefüllten
Checkliste; Fehler je ein eigenes Issue mit Verweis auf den Schritt.
