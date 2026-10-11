# Lumaflip – Spezifikation

| | |
|---|---|
| **Name** | **Lumaflip** (luma = Helligkeit/Bildsignal, flip = durchblättern) |
| **Bundle-ID** | `io.github.brokoskokoli.lumaflip` |
| **Status** | v1.x – M1 bis M3 umgesetzt; Version 1.1.0 in der Abnahme (docs/TESTING-M3.md) |
| **Stand** | 2026-10-10 |
| **Lizenz** | Open Source, GPL-3.0-or-later; veröffentlicht auf GitHub |
| **Vertrieb** | GitHub Releases, signiert (Developer ID) und notarisiert; zusätzlich Homebrew Cask |
| **Zielplattform** | macOS 15 Sequoia oder neuer (Apple Silicon, primär getestet auf macOS 27) |
| **Später** | Windows 10/11, Linux (X11/Wayland) – Architektur muss das ermöglichen, Features sind aber zunächst nur für macOS spezifiziert |

---

## 1. Ausgangslage und Ziel

Preview.app kann nach einem Doppelklick auf ein Bild **nicht** mit den Pfeiltasten durch die anderen Bilder des Ordners blättern. Das geht nur, wenn man vorher alle Dateien markiert und gemeinsam öffnet. Quick Look (Leertaste) ist kein Ersatz: Es schließt sich beim Klicken, bietet keinen Zoom und blättert auch durch Nicht-Bilder.

Die vorhandenen Alternativen haben typische Schwächen:

- **Bezahlschranken bzw. Abos für Grundfunktionen:** ApolloOne, Pixea Plus, BandiView.
- **Nicht mehr gepflegt:** Xee³, Sequential, Honeyview.
- **Sandbox-Probleme:** App-Store-Versionen fragen ständig nach Ordnerrechten (ViewPic) oder verlangen Festplattenvollzugriff (Picview).
- **Langsam oder schwerfällig:** z. B. XnView MP startet langsam.
- **Sortierung:** Keine der recherchierten Alternativen übernimmt die Sortierung des Finder-Fensters.

**Ziel:** Ein schneller, kostenloser, nativ wirkender Bildbetrachter:

1. Er öffnet per Doppelklick **jedes gängige Bildformat**.
2. Er lädt automatisch **den ganzen Ordner**, möglichst **in derselben Reihenfolge wie der Finder**.
3. Er blättert mit ←/→ **ohne spürbare Verzögerung**.
4. Er ist standardmäßig schlicht, bietet aber per Tastatur viel Leistung für Fortgeschrittene (Aussortieren, Vergleichen, Verschieben).

### 1.1 Alleinstellungsmerkmale (bewusst angestrebt)

1. **Finder-Sortierung übernehmen** (Abschnitt 6). Laut Recherche bietet das kein anderer Viewer.
2. **Kein Stocken beim Blättern** durch Vorausladen, eingebettete Vorschauen und GPU-Darstellung.
3. **Korrekte Farben und HDR.** Gain-Map-HDR (iPhone-Fotos, Ultra HDR, AVIF/JXL-HDR) auf EDR-Displays darstellen. Das bieten die meisten Viewer nicht.
4. **Keine Rechte-Odyssee.** Direktvertrieb ohne Sandbox, mit sauberer Behandlung der macOS-Datenschutzabfragen.
5. **Aussortier-Workflow** (Culling) mit Ein-Tasten-Aktionen, aber nur bei Bedarf sichtbar.

### 1.2 Nicht-Ziele (vorerst)

- Keine Bildbearbeitung über verlustfreies Drehen/Spiegeln und einfaches Zuschneiden hinaus. Kein Ersatz für Lightroom oder Photoshop.
- Keine Bibliotheks- oder Katalogdatenbank. Das Dateisystem ist die einzige Wahrheit.
- Keine Cloud-Synchronisation, kein Konto, keine Telemetrie.
- Keine Mac-App-Store-Version in der ersten Ausbaustufe (Begründung in Abschnitt 10).

---

## 2. Begriffe

| Begriff | Bedeutung |
|---|---|
| **Sammlung** | Die geordnete Liste von Bilddateien, durch die geblättert wird. In der Regel der Ordner des geöffneten Bildes. |
| **Aktuelles Bild** | Das gerade angezeigte Element der Sammlung. |
| **Decoder** | Komponente, die eine Datei in Pixel, Metadaten und Farbprofil übersetzt. |
| **Backend** | Konkrete Decoder-Implementierung, z. B. Apple ImageIO, libjxl, LibRaw. |
| **Vorladen (Prefetch)** | Hintergrund-Dekodierung der Nachbarbilder des aktuellen Bildes. |
| **EDR** | Extended Dynamic Range: Apples HDR-Darstellung über dem SDR-Weiß. |
| **Aussortieren (Culling)** | Schnelles Bewerten, Markieren und Verwerfen vieler Bilder. |

---

## 3. Nutzerszenarien

1. **Blättern (Kernfall):** Doppelklick auf `IMG_0423.HEIC` im Finder.
   - Der Viewer öffnet sofort genau dieses Bild.
   - Ohne Verzögerung steht der ganze Ordner zur Verfügung, sortiert wie im Finder-Fenster (z. B. „Hinzugefügt am ↓“).
   - → zeigt das nächste Bild, ← das vorherige.
2. **Fotos aussortieren:** Nach einem Fotoshooting liegen 800 RAW-Dateien in einem Ordner.
   - Im Vollbild blättern, mit `P` behalten, mit `X` verwerfen, mit `1`–`5` Sterne vergeben.
   - Mit `Z` auf 100 % prüfen, ob das Bild scharf ist.
   - Am Ende alle verworfenen Bilder in den Papierkorb legen.
3. **Vergleichen:** Aus einer Serienaufnahme 3 Bilder nebeneinander zeigen. Zoom und Ausschnitt sind synchron, damit man die Schärfe vergleichen kann.
4. **Einsortieren:** Screenshots durchblättern und mit `M` und einer Ziffer `1`–`9` in vorkonfigurierte Zielordner verschieben.
5. **Exotisches Format:** Eine `.jxl`-, `.psd`-, `.exr`- oder `.cr3`-Datei per Doppelklick öffnen. Sie funktioniert, ohne dass etwas nachinstalliert werden muss.
6. **Ordner öffnen:** Einen Ordner auf das Dock-Symbol ziehen. Es öffnet sich das erste Bild in Finder-Reihenfolge.
7. **Mehrfachauswahl:** 5 Dateien im Finder markieren und gemeinsam öffnen. Die Sammlung besteht dann nur aus diesen 5 Dateien.

---

## 4. Funktionsumfang

Jede Anforderung hat eine ID und eine Ausbaustufe:

- **M1 (MVP):** Grundfunktion. Doppelklick → Ordner blättern, schnell, viele Formate.
- **M2 (v1.0):** Vollwertiger Alltags-Viewer.
- **M3 (v1.x):** Profifunktionen wie Aussortieren, Vergleichen und HDR-Feinschliff.
- **M4 (später):** Nice-to-have und plattformübergreifende Builds.

### 4.1 Öffnen und Sammlung

| ID | Anforderung | Stufe |
|---|---|---|
| F-OPEN-01 | Öffnen per Doppelklick bzw. „Öffnen mit“ im Finder, per Drag & Drop aufs Dock-Symbol oder Fenster, per `open -a`, per Menü „Öffnen…“ (⌘O) und per Kommandozeile. | M1 |
| F-OPEN-02 | **Eine Datei** geöffnet: Die Sammlung ist der Ordner dieser Datei, gefiltert auf unterstützte Bildformate. Das geöffnete Bild ist das aktuelle Bild. | M1 |
| F-OPEN-03 | Das geöffnete Bild wird **sofort** angezeigt, bevor der Ordner vollständig eingelesen ist. Das Einlesen und Sortieren läuft im Hintergrund; die Position in der Sammlung wird nachgezogen. | M1 |
| F-OPEN-04 | **Mehrere Dateien** geöffnet (eine Öffnen-Anfrage oder mehrere Anfragen innerhalb von ca. 150 ms): Die Sammlung besteht genau aus diesen Dateien. | M1 |
| F-OPEN-05 | **Ordner** geöffnet: Die Sammlung ist der Ordnerinhalt; das erste Bild in Sortierreihenfolge wird angezeigt. | M1 |
| F-OPEN-06 | Versteckte Dateien (Punkt-Dateien, AppleDouble `._*`) und Pakete werden ignoriert. Ob ein Format unterstützt wird, entscheidet der Typ (UTType bzw. Signatur), nicht nur die Dateiendung. | M1 |
| F-OPEN-07 | Ordnerüberwachung (FSEvents): Neue, gelöschte und umbenannte Dateien aktualisieren die Sammlung live. Das aktuelle Bild bleibt erhalten. Wird das aktuelle Bild extern gelöscht, wird zum nächsten Bild gewechselt. | M2 |
| F-OPEN-08 | Option „Unterordner einbeziehen“ (rekursiv), umschaltbar pro Sitzung. Reihenfolge: ordnerweise, die Ordner pfadweise in Finder-Namensreihenfolge (ein Ordner vor seinen Unterordnern, wie eine aufgeklappte Listendarstellung); innerhalb eines Ordners gilt die Sortierung des Fensters. Versteckte Ordner und Pakete werden übersprungen, symbolische Links nicht verfolgt. | M3 |
| F-OPEN-09 | Zum nächsten/vorherigen Geschwisterordner springen (⌥→ / ⌥←, wenn nicht gezoomt; gezoomt gilt F-IN-07 bzw. F-VIEW-04; zusätzlich über Menü und frei belegbares Kürzel). | M3 |
| F-OPEN-10 | Archive (ZIP/CBZ, RAR/CBR, 7z) und PDF wie Ordner durchblättern. | M4 |
| F-OPEN-11 | „Zuletzt geöffnet“-Menü (Dateien und Ordner). | M2 |
| F-OPEN-12 | Startbildschirm ohne Bild: Drop-Fläche, „Öffnen…“, zuletzt geöffnete Elemente (bis zu 8, Dateien und Ordner gemischt in der Reihenfolge der letzten Nutzung, anklickbar). | M2 |

### 4.2 Navigation

| ID | Anforderung | Stufe |
|---|---|---|
| F-NAV-01 | ← / → wechseln zum vorherigen/nächsten Bild. Gedrückt halten blättert fortlaufend und zeigt dabei, falls nötig, Vorschauen. | M1 |
| F-NAV-02 | Pos1/Ende bzw. ⌘↑/⌘↓ springen zum ersten/letzten Bild. | M1 |
| F-NAV-03 | Verhalten am Ende der Sammlung einstellbar: anhalten (Standard, mit kurzem visuellem Hinweis) oder umlaufen. | M1 |
| F-NAV-04 | Trackpad: Zweifinger-Wischen wechselt das Bild, Pinch zoomt. Im gezoomten Zustand verschiebt das Wischen den Ausschnitt. | M2 |
| F-NAV-05 | Mausrad, Bild↑/Bild↓, Leertaste, Klick, Doppelklick, mittlere Maustaste und weitere Eingaben sind in den Einstellungen belegbar (siehe 4.14). M1 enthält die Standardbelegung und die Mausrad-Einstellung, M2 alle Optionen aus 4.14. | M1/M2 |
| F-NAV-06 | Zu Bild Nr. *n* springen (⌘G). Zufällige Reihenfolge an/aus. | M3 |
| F-NAV-07 | Position anzeigen („23 / 412“) in Titelleiste und Statuszeile. | M1 |
| F-NAV-08 | Dateinamen filtern (Schnellfilter, ⌘F; Tippen eines Zeichens ohne andere Belegung öffnet das Feld ebenfalls). | M3 |

