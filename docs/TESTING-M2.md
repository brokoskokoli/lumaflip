<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Manuelle Abnahme M2 (Version 1.0)

Diese Anleitung führt durch die Prüfungen für Version 1.0, die nur ein Mensch sinnvoll machen
kann (SPEC 15.3): echte Geräte (Trackpad, zweiter Monitor, ProMotion), Finder und Dock,
fremde Apps (Teilen, Drag-out, Editor), Farbeindruck, Gefühl beim Blättern und echte
Updates. Alles andere prüfen die automatischen Tests (`tools/check.sh --stage M2`). Die
einzelnen nicht automatisierbaren Anforderungen mit Begründung stehen in
[MANUAL_CHECKS.md](MANUAL_CHECKS.md); der Kernfall aus M1 (Doppelklick → blättern,
Finder-Sortierung, Papierkorb, Datenschutzabfragen) in [TESTING-M1.md](TESTING-M1.md) gilt
weiter und wird hier vorausgesetzt.

Aufbau jedes Schritts: **Tun** → **Erwartet** (Checkbox) · in Klammern die Anforderungs-IDs.

---

## 0. Vorbereitung

1. **Testrechner:** Mac mit macOS 15 oder neuer; ideal ein MacBook mit ProMotion-Display
   (120 Hz), ein zweiter Monitor (sRGB) und ein Trackpad. Eine Maus mit Rad und Tasten 4/5
   hilft bei Abschnitt 10.
2. **Testbilder:**
   ```sh
   tests/corpus/make_corpus.sh            # Formatkorpus nach tests/corpus/generated/
   tests/corpus/download_raw.sh           # RAW-Beispiele (CC0, per SHA-256 geprüft)
   mkdir -p /tmp/lf-m2 && cp -R tests/corpus/generated/. /tmp/lf-m2/   # Arbeitskopie
   ```
   Dazu ein eigener Fotoordner (Kopie!) mit 500+ Fotos, darunter Hochformat, RAW, HEIC vom
   iPhone, ein Live-/Serienfoto und ein animiertes GIF.
3. **App:** das Release-Paket aus Abschnitt 1 nach `/Applications` installieren (nicht aus
   `build/` starten – sonst stimmen Dateizuordnung, Dock-Menü und „Zuletzt benutzt“ nicht).
4. **Finder-Fixtures (F-SORT-08, Rest aus M1):** einmal je macOS-Hauptversion die echten
   `.DS_Store`-Testdateien erzeugen und einchecken – Ablauf in TESTING-M1 §4
   („Echte .DS_Store-Testdateien erzeugen“):
   ```sh
   tools/make_finder_fixtures.sh --ui-order --gui    # ca. 2 Minuten, Finder nicht benutzen
   cmake --build build/check && build/check/tests/lf_unit_tests --gtest_filter='FinderFixtures.*'
   git add tests/data/finder/
   ```
   - [ ] Skript läuft durch, `FinderFixtures.*` besteht. Sobald `tests/data/finder/` im Repo
         ist, verlangt die CI die Fixtures (`LF_REQUIRE_FINDER_FIXTURES=1`).
5. **Ergebnisse festhalten:** Diese Datei kopieren (z. B. `TESTING-M2-<datum>-<mac>.md`),
   Punkte abhaken, Abweichungen direkt darunter notieren (Format siehe §18). Am Ende ein
   GitHub-Issue „Abnahme M2: <Mac>, macOS <Version>“ mit der ausgefüllten Liste anlegen
   bzw. sie dem Spezifikations-Prüfer übergeben.

## 1. Bauen, Paket, Start

```sh
brew install cmake ninja qt jpeg-turbo googletest clang-format   # einmalig
tools/fetch_sparkle.sh                 # Sparkle 2 (für §14)
tools/check.sh --stage M2              # Build, Tests, Format, SPDX, Spec-Abdeckung M2, Benchmarks
tools/package.sh                       # dist/Lumaflip-1.0.0.dmg + .zip + .sha256 (ad-hoc signiert)
```

- [ ] `check.sh --stage M2` endet mit „All checks passed.“ (Benchmarks bei hoher Last:
      es wartet bzw. wiederholt einzelne Benchmarks einmal – beide Ergebnisse stehen in der
      Ausgabe).
