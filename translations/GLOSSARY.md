# Glossar und Stilregeln der Oberfläche

Deutsch ist die Ausgangssprache von Lumaflip: Jeder Text in `tr()` ist deutsch, die 17
Übersetzungen (`lumaflip_<sprache>.ts`) übersetzen daraus. Dieses Glossar legt fest, **welches
Wort für welchen Begriff** steht – überall, in Menüs, Dialogen, Statusmeldungen, Tooltips,
Einstellungen, Dokumentation und Website. Ein Begriff, ein Wort.

Referenzen, in dieser Reihenfolge:

1. **macOS (Deutsch), wie das System selbst es anzeigt.** Geprüft an den Lokalisierungen von
   macOS 27 auf diesem Rechner (`AppKit.framework/Resources/*.loctable`,
   `Preview.app/…/MainMenu.loctable`, Abschnitt „de“). Diese Dateien sind die verlässlichste
   Quelle für Apple-Begriffe (Menüs, Standardbefehle, Tastennamen).
2. **Apple Human Interface Guidelines** und Apple-Stil (Anrede, Auslassungspunkte, Anführungszeichen).
3. **Fotografie:** deutsche Oberfläche von Adobe Lightroom (Markieren/Ablehnen, Bewertung),
   Capture One, Apple Fotos. Lightroom Classic war hier nicht zum Nachsehen verfügbar; die
   Begriffe stammen aus der deutschen Adobe-Hilfe zu Lightroom und deutschen Fachartikeln
   (Unsicherheit siehe Anmerkungen).
4. **Windows-spezifische Texte:** Microsoft-Terminologie (Desktophintergrund, Standard-Apps,
   Programme (*.exe)).

Änderungen an diesem Glossar → Quelltexte mit `tools/i18n_rename_source.py` umbenennen (siehe
unten), damit vorhandene Übersetzungen erhalten bleiben.

## Stilregeln (Deutsch)

| Thema | Regel | Beispiel |
|---|---|---|
| Anrede | **du**, klein geschrieben (wie macOS seit 2021: „Möchtest du die Änderungen sichern?“). Nie „Sie“. Wer den Nutzer direkt auffordert, nutzt den Imperativ in der du-Form; „bitte“ sparsam. | „Wähle eine andere App.“, „Öffne einen kleineren Ordner.“ |
| Kurzlabels | Menüs, Knöpfe, Spaltenköpfe, Platzhalter: Infinitiv oder Substantiv, **ohne** Punkt. | „In den Papierkorb legen“, „Bild oder Ordner hierher ziehen“ |
| Sätze | Meldungen, Erklärungen und Tooltips mit ganzen Sätzen enden mit Punkt; kurze Statusmeldungen (Toast, Statusleiste) ohne Punkt. | „Die Datei existiert nicht mehr.“ / „Bild kopiert“ |
| Groß-/Kleinschreibung | Normale deutsche Rechtschreibung, **kein** englisches Title Case. | „Nach Bewertung automatisch weiter“ |
| Auslassungspunkte | Befehle, die vor der Ausführung noch etwas abfragen (Dialog, Dateiauswahl), enden auf **Leerzeichen + „…“** (U+2026, nie drei Punkte). macOS selbst setzt ein geschütztes Leerzeichen (U+00A0, „Einstellungen …“); im Quelltext genügt ein normales Leerzeichen – Menüeinträge brechen nicht um. Gleiches für laufende Vorgänge. | „Öffnen …“, „Exportieren …“, „Wird geladen …“ |
| Anführungszeichen | Deutsche Anführungszeichen **„…“** (U+201E/U+201C). Für Namen von Dateien, Menübefehlen und Knöpfen in Fließtext. Gerade `"` nur in Code/Shell-Beispielen. | „„%1“ existiert bereits.“ |
| Menüpfade | Mit **→** und Leerzeichen; Menüs ohne Anführungszeichen, der letzte Befehl darf in „…“. | „Einstellungen → Bewertung → Schnellablage“, „Fenster → „Fenster koppeln …““ |
| Tastenkürzel | macOS-Symbole ohne Leerzeichen und ohne „+“: ⌃ ⌥ ⇧ ⌘ in dieser Reihenfolge, dann die Taste. Einzelne Tasten als Großbuchstabe bzw. Name. Tastennamen wie macOS: **Leertaste**, **Esc**, **Enter**/Eingabetaste, **Bild↑ / Bild↓**, **Pos1 / Ende**, Pfeile **← → ↑ ↓**. | „⇧⌘H“, „⌥←“, „Taste E“ |
| Bereiche | Gedankenstrich ohne Leerzeichen für Zahlenbereiche. | „1–9“, „0–5 Sterne“ |
| Gedankenstrich | Halbgeviertstrich mit Leerzeichen „ – “ in Sätzen. Fenstertitel trennen mit „ — “ (wie macOS-Titelleisten). | „Nichts gesichert – schneide das Bild erneut zu.“ |
| Zahlen und Einheiten | Zahl, geschütztes oder normales Leerzeichen, Einheit: „100 %“, „15 Pixel“, „2 GB“, „120 ms“. Dezimalzahlen, Größen und Daten **nicht** selbst formatieren, sondern `core/LocaleFormat.h`. Malzeichen „×“ mit Leerzeichen bei Maßen („4000 × 3000“). | „Einpassen ↔ 100 %“ |
| Plural | Immer `tr("%n …", nullptr, n)`; Quelltext mit Klammerform „Bild(er)“, die echten Formen stehen in `lumaflip_de.ts`. | „%n Bild(er) kopiert“ |
| Geschlechtergerechte Sprache | Neutral formulieren, wo es geht („Fotografie / Lightroom“ statt „Fotograf:in“). Wenn nicht möglich: Doppelpunkt wie macOS („Benutzer:in“). | – |
| Keine Abkürzungen | In der Oberfläche keine „bzw.“, „inkl.“, „ggf.“ in Kurzlabels; in Sätzen sparsam. | „Vollbild oder Modus beenden“ |
| Umgangssprache | Nicht „geht nicht“, „ist nicht mehr da“, „gibt es schon“; stattdessen „ist nicht möglich“, „existiert nicht mehr“, „existiert bereits“. | „Zuschneiden ist nur in der Einzelbildansicht möglich.“ |
| Produktnamen | Lumaflip, Finder, Quick Look, Live Text, Spotlight, Kurzbefehle-App, Apple Karten, Lightroom bleiben unverändert. | – |