### 4.3 Anzeige und Zoom

| ID | Anforderung | Stufe |
|---|---|---|
| F-VIEW-01 | Zoom-Modi: **Einpassen** (Standard; kleine Bilder werden nicht vergrößert), **Einpassen inkl. Vergrößern**, **100 % / 1:1 Pixel**, **Breite füllen**, **Höhe füllen**, **Fenster füllen**. | M1 |
| F-VIEW-02 | Tastenkürzel: ⌘0 = 100 %, ⌘9 = Einpassen, ⌘+ / ⌘− = Zoom. Zusätzlich die Einzeltasten `1`/`2`/`3` für die Modi, sofern nicht im Aussortiermodus. | M1 |
| F-VIEW-03 | `Z` bzw. – falls in F-IN-09 gewählt – Klick schaltet zwischen Einpassen und 100 % **an der Cursorposition** um. | M2 |
| F-VIEW-04 | Ausschnitt verschieben per Ziehen, Pfeiltasten (wenn gezoomt, mit ⌥) und Trackpad. Trägheitsscrollen wie bei macOS üblich. | M1 |
| F-VIEW-05 | „Zoom fixieren“: Zoomstufe und Ausschnitt bleiben beim Blättern erhalten (wichtig für Serienaufnahmen). | M2 |
| F-VIEW-06 | Hochwertiges Skalieren: Beim Verkleinern Mipmaps bzw. Lanczos-ähnliche Filterung, beim Vergrößern über 100 % wahlweise „Pixel scharf“ (nearest) oder glatt. | M1 |
| F-VIEW-07 | **Retina-korrekt:** 100 % bedeutet 1 Bildpixel = 1 Gerätepixel (einstellbar: Gerätepixel oder Punkte). | M1 |
| F-VIEW-08 | EXIF-Orientierung wird automatisch angewendet. | M1 |
| F-VIEW-09 | Hintergrund wählbar: dunkel, hell, Systemdarstellung, Schachbrett (für Transparenz), eigene Farbe. | M1 |
| F-VIEW-10 | Vollbild (⌃⌘F, `F`, Doppelklick). Im Vollbild werden Bedienelemente ausgeblendet und erscheinen, wenn die Maus an den Rand fährt. | M1 |
| F-VIEW-11 | Lupe: Taste gedrückt halten (Standard `L`, wahlweise mittlere Maustaste, F-IN-11) zeigt eine 100-%- bzw. 200-%-Lupe am Cursor. | M3 |
| F-VIEW-12 | „Immer im Vordergrund“ für das Fenster. | M3 |
| F-VIEW-13 | Rahmenloser bzw. minimaler Fenstermodus, pro Fenster umschaltbar (Fenster → „Minimaler Fenstermodus“). Seit M4 ist das das randlose Fenster aus F-VIEW-14. | M3 |
| F-VIEW-14 | **Randloses Fenster mit einblendender Titelleiste** (Standard für neue Fenster): Das Bild reicht bis an den oberen Rand, ohne sichtbare Titelleiste. Bewegt man die Maus über dem Fenster oder wird das Fenster aktiv (Tastaturbedienung), blendet oben ein halbtransparent schwarzer Streifen ein: Fenstertitel in Weiß (Schrift des Systems für Fenstertitel) und die Fensterknöpfe – macOS links (die Systemknöpfe, die mit dem Streifen ein- und ausblenden), Windows rechts (Minimieren/Maximieren/Schließen, links ein Knopf mit den Menüs des Fensters), Linux auf der Seite der Desktop-Einstellung (GNOME `button-layout`, KDE `kwinrc`, sonst rechts). Nach 1,5 s ohne Mausbewegung blendet er wieder aus, solange die Maus nicht über ihm steht. Ziehen am Streifen bewegt das Fenster (Windows: Aero Snap, Snap-Layouts über dem Maximieren-Knopf), Doppelklick zoomt bzw. maximiert (macOS nach der Systemeinstellung „Doppelklick auf Titelleiste“), die Ränder bleiben zum Ändern der Größe; Dateien lassen sich weiter auf das Fenster ziehen. Einstellung *Fenster → Titelleiste*: „beim Bewegen der Maus einblenden“ (Standard) oder „immer zeigen“ (Titelleiste des Systems); Fenster → „Minimaler Fenstermodus“ schaltet ein einzelnes Fenster um (F-VIEW-13). Im Vollbild gilt F-VIEW-10; ein Fenster mit macOS-Tabs (F-WIN-05) zeigt die Titelleiste des Systems. | M4 |

### 4.4 Formate

Ziel: **So viele Formate wie möglich, ohne Nachinstallation.** Die Formate werden in Gruppen geliefert:

| Gruppe | Formate | Quelle auf macOS | Stufe |
|---|---|---|---|
| Standard | JPEG, PNG, GIF, BMP, TIFF (inkl. mehrseitig), WebP, ICO/CUR, ICNS | ImageIO | M1 |
| Modern | HEIC/HEIF, AVIF, JPEG XL | ImageIO | M1 |
| RAW | CR2, CR3, CRW, NEF, NRW, ARW, SR2, RAF, ORF, RW2, DNG, PEF, IIQ, 3FR, … | ImageIO (Apple RAW Engine) | M1 |
| Pro/HDR | PSD (Kompositbild), OpenEXR, Radiance HDR, JPEG 2000, TGA, DDS, KTX | ImageIO | M2 |
| Animiert | GIF, APNG, animiertes WebP, animiertes AVIF/HEICS, animiertes JXL | ImageIO + eigene Wiedergabe | M2 |
| Vektor | SVG | v1.0: Qt SVG (SVG Tiny 1.2); ab M3: resvg (vollständiges SVG 1.1 mit CSS/Filtern, Homebrew-Bibliothek `libresvg`), Qt SVG als Rückfall; beides im Decoder-Hilfsprozess (8.5) | M2/M3 |
| Rest | QOI, PCX, PNM/PAM/PFM, SGI, ORA, KRA | eigene Decoder im Hilfsprozess | M3 |
| Rest | XCF (GIMP, Ebenen zusammengeführt, gängige Ebenenmodi), PSB (Photoshop Large Document, eingebettetes Gesamtbild) | eigene Decoder im Hilfsprozess (KImageFormats ist in Homebrew nicht verfügbar) | M3 |
| Exotisch | JPEG XR, DICOM, EPS | später prüfen | M4 |

- F-FMT-01 (M1): Die Formaterkennung erfolgt per Dateisignatur, mit Dateiendung bzw. UTType als Hinweis.
- F-FMT-02 (M1): Defekte oder nicht unterstützte Dateien zeigen eine verständliche Fehlerkarte (Dateiname, Grund, „Im Finder zeigen“) statt eines leeren Fensters. Das Blättern funktioniert weiter.
- F-FMT-03 (M2): Animationen mit Abspielen/Pause (Leertaste, wenn animiert), Einzelbild vor/zurück (`,` / `.`) und Geschwindigkeit.
- F-FMT-04 (M2): Mehrseitige Formate (TIFF, ICO, HEIF-Sammlungen) mit Seitennavigation (⌥↑/⌥↓) und Anzeige „Seite 2/5“.
- F-FMT-05 (M2): RAW: Zuerst die eingebettete JPEG-Vorschau anzeigen, dann die volle Entwicklung nachladen. Einstellbar: „Nur eingebettete Vorschau“ für maximale Geschwindigkeit.
- F-FMT-06 (M3): RAW+JPEG-Paare mit gleichem Basisnamen wahlweise als ein Element behandeln (Standard: beide anzeigen).
- Stand Restformate (M3): QOI, PCX, PNM/PAM/PFM, SGI, OpenRaster und Krita (zusammengesetztes Bild), XCF (sichtbare Ebenen zusammengesetzt; Ebenenmodi ohne Gegenstück vereinfacht) und PSB (Gesamtbild) über eigene Decoder; SVG über resvg (Homebrew `libresvg`, dynamisch), Qt SVG als Rückfall.

### 4.5 Farbe und HDR

| ID | Anforderung | Stufe |
|---|---|---|
| F-COL-01 | Eingebettete ICC-Profile werden beachtet; die Anzeige wird ins Monitorprofil konvertiert (Display P3 etc.). Bilder ohne Profil gelten als sRGB. **Keine Abflachung auf sRGB.** | M1 |
| F-COL-02 | Beim Verschieben des Fensters auf einen anderen Monitor wird das Zielprofil gewechselt. | M2 |
| F-COL-03 | **HDR/EDR:** Gain-Map-Bilder (Apple HDR, ISO 21496-1, Ultra HDR JPEG), HDR-AVIF/HEIF/JXL (PQ/HLG) und EXR werden auf EDR-fähigen Displays mit erweitertem Dynamikumfang dargestellt. | M3 |
| F-COL-04 | HDR-Schalter: HDR / SDR-Vorschau (Tone-Mapping). HDR-Bilder werden markiert (Badge). | M3 |
| F-COL-05 | Für EXR/HDR: Belichtungsregler (±EV), der nur die Anzeige verändert. | M3 |
| F-COL-06 | CMYK-JPEG und -TIFF werden korrekt dargestellt. | M2 |

### 4.6 Informationen und Metadaten

| ID | Anforderung | Stufe |
|---|---|---|
| F-INFO-01 | Statuszeile bzw. Overlay (abschaltbar): Dateiname, Position, Pixelmaße, Dateigröße, Zoomstufe. | M1 |
| F-INFO-02 | Infopanel (⌘I bzw. `I`) als Seitenleiste, die **beim Blättern offen bleibt**. Inhalt: Datei (Pfad, Größe, Datumsangaben, Typ), Bild (Maße, Farbraum, Profil, Bittiefe, HDR ja/nein), Kamera (Modell, Objektiv, Brennweite, Blende, Zeit, ISO, Belichtungskorrektur, Blitz), Aufnahmedatum, GPS. | M2 |
| F-INFO-03 | Histogramm (RGB und Luminanz) im Infopanel. | M2 |
| F-INFO-04 | GPS: Mini-Karte bzw. „In Karten öffnen“. | M3 |
| F-INFO-05 | Vollständige Rohmetadaten (EXIF/IPTC/XMP-Baum) mit Kopierfunktion. | M3 |
| F-INFO-06 | Finder-Tags anzeigen und setzen. | M2 |
| F-INFO-07 | Erweiterte Fototechnik-Overlays: Fokuspunkte (soweit in den MakerNotes vorhanden), Überbelichtungs- und Unterbelichtungswarnung, Focus Peaking. | M4 |
| F-INFO-08 | **Ordnerkarte:** Ansicht „Karte“ (Menü Darstellung, Taste/Knopf im Infopanel) zeigt alle Bilder der aktuellen Sammlung mit GPS-Daten als Markierungen auf einer Karte (MapKit, zusammengefasst bei vielen nahen Punkten, Vorschaubild beim Überfahren). Das aktuelle Bild ist hervorgehoben und folgt beim Blättern mit ←/→; Klick auf eine Markierung springt zum Bild. Bilder ohne GPS werden gezählt („312 von 412 Bildern mit Ort“). Die GPS-Daten werden auf einem Worker aus den Metadaten gelesen (ohne Pixel zu dekodieren). Datenschutz (10.3): Kartendaten werden erst geladen, wenn der Nutzer die Karte öffnet. | M3 |