- [ ] `package.sh` meldet „TurboJPEG: …/libturbojpeg.a“ und keine Homebrew-Verweise.
- [ ] DMG öffnen, Lumaflip nach „Programme“ ziehen, starten: Über Lumaflip zeigt
      **Version 1.0.0**; „Lizenzen von Drittanbietern …“ listet u. a. Qt, libjpeg-turbo, Sparkle.
- [ ] Mit dem **Release-DMG von GitHub** (notarisiert): Gatekeeper zeigt nur „aus dem Internet
      geladen“ (Abschnitt „Auslieferung“ in MANUAL_CHECKS.md).

## 2. Startbildschirm und „Zuletzt geöffnet“ (F-OPEN-11, F-OPEN-12)

- [ ] Lumaflip ohne Datei starten: Startbildschirm mit Drop-Fläche („Bild oder Ordner hierher
      ziehen“), „Öffnen …“ und – nach einigen geöffneten Bildern/Ordnern – bis zu **8 zuletzt
      geöffnete Elemente**, Dateien und Ordner gemischt, neueste zuerst. Klick öffnet sie.
      (F-OPEN-12)
- [ ] Ablage → Zuletzt geöffnet zeigt dieselben Einträge; Rechtsklick aufs Dock-Symbol zeigt
      sie unter „Zuletzt benutzt“. „Einträge löschen“ leert beide Listen. (F-OPEN-11)
- [ ] Ein Eintrag, dessen Datei inzwischen gelöscht ist: verständliche Meldung, kein Absturz.

## 3. Ordnerüberwachung (F-OPEN-07, F-SORT-07)

Ordner `/tmp/lf-m2` in Lumaflip und im Finder nebeneinander öffnen.

- [ ] Datei in den Ordner kopieren: Anzahl im Titel steigt nach höchstens ca. 1 s, das aktuelle
      Bild bleibt, die neue Datei steht an ihrer Sortierposition. (F-OPEN-07, F-SORT-07)
- [ ] Eine andere Datei umbenennen (z. B. `a.png` → `zz.png`): Position wird neu sortiert, das
      aktuelle Bild bleibt. Das **aktuelle** Bild im Finder umbenennen: Lumaflip zeigt es weiter
      unter neuem Namen. (F-SORT-07)
- [ ] Das **aktuelle** Bild im Finder löschen: Lumaflip wechselt zum nächsten Bild (am Ende zum
      vorherigen). (F-OPEN-07)
- [ ] 200 Dateien auf einmal hineinkopieren (`cp`): ein Aktualisierungsschub, kein Flackern,
      Blättern bleibt flüssig.
- [ ] Zwei Fenster mit demselben Ordner: beide aktualisieren sich (F-WIN-09).
- [ ] **Netzlaufwerk** (SMB) bzw. USB-Stick: Änderungen von *diesem* Mac erscheinen. Änderungen
      von einem **anderen Rechner** meldet macOS auf Netzlaufwerken oft nicht (FSEvents) – dann
      erscheinen sie erst beim erneuten Öffnen des Ordners. Das ist eine bekannte Grenze (§17),
      kein Fehler; ein Absturz oder Hänger dabei schon.

## 4. Trackpad und Zoom (F-NAV-04, F-VIEW-03, F-VIEW-05)

- [ ] Zweifinger-Wischen horizontal: **genau ein Bild pro Geste**, auch mit Schwung; mit und
      ohne „Natürliche Scrollrichtung“ fühlt sich die Richtung richtig an. (F-NAV-04)
- [ ] Pinch zoomt flüssig um die Fingerposition; gezoomt verschiebt Wischen den Ausschnitt
      statt zu blättern. (F-NAV-04, F-IN-13)
- [ ] `Z` über einem Detail: wechselt Einpassen ↔ 100 % **an der Cursorposition** (das Detail
      bleibt unter dem Zeiger). Einstellungen → Maus & Tastatur → Einfacher Klick
      „Einpassen ↔ 100 %“: Klick tut dasselbe. (F-VIEW-03, F-IN-09)