## Begriffe

Spalten: **Begriff** (Konzept) · **Deutsch** (verbindlich) · **Englisch** (Referenz aus
`lumaflip_en.ts`) · **Vermeiden** · **Quelle/Anmerkung**.

### Menüs und Standardbefehle (macOS)

| Begriff | Deutsch | Englisch | Vermeiden | Quelle / Anmerkung |
|---|---|---|---|---|
| Menü File | Ablage | File | Datei | macOS |
| Menü Edit | Bearbeiten | Edit | | macOS |
| Menü View | Darstellung | View | Ansicht | macOS |
| Menü Window | Fenster | Window | | macOS |
| Menü Help | Hilfe | Help | | macOS |
| Settings… | Einstellungen … | Settings… | Einstellungen…, Voreinstellungen | AppKit `Settings…` = „Einstellungen …“ |
| About | Über Lumaflip | About Lumaflip | | macOS |
| Quit | Lumaflip beenden | Quit Lumaflip | | macOS |
| Open… | Öffnen … | Open… | | macOS |
| Open Recent | Benutzte Dokumente | Open Recent | Zuletzt geöffnet (im Menü) | AppKit `Open Recent`; der Startbildschirm darf die Überschrift „Zuletzt geöffnet“ tragen |
| Clear Menu | Einträge löschen | Clear Menu | | AppKit |
| Close / Close All | Schließen / Alle schließen | Close / Close All | | AppKit |
| Save | **Sichern** | Save | Speichern (für den Befehl) | AppKit/Vorschau: „Sichern“, „Sichern unter:“, „Trotzdem sichern“. „Speichern“ nur im Sinn von *ablegen/aufbewahren* (ein Format „kann keine Ausrichtung speichern“, „Speicherort“) |
| Save As | Sichern unter … | Save As… | Speichern unter | AppKit |
| Undo / Redo | **Widerrufen** / Wiederholen | Undo / Redo | Rückgängig | AppKit; Verb im Satz: „⌘Z widerruft …“. „„%1“ widerrufen“ für benannte Schritte |
| Repeat (Diashow) | Wiederholen | Repeat | | gleiches Wort wie Redo, Kontext trennt |
| Copy | Kopieren | Copy | | |
| Move to Trash | In den Papierkorb legen | Move to Trash | In den Papierkorb, löschen | macOS/Vorschau. Verb im Satz: „in den Papierkorb legen“, Fortsetzung „aus dem Papierkorb bewegen“ |
| Show in Finder | Im Finder zeigen | Show in Finder | Im Finder anzeigen | Finder-Kontextmenü (AppKit nutzt für Dokumente auch „im Finder anzeigen“) |
| Get Info | Informationen | Get Info | | Finder: Ablage → Informationen |
| Open With | Öffnen mit | Open With | | AppKit |
| Share | Teilen | Share | | AppKit |
| Rename | Umbenennen … | Rename… | | |
| Minimize / Zoom | Minimieren / Zoomen | Minimize / Zoom | | AppKit („Im Dock ablegen“ ist „Minimize All“-Variante) |
| Bring All to Front | Alle nach vorne bringen | Bring All to Front | | AppKit |
| Full screen | Vollbild (Befehl/Zustand), Vollbildmodus (macOS-Menübefehl) | Full Screen | Vollbild aktivieren | AppKit: „Full Screen“ = „Vollbild“, „Enter Full Screen“ = „Vollbildmodus“ |
| Tabs | Tab, Tableiste, Vorherigen/Nächsten Tab anzeigen, Tab in ein neues Fenster bewegen, Alle Fenster zusammenführen | Tab | Reiter, Registerkarte | AppKit |
| Choose… / Choose | Wählen … / Auswählen | Choose… / Choose | Auswählen … | AppKit `Choose…` = „Wählen …“ |
| Set as Desktop Picture | Als Schreibtischhintergrund festlegen | Set as Desktop Picture | Hintergrundbild | Windows-Text: „Desktophintergrund“ (Microsoft) |
| Check for Updates | Nach Updates suchen … | Check for Updates… | | Sparkle |