### 4.7 Dateioperationen

| ID | Anforderung | Stufe |
|---|---|---|
| F-FILE-01 | In den Papierkorb legen (⌘⌫, zusätzlich `Entf`), **mit Undo (⌘Z)**. Danach wird automatisch das nächste Bild gezeigt. Optional mit Bestätigungsabfrage (Standard: aus, weil Undo möglich ist). | M1 |
| F-FILE-02 | Im Finder zeigen (⌘⇧R bzw. Menü). | M1 |
| F-FILE-03 | Bild in die Zwischenablage kopieren (⌘C: Bilddaten und Datei-URL), Pfad kopieren (⌥⌘C). | M1 |
| F-FILE-04 | Umbenennen (Enter oder F2): Inline-Feld, die Dateiendung ist geschützt. | M2 |
| F-FILE-05 | „Öffnen mit…“ und „In Standard-Editor öffnen“ (`E`, Editor konfigurierbar). | M2 |
| F-FILE-06 | Teilen-Menü (macOS Share Sheet). | M2 |
| F-FILE-07 | Bild aus dem Fenster per Drag & Drop in andere Apps ziehen (als Datei). | M2 |
| F-FILE-08 | **Schnellablage:** `M` + `1`–`9` verschiebt, `C` + `1`–`9` kopiert in konfigurierte Zielordner. `M M` wiederholt das letzte Ziel. Alles ist rückgängig machbar. | M3 |
| F-FILE-09 | Als Schreibtischhintergrund setzen. | M4 |
| F-FILE-10 | Eigene Aktionen: Shell-Befehl oder Shortcuts-Kurzbefehl mit dem Dateipfad als Argument, auf eine Taste legbar (auch ohne ⌘/⌃, z. B. F5 oder K; eine Überschneidung mit einer Taste der App wird beim Festlegen angezeigt, im Bildfenster hat die eigene Aktion Vorrang). Der erste Lauf eines Befehls wird bestätigt – auch nach dem Import von Einstellungen. | M4 |
| F-FILE-11 | Bei Namenskonflikten (Verschieben/Kopieren): Dialog mit Ersetzen / Beide behalten / Überspringen. | M3 |

### 4.8 Einfache Bearbeitung

| ID | Anforderung | Stufe |
|---|---|---|
| F-EDIT-01 | Drehen um 90° (⌘L / ⌘R) und Spiegeln, zunächst **nur in der Anzeige**. | M1 |
| F-EDIT-02 | Drehung speichern: JPEG **verlustfrei** (Transformation der DCT-Blöcke bzw. EXIF-Orientierung setzen – Strategie einstellbar), andere Formate über das EXIF-Orientation-Tag, wo möglich. Nie still verlustbehaftet neu kodieren; falls das nötig wäre, vorher nachfragen. | M2 |
| F-EDIT-03 | Zuschneiden (einfach, JPEG wenn möglich verlustfrei an MCU-Grenzen), „Speichern unter“ (eine vorhandene Datei nur nach Rückfrage ersetzen). Was nicht an Ort und Stelle zugeschnitten werden kann (RAW, Animationen, Bilder in Archiven/PDFs, Formate ohne Encoder), wird als „Zugeschnitten exportieren“ angeboten. | M4 |
| F-EDIT-04 | Exportieren / Konvertieren eines Bildes (JPEG/PNG/HEIC/AVIF/TIFF und JXL, sofern ein Encoder verfügbar ist; Qualität, maximale Größe, Metadaten entfernen). Angeboten werden nur Formate, die die Plattform schreiben kann – macOS-ImageIO kann JXL nur lesen, deshalb fehlt JXL dort. | M4 |

### 4.9 Aussortieren (Culling) und Bewertung

| ID | Anforderung | Stufe |
|---|---|---|
| F-CULL-01 | Bewertung `0`–`5` Sterne, `P` = behalten (Pick), `X` = verwerfen (Reject), `U` = zurücksetzen. | M3 |
| F-CULL-02 | **Speicherort der Bewertung wählbar** (Einstellungen → Bewertung), Mehrfachauswahl möglich: **(a) XMP-Zusatzdatei** neben dem Bild (`Bild.xmp`, kompatibel zu Lightroom, Capture One, darktable; Standard), **(b) im Bild selbst** als eingebettetes XMP (JPEG, HEIC, TIFF, PNG, DNG; für Formate ohne sicheres Schreiben, z. B. proprietäre RAW-Dateien, fällt die App automatisch auf die Zusatzdatei zurück und zeigt das an), **(c) Finder-Tags** (z. B. Tags „★★★“, „Pick“, „Reject“ und Farbetiketten; Tag-Namen und Farben einstellbar; im Finder sichtbar und durchsuchbar). Beim Lesen werden alle aktivierten Quellen berücksichtigt; bei Widerspruch gilt die feste Reihenfolge XMP-Zusatzdatei → im Bild → Finder-Tags (die erste aktivierte Quelle mit Wert gewinnt). Schreiben ins Bild ändert nur den Metadatenblock (keine Neukodierung der Pixel), behält das Änderungsdatum auf Wunsch bei und erfolgt atomar (temporäre Datei + Austausch). | M3 |
| F-CULL-03 | Option „Nach Bewertung automatisch weiter“. | M3 |
| F-CULL-04 | Filter: nur Picks / nur ≥ n Sterne / nur unbewertete / nur Rejects. | M3 |
| F-CULL-05 | Sammelaktion: „Alle verworfenen in den Papierkorb“ bzw. „… nach `_Rejected/` verschieben“. | M3 |
| F-CULL-06 | Aussortiermodus als eigener Modus: Overlay mit Bewertung, gesperrte Zoomstufe, große Tastatur-Hilfe. | M3 |

### 4.10 Übersicht und Vergleich

| ID | Anforderung | Stufe |
|---|---|---|
| F-GRID-01 | Filmstreifen (`T`, unten oder ausblendbar) mit Vorschaubildern, Markierung des aktuellen Bildes und Klick zum Springen. | M2 |
| F-GRID-02 | Rasteransicht (`G`, Enter bzw. ⌘↓ öffnet das Bild; ⌘↑ bleibt „erstes Bild“ gemäß F-NAV-02): alle Bilder der Sammlung, Größe per Pinch bzw. ⌘+/⌘− einstellbar, Mehrfachauswahl. | M3 |
| F-GRID-03 | Vergleichsmodus: 2–4 markierte Bilder nebeneinander, **synchroner Zoom und Ausschnitt**; mit ⌥ wird ein einzelnes Bild unabhängig bewegt. | M3 |
| F-GRID-04 | Differenz-Overlay bzw. Überblenden zweier Bilder. | M4 |

### 4.11 Diashow

| ID | Anforderung | Stufe |
|---|---|---|
| F-SLIDE-01 | Diashow (⇧⌘F bzw. `S`), Intervall einstellbar; während der Diashow setzen die Tasten `1`–`9` das Intervall in Sekunden. | M2 |
| F-SLIDE-02 | Optionen: Wiederholen, Zufallsreihenfolge, Überblendung (nur dezent, keine Effektsammlung). | M2 |
| F-SLIDE-03 | Während der Diashow bleibt die Bewertung per Taste möglich. | M3 |

### 4.12 Weitere macOS-Integration

| ID | Anforderung | Stufe |
|---|---|---|
| F-MAC-01 | Natives Menü mit allen Befehlen, Tastenkürzeln, Fenster-Menü und Hilfe-Suche. | M1 |
| F-MAC-02 | Hell- und Dunkelmodus nach Systemvorgabe. | M1 |
| F-MAC-03 | **Mehrere Fenster, beliebig viele** – siehe 4.15. | M1/M2/M3 |
| F-MAC-04 | „Als Standard-App für Bilder festlegen“: Button in den Einstellungen, pro UTType über `NSWorkspace.setDefaultApplication`. Die Liste der Typen ist abwählbar (Checkliste, alle vorausgewählt; die Auswahl wird gemerkt). Abgewählte Typen werden nicht angefasst und bleiben beim bisherigen Programm. | M2 |
| F-MAC-05 | Fensterposition, Fenstergröße und letzter Zoommodus werden gemerkt. | M1 |
| F-MAC-06 | Live Text (Text im Bild markieren und kopieren) und Motiv freistellen über VisionKit; abschaltbar. | M4 |
| F-MAC-07 | Quick-Look-Leiste und Services-Menü. | M4 |
| F-MAC-08 | Automatische Updates (Sparkle). | M2 |

### 4.13 Einstellungen

- F-SET-01 (M1): Einstellungsfenster (⌘,) mit den Bereichen *Allgemein*, *Fenster*, *Anzeige*, *Maus & Tastatur*, *Navigation & Sortierung*, *Bewertung*, *Formate*, *Erweitert*.
- F-SET-02 (M2): **Alle Tastenkürzel neu belegbar**, Konflikte werden erkannt, Rücksetzen auf die Standardbelegung.
- F-SET-03 (M2): Export und Import der Einstellungen als Datei.
- F-SET-04 (M1): Sinnvolle Standardwerte. Ohne jede Einstellung muss der Kernfall perfekt funktionieren.

### 4.14 Eingabe-Einstellungen (Maus, Trackpad, Tastatur)

Für die wichtigsten Eingaben gibt es in *Einstellungen → Maus & Tastatur* eine Auswahlliste mit festen Optionen. Darunter folgt der freie Tastenkürzel-Editor (F-SET-02) für alles andere.