- [ ] Serienaufnahme: auf 200 % an ein Detail zoomen, **⇧Z** („Zoom fixiert“), mit → blättern:
      Zoomstufe und Ausschnitt bleiben; erneut ⇧Z löst („Zoom gelöst“). Bei unterschiedlich
      großen Bildern bleibt der Ausschnitt relativ an derselben Stelle. (F-VIEW-05)

## 5. Formate (Pro/HDR, Animiert, SVG, Seiten, RAW, CMYK)

Pro/HDR (SPEC 4.4):
- [ ] PSD (Kompositbild), OpenEXR, Radiance HDR (`.hdr`), JPEG 2000, TGA, **DDS** und **KTX**
      öffnen: Bild korrekt und richtig herum; EXR/HDR ohne Belichtungsregler (der kommt in M3),
      aber nicht schwarz oder ausgebrannt. Nicht unterstützte DDS/KTX-Varianten (z. B. BC7/ASTC,
      die ImageIO nicht kennt) zeigen eine Fehlerkarte mit Grund, kein leeres Fenster. (F-FMT-02)

Animiert (F-FMT-03):
- [ ] Animiertes GIF, APNG, animiertes WebP, HEICS (Live-/Serienfoto) und – falls vorhanden –
      animiertes AVIF/JXL: läuft gleichmäßig ohne Flackern, auch im Vollbild und auf ProMotion.
- [ ] Leertaste pausiert/spielt, `,` / `.` gehen Einzelbilder, Bild → Animation → Geschwindigkeit
      0,25×–4× wirkt sofort. Ein langes GIF (> 200 Bilder) läuft minutenlang ohne
      Speicherwachstum (Aktivitätsanzeige).

SVG (Vektor, Qt SVG):
- [ ] Einfache SVGs (Symbole, Logos) werden scharf in jeder Zoomstufe dargestellt.
- [ ] Grenzen von v1.0: Qt SVG kann **SVG Tiny 1.2** – Filter, Masken, komplexes CSS,
      eingebettete Schriften fehlen oder werden vereinfacht dargestellt. Erwartet: Bild
      erscheint ohne diese Effekte bzw. Fehlerkarte, **kein Absturz, kein Hänger**. Volles
      SVG 1.1 kommt mit resvg in M3 (§17).

Mehrseitig (F-FMT-04):
- [ ] Mehrseitiges TIFF, ICO mit mehreren Größen, HEIF-Sammlung: ⌥↓/⌥↑ blättert die Seiten,
      Statuszeile „Seite 2/5“, jede Seite eingepasst; → wechselt weiter zum nächsten Bild.

RAW (F-FMT-05):
- [ ] RAW (CR3/NEF/ARW/DNG): zuerst erscheint sofort die eingebettete Vorschau, kurz danach die
      volle Entwicklung (sichtbar an Schärfe/Farbe beim Hineinzoomen). Einstellungen → Formate →
      „RAW: nur eingebettete Vorschau anzeigen (schneller)“: Blättern durch RAWs deutlich
      schneller, Zoom zeigt nur die Vorschauauflösung.

CMYK (F-COL-06):
- [ ] CMYK-JPEG und CMYK-TIFF (z. B. aus Druckvorstufe): Farben wie in Vorschau.app, nicht
      invertiert oder übersättigt.

## 6. Farbe beim Monitorwechsel (F-COL-02)

- [ ] Display-P3-Bild (z. B. `p3.png`) auf dem MacBook-Display (P3) öffnen, Fenster auf einen
      **sRGB-Monitor** ziehen: Farben passen sich sofort an (Vergleich mit Vorschau.app auf
      beiden Monitoren). Fenster halb auf beiden Monitoren: kein Flackern.
- [ ] Systemeinstellungen → Monitore → Farbprofil wechseln, während das Bild offen ist: Anzeige
      folgt ohne Neustart.

## 7. Infopanel, Histogramm, Tags, Filmstreifen (F-INFO-02/03/06, F-GRID-01)