### Allgemeine Begriffe

| Begriff | Deutsch | Englisch | Vermeiden | Quelle / Anmerkung |
|---|---|---|---|---|
| Application | **App** | app | Programm, Anwendung | macOS und Windows 10/11 sagen „App“ („Standard-App“, „Kurzbefehle-App“). Ausnahme: Windows-Dateifilter „Programme (*.exe)“ und ausführbare Programme in Befehlszeilen eigener Aktionen |
| App bundle | App-Paket | app bundle | Programmpaket | |
| Default app | Standard-App | default app | Standardprogramm | |
| Editor | Editor („Im Editor öffnen“) | Editor | Standard-Editor | Der Editor ist einstellbar (Einstellungen → Allgemein → Editor) |
| Settings (Einstellungen-Fenster) | Einstellungen | Settings | Optionen, Voreinstellungen | Bereiche: Allgemein, Fenster, Anzeige, Maus & Tastatur, Navigation & Sortierung, Bewertung, Formate, Erweitert |
| Keyboard shortcut | Tastenkürzel | keyboard shortcut | Kurzbefehl (= Apple Shortcuts), Hotkey | Spaltenkopf darf „Kürzel“ heißen |
| Shortcut (Apple Shortcuts) | Kurzbefehl, Kurzbefehle-App | Shortcut, Shortcuts app | | macOS |
| Custom action | Eigene Aktion | Custom Action | | |
| Status bar | Statusleiste | status bar | Statuszeile | Finder „Statusleiste einblenden“ |
| Title bar | Titelleiste | title bar | | |
| Info panel | Infopanel (Befehl), Titel „Informationen“ | Info Panel / Info | Infobereich | |
| Show/hide UI | einblenden / ausblenden | show / hide | zeigen (für UI-Elemente) | macOS. „anzeigen“ für Inhalte und Darstellungsarten („HDR anzeigen“, „RAW anzeigen“) |
| Collection | Sammlung | collection | Liste | alle Bilder eines Fensters |
| Folder | Ordner | folder | Verzeichnis | |
| File extension | Endung | extension | Dateiendung, Suffix | Finder („Endung „.jpg“ beibehalten“) |
| Already exists | „%1“ existiert bereits. | already exists | gibt es schon | |
| No longer exists | existiert nicht mehr | no longer exists | ist nicht mehr vorhanden/da | |
| Password | Passwort, passwortgeschützt | password | Kennwort | macOS |
| Memory (RAM) | Arbeitsspeicher | memory | Speicher (allein) | „Nicht genug Arbeitsspeicher für das Bild.“ |
| Disk space | Speicherplatz | space | Platz | |
| Backup (für Widerrufen) | Sicherungskopie | backup copy | Sicherung | „Sicherungskopie für „Widerrufen““ |
| Backup (Einstellungen) | Backup der Einstellungen | Back Up Settings | Einstellungen sichern | „sichern“ ist für den Befehl Sichern reserviert |
| Mouse pointer | Mauszeiger | pointer / cursor | Cursor, Zeiger | |
| Scroll wheel | Mausrad | scroll wheel | | |
| Click / double-click | Klick, Doppelklick, klicken | click | | |
| Drag and drop | Drag & Drop; ziehen | drag and drop | | |
| Internet connection | Internetverbindung | internet connection | Netz | |
| Desktop environment (Linux) | Desktopumgebung | desktop environment | Arbeitsumgebung | |
| Display | Bildschirm | display / screen | Monitor, Display | |
| Leave a mode | (Modus) beenden | exit | verlassen | „Vollbild oder Modus beenden“ |
| Restore defaults | Auf Standard zurücksetzen / Standard-… wiederherstellen | Restore Defaults | | |