| ID | Eingabe | Wählbare Optionen | Standard |
|---|---|---|---|
| F-IN-01 | **Mausrad** (ohne Modifier) | Bild wechseln · Zoomen (am Cursor) · Ausschnitt scrollen (wenn gezoomt, sonst Bild wechseln) | Bild wechseln |
| F-IN-02 | Mausrad **mit ⌘** / **mit ⌥** / **mit ⇧** | jeweils dieselben Optionen wie F-IN-01, zusätzlich „horizontal scrollen“ | ⌘ = Zoomen, ⌥ = Ausschnitt scrollen, ⇧ = horizontal scrollen |
| F-IN-03 | Mausrad-Feinheiten | Richtung umkehren · Raststufen pro Bildwechsel (1–5) · Mindestabstand zwischen Bildwechseln (0–300 ms, gegen Überspringen bei Rädern ohne Rasterung) · Zoomschritt pro Raste (5–50 %) | normal · 1 · 120 ms · 15 % |
| F-IN-04 | **Bild↑ / Bild↓** | vorheriges/nächstes Bild · 10 Bilder zurück/vor (Sprungweite einstellbar) · vorheriger/nächster Ordner · Seite zurück/vor bei mehrseitigen Dateien, sonst Bild · Ausschnitt um eine Bildschirmhöhe scrollen (wenn gezoomt, sonst Bild) | vorheriges/nächstes Bild |
| F-IN-05 | **Pos1 / Ende** | erstes/letztes Bild · Anfang/Ende des Bildes scrollen (wenn gezoomt, sonst erstes/letztes) | erstes/letztes Bild |
| F-IN-06 | **Leertaste / ⇧Leertaste** | nächstes/vorheriges Bild · Animation abspielen/pausieren, sonst Bild · Einpassen ↔ 100 % · Diashow starten/pausieren | Animation, sonst nächstes/vorheriges Bild |
| F-IN-07 | **← / →, wenn gezoomt** | immer Bild wechseln · erst Ausschnitt bis zum Rand scrollen, dann Bild wechseln · Ausschnitt scrollen (Bildwechsel dann nur mit ⌥←/⌥→) | immer Bild wechseln |
| F-IN-08 | **↑ / ↓** | Ausschnitt scrollen (wenn gezoomt), sonst nichts · vorheriges/nächstes Bild · zoomen | Ausschnitt scrollen, sonst vorheriges/nächstes Bild |
| F-IN-09 | **Einfacher Klick** ins Bild | nichts · Einpassen ↔ 100 % am Cursor · nächstes Bild (linke Hälfte: vorheriges) | nichts |
| F-IN-10 | **Doppelklick** ins Bild | Vollbild umschalten · Einpassen ↔ 100 % · Fenster schließen | Vollbild umschalten |
| F-IN-11 | **Mittlere Maustaste** | nichts · Vollbild · Fenster schließen · Einpassen ↔ 100 % · Lupe (gedrückt halten) | Einpassen ↔ 100 % |
| F-IN-12 | **Maustasten 4/5** (Zurück/Vor) | vorheriges/nächstes Bild · nichts | vorheriges/nächstes Bild |
| F-IN-13 | **Trackpad-Wischen** (zwei Finger, horizontal) | Bild wechseln · Ausschnitt scrollen · aus | Bild wechseln (gezoomt: scrollen) |
| F-IN-14 | **Ziehen mit linker Maustaste** | Ausschnitt verschieben · Datei herausziehen (Drag-out) · ⌥-Ziehen für die jeweils andere Aktion | Ausschnitt verschieben; ⌥ = Drag-out |
| F-IN-15 | **Esc** | Vollbild bzw. Modus verlassen, sonst nichts · …, sonst Fenster schließen | Vollbild bzw. Modus verlassen, sonst nichts |
| F-IN-16 | **Ende der Sammlung** (Wiederholung von F-NAV-03). Nächster Ordner = nächster Geschwisterordner in Finder-Namensreihenfolge, der Bilder enthält; Ordner ohne Bilder werden übersprungen; rückwärts landet man auf dem letzten Bild des vorherigen Ordners. | anhalten · umlaufen · zum nächsten Ordner weitergehen | anhalten |

- F-IN-17 (M2): Neben jeder Option erklärt eine kurze Hilfe die Auswirkung. „Auf Standard zurücksetzen“ gibt es pro Bereich.
- F-IN-18 (M2): Profilvorlagen mit einem Klick: **„Mac-Standard“** (Vorgabe), **„Windows-Viewer-Gewohnheiten“** (Mausrad = Bild wechseln, Bild↑/↓ = Bild, Enter = Vollbild, wie IrfanView/FastStone), **„Fotograf:in / Lightroom“** (Mausrad = Zoom, Ziffern = Sterne, P/X/U aktiv).

### 4.15 Mehrere Fenster

| ID | Anforderung | Stufe |
|---|---|---|
| F-WIN-01 | Beliebig viele Fenster gleichzeitig. Jedes Fenster ist unabhängig: eigene Sammlung (Ordner), eigene Position, eigener Zoom und eigene Sortierung. | M1 |
| F-WIN-02 | Verhalten beim Öffnen per Finder einstellbar: **immer neues Fenster** (Standard) · im vordersten Fenster ersetzen · neues Fenster, außer die Datei ist bereits in einem Fenster geöffnet (dann dieses nach vorn holen). | M1 |
| F-WIN-03 | ⌘N öffnet ein neues leeres Fenster, ⌥⌘N dupliziert das aktuelle Fenster mit gleichem Ordner, Bild und Zoom (z. B. um zwei Stellen desselben Ordners nebeneinander zu sehen). ⌘W schließt das Fenster, ⌥⌘W alle Fenster. | M1 |
| F-WIN-04 | Menü **Fenster → Anordnen**: alle Fenster des aktuellen Bildschirms nebeneinander, untereinander, im Raster (2×2, 3×2 …) oder kaskadiert. Bei macOS-15-Fensterkacheln werden die Systembefehle zusätzlich unterstützt. | M2 |
| F-WIN-05 | macOS-Tabs (Fenster → Alle Fenster zusammenführen) werden unterstützt. Standard: keine Tabs. | M2 |
| F-WIN-06 | Fensterwiederherstellung: Beim nächsten Start werden alle Fenster mit Ordner, Bild, Zoom und Position wiederhergestellt (abschaltbar). Der Standard folgt der Systemeinstellung „Fenster beim Beenden schließen“ (`NSQuitAlwaysKeepsWindows`; macOS-Standard: keine Wiederherstellung). Wird die App durch eine Öffnen-Anfrage gestartet (Finder, Dock, `open`), wird **nicht** wiederhergestellt: Das geöffnete Bild hat Vorrang (P-01, Szenario 1). | M2 |
| F-WIN-07 | **Fenster koppeln:** Mehrere Fenster lassen sich koppeln. Dann laufen Blättern (synchron nach Index oder nach Dateiname), Zoom und Ausschnitt gemeinsam, z. B. um zwei Ordner mit Varianten derselben Aufnahmen zu vergleichen (Vorbild: nomacs). | M3 |
| F-WIN-08 | Neue Fenster werden kaskadiert bzw. an freien Stellen platziert, nie exakt übereinander. Die Fenstergröße folgt der letzten Größe bzw. passt sich wahlweise an das Bild an (Einstellung: „Fenstergröße an Bild anpassen“, Standard aus). | M1 |
| F-WIN-09 | **Gemeinsame Ressourcen:** Alle Fenster teilen sich einen Decoder-Pool, den Vorschau-Cache und das Speicherbudget (P-09). Das Vorladen wird nach Fensteraktivität priorisiert (vorderstes Fenster zuerst, verdeckte und minimierte Fenster halten nur das aktuelle Bild). Der Ordner wird pro Pfad nur einmal eingelesen und überwacht, auch wenn mehrere Fenster ihn zeigen. | M1 |
| F-WIN-10 | Die Titelleiste zeigt den Dateinamen und optional den Ordnernamen. Das Dock-Menü und das Fenster-Menü listen alle offenen Fenster. | M1 |

---

### 4.16 Mehrsprachigkeit

| ID | Anforderung | Stufe |
|---|---|---|
| F-I18N-01 | Die Oberfläche ist vollständig übersetzbar: alle sichtbaren Texte (Menüs, Dialoge, Einstellungen, Fehlerkarten, Statuszeile, Infopanel-Bezeichnungen, Tastenkürzel-Übersicht, Hilfetexte) laufen über `tr()`; Qt-eigene Texte über die mitgelieferten Qt-Übersetzungen. Ausgangssprache der Quelltexte bleibt Deutsch. | M3 |
| F-I18N-02 | Mitgelieferte Sprachen: Deutsch, Englisch, Französisch, Spanisch, Italienisch, Portugiesisch (Brasilien und Portugal), Niederländisch, Polnisch, Schwedisch, Dänisch, Norwegisch, Finnisch, Tschechisch, Japanisch, Chinesisch (vereinfacht und traditionell), Koreanisch. | M3 |
| F-I18N-03 | Standard: Sprache folgt der Systemsprache (bevorzugte Sprachen von macOS, in Reihenfolge; Fallback Englisch). In den Einstellungen („Allgemein → Sprache“) lässt sich eine Sprache fest wählen; Wechsel ohne Neustart, falls technisch sauber möglich, sonst mit Hinweis „wirksam nach Neustart“. Die macOS-Funktion „Sprache pro App“ (Systemeinstellungen) wird ebenfalls respektiert. | M3 |
| F-I18N-04 | Zahlen, Datums- und Größenangaben (z. B. „2,8 MB“ / „2.8 MB“), Belichtungswerte und GPS-Koordinaten werden nach dem Gebietsschema formatiert. Die Finder-Namenssortierung folgt unabhängig von der UI-Sprache dem System-Gebietsschema (wie der Finder). | M3 |
| F-I18N-05 | Übersetzungsablauf: `.ts`-Dateien pro Sprache im Repo (`translations/`), Erzeugung mit `lupdate`/`lrelease` im Build; `CFBundleLocalizations` listet alle Sprachen. Eine Prüfung im CI meldet fehlende oder veraltete Übersetzungen und Texte ohne `tr()`. Erstübersetzungen dürfen maschinell entstehen, sind als „ungeprüft“ markiert und werden vor einem Release von Muttersprachlern gegengelesen (Beiträge über GitHub). | M3 |
| F-I18N-06 | Layout verträgt längere Texte (z. B. Deutsch/Finnisch) und CJK-Schriften: keine abgeschnittenen Beschriftungen in Menüs, Einstellungen und Panels; automatischer Test, der jede Sprache lädt und Dialoge auf abgeschnittene Texte prüft. | M3 |

---

## 5. Bedienkonzept

### 5.1 Fensteraufbau

```
┌──────────────────────────────────────────────────────────┐
│ ● ● ●   IMG_0423.HEIC — 23 / 412          [i] [⊞] [⋯]     │  Titelleiste (Toolbar, ausblendbar)
├──────────────────────────────────────────────┬───────────┤
│                                              │ Info-     │
│                                              │ panel     │
│                 B I L D                      │ (optional)│
│                                              │ EXIF      │
│                                              │ Histogr.  │
├──────────────────────────────────────────────┴───────────┤
│ ▢▢▢▢▣▢▢▢▢  Filmstreifen (optional)                       │
├──────────────────────────────────────────────────────────┤
│ 4032×3024 · 2,1 MB · 37 % · Sortiert wie Finder: Hinzugefügt ↓ │  Statuszeile (optional)
└──────────────────────────────────────────────────────────┘
```