- [ ] `I` bzw. ⌘I: Infopanel rechts mit Datei (Pfad, Größe, Daten, Typ), Bild (Maße, Farbraum,
      Profil, Bittiefe, HDR ja/nein), Kamera (Modell, Objektiv, Brennweite, Blende, Zeit, ISO,
      Belichtungskorrektur, Blitz), Aufnahme, GPS. → gedrückt halten: Panel bleibt offen,
      Werte wechseln, Blättern ruckelt nicht. Rechtsklick → „Wert kopieren“. (F-INFO-02)
- [ ] Histogramm (RGB und Luminanz) bei normalem, sehr dunklem und überbelichtetem Foto
      plausibel, in Hell und Dunkel gut sichtbar. (F-INFO-03)
- [ ] Tags: „Rot“ und einen eigenen Tag hinzufügen, einen entfernen → Finder zeigt es; im
      Finder gesetzter Tag erscheint in Lumaflip. (F-INFO-06)
- [ ] `T`: Filmstreifen mit Vorschaubildern, aktuelles Bild markiert und mittig, Klick springt,
      Scrollen mit Rad/Trackpad; nach Neustart erscheinen die Vorschauen sofort (Cache in
      `~/Library/Caches/Lumaflip/`). (F-GRID-01, P-04)
- [ ] Vollbild mit offenem Panel/Filmstreifen: nur das Bild; Maus an den rechten bzw. unteren
      Rand blendet sie ein, nach 1,5 s ohne Bewegung wieder aus. (F-VIEW-10)
- [ ] Der Zustand (Panel/Filmstreifen an/aus, Breite) bleibt nach Neustart erhalten.

## 8. Dateioperationen (F-FILE-04 … 07, F-EDIT-02)

Nur mit Kopien arbeiten (`/tmp/lf-m2`, Kopie des Fotoordners).

Umbenennen (F-FILE-04):
- [ ] Enter bzw. F2: Feld über dem Bild, nur der Name ohne Endung markiert. Neuer Name → Titel
      und Position folgen sofort (Ordner nach Name sortiert), ein zweites Fenster zeigt den
      neuen Namen, ⌘Z macht es rückgängig.
- [ ] Endung ändern bzw. löschen: Rückfrage („Soll die Endung … geändert werden?“). Ungültige
      Namen (`/`, `:`, führender Punkt, schon vorhanden): verständliche Meldung, nichts passiert.

Öffnen mit, Editor (F-FILE-05):
- [ ] Ablage → Öffnen mit: Liste wie im Finder (Standard-App zuerst, mit Symbolen, ohne
      Lumaflip); „Andere …“ funktioniert. `E` öffnet Vorschau bzw. den in Einstellungen →
      Allgemein gewählten Editor – auch wenn Lumaflip selbst Standard-App ist.

Teilen, Drag-out (F-FILE-06, F-FILE-07):
- [ ] Ablage → Teilen: macOS-Teilen-Fenster; AirDrop/Mail/Nachrichten bekommen die
      **Originaldatei**.
- [ ] ⌥-Ziehen aus dem Bild (bzw. bei Einstellung „Datei herausziehen“ normales Ziehen) in den
      Finder, in eine Mail, in Nachrichten: Originaldatei wird kopiert/angehängt, beim Ziehen
      ist ein Vorschaubild sichtbar. (F-IN-14)

Drehung speichern (F-EDIT-02):
- [ ] Hochformat-JPEG mit ⌘R drehen, **Drehung speichern** (Menü Bild): Finder-Vorschau,
      Quick Look, Vorschau.app und Fotos zeigen es gedreht; Finder-Tags und Kommentar bleiben,
      mit „Änderungsdatum beibehalten“ auch das Datum. ⌘Z stellt die Originaldatei wieder her.
- [ ] Strategie „JPEG verlustfrei drehen (DCT)“: auch ein Browser mit
      `image-orientation: none` zeigt es gedreht; bei Maßen, die nicht durch 8/16 teilbar sind,
      erscheint die Frage „Verlustfrei drehen geht nur mit beschnittenem Rand.“
- [ ] Strategie „EXIF-Ausrichtung setzen“: nur das Kennzeichen ändert sich (Dateigröße fast
      gleich), Bilddaten unverändert.