### Bildansicht

| Begriff | Deutsch | Englisch | Vermeiden | Quelle / Anmerkung |
|---|---|---|---|---|
| Image | Bild | image | Foto (in UI-Texten; „Foto“ nur im Marketing) | |
| Visible area (viewport) | **Ausschnitt** | view / visible area | Bildausschnitt für den Zuschnitt | „Ausschnitt verschieben“ = pan |
| Pan | Ausschnitt verschieben | pan | Ausschnitt scrollen | „scrollen“ nur für Bildanfang/-ende und Mausrad allgemein |
| Zoom to fit | Einpassen | Zoom to Fit | Größe an Fenster anpassen (Vorschau) | kurz und durchgehend genutzt; Varianten „Einpassen (auch vergrößern)“, „Einpassen (nicht vergrößern)“ |
| Actual size | 100 % | Actual Size / 100% | Originalgröße | Menü: „100 % (1:1 Pixel)“ |
| Zoom in / out | Vergrößern / Verkleinern | Zoom In / Out | | macOS |
| Lock zoom | Zoom fixieren | Lock Zoom | | |
| Filmstrip | Filmstreifen | Filmstrip | | |
| Grid | Raster | Grid | Übersicht | |
| Loupe | Lupe | loupe | | Vorschau |
| Slideshow | Diashow | Slideshow | | macOS |
| Frame (animation) | Einzelbild | frame | Frame | |
| Page | Seite | page | | |
| Overlays (Foto-Hilfen) | Einblendungen | Overlays | Overlays | |
| Clipping warning | Über-/Unterbelichtungswarnung | clipping warning | Über-/Unterbelichtung (allein) | |
| Focus peaking | Focus Peaking | focus peaking | | Fachbegriff, unübersetzt |
| AF points | Fokuspunkte | AF points | AF-Punkte | |
| Exposure | Belichtung, Belichtungsregler | exposure | | |
| Bit depth | Bittiefe | bit depth | Farbtiefe | |
| Compression | Komprimierung, Komprimierungsart | compression | Kompression | |
| Folder map | Ordnerkarte | Folder Map / Map | Karte (allein, für die Ordnerkarte) | „Karte einblenden“/„Karte ausblenden“ = Kartenausschnitt im Infopanel |
| Location (GPS) | Aufnahmeort | location | Ort, Ortsangabe, Ortsdaten | Metadaten: Breitengrad, Längengrad, Höhe |

### Bearbeiten und Dateien