- **Standardansicht minimal:** nur das Bild und eine schlanke Titelleiste. Alles andere lässt sich einblenden; der Zustand wird gemerkt. Seit M4 ist auch die Titelleiste ausgeblendet und erscheint als Streifen über dem Bild, wenn sich die Maus bewegt (F-VIEW-14).
- **Vollbild:** reines Bild. Bedienelemente erscheinen, wenn die Maus an den Rand fährt, und blenden sich nach 1,5 s Inaktivität wieder aus.

### 5.2 Standard-Tastenbelegung

Sie folgt den macOS-Konventionen und ergänzt Einzeltasten für Fortgeschrittene. Alle Tasten sind neu belegbar (F-SET-02).

| Aktion | Taste(n) |
|---|---|
| Nächstes / vorheriges Bild | → / ←, Leertaste / ⇧Leertaste (wenn nicht animiert), Bild↓ / Bild↑, Mausrad, Maustasten 4/5 (alle umstellbar, siehe 4.14) |
| Neues Fenster / Fenster duplizieren | ⌘N / ⌥⌘N |
| Erstes / letztes | Pos1 / Ende, ⌘↑ / ⌘↓ |
| Zoom + / − | ⌘+ / ⌘−, `+` / `−`, Pinch |
| 100 % | ⌘0, `3` |
| Einpassen | ⌘9, `1` |
| Breite füllen | `2` |
| Einpassen ↔ 100 % an Cursor | `Z` (Klick nur, wenn in F-IN-09 gewählt; Standard: Klick ohne Wirkung) |
| Zoom fixieren | ⇧`Z` |
| Vollbild | ⌃⌘F, `F`, Doppelklick |
| Vollbild verlassen / Modus verlassen | Esc |
| Diashow | ⇧⌘F, `S` |
| Drehen links / rechts | ⌘L / ⌘R |
| Spiegeln horizontal / vertikal | `H` / ⇧`H` |
| Infopanel | ⌘I, `I` |
| Filmstreifen / Raster | `T` / `G` |
| In den Papierkorb | ⌘⌫, Entf |
| Rückgängig / Wiederholen | ⌘Z / ⇧⌘Z |
| Umbenennen | Enter, F2 |
| Im Finder zeigen | ⇧⌘R |
| Kopieren | ⌘C (Bild), ⌥⌘C (Pfad) |
| Im Editor öffnen | `E` |
| Bewerten | `0`–`5`, `P` / `X` / `U` (nur im Aussortiermodus bzw. wenn aktiviert, damit es keinen Konflikt mit den Zoom-Ziffern gibt) |
| Verschieben / Kopieren in Ziel n | `M` + `1`–`9` / `C` + `1`–`9` |
| Animation Pause / Einzelbild | Leertaste / `,` `.` |
| Seite vor / zurück (mehrseitig) | ⌥↓ / ⌥↑ |
| Sortierung wählen | Menü „Darstellung → Sortieren nach“ |
| Einstellungen | ⌘, |
| Tastenkürzel-Übersicht | `?` |

> **Konfliktregel:** Ziffern bedeuten außerhalb des Aussortiermodus Zoom-Modi, im Aussortiermodus Sterne. Der Modus wird in der UI deutlich angezeigt.

---

## 6. Sortierung „wie im Finder“

Das ist das zentrale Komfortmerkmal und zugleich technisch die heikelste Funktion. Apple bietet dafür **keine offizielle API**. Daher gibt es eine gestufte Best-Effort-Strategie mit klarer Anzeige in der UI.

### 6.1 Ablauf

```
Bild geöffnet
  │
  ├─ Nutzer hat für diesen Ordner manuell eine Sortierung gewählt? ──► diese verwenden
  │
  ├─ [Stufe 3, optional] „Live-Finder-Fenster abfragen“ aktiviert
  │      und vorderstes Finder-Fenster zeigt genau diesen Ordner? ──► dessen Sortierung
  │
  ├─ [Stufe 1] .DS_Store auslesen:
  │      1. <Elternordner>/.DS_Store, Eintrag <Ordnername>
  │      2. <Ordner>/.DS_Store, Eintrag "."
  │      → Ansicht (vstl) → Listenansicht: lsvC/lsvp.sortColumn + ascending
  │                       → Symbolansicht: icvp.arrangeBy (bei "none": Iloc-Positionen)
  │                       → Galerieansicht: glvp.arrangeBy des Ordners, sonst globale GalleryViewSettings
  │                       → Spaltenansicht: globale Einstellung (ColumnViewOptions.ArrangeBy)
  │
  ├─ [Stufe 2] Globale Finder-Einstellungen (com.apple.finder):
  │      Spezialorte (Schreibtisch, iCloud …) → StandardViewSettings → FXPreferredViewStyle
  │
  └─ [Stufe 4] Fallback: Name, aufsteigend (Finder-Namensvergleich)
```

### 6.2 Anforderungen

| ID | Anforderung | Stufe |
|---|---|---|
| F-SORT-01 | Namenssortierung **identisch zum Finder**: `localizedStandardCompare` bzw. `CFStringCompareWithOptionsAndLocale` mit den Flags caseInsensitive, numeric, widthInsensitive, forcedOrdering und localized, Locale = `autoupdatingCurrent`. Verglichen wird der vollständige Dateiname inklusive Endung. Die Sortierschlüssel werden für große Ordner vorab berechnet. | M1 |
| F-SORT-02 | Manuelle Sortierung: Name, Änderungsdatum, Erstellungsdatum, Hinzugefügt am, Aufnahmedatum (EXIF), Größe, Art, Pixelzahl, Bewertung; jeweils auf- oder absteigend. Bei Gleichstand entscheidet der Name. | M1 |
| F-SORT-03 | Modus „Wie Finder“ (Standard) mit Stufe 1 (.DS_Store), Stufe 2 (globale Einstellungen) und Stufe 4 (Fallback). | M1 |
| F-SORT-04 | Stufe 3 (AppleScript-Abfrage des vordersten Finder-Fensters) als **Opt-in** in den Einstellungen. Die Automatisierungs-Berechtigung wird vorher erklärt. Abfrage nur, wenn `target of front window` dem Ordner entspricht. Zeitlimit 300 ms. | M2 |
| F-SORT-05 | Die UI zeigt die erkannte Sortierung an („Wie Finder: Hinzugefügt am ↓“ bzw. „Name ↑ (Finder-Info nicht gefunden)“). | M1 |
| F-SORT-06 | Manuelle Wahl wird pro Ordner gemerkt (in den App-Einstellungen, nicht im Ordner). Es gibt eine Option „Für alle Ordner verwenden“: Sie macht die Wahl zur globalen Vorgabe für Ordner ohne eigene Wahl; bereits gemerkte Ordner-Einstellungen bleiben erhalten. | M2 |
| F-SORT-07 | Nach Dateisystemänderungen (F-OPEN-07) wird neu sortiert, und das aktuelle Bild bleibt erhalten. | M2 |
| F-SORT-08 | Der .DS_Store-Leser ist robust gegen unbekannte oder defekte Daten: Er wirft keine Fehler, sondern fällt auf die nächste Stufe zurück. Er wird mit echten .DS_Store-Testdateien abgesichert: Diese erzeugt der Finder selbst in künstlichen Testordnern per Skript (`tools/make_finder_fixtures.sh`, AppleScript), je macOS-Hauptversion, ohne Nutzerdaten. Zusätzlich manueller Abgleich in eigenen Ordnern (`docs/TESTING-M<n>.md`). | M1 |

### 6.3 Bekannte Grenzen (werden dokumentiert, nicht gelöst)

- Der Finder schreibt `.DS_Store` verzögert; eine gerade geänderte Sortierung ist evtl. noch nicht gespeichert. Abhilfe: Stufe 3.
- Auf Netzlaufwerken bzw. USB-Datenträgern mit `DSDontWriteNetworkStores`/`DSDontWriteUSBStores` sowie in manchen Cloud-Ordnern (File Provider) gibt es keine .DS_Store-Datei.
- „Gruppen verwenden“, Spaltenansicht, Suchergebnis- und „Zuletzt benutzt“-Fenster lassen sich nicht exakt nachbilden.
- Das Format ist undokumentiert und kann sich mit macOS-Updates ändern. Deshalb gibt es Tests gegen echte Dateien jeder neuen macOS-Version.

---

## 7. Leistungsanforderungen

Referenzsystem: MacBook mit Apple M1, 16 GB, interne SSD.

| ID | Kennzahl | Ziel |
|---|---|---|
| P-01 | Kaltstart: Zeit vom Prozessstart bis zum ersten dargestellten Frame mit dem geöffneten Bild in Bildschirmauflösung (nicht nur eingebettete Vorschau), 24-MP-JPEG. App läuft vorher nicht, System-Caches sind warm; Median aus 5 Läufen. Ausgenommen ist der erste Start nach Installation/Update (einmalige Gatekeeper-/Signaturprüfung), dafür gilt < 1 s. Die Zeit von Doppelklick bis Prozessstart (LaunchServices) wird manuell geprüft (gesamt < 500 ms). | < 400 ms |
| P-02 | Warmstart bzw. Öffnen bei laufender App | < 150 ms |
| P-03 | Bildwechsel zu vorgeladenem Nachbarn | < 16 ms (ein Frame) |
| P-04 | Bildwechsel zu nicht vorgeladenem Bild: erste Darstellung (Vorschau/Thumbnail) | < 100 ms |
| P-05 | Ordner mit 10 000 Dateien einlesen und sortieren (nach Name) | < 500 ms, ohne die UI zu blockieren |
| P-06 | Dauerhaftes Halten von → bei 24-MP-JPEGs | ≥ 15 Bilder/s angezeigt (mit Vorschau-Stufe) |
| P-07 | Zoomen und Verschieben | konstant 60 fps bzw. ProMotion 120 fps |
| P-08 | Sehr große Bilder (z. B. 30 000 × 20 000 px) | Anzeige ohne Absturz, kachelbasiert; der dekodierte Pixelpuffer eines Bildes überschreitet nie das Speicherbudget (P-09) – größere Bilder werden beim Dekodieren verkleinert bzw. ab Zoom > Einpassen regionsweise dekodiert |
| P-09 | Speicher des Bild-Caches | Standard max. 25 % des RAM bzw. 2 GB, einstellbar |

### 7.1 Strategie

- **Vorladen:** Die Nachbarbilder (Standard: +3 in Blätterrichtung, −1 entgegen) werden im Hintergrund dekodiert und als GPU-Texturen gehalten. Die Richtung wird adaptiv an das Blätterverhalten angepasst.
- **Stufenweise Darstellung:**
  1. Eingebettete Vorschau bzw. Thumbnail (EXIF/RAW-Preview, ImageIO-Thumbnail mit `kCGImageSourceCreateThumbnailFromImageIfAbsent`).
  2. Eine an die Bildschirmgröße angepasste Dekodierung (Downsampling beim Dekodieren).
  3. Volle Auflösung erst bei Zoom > Einpasszoom.