- [ ] PNG/HEIC/TIFF: wird über das Orientation-Tag gespeichert; wo das nicht geht, kommt eine
      Rückfrage („… nur durch Neukodieren“) – **nie** stilles Neukodieren. RAW: wird abgelehnt
      mit verständlicher Meldung.
- [ ] **Symlink:** Drehung über einen symbolischen Link speichern → die **Zieldatei** ist
      gedreht, der Link bleibt ein Link (im Finder weiterhin Alias-Pfeil). ⌘Z stellt das Ziel
      wieder her.
- [ ] **Hardlink** (`ln bild.jpg zweitname.jpg`): nach dem Speichern zeigen **beide** Namen das
      gedrehte Bild (gleiche Datei, `ls -i` gleiche Inode) – oder Lumaflip lehnt mit klarer
      Meldung ab; in keinem Fall zeigen die beiden Namen danach unterschiedliche Inhalte, ohne
      dass das angekündigt wurde.
- [ ] **HDR-JPEG** (iPhone-Foto mit Gain Map bzw. Ultra-HDR-JPEG): nach verlustfreiem Drehen ist
      das Bild in Fotos/Vorschau weiterhin HDR (heller Glanz auf HDR-Display) bzw. Lumaflip
      warnt, dass die Gain Map verloren ginge – kein stiller Verlust.
- [ ] **Undo nach externer Änderung:** Drehung speichern, dann die Datei in einem anderen
      Programm ändern und sichern, danach in Lumaflip ⌘Z: Lumaflip überschreibt die fremde
      Änderung **nicht** still (Rückfrage oder Meldung „Datei wurde geändert“).

## 9. Diashow (F-SLIDE-01, F-SLIDE-02)

- [ ] ⇧⌘F bzw. `S`: Diashow im Vollbild, Hinweis „Diashow – Leertaste: Pause · 1–9: Sekunden ·
      Esc: Ende“. Tasten `1`–`9` setzen das Intervall („Intervall: 3 Sekunden“), Leertaste
      pausiert/setzt fort, Esc beendet.
- [ ] Optionen (Einstellungen bzw. Diashow-Optionen): **Überblenden** dezent (ca. 0,3 s), ohne
      Aufblitzen des Hintergrunds, auch zwischen Hoch- und Querformat; **Zufällige Reihenfolge**
      ohne direkte Wiederholung; **Wiederholen** läuft endlos, aus = „Diashow beendet“ am Ende.
- [ ] Animierte Bilder in der Diashow spielen ab; ein defektes Bild hält die Diashow nicht an.
- [ ] Während der Diashow geht der Bildschirm nicht in den Ruhezustand (wünschenswert, keine
      Anforderung der SPEC – Abweichung nur notieren).

## 10. Eingaben und Einstellungen (F-IN-01 … 18, F-SET-02, F-SET-03, F-NAV-05)

Einstellungen (⌘,) → Maus & Tastatur. Jede Option einmal umstellen und ausprobieren:

- [ ] Mausrad ohne/mit ⌘/⌥/⇧: Bild wechseln · Zoomen am Cursor · Ausschnitt scrollen ·
      horizontal scrollen (F-IN-01, F-IN-02). Feinheiten: Richtung umkehren, Raststufen 1–5,
      Mindestabstand 0–300 ms (bei Freilauf-Rädern kein Überspringen), Zoomschritt (F-IN-03).
- [ ] Bild↑/Bild↓ (inkl. Sprungweite, Seite bei mehrseitigen Dateien), Pos1/Ende, Leertaste/
      ⇧Leertaste, ← / → gezoomt (alle drei Varianten), ↑ / ↓ (F-IN-04 … F-IN-08).
- [ ] Einfacher Klick, Doppelklick, mittlere Maustaste, Maustasten 4/5, Trackpad-Wischen,
      Ziehen (Ausschnitt ↔ Drag-out, ⌥ jeweils umgekehrt), Esc, Ende der Sammlung (anhalten ·
      umlaufen · zum nächsten Ordner) (F-IN-09 … F-IN-16).