| Begriff | Deutsch | Englisch | Vermeiden | Quelle / Anmerkung |
|---|---|---|---|---|
| Crop (action) | Zuschneiden | Crop | Beschneiden | Vorschau |
| Crop (result/area) | Zuschnitt, Zuschnittbereich | crop / selection | Ausschnitt | „Ausschnitt“ ist der sichtbare Bildteil |
| Rotate | Drehen, Links/Rechts drehen, Drehung sichern | Rotate | | Vorschau |
| Flip | Horizontal/Vertikal spiegeln | Flip | | Vorschau |
| Lossless | verlustfrei | lossless | | |
| Re-encode | neu kodieren, Neukodieren | re-encode | neu komprimieren | |
| Auxiliary images | Zusatzbilder | auxiliary images | weitere Bilder | HDR-Gain-Map, Tiefenkarte, Porträt-Maske |
| Sidecar file | XMP-Zusatzdatei | sidecar file | Begleitdatei, Sidecar | |
| Export | Exportieren, Export | Export | | |
| Quick move | Schnellablage, Ziel der Schnellablage | Quick Move | | |
| Name conflict | Namenskonflikt; Ersetzen / Beide behalten / Überspringen | Replace / Keep Both / Skip | | Finder |
| Keep (unchanged) | beibehalten | keep | behalten (bei Endung, Datum) | Finder „„.jpg“ beibehalten“; „Beide behalten“ bleibt (Finder) |

### Bewerten und Aussortieren

| Begriff | Deutsch | Englisch | Vermeiden | Quelle / Anmerkung |
|---|---|---|---|---|
| Rating | Bewertung, bewerten, Sterne | rating, stars | | Lightroom, Fotos |
| Culling | Aussortieren, Aussortiermodus | culling | Sichten | |
| Flag (allgemein) | Markierung | flag | | Lightroom: „Markierung“ |
| Pick (Befehl, `P`) | **Markieren** | Pick | Behalten, Pick | Lightroom (deutsch): „P markiert ein Foto“. „Auswählen“ (Lightroom-Web) ist hier vermieden, weil „Auswahl“ schon die Mehrfachauswahl im Raster ist |
| Pick (Zustand) | **Markiert**, „nur markierte“ | Pick / picks | Pick, Behalten | |
| Reject (Befehl, `X`) | **Ablehnen** | Reject | Verwerfen | Lightroom: „Ablehnen“, Zustand „Abgelehnt“ |
| Reject (Zustand) | **Abgelehnt**, „nur abgelehnte“ | Rejected | Verworfen | |
| Unflag (`U`) | Markierung aufheben | Unflag | Markierung zurücksetzen/weg | Lightroom |
| Unrated | unbewertet | unrated | | |
| `_Rejected` folder | Ordner „_Rejected“ | | | fester Ordnername, nicht übersetzen |
| Finder tag names | „Pick“, „Reject“, „★★★“ | | | Standardwerte der Tag-Namen sind **Daten**, nicht Oberfläche – nicht übersetzen |

## Hinweise für die anderen Sprachen

Spätere Durchgänge je Sprache ergänzen dieses Glossar um eine Spalte pro Sprache (oder eine
eigene Datei `GLOSSARY.<code>.md` mit denselben Zeilen). Struktur:

| Begriff | Deutsch | Englisch | fr | es | … | Quelle (Apple-Lokalisierung der Sprache) |
|---|---|---|---|---|---|---|

- Für Menüs und Standardbefehle die Begriffe der jeweiligen macOS-Lokalisierung verwenden – sie
  lassen sich auf einem Mac aus `/System/Library/Frameworks/AppKit.framework/Resources/MenuCommands.loctable`
  und `…/Preview.app/Contents/Resources/MainMenu.loctable` lesen (Abschnitt mit dem Sprachcode).
- Anrede, Auslassungspunkte und Anführungszeichen nach den Regeln der Sprache bzw. Apples
  (z. B. Französisch: „…“ mit geschütztem Leerzeichen, « » mit Leerzeichen; Japanisch: „…“ ohne Leerzeichen).
- Ein Begriff des deutschen Glossars ergibt in jeder Sprache **genau einen** Begriff.

## Quelltexte umbenennen, ohne Übersetzungen zu verlieren

Eine reine Umformulierung (gleiche Bedeutung) in eine Zuordnungsdatei schreiben
(`tools/i18n_renames/*.tsv`: `Kontext<TAB>alt<TAB>neu[<TAB>Pluralersetzungen]`), dann:

```sh
python3 tools/i18n_rename_source.py tools/i18n_renames/<datei>.tsv --src src --dry-run   # prüfen
python3 tools/i18n_rename_source.py tools/i18n_renames/<datei>.tsv --src src             # anwenden
cmake --build build/check --target lf_lupdate
```