- **Abbruch:** Beim schnellen Blättern werden veraltete Dekodier-Aufträge sofort verworfen bzw. depriorisiert.
- **Kacheln:** Bilder, die größer als die maximale Texturgröße sind (16k), werden in Kachel-Pyramiden aufgeteilt.
- **Vorschau-Cache auf der Festplatte** für Filmstreifen und Raster (im Ordner `~/Library/Caches/Lumaflip/`), Größe begrenzt, LRU.
- **Die UI ist niemals blockiert:** Dateisystem, Dekodierung und Metadaten laufen ausschließlich in Worker-Threads.

---

## 8. Architektur

### 8.1 Leitprinzipien

1. **Plattformneutraler Kern, dünne Plattformschicht.** Alles außer Fenster-, Menü- und OS-Integration liegt im Kern.
2. **Decoder-Abstraktion** mit mehreren Backends. Auf dem Mac ist das **Apple ImageIO** (beste Formatabdeckung, inkl. HEVC-Lizenz und RAW-Engine, ohne Zusatzkosten), auf Windows und Linux sind es Bibliotheken.
3. **GPU-Darstellung** über eine Abstraktion (Metal / D3D12 / Vulkan).
4. **Robustheit:** Ein defektes oder bösartiges Bild darf die App nicht abstürzen lassen. Dekodierung im separaten Hilfsprozess ist ab M3 vorgesehen (siehe 8.5).

### 8.2 Komponenten

```
┌──────────────────────────────── App (Qt 6) ─────────────────────────────────┐
│  UI-Schicht: Hauptfenster, Menüs, Panels, Einstellungen, Tastatur-Mapping    │
│  ImageView (QRhi: Metal/D3D12/Vulkan) – Zoom, Pan, Kacheln, HDR-Swapchain     │
├─────────────────────────────────── Core (C++20) ────────────────────────────┤
│  CollectionManager   – Ordner einlesen, filtern, sortieren, überwachen         │
│  SortEngine          – Sortierschlüssel, Finder-Order-Provider (Interface)     │
│  ImagePipeline       – Prefetch-Planer, Prioritäten, Abbruch, Speicherbudget   │
│  DecoderRegistry     – Format-Erkennung → Backend-Wahl (Prioritätenliste)      │
│  ColorManager        – ICC → Arbeitsfarbraum → Display (lcms2 / ColorSync)     │
│  MetadataService     – EXIF/IPTC/XMP lesen, XMP-Sidecar schreiben              │
│  ThumbnailCache      – Speicher + Disk, LRU                                    │
│  FileOps             – Papierkorb, Verschieben, Kopieren, Umbenennen, Undo-Stack │
│  Settings            – typisierte Einstellungen, Import/Export                 │
├──────────────────────────── Plattform-Adapter ──────────────────────────────┤
│  macOS (Objective-C++/Swift-Brücke):                                           │
│    ImageIODecoder, FinderOrderProvider (.DS_Store + defaults + AppleScript),   │
│    FinderNameCollator, TrashService (NSWorkspace/FileManager), FinderTags,     │
│    FSEventsWatcher, EDR-Konfiguration (CAMetalLayer), DefaultAppRegistrar,     │
│    Open-Events (NSApplicationDelegate application:openURLs:), VisionKit (M4)   │
│  Windows (später): WIC-Decoder (optional), Explorer-Sortierung, Shell-Papierkorb│
│  Linux (später):   inotify, freedesktop-Papierkorb, XDG-Pfade                   │
└──────────────────────────────────────────────────────────────────────────────┘
```

### 8.3 Decoder-Schnittstelle (Skizze)

```cpp
struct ImageInfo {
    Size pixelSize; int frameCount; int pageCount;
    Orientation exifOrientation; bool hasAlpha; bool isHdr;
    ColorProfile profile;        // ICC-Bytes oder CICP (Primärfarben/Transfer)
    std::optional<GainMapInfo> gainMap;
};

class Decoder {
public:
    virtual bool          canDecode(const FileProbe&) const = 0;  // Signatur + UTType
    virtual ImageInfo     info(const Source&) = 0;
    virtual Metadata      metadata(const Source&) = 0;
    virtual Bitmap        thumbnail(const Source&, int maxPx) = 0;  // eingebettet oder heruntergerechnet
    virtual Bitmap        decode(const Source&, const DecodeRequest&) = 0; // Seite, Frame, Maßstab, Region, HDR ja/nein
    virtual FrameTiming   frameTiming(const Source&, int frame) = 0;
};
```

- Eine `Bitmap` ist ein Pixelpuffer in 8-Bit-, 16-Bit- oder Float16-RGBA-Darstellung mit angehängtem Farbprofil.
- Jedes Backend meldet unterstützte Formate und eine Priorität.
- Die Registry probiert die Backends in Prioritätsreihenfolge; schlägt eines fehl, ist das nächste dran.

**Backend-Reihenfolge auf macOS:**

1. `ImageIODecoder` – praktisch alles (laut Messung auf macOS 27 sind es 62 Typen, inklusive HEIC, AVIF, JXL, RAW, PSD, EXR, JPEG 2000, DDS).
2. `ResvgDecoder` – SVG.
3. `KImageFormatsDecoder` – Restformate (QOI, PCX, XCF, KRA, ORA, PSB …) über `QImageReader`.
4. (später) eigene Library-Backends für Plattformgleichheit und Spezialfälle.

**Backend-Reihenfolge auf Windows/Linux (später):** libjpeg-turbo, libpng/spng, libwebp, giflib, libtiff, libavif + dav1d, libjxl, OpenEXR, OpenJPEG, LibRaw (CDDL-Lizenz), libheif (+ libde265 optional, siehe Lizenzrisiken), resvg und KImageFormats. Alternativ OpenImageIO als Sammelbibliothek. Optional nutzt Windows WIC-Codecs, sofern der Nutzer sie installiert hat.

### 8.4 Darstellung und Farbe

- **Darstellungsfläche:** `QRhiWidget` (Qt ≥ 6.7) bzw. ein eigenes Qt-Quick-Item mit Metal-Backend auf macOS.
- **Bildspeicherung:** Bilder liegen als Texturen (bzw. Kachel-Texturen mit Mipmaps) vor. Die Skalierung übernimmt ein Shader (hochwertiger Verkleinerungsfilter, nearest bzw. linear beim Vergrößern).
- **Farbe:** Beim Dekodieren bzw. Hochladen wird ins **lineare erweiterte Display P3** (Float16) konvertiert. Die Swapchain ist bei EDR-Displays „extended linear Display P3“ (Qt `QRhiSwapChain::HDRExtendedDisplayP3Linear`, sonst SDR P3/sRGB mit Tone-Mapping).
  - Auf macOS erfolgt die Konvertierung über ColorSync/ImageIO, plattformübergreifend über Little CMS 2 (MIT-Lizenz; **ohne** das GPL-lizenzierte fast_float-Plugin).
  - Auf macOS liegt die EDR-Fläche als eigenes natives Fenster (QRhiSwapChain HDRExtendedDisplayP3Linear) über der Bildfläche, nur solange ein HDR-Bild bei aktivem HDR auf einem EDR-Display gezeigt wird; Bedienelemente über dem Bild werden in diesen Durchgang übernommen.
  - Der EDR-Spielraum wird über `NSScreen.maximumExtendedDynamicRangeColorComponentValue` abgefragt und laufend aktualisiert.
- **Gain-Map-HDR** wird auf macOS per ImageIO mit `kCGImageSourceDecodeToHDR` dekodiert.

### 8.5 Prozessmodell und Sicherheit

- **M1/M2:** Die Dekodierung läuft im Hauptprozess, in Worker-Threads. Abstürze in ImageIO sind selten, und ImageIO ist ohnehin systemseitig gehärtet.
- **M3:** Die Nicht-System-Decoder (Qt-Bildplugins, Qt SVG, eigene Parser der Restformate; später KImageFormats, resvg, LibRaw, libheif) laufen in einem Hilfsprozess (`lumaflip-decoder`, Unterprozess mit Pipe-Protokoll und eigener Sandbox: kein Netz, keine Schreibzugriffe). Pixel werden per Shared Memory übergeben. Stürzt der Hilfsprozess ab oder hängt er, wird er neu gestartet und das Bild erhält eine Fehlerkarte. ImageIO bleibt im Hauptprozess (P-01…P-06).
- Keine ImageMagick-Abhängigkeit im Hauptprozess (wegen der vielen bekannten Sicherheitslücken).
- Fuzz-Tests (libFuzzer) für eigene Parser, insbesondere den `.DS_Store`-Leser und den XMP-Schreiber.

### 8.6 Datenhaltung

- **Einstellungen:** `QSettings` (auf macOS als plist in `~/Library/Preferences/io.github.brokoskokoli.lumaflip.plist`).
- **Merkdaten pro Ordner** (manuelle Sortierung, letzte Position): eine kleine SQLite- oder JSON-Datei in `~/Library/Application Support/Lumaflip/`.
- **Vorschau-Cache:** `~/Library/Caches/Lumaflip/thumbs/`, mit Schlüssel aus Pfad, Größe und Änderungsdatum.
- **Bewertungen:** **nicht** in einer App-Datenbank, sondern als XMP bzw. Finder-Tags an der Datei (siehe F-CULL-02), damit andere Programme sie lesen können.

---

## 9. Technologie-Entscheidung

### 9.1 Empfehlung: Qt 6 (LTS ≥ 6.8) + C++20, plattformspezifische Teile in Objective-C++

**Begründung:**

- **Qt ist bei Bildbetrachtern erprobt:** qView, nomacs, qimgv und Gwenview basieren darauf. Qt liefert native Menüs, Dateidialoge, Drag & Drop, Barrierefreiheit und Tastaturhandling auf allen drei Plattformen.
- **QRhi** abstrahiert Metal, D3D12 und Vulkan und unterstützt seit Qt 6.6 HDR-Swapchains (extended linear Display P3 / EDR auf macOS, scRGB/HDR10 auf Windows).
- **KImageFormats** (LGPL) bringt viele Exotenformate praktisch kostenlos mit.
- **Objective-C++-Dateien** (`.mm`) binden ImageIO, AppKit, FSEvents und VisionKit direkt ein, ohne zusätzliche Brücken.
- Ein späterer Windows- bzw. Linux-Build betrifft nur die Plattform-Adapter und die Decoder-Backends; UI und Core bleiben gleich.

### 9.2 Geprüfte Alternativen

| Option | Bewertung |
|---|---|
| **Rust-Kern + wgpu + Slint/egui** | Starke Alternative (Speichersicherheit, gutes Decoder-Ökosystem wie `image`, zune, jxl-oxide). Aber weniger ausgereifte Desktop-UI (Menüs, Barrierefreiheit, Texteingabe), und HDR- bzw. EDR-Anbindung muss selbst gebaut werden. **Zweite Wahl.** |
| Rust/Tauri 2, Electron | Die Webview kann RAW, PSD, EXR und unter Windows JXL/HEIC nicht dekodieren. Pixel müssten über IPC geschoben werden. Farbmanagement und HDR verhalten sich je nach Plattform unterschiedlich (WebKitGTK schwach). **Ungeeignet für die Bildfläche.** |
| Swift/SwiftUI/AppKit | Bestes Mac-Ergebnis mit dem geringsten Aufwand, aber keine UI-Wiederverwendung auf Windows/Linux. Erfüllt die Portierbarkeits-Anforderung nicht. |
| Flutter | Schwaches Codec- und Farbmanagement-Modell auf dem Desktop. Nicht empfohlen. |
| .NET Avalonia | Machbar (ImageGlass 10 nutzt es), aber stützt sich auf ImageMagick, und HDR wird nur per Tone-Mapping dargestellt. Nur sinnvoll bei .NET-Vorliebe. |