- [ ] Neben jeder Option eine kurze Hilfe; „Auf Standard zurücksetzen“ pro Bereich. (F-IN-17)
- [ ] Profilvorlagen: „Mac-Standard“, „Windows-Viewer-Gewohnheiten“ (Mausrad und Bild↑/↓ =
      Bild, Enter = Vollbild), „Fotograf:in / Lightroom“ (Mausrad = Zoom; Ziffern/P/X/U erst in
      M3) – ein Klick setzt die Optionen, sichtbar in der Liste. (F-IN-18)
- [ ] **Tastenkürzel-Editor:** Kürzel einer Aktion ändern → wirkt sofort und erscheint im Menü;
      ein schon belegtes Kürzel zeigt „Konflikt mit: …“; bis zu drei Kürzel pro Aktion; „Alle
      Tastenkürzel zurücksetzen“ stellt die Standardbelegung her. (F-SET-02)
- [ ] **Export/Import:** Einstellungen → „Exportieren …“ schreibt eine `.json`; Einstellungen
      ändern; „Importieren …“ stellt alles (inkl. Tastenkürzel) wieder her. Eine kaputte Datei
      importieren: „Import fehlgeschlagen: …“, bestehende Einstellungen bleiben. (F-SET-03)
- [ ] „Alle Einstellungen zurücksetzen“ fragt nach und stellt die Standards her; der Kernfall
      funktioniert danach ohne jede Einstellung. (F-SET-04)

## 11. Fenster (F-WIN-04, F-WIN-05, F-WIN-06)

- [ ] 2–6 Fenster öffnen, Fenster → Anordnen → Nebeneinander / Untereinander / Raster /
      Kaskadieren: Fenster füllen den Bildschirm des vordersten Fensters ohne Überlappung;
      Fenster auf einem zweiten Monitor bleiben unberührt. (F-WIN-04)
- [ ] macOS-Kacheln: grünen Knopf gedrückt halten bzw. Globus+⌃+Pfeil – Lumaflip-Fenster lassen
      sich kacheln und in Vollbild-Kacheln setzen. (F-WIN-04)
- [ ] Einstellungen → Fenster → „Fenster als Tabs öffnen“: ⌘N öffnet einen Tab; Fenster → „Alle
      Fenster zusammenführen“, „Tab in neues Fenster bewegen“, Tabs per Maus herausziehen; aus:
      neue Fenster wieder eigenständig. Standard: keine Tabs. (F-WIN-05)
- [ ] **Wiederherstellung folgt der Systemeinstellung:** Systemeinstellungen → Schreibtisch &
      Dock → „Fenster beim Beenden eines Programms schließen“ **an** (macOS-Standard): zwei
      Fenster offen, ⌘Q, Lumaflip über das Dock starten → **keine** Wiederherstellung
      (Startbildschirm). Systemeinstellung **aus**: dieselben Schritte → beide Fenster mit
      Ordner, Bild, Zoom und Position zurück. Die Lumaflip-Einstellung „Fenster beim nächsten
      Start wiederherstellen“ übersteuert das. (F-WIN-06)
- [ ] **Doppelklick-Start stellt nicht wieder her:** Wiederherstellung an, Fenster offen, ⌘Q,
      dann ein Bild im Finder doppelklicken → nur **dieses** Bild erscheint (schnell, P-01), keine
      alten Fenster. (F-WIN-06)
- [ ] Beenden über das Dock-Menü „Beenden“ verhält sich wie ⌘Q.

## 12. Standard-App (F-MAC-04)

- [ ] Einstellungen → Allgemein → „Als Standard-App für Bilder festlegen“: Checkliste der
      Dateitypen (JPEG, PNG, HEIC, HEIF, AVIF, JPEG XL, WebP, GIF, TIFF, BMP, RAW, PSD, EXR,
      Radiance HDR, SVG …), **alle vorausgewählt**.
- [ ] Zwei Typen abwählen (z. B. PSD, SVG), festlegen: „Lumaflip ist jetzt die Standard-App für
      Bilder.“ Im Finder (Informationen → Öffnen mit) sind die gewählten Typen Lumaflip, die
      abgewählten **unverändert** beim bisherigen Programm (z. B. Photoshop/Vorschau).
- [ ] Dialog erneut öffnen: die Auswahl ist gemerkt.
- [ ] Rückweg dokumentiert: Finder → Informationen → Öffnen mit → Vorschau → „Alle ändern…“.