Das Werkzeug benennt die `<source>` in allen `.ts`-Dateien um (Übersetzungen bleiben „fertig“),
passt die deutschen Pluralformen in `lumaflip_de.ts` an und ersetzt mit `--src` die
Zeichenkettenliterale im Code (auch über mehrere Zeilen verteilte). Jede Ersetzung im Code wird
ausgegeben: Treffer in Daten (z. B. Tag-Standardwerte) oder in einem anderen Kontext von Hand
zurücknehmen. Ändert sich die Bedeutung, nicht umbenennen – dann soll lupdate den Text neu
(„unfinished“) anlegen.

## Englisch, Polnisch, Tschechisch (en, pl, cs)

Durchgesehen im Oktober 2026 (alle Texte, maschinell; Review durch Muttersprachler:innen steht
aus). Quellen: macOS-Lokalisierungen auf einem Mac mit macOS 27 (`AppKit …/MenuCommands.loctable`,
`Preview.app …/MainMenu.loctable`, `FinderKit …/LocalizableMerged.loctable`, Abschnitte
`en`/`en_CA`, `pl`, `cs`), Apple Style Guide und Human Interface Guidelines (Englisch),
Microsoft-Terminologie für reine Windows-Texte, Lightroom-Begriffe für die Fotografie.

### Stilregeln

| Thema | Englisch (en) | Polnisch (pl) | Tschechisch (cs) |
|---|---|---|---|
| Anrede | *you*, Imperativ ohne „please“ („Choose a different name.“) | du-Form wie Apple pl („Czy na pewno chcesz …?“, „Wybierz …“) | Sie-Form (vykání) wie Apple cs („Opravdu chcete …?“, „Zkuste to znovu.“) |
| Befehle | **Title Case** (HIG): Menüs, Menübefehle, Knöpfe, Fenstertitel, Gruppenüberschriften, Spaltenköpfe, Einträge von Aufklappmenüs („Show When Pointer Moves“) | Imperativ wie Apple pl („Zamknij“, „Przenieś do Kosza“) | Infinitiv wie Apple cs („Zavřít“, „Přesunout do koše“) |
| Labels, Hilfetexte | **Sentence case**: Checkboxen, Feldbeschriftungen („Maximum size:“), Tooltips, Status- und Fehlermeldungen | Satzschreibung | Satzschreibung |
| Kurzformen | wie Apple: can’t, couldn’t, isn’t, doesn’t, won’t (Apostroph U+2019) | – | – |
| Auslassungspunkte | „…“ direkt angehängt: „Open…“, „Loading…“ | „Otwórz…“ | „Otevřít…“ |
| Anführungszeichen | “…” (U+201C/U+201D) | „…” (U+201E/U+201D, wie Apple pl) | „…“ (U+201E/U+201C) |
| Gedankenstrich im Satz | Geviertstrich ohne Leerzeichen („nothing was saved—crop again“); Fenstertitel und Trenner zwischen Namen „ — “ | „ – “ | „ – “ |
| Aufzählungen | serielles Komma („Lightroom, Capture One, and darktable“) | – | – |
| Schreibung | US-Englisch (color, analyze, canceled) | – | – |
| Zahlen | „100%“, „2 GB“, „1–9“ | „100%“, „2 GB“ | „100 %“, „2 GB“ |

### Begriffe