### 9.3 Build und Werkzeuge

- CMake ≥ 3.28, Ninja; Abhängigkeiten über **vcpkg** (Manifest-Modus) bzw. Qt-Online-Installer / `aqtinstall` in CI.
- Compiler: Apple Clang (Xcode 26+); Ziel `arm64` (optional Universal Binary mit `x86_64`).
- Tests: GoogleTest bzw. Qt Test für den Core; Testkorpus mit Beispielbildern aller Formate; Screenshot-Vergleiche für die Farbkorrektheit.
- CI: GitHub Actions (macOS-Runner). Ab M4 kommen Windows- und Linux-Jobs hinzu, damit die Plattformneutralität des Core früh abgesichert ist (der Core wird bereits ab M1 auch unter Linux kompiliert und getestet, nur ohne App).
- Statische Analyse: clang-tidy, AddressSanitizer bzw. UBSan in Debug-CI.

---

## 10. Auslieferung, Rechte und Datenschutz

### 10.1 Vertrieb: Direktvertrieb, ohne Sandbox (Entscheidung für M1–M3)

- **Grund:** Eine sandboxed App (Pflicht im Mac App Store) erhält beim Doppelklick **nur die geöffnete Datei**, nicht deren Ordner, und auch nicht die `.DS_Store` im Elternordner. Der Kernfall wäre nur mit einer zusätzlichen Ordnerfreigabe per Dialog (Security-Scoped Bookmark) möglich, und die Finder-Sortierung wäre stark eingeschränkt.
- **Umsetzung:** Signiert mit **Developer ID** (Konto vorhanden), Hardened Runtime, **notarisiert** und „gestapelt“ (`xcrun stapler`).
- **Veröffentlichung:** Quellcode und Releases auf **GitHub**. Jedes Release enthält ein DMG (Universal oder arm64) und ein ZIP, dazu SHA-256-Prüfsummen. Ein eigener **Homebrew Cask** (zunächst im eigenen Tap, später Antrag für `homebrew/cask`).
- **Release-Pipeline:** GitHub Actions auf Tag `v*`: bauen → signieren → notarisieren → DMG erzeugen → GitHub Release als Entwurf anlegen → Sparkle-Appcast aktualisieren (geprüft gegen den vorherigen) → Release veröffentlichen. Appcast-Aktualisierungen laufen nie parallel; der Sparkle-Schlüssel ist nur im Appcast-Schritt verfügbar, Zertifikat und Notar-Schlüssel nur im Build-Schritt. Zertifikat und App-Store-Connect-API-Schlüssel liegen als verschlüsselte GitHub Secrets vor, nur für Tag-Builds aus dem Hauptrepo (nicht für Forks oder PRs).
- **Updates:** Sparkle 2 mit EdDSA-signiertem Appcast, gehostet über GitHub Pages bzw. Release-Assets.
- **Reproduzierbarkeit:** Eigene Builds aus dem Quellcode (unsigniert bzw. ad-hoc signiert) sind mit einem dokumentierten Befehl möglich.
- **Entitlements:** `com.apple.security.automation.apple-events` (nur für das Opt-in in F-SORT-04) und `NSAppleEventsUsageDescription`.
- **Mac-App-Store-Edition:** eventuell später (M4) mit Ordnerfreigabe-Workflow und gespeicherten Bookmarks.

### 10.2 macOS-Datenschutzabfragen (TCC)

- Das Lesen des Ordners in **Schreibtisch / Dokumente / Downloads**, auf **Wechseldatenträgern** und **Netzlaufwerken** löst einmalig eine Systemabfrage aus.
- F-PRIV-01 (M1): Wird der Zugriff verweigert, zeigt die App nur die geöffnete Datei an, dazu einen dezenten Hinweis mit Erklärung und dem Button „Zugriff erlauben…“, der die passenden Systemeinstellungen öffnet.
- F-PRIV-02 (M1): Die App verlangt **keinen** Festplattenvollzugriff. Er wird höchstens als Option erklärt.

### 10.3 Datenschutz

- Keine Telemetrie, kein Netzwerkverkehr außer der Update-Prüfung (abschaltbar) und Kartenausschnitten für F-INFO-04. Karten (Infopanel-Ausschnitt F-INFO-04 und Ordnerkarte F-INFO-08) werden erst geladen, wenn der Nutzer sie öffnet bzw. auf „Karte anzeigen“ klickt; dabei gehen die Koordinaten an Apple (MapKit). Der Klick gilt für dieses Fenster, bis es geschlossen wird: weitere Bilder mit GPS-Daten laden ihre Karte dort automatisch, „Karte ausblenden“ nimmt die Zustimmung zurück. Die Zustimmung wird nicht gespeichert; neue Fenster und jeder Neustart fragen erneut.

### 10.4 Dateiverknüpfung

- In `Info.plist` stehen `CFBundleDocumentTypes` mit der Rolle `Viewer`:
  - `LSHandlerRank = Default` für die gängigen Typen: `public.jpeg`, `public.png`, `public.heic`, `public.heif`, `public.avif`, `public.jpeg-xl`, `org.webmproject.webp`, `com.compuserve.gif`, `public.tiff`, `com.microsoft.bmp`, `public.camera-raw-image`, `com.adobe.raw-image`, `com.adobe.photoshop-image`, `com.ilm.openexr-image`, `public.radiance`, `public.svg-image` und weitere.
  - `Alternate` für `public.image`.
  - `public.folder` für das Öffnen von Ordnern.
- Für Formate, die das System nicht kennt (QOI, KRA, ORA …), gibt es `UTImportedTypeDeclarations`.
- Die App wird Standard-App per Button (F-MAC-04) oder per Finder „Informationen → Öffnen mit → Alle ändern“.

---

## 11. Lizenzen

| Komponente | Lizenz | Konsequenz |
|---|---|---|
| Qt 6 | LGPLv3 | Dynamisch linken (Frameworks im App-Bundle); Lizenztexte beilegen; Nutzer dürfen Qt austauschen. |
| KImageFormats | LGPL-2.1 | dynamisch als Qt-Plugin |
| Little CMS 2 | MIT | ok; **kein** fast_float-Plugin (GPL) |
| resvg (ab M3, dynamisch gelinkt, Lizenztext im Bundle) | Apache-2.0/MIT | ok |
| LibRaw (später) | CDDL-1.0 gewählt | statisch erlaubt, Namensnennung |
| libheif / libde265 (später, Win/Linux) | LGPL-3.0 | dynamisch; **HEVC-Patentrisiko** außerhalb von macOS → optional bzw. Systemcodec bevorzugen |
| libavif/dav1d, libjxl, libjpeg-turbo, libpng, libwebp, libtiff, OpenEXR | BSD/zlib-artig | ok |
| exiv2 | GPL-2.0+ | mit GPL-3.0-or-later **vereinbar**, daher erlaubt. Kandidat für das Schreiben von XMP in Bilddateien (F-CULL-02) und für plattformübergreifende Metadaten. Lesen auf dem Mac weiterhin über ImageIO. |

**Lizenz der App: GPL-3.0-or-later.** Begründung:
- Sie ist mit allen geplanten Abhängigkeiten vereinbar: Qt (LGPLv3), KImageFormats, exiv2 (GPL-2.0+), LibRaw (LGPL-Variante) und den BSD- bzw. MIT-Bibliotheken.
- Sie verhindert, dass jemand den Code nimmt und als geschlossene Bezahl-App verkauft. Genau das ist das Problem, das dieses Projekt lösen soll.
- Sie ist bei freien Bildbetrachtern üblich (nomacs, qimgv, Gwenview).

Weitere Pflichten:
<!-- REUSE-IgnoreStart -->
- `LICENSE` im Repo, SPDX-Kopfzeile (`SPDX-License-Identifier: GPL-3.0-or-later`) in jeder Quelldatei.
<!-- REUSE-IgnoreEnd -->
- Fenster „Über“ mit Drittanbieter-Lizenzen; die Lizenztexte liegen im App-Bundle.
- Beiträge: kein CLA; inbound = outbound (Beiträge stehen unter GPL-3.0-or-later), ein DCO-`Signed-off-by` ist willkommen, aber keine Pflicht (CONTRIBUTING.md).
- **Hinweis:** Eine spätere Mac-App-Store-Edition verträgt sich nicht mit der GPL (Konflikt mit den App-Store-Bedingungen) und ist damit praktisch ausgeschlossen. Das ist akzeptiert, weil der Direktvertrieb ohnehin die bessere Lösung ist (10.1).

---

## 12. Qualität und Tests

- **Formatkorpus:** mindestens eine Datei je unterstütztem Format und Untervariante (z. B. progressives JPEG, 16-Bit-PNG, CMYK-TIFF, mehrseitiges TIFF, animiertes WebP, HDR-Gain-Map-HEIC vom iPhone, Ultra-HDR-JPEG, PQ-AVIF, JXL lossless und lossy, CR3, NEF, ARW, DNG 1.7 mit JXL-Kompression, PSD 16-Bit, EXR halbe Präzision).
- **Sortier-Tests:**
  - Testmenge für die Namenssortierung, gegen `localizedStandardCompare` verifiziert (u. a. `IMG_2 (1).jpg` vor `IMG_2.jpg`, `001` neben `1`, Umlaute, Vollbreitenzeichen).
  - Ein Satz echter `.DS_Store`-Dateien (Liste/Symbol/Galerie, alle Sortierspalten, auf-/absteigend) mit erwarteter Ordnung.
- **Leistungstests (automatisiert):** P-01 bis P-06 als Benchmarks im CI, mit Toleranz.
- **Robustheit:** abgeschnittene bzw. defekte Dateien, 0-Byte-Dateien, Ordner mit 50 000 Dateien, Netzlaufwerk-Simulation (langsame E/A), gleichzeitiges Löschen durch den Finder.
- **Farbtests:** Referenzbilder in sRGB, Display P3 und Adobe RGB werden gegen erwartete Pixelwerte im Framebuffer geprüft.
- **Manuelle Abnahme** des Kernszenarios auf jeder neuen macOS-Hauptversion.

### 12.1 Abnahmekriterien M1 (MVP)