## 13. Live-Finder-Sortierung und Sortierung pro Ordner (F-SORT-04, F-SORT-06)

- [ ] Einstellungen → Navigation & Sortierung → „Sortierung live vom Finder-Fenster übernehmen“
      einschalten: vorher erscheint eine Erklärung der Automatisierungs-Berechtigung. (F-SORT-04)
- [ ] Ordner im Finder in Listenansicht nach Größe ↓ sortieren, Finder-Fenster **vorne und offen
      lassen**, Bild daraus öffnen: macOS fragt einmal nach „Automation“ (Lumaflip → Finder);
      danach Statuszeile „Wie Finder (live): Größe ↓“ und gleiche Reihenfolge – auch direkt
      nach dem Umsortieren, ohne das Finder-Fenster zu schließen. (F-SORT-04)
- [ ] Bei „Nicht erlauben“ bzw. Finder zeigt einen anderen Ordner: die `.DS_Store`-Stufe greift
      ohne Fehlermeldung und ohne spürbare Verzögerung (Zeitlimit 300 ms). (F-SORT-04)
- [ ] Darstellung → Sortieren nach → „Größe“ in Ordner A; Ordner B öffnen (bleibt „Wie Finder“);
      zurück zu A und nach Neustart: A ist weiter nach Größe sortiert. (F-SORT-06)
- [ ] „Für alle Ordner verwenden“: Meldung „Sortierung gilt jetzt für alle Ordner“; ein
      neuer Ordner C ohne eigene Wahl übernimmt sie, A behält seine eigene. (F-SORT-06)

## 14. Updates (Sparkle Ende-zu-Ende, F-MAC-08)

Braucht zwei veröffentlichte, notarisierte Releases (z. B. 1.0.0 und 1.0.1) – Ablauf
[RELEASING.md](RELEASING.md), Details Abschnitt „Auslieferung“ in MANUAL_CHECKS.md.

- [ ] Release N installieren, zweimal starten: beim **zweiten** Start fragt Sparkle
      „Automatisch nach Updates suchen?“ (beim ersten nichts – kein Netzwerkverkehr).
- [ ] Release N+1 taggen. Nach dem Workflow: GitHub Release ist öffentlich, der Appcast
      `https://brokoskokoli.github.io/lumaflip/appcast.xml` enthält **beide** Versionen mit
      ihren Download-URLs und endet mit der Signatur (`sparkle-signatures`).
- [ ] In Version N: Lumaflip → „Nach Updates suchen …“ zeigt N+1 mit Release-Notizen;
      „Installieren und neu starten“ ersetzt die App, Über-Fenster zeigt N+1.
- [ ] „Updates automatisch installieren“ an: die neue Version wird im Hintergrund geladen und
      beim Beenden installiert.
- [ ] „Automatisch nach Updates suchen“ aus: kein Netzwerkzugriff (LuLu/Little Snitch oder
      `nettop -p Lumaflip`). (SPEC 10.3)

## 15. Gefühl und Leistung (P-01 … P-07)

- [ ] **Doppelklick bis Bild < 0,5 s:** Lumaflip beendet, Doppelklick auf ein 24-MP-JPEG im
      Finder: das Bild steht gefühlt sofort (Stoppuhr-Video mit dem iPhone in Zeitlupe:
      Klick → Bild < 500 ms). Erster Start nach Installation/Update darf bis 1 s dauern. (P-01)
- [ ] Bei laufender App: weiteres Bild öffnen < 150 ms gefühlt sofort. (P-02)
- [ ] → gedrückt halten im Fotoordner (Tastenwiederholung „schnell“): Bilder wechseln flüssig,
      ggf. kurz als Vorschau, keine Hänger. (P-03, P-06)
- [ ] Großer Ordner (10 000 Dateien, siehe TESTING-M1 §3): Bild sofort, Position < 0,5 s,
      Fenster bleibt bedienbar. (P-05)
- [ ] **120 Hz:** auf ProMotion Zoomen (Pinch) und Verschieben eines 24-MP-Bildes ohne Ruckler;
      Quartz Debug bzw. Instruments „Core Animation FPS“ zeigt ~120 fps. Filmstreifen-Scrollen
      ebenso flüssig. (P-07)