| Begriff | Deutsch | Englisch | Polnisch | Tschechisch |
|---|---|---|---|---|
| Menüs | Ablage / Bearbeiten / Darstellung / Fenster / Hilfe | File / Edit / View / Window / Help | Plik / Edycja / Widok / Okno / Pomoc | Soubor / Úpravy / Zobrazení / Okno / Nápověda |
| Settings… | Einstellungen … | Settings… | Ustawienia… | Nastavení… |
| Einstellungsbereich „Fenster“ | Fenster | Windows | Okna | Okna |
| Open Recent / Clear Menu | Benutzte Dokumente / Einträge löschen | Open Recent / Clear Menu | Otwórz ostatnie / Wyczyść menu | Otevřít poslední položku / Vyprázdnit nabídku |
| Startbildschirm | Zuletzt geöffnet | Recently Opened | Ostatnio otwierane | Naposledy otevřené |
| Save / Save As | Sichern / Sichern unter … | Save / Save As… | Zachowaj / Zachowaj jako… | Uložit / Uložit jako… |
| Undo / Redo | Widerrufen / Wiederholen | Undo / Redo | Cofnij / Przywróć | Odvolat akci / Opakovat akci (im Satz: odvolat) |
| Copy | Kopieren | Copy | Kopiuj | Zkopírovat |
| Move to Trash | In den Papierkorb legen | Move to Trash (im Satz: move to the Trash) | Przenieś do Kosza | Přesunout do koše |
| Show in Finder | Im Finder zeigen | Show in Finder (Fließtext: „in the Finder“) | Pokaż w Finderze | Zobrazit ve Finderu |
| Get Info | Informationen | Get Info / Info | Informacje | Informace |
| Open With / Share | Öffnen mit / Teilen | Open With / Share | Otwórz w / Udostępnij | Otevřít pomocí / Sdílet |
| Rename | Umbenennen … | Rename… | Zmień nazwę… | Přejmenovat… |
| Minimize / Zoom / Bring All to Front | Minimieren / Zoomen / Alle nach vorne bringen | Minimize / Zoom / Bring All to Front | Minimalizuj / Zoom / Umieść wszystko na wierzchu | Minimalizovat / Přepnout velikost / Převést vše do popředí |
| Full screen | Vollbild | Full Screen | Pełny ekran | Celá obrazovka |
| Tab | Tab | tab | karta | panel |
| Choose… | Wählen … | Choose… | Wybierz… | Vybrat… |
| Check for Updates | Nach Updates suchen … | Check for Updates… | Sprawdź uaktualnienia… | Vyhledat aktualizace… |
| Desktop picture | Schreibtischhintergrund (Windows: Desktophintergrund) | desktop picture (Windows: desktop background) | obraz biurka (Windows: tło pulpitu) | obrázek plochy (Windows: pozadí plochy) |
| App / default app | App / Standard-App | app / default app (Windows: Default apps) | aplikacja / domyślna aplikacja (Windows: Aplikacje domyślne) | aplikace / výchozí aplikace (Windows: Výchozí aplikace) |
| Keyboard shortcut | Tastenkürzel | keyboard shortcut | skrót klawiszowy | klávesová zkratka |
| Shortcut (Apple) | Kurzbefehl, Kurzbefehle-App | shortcut, Shortcuts app | skrót, aplikacja Skróty | zkratka, aplikace Zkratky |
| Custom action | Eigene Aktion | Custom Action | własna akcja | vlastní akce |
| Status bar / title bar | Statusleiste / Titelleiste | status bar / title bar | pasek stanu / pasek tytułu | stavový řádek / záhlaví okna |
| Info panel | Infopanel | Info Panel | panel informacji | informační panel |
| Collection | Sammlung | collection | kolekcja | kolekce |
| File extension | Endung | extension | rozszerzenie | přípona |
| Memory / disk space | Arbeitsspeicher / Speicherplatz | memory / disk space | pamięć / miejsce na dysku | paměť / místo na disku |
| Display | Bildschirm | display | ekran | monitor |
| Pointer / scroll wheel | Mauszeiger / Mausrad | pointer / scroll wheel | wskaźnik / kółko myszy | ukazatel / kolečko myši |
| Backup copy (Widerrufen) | Sicherungskopie | backup copy | kopia zapasowa | záložní kopie |
| Settings backup | Backup der Einstellungen | Settings Backup | Kopia zapasowa ustawień | Záloha nastavení |
| Image | Bild | image (nicht *photo*) | obraz | obrázek |
| Visible area / pan | Ausschnitt / Ausschnitt verschieben | visible area / Pan (im Satz: pan the view) | widoczny obszar / przesuń widok | zobrazená oblast / posunout zobrazení |
| Zoom to fit / actual size | Einpassen / 100 % | Zoom to Fit / Actual Size (100%) | Dopasuj / 100% | Velikost podle okna / 100 % |
| Zoom in / out / lock | Vergrößern / Verkleinern / Zoom fixieren | Zoom In / Zoom Out / Lock Zoom | Powiększ / Pomniejsz / Zablokuj powiększenie | Zvětšit / Zmenšit / Zamknout zvětšení |
| Filmstrip / grid / loupe | Filmstreifen / Raster / Lupe | Filmstrip / Grid / Loupe | Taśma filmowa / Siatka / Lupa | Filmový pás / Mřížka / Lupa |
| Slideshow / frame | Diashow / Einzelbild | Slideshow / frame | Pokaz slajdów / klatka | Prezentace / snímek |
| Overlays | Einblendungen | Overlays | Nakładki | Překryvy |
| Clipping warning | Über-/Unterbelichtungswarnung | Clipping Warning | ostrzeżenie o prześwietleniu i niedoświetleniu | upozornění na přeexpozici/podexpozici |
| Focus peaking / focus points | Focus Peaking / Fokuspunkte | Focus Peaking / Focus Points | Focus peaking / punkty ostrości | Focus peaking / body zaostření |
| Folder map / location | Ordnerkarte / Aufnahmeort | Folder Map / location | Mapa folderu / lokalizacja | Mapa složky / poloha |
| Compare | Vergleich | Compare | Porównanie | Porovnání |
| Crop (action / area) | Zuschneiden / Zuschnitt | Crop / crop, crop area (nie *selection*) | Kadruj / kadr, obszar kadrowania | Oříznout / oříznutí, oblast oříznutí |
| Rotate / flip | Links drehen / Horizontal spiegeln | Rotate Left / Flip Horizontal | Obróć w lewo / Odwróć poziomo | Otočit doleva / Převrátit vodorovně |
| Lossless / re-encode | verlustfrei / neu kodieren | lossless / re-encode | bezstratnie / zakodować ponownie | bezeztrátový / překódovat |
| Auxiliary images / sidecar | Zusatzbilder / XMP-Zusatzdatei | auxiliary images / XMP sidecar file | obrazy dodatkowe / plik dodatkowy XMP | doplňkové obrázky / doprovodný soubor XMP |
| Quick Move | Schnellablage | Quick Move | Szybkie przenoszenie | Rychlé přesunutí |
| Name conflict | Ersetzen / Beide behalten / Überspringen | Replace / Keep Both / Skip | Zastąp / Pozostaw obie / Pomiń | Nahradit / Ponechat obě / Přeskočit |
| Rating / stars / unrated | Bewertung / Sterne / unbewertet | Rating / stars / unrated | ocena / gwiazdki / bez oceny | hodnocení / hvězdičky / bez hodnocení |
| Culling | Aussortieren / Aussortiermodus | Culling / Culling Mode | selekcja / tryb selekcji | třídění / režim třídění |
| Pick (Befehl / Zustand / Filter) | Markieren / Markiert / nur markierte | Pick / Picked / Picks Only | Oznacz jako wybrane / Wybrane / Tylko wybrane | Označit jako vybrané / Vybráno / Jen vybrané |
| Reject (Befehl / Zustand) | Ablehnen / Abgelehnt | Reject / Rejected | Odrzuć / odrzucone | Odmítnout / Odmítnuto |
| Unflag | Markierung aufheben | Unflag | Usuń flagę | Zrušit označení |

Anmerkungen:

- **Englisch:** Wo die Spalte „Englisch“ der Tabellen oben (Stand vor dieser Durchsicht)
  abweicht, gilt dieser Abschnitt. Geändert wurden unter anderem: *Folder Map* (statt „Map“), *Focus Points* (statt
  „AF Points“), *Clipping Warning* (statt „Over-/Underexposure“), *Minimal Window Mode*,
  *Open in Editor* (statt „Open in Default Editor“), *Restore Default Zoom*, Presets
  *Mac Default* / *Windows Image Viewer* / *Photography / Lightroom*.
- **Polnisch:** Apple pl verwendet für „Save“ *Zachowaj*; „beibehalten“ ist deshalb *Pozostaw*.
  „Quick Look“ und „Live Text“ bleiben als Produktnamen (macOS pl zeigt „Szybki przegląd“ bzw.
  „Tekst na żywo“ – beim Review entscheiden).
- **Tschechisch:** Vorschau (cs) verwendet sowohl *Zvětšit/Zmenšit* als auch *Přiblížit/Oddálit*;
  Lumaflip nimmt *Zvětšit/Zmenšit* (passt zum Substantiv *zvětšení*).