1. Doppelklick auf eine JPEG-, HEIC-, PNG-, WebP-, AVIF-, JXL-, GIF- und RAW-Datei (CR3/NEF/ARW/DNG) öffnet das Bild korrekt orientiert und farbrichtig.
2. ←/→ blättert durch alle Bilder des Ordners; Nicht-Bilder werden übersprungen.
3. Hat der Finder-Ordner in der Listenansicht „Änderungsdatum ↓“, ist die Reihenfolge in der App identisch. Dasselbe gilt für Name ↑, Größe und „Hinzugefügt am“.
4. Ohne .DS_Store-Information gilt die Namenssortierung, identisch zur Finder-Namenssortierung.
5. Leistungsziele P-01, P-03 und P-05 sind erfüllt.
6. ⌘⌫ legt die Datei in den Papierkorb, und ⌘Z stellt sie wieder her.
7. Einpassen, 100 % und Zoom mit Trackpad bzw. Tastatur funktionieren flüssig.
8. Fehlende Ordnerrechte führen nicht zu einem Fehlerzustand, sondern zu einer verständlichen Meldung.

---

## 13. Fahrplan

| Stufe | Inhalt (Kurzfassung) |
|---|---|
| **M0 – Spike** (vorab) | Qt-6-Gerüst mit QRhiWidget auf Metal, ImageIO-Decoder in Objective-C++, ein Bild farbrichtig anzeigen, ←/→ im Ordner, .DS_Store-Leser als Prototyp. **Ziel: Technikentscheidung absichern** (HDR-Swapchain, Leistung, Farbe). |
| **M1 – MVP** | F-OPEN-01…06, F-NAV-01…03/07, F-VIEW-01…02/04/06…10, Formatgruppen Standard/Modern/RAW, F-COL-01, F-INFO-01, F-FILE-01…03, F-EDIT-01, F-SORT-01/02/03/05/08, F-MAC-01/02/05, F-SET-01/04, F-PRIV-01/02, F-WIN-01/02/03/08/09/10, F-IN-01 (Mausrad-Einstellung), Signierung und Notarisierung |
| **M2 – v1.0** | Ordnerüberwachung, Infopanel, Histogramm, Filmstreifen, Diashow, Animation, mehrseitige Formate, SVG, Pro-Formate, verlustfreies Drehen, Umbenennen, Teilen, Drag-out, Tastatur-Editor, alle Eingabe-Einstellungen (4.14) inkl. Profilvorlagen, Fenster anordnen, Tabs, Fensterwiederherstellung, Standard-App-Button, Live-Finder-Abfrage (Opt-in), Sparkle |
| **M3 – v1.x** | Mehrsprachigkeit (4.16), Aussortiermodus und Bewertungen (XMP-Zusatzdatei, im Bild, Finder-Tags), Fenster koppeln, Schnellablage, Raster und Vergleich, HDR/EDR vollständig, Lupe, rekursive Ordner, Hilfsprozess für Decoder, Restformate inkl. XCF/PSB, SVG über resvg, Ordnerkarte (F-INFO-08) |
| **M4 – später** | Archive und PDF, Live Text, Zuschneiden und Export, eigene Aktionen, Overlays für Fototechnik, **Windows- und Linux-Builds**, eventuell eine App-Store-Edition |

---

## 14. Entscheidungen und offene Fragen

### 14.1 Entschieden (2026-10-10)

| # | Thema | Entscheidung |
|---|---|---|
| 1 | Name | **Lumaflip**. Laut Recherche frei: GitHub-Org, Homebrew-Cask/-Formel, App Store, npm, crates.io; `lumaflip.app` ohne DNS-Einträge. Ausweichnamen: Skimview, Flipvue. Eine Markenrecherche (EUIPO/USPTO, Klasse 9) steht vor der ersten Veröffentlichung noch aus. |
| 2 | Lizenz | Open Source, **GPL-3.0-or-later**, veröffentlicht auf GitHub (Abschnitt 11) |
| 3 | Signierung | Developer-ID-Konto vorhanden → signierte und notarisierte Releases (Abschnitt 10.1) |
| 4 | Technik | Qt 6 + C++20 (Abschnitt 9). Der M0-Spike bestätigt die Wahl; Rust bleibt nur als Ausweichlösung. |
| 5 | Mindestversion | **macOS 15 Sequoia** |
| 6 | Bewertungen | Speicherort **wählbar**: XMP-Zusatzdatei, im Bild eingebettet und/oder Finder-Tags (F-CULL-02) |
| 7 | Mausrad & Co. | in den Einstellungen wählbar, mit sinnvollen Standards und Profilvorlagen (Abschnitt 4.14) |
| 8 | Fenster | **beliebig viele Fenster**, Standard: neues Fenster pro Öffnen; Anordnen und Koppeln (Abschnitt 4.15) |

### 14.2 Noch offen

1. **Plattformgleiche Darstellung:** Soll RAW unter Windows/Linux später genauso aussehen wie auf dem Mac? Apple RAW Engine und LibRaw entwickeln unterschiedlich. Entscheidung erst in M4 nötig.
2. **resvg und KImageFormats (entschieden in M3):** resvg kommt als vorgebaute Homebrew-Bibliothek `libresvg` (C-API, dynamisch gelinkt, `LF_ENABLE_RESVG`); XCF und PSB lesen eigene Decoder, KImageFormats wird nicht gebraucht.

---

## 15. Entwicklungsprozess und automatische Prüfungen

Die Entwicklung erfolgt in Stufen (Abschnitt 13). Jede Anforderungs-ID aus Abschnitt 4 ist **nachverfolgbar**: im Code-Kommentar oder Testnamen, z. B. `TEST(SortEngine, F_SORT_01_FinderNameOrder)`.

### 15.1 Automatische Prüfungen (Pflicht vor jedem Merge)

| Prüfung | Werkzeug | Inhalt |
|---|---|---|
| Build | CMake + Ninja, `-Werror` für eigene Quellen | App, Core-Bibliothek und Tests bauen fehlerfrei |
| Unit-Tests | GoogleTest (Core), Qt Test (UI-Logik) | SortEngine, .DS_Store-Leser, Collection, Prefetch-Planer, Einstellungen, Eingabe-Belegung, FileOps/Undo, XMP |
| Integrationstests | Qt Test, Offscreen-Plattform bzw. echtes Fenster auf dem macOS-Runner | Datei öffnen → Sammlung → Navigation; mehrere Fenster; Papierkorb + Undo; Formatkorpus dekodieren |
| Formatkorpus-Test | eigener Testlauf | jede Datei in `tests/corpus/` wird dekodiert; Maße, Orientierung und Pixelstichproben werden mit `expected.json` verglichen |
| Leistungstests | Benchmark-Ziel `lf_bench` | alle P-IDs mit Toleranz; Regression > 20 % schlägt fehl. Benchmarks gelten nur bei geringer Systemlast (CI: eigener Runner, Toleranz 2,0) |
| Sanitizer | ASan + UBSan im Debug-Build | ganze Testsuite ohne Befund |
| Statische Analyse | clang-tidy (Profil im Repo), clang-format-Prüfung | keine neuen Warnungen |
| Lizenzprüfung | REUSE-Tool | SPDX-Kopfzeilen vollständig |
| Spezifikationsabdeckung | Skript `tools/spec_coverage.py` | listet alle F-/P-IDs der aktuellen Stufe und prüft, ob es zu jeder mindestens einen Test oder eine dokumentierte manuelle Prüfung gibt |

Lokal läuft alles mit einem Befehl: `./tools/check.sh`. In GitHub Actions laufen dieselben Schritte auf `macos-15`.

### 15.2 Prüfende Agenten (Review)

Zusätzlich zu den automatischen Prüfungen prüfen KI-Agenten jede abgeschlossene Arbeitseinheit:

1. **Spezifikations-Prüfer:** vergleicht die Umsetzung Anforderung für Anforderung mit diesem Dokument und meldet Lücken bzw. Abweichungen mit ID.
2. **Code-Prüfer:** sucht Fehler, Abstürze, Threading- und Speicherprobleme sowie Verstöße gegen die Architektur (Abschnitt 8).
3. **Abnahme-Tester:** startet die gebaute App mit dem Testkorpus, führt die Abnahmekriterien (12.1) per Skript bzw. UI-Automatisierung aus und berichtet.

Befunde werden behoben und erneut geprüft, bis keine Befunde mehr offen sind. Erst dann gilt eine Stufe als fertig.

### 15.3 Manuelle Abnahme

Am Ende jeder Stufe gibt es eine **Testanleitung** (`docs/TESTING-M<n>.md`) mit den Schritten, die nur ein Mensch sinnvoll prüfen kann: Farbeindruck, HDR auf echtem Display, Gefühl beim Blättern, Finder-Sortierung in eigenen Ordnern.

---

## Anhang A – Recherche-Notizen (Kurzfassung)

- **Vergleichbare Programme:**
  - macOS: Xee³, Phoenix Slides, Pixea, ApolloOne, Lyn, ViewPic, Picview, qView, XnView MP, Sequential.
  - Windows/Linux: IrfanView, FastStone, ImageGlass, nomacs, qimgv, Gwenview, Loupe, feh, JPEGView, BandiView.
- **Funktionen, die übernommen werden** (Vorbilder in Klammern):
  - Schnellablage per Ziffer (FastStone, qimgv)
  - Aussortieren mit P/X und `_Rejected` (FastRawViewer)
  - Vergleich mit synchronem Zoom (FastStone, XnView)
  - Randpanels im Vollbild (FastStone)
  - Ziffern-Zoommodi (ImageGlass, qimgv)
  - Diashow-Intervall per Ziffer (Phoenix Slides)
  - Gain-Map-HDR mit SDR-Vorschau (ApolloOne Pro)
  - abgeschottete Decoder (Loupe/glycin)
  - Lupe (FastStone)
- **Häufige Beschwerden über andere Viewer, die hier vermieden werden sollen:**
  - Sandbox-Abfragen
  - Abos
  - aufgegebene Apps
  - langsamer Start
  - Hängen auf Netzlaufwerken
  - Infopanel schließt beim Blättern
  - nicht ausblendbare Bedienelemente
  - falsche Sortierung
  - fehlendes HDR
- **ImageIO auf macOS 27.0.1** (gemessen per `CGImageSourceCopyTypeIdentifiers`): 62 lesbare Typen, inklusive JPEG XL, AVIF, HEIC, nahezu aller RAW-Formate, PSD, EXR, HDR, JPEG 2000, DDS, KTX und DICOM. Nicht enthalten sind SVG, QOI, PCX, XCF, KRA, ORA und JPEG XR.
- **.DS_Store auf diesem Mac:** In 162 echten Dateien wurden `vstl`, `lsvC` (sortColumn + columns[].ascending), `lsvp`, `icvp` (arrangeBy), `Iloc`, `bwsp` und `GRP0` gefunden. Die Einstellung für Ordner X steht in der `.DS_Store` des **Elternordners** unter dem Namen X.
- **Finder-Namenssortierung:** `localizedStandardCompare` entspricht `compare(options: [.caseInsensitive, .numeric, .widthInsensitive, .forcedOrdering], locale: .current)`. Das wurde auf diesem Mac verifiziert. Plattformübergreifendes Äquivalent: ICU-Collator mit numerischer Sortierung und Stärke SECONDARY.