- [ ] Sehr großes Bild (z. B. 30 000 × 20 000 px Panorama): öffnet ohne Absturz, Zoom auf 100 %
      zeigt scharfe Details, Speicher bleibt im Budget. (P-08, P-09)

## 16. Hell/Dunkel und Deutsch (F-MAC-02, F-MAC-01)

- [ ] Erscheinungsbild Hell ↔ Dunkel umschalten, während Infopanel, Filmstreifen, Einstellungen,
      Startbildschirm, Fehlerkarte und Umbenennen-Feld offen sind: alles folgt sofort und ist gut
      lesbar; Histogramm und Tags-Farbpunkte erkennbar. (F-MAC-02)
- [ ] Alle neuen Texte (Menüs, Einstellungen, Rückfragen, Sparkle-Dialoge, Teilen-Menü) sind
      deutsch, auch wenn macOS auf Englisch steht (Sparkle-Fenster ggf. in Systemsprache –
      notieren). (F-MAC-01)
- [ ] Tastenkürzel-Übersicht (`?`) zeigt die M2-Belegung (u. a. ⇧Z, `T`, `I`, `S`, ⌥↑/⌥↓,
      `,`/`.`, Enter/F2, `E`).

## 17. Bekannte Grenzen von Version 1.0

- **Netzlaufwerke:** Änderungen von anderen Rechnern meldet macOS nicht zuverlässig (FSEvents);
  sie erscheinen beim erneuten Öffnen des Ordners. `.DS_Store` fehlt dort oft
  (`DSDontWriteNetworkStores`) → Namenssortierung bzw. Live-Abfrage.
- **Finder-Sortierung:** Spaltenansicht, Galerie, „Gruppen verwenden“, Suchergebnis- und
  „Zuletzt benutzt“-Fenster nicht exakt (SPEC 6.3). Ohne Live-Abfrage wirkt eine gerade
  geänderte Finder-Sortierung erst, wenn der Finder die `.DS_Store` geschrieben hat.
- **SVG:** nur SVG Tiny 1.2 über Qt SVG (keine Filter/Masken/komplexes CSS); volles SVG 1.1
  mit resvg in M3.
- **HDR:** HDR-Bilder werden in v1.0 SDR dargestellt (Tone-Mapping durch ImageIO); EDR-Anzeige,
  HDR-Schalter und Belichtungsregler kommen in M3 (F-COL-03 … 05).
- **DDS/KTX:** nur die Varianten, die ImageIO dekodiert.
- **Noch nicht enthalten (M3):** Bewertungen/Aussortieren, Raster- und Vergleichsansicht,
  Fenster koppeln, Schnellablage, Lupe, rekursive Ordner, ⌘G; Restformate (QOI, XCF, KRA …).
- **Update-Concurrency:** werden drei Release-Tags in kurzer Folge gepusht, kann GitHub den
  wartenden Appcast-Job abbrechen; dann neu starten (RELEASING.md).

## 18. Fehler melden

Bitte angeben: macOS-Version, Mac-Modell (Display: ProMotion/extern), Lumaflip-Version (Über
Lumaflip), **Abschnitt und Schritt** aus dieser Liste, erwartetes und tatsächliches Ergebnis,
betroffene Datei (falls teilbar; keine privaten Fotos/`.DS_Store` ins Repo). Hilfreich:

```sh
log stream --style compact --predicate 'process == "Lumaflip"'   # während des Fehlers
log show --last 10m --predicate 'process == "Lumaflip"' > lumaflip.log
```

Absturzberichte: `~/Library/Logs/DiagnosticReports/Lumaflip-*.ips`. Leistungsprobleme: Ausgabe
von `tools/check.sh --stage M2` (Benchmarks) bzw. eine Instruments-Aufnahme („Time Profiler“)
beilegen. Ergebnis der Abnahme als GitHub-Issue „Abnahme M2: <Mac>, macOS <Version>“ mit der
ausgefüllten Checkliste; Fehler je ein eigenes Issue mit Verweis auf den Schritt.
