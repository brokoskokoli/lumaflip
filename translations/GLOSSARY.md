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

## Japanisch, Chinesisch (vereinfacht/traditionell), Koreanisch (ja, zh_CN, zh_TW, ko)

Quelle für Menüs, Standardbefehle und Systembegriffe sind die Lokalisierungen von macOS 27
(`AppKit.framework/Resources/MenuCommands.loctable`, `Menus.loctable`, `Document.loctable`,
`Preview.app/…/MainMenu.loctable`, `FinderKit.framework/…/LocalizableMerged.loctable`,
`Photos.app`/`PhotosUICore.framework`, `System Settings.app`/`ExtensionKit`), Abschnitte `ja`,
`zh_CN`, `zh_TW`, `ko`. Texte, die nur unter Windows erscheinen, folgen der Microsoft-Terminologie;
Fotografiebegriffe (Markieren/Ablehnen) der lokalisierten Lightroom-Oberfläche, sonst Apple Fotos.
Unsichere Entscheidungen sind mit **(?)** markiert.

### Stilregeln

| Thema | ja | zh_CN | zh_TW | ko |
|---|---|---|---|---|
| Register | Sätze in です/ます („…できません。“, „…してください。“); Menüs, Knöpfe, Spaltenköpfe als Nomen bzw. Verb-Grundform („開く“, „ゴミ箱に入れる“) | neutral-höflich wie Apple, „你“ (nie „您“); Aufforderungen mit „请“ sparsam | wie zh_CN, „你“ | formell wie Apple: Aussagen auf „-습니다/-ㅂ니다“, Aufforderungen „-하십시오“, Fragen „-하겠습니까?“; Menüs als Nomen („열기“, „휴지통으로 이동“) |
| Leerzeichen CJK ↔ Latein/Ziffern | **keins** („Finderに表示“, „Lumaflipについて“, „%1枚“) | **keins** („在访达中显示“, „移到iCloud“) | **keins** („在Finder中顯示“, „設為預設App“) – Apple setzt in zh_TW keine Leerzeichen (≈3000 Texte ohne, 8 mit) | normale koreanische Wortabstände; Partikeln direkt an Latein/Platzhalter („Finder에서“, „%1개“) |
| Satzzeichen | 、。 vollbreit; Klammern （）; Doppelpunkt und Fragezeichen halbbreit wie Apple („移動先:“, „…よろしいですか?“) | ，。：；？！（） vollbreit | ，。：；？！（） vollbreit | westliche Satzzeichen, Klammern ohne Leerzeichen davor („무제(%1)“) |
| Anführungszeichen | “%1” (Apple ja, nicht 「」) | “%1” | 「%1」 | ‘%1’ |
| Auslassungspunkte | „…“ direkt am Wort („開く…“) | „…“ („打开…“) | „…“ (U+2026) – Apple nutzt in zh_TW „⋯“ (U+22EF), der Test `F_I18N_06_EllipsisFollowsTheLanguageStyle` verlangt aber U+2026 in allen Sprachen **(?)** | „…“ („열기…“) |
| Gedankenstrich „ – “ | nicht übernehmen: Satz teilen, „。“, „：“ oder Klammer | „——“ oder Satz teilen | „——“ oder Satz teilen | Satz teilen oder „ - “ nur in Kurzmeldungen |
| Menüpfade | „設定 → 一般“ (Pfeil wie im Deutschen), Befehle in “” | „设置 → 通用“, Befehle in “” | 「設定」→「一般」 bzw. „設定 → 一般“ | „설정 → 일반“, Befehle in ‘’ |
| Zahlen/Einheiten | „100%“, „15ピクセル“, „%n枚“ | „100%“, „15像素“, „%n张“ | „100%“, „15像素“, „%n張“ | „100%“, „15픽셀“, „%n개“/„%n장“ |
| Plural | eine Form (`%n` bleibt) | eine Form | eine Form | eine Form |
| Produktnamen | Finder, Lumaflip, Spotlight, Quick Look → クイックルック, Live Text → テキスト認識表示, Kurzbefehle-App → “ショートカット”App, Apple Karten → “マップ” | Finder → **访达** (in Sätzen in “”: 在“访达”中), Quick Look → 快速查看, Live Text → 实况文本, Spotlight → 聚焦, Kurzbefehle → “快捷指令”App, Karten → “地图” | Finder, Quick Look → 快速查看, Live Text → 原況文字, Spotlight, Kurzbefehle → 「捷徑」App, Karten → 「地圖」 | Finder, Quick Look → 훑어보기, Live Text → 라이브 텍스트, Spotlight, Kurzbefehle → 단축어 앱, Karten → 지도 앱 |

### Menüs und Standardbefehle (macOS)

| Deutsch | ja | zh_CN | zh_TW | ko |
|---|---|---|---|---|
| Ablage (File) | ファイル | 文件 | 檔案 | 파일 |
| Bearbeiten | 編集 | 编辑 | 編輯 | 편집 |
| Darstellung (View) | 表示 | 显示 | 顯示方式 | 보기 |
| Fenster | ウインドウ | 窗口 | 視窗 | 윈도우 |
| Hilfe | ヘルプ | 帮助 | 輔助說明 | 도움말 |
| Einstellungen … | 設定… | 设置… | 設定… | 설정… |
| Über Lumaflip | Lumaflipについて | 关于Lumaflip | 關於Lumaflip | Lumaflip에 관하여 |
| Lumaflip beenden | Lumaflipを終了 | 退出Lumaflip | 結束Lumaflip | Lumaflip 종료 |
| Öffnen … | 開く… | 打开… | 打開… | 열기… |
| Benutzte Dokumente | 最近使った項目を開く | 打开最近使用 | 打開最近使用過的檔案 | 최근 사용 열기 |
| Einträge löschen (Menü) | メニューを消去 | 清除菜单 | 清除選單 | 메뉴 지우기 |
| Schließen / Alle schließen | 閉じる / すべてを閉じる | 关闭 / 全部关闭 | 關閉 / 關閉全部 | 닫기 / 모두 닫기 |
| Sichern / Sichern unter … | 保存 / 別名で保存… | 保存 / 保存为… | 儲存 / 儲存為… | 저장 / 별도 저장… |
| Widerrufen / Wiederholen | 取り消す / やり直す | 撤销 / 重做 | 還原 / 重做 | 실행 취소 / 실행 복귀 |
| „%1“ widerrufen | “%1”を取り消す | 撤销“%1” | 還原「%1」 | ‘%1’ 실행 취소 |
| Kopieren | コピー | 拷贝 | 拷貝 | 복사하기 |
| In den Papierkorb legen | ゴミ箱に入れる | 移到废纸篓 | 丟到垃圾桶 | 휴지통으로 이동 |
| Papierkorb | ゴミ箱 | 废纸篓 | 垃圾桶 | 휴지통 |
| Im Finder zeigen | Finderに表示 | 在访达中显示 | 在Finder中顯示 | Finder에서 보기 |
| Informationen (Get Info) | 情報を見る | 显示简介 | 取得資訊 | 정보 가져오기 |
| Öffnen mit | このアプリケーションで開く | 打开方式 | 打開方式 | 다음으로 열기 |
| Teilen | 共有 | 共享 | 分享 | 공유 |
| Umbenennen … | 名称変更… | 重新命名… | 重新命名… | 이름 변경… |
| Minimieren / Zoomen | しまう / 拡大/縮小 | 最小化 / 缩放 | 縮到最小 / 縮放 | 최소화 / 확대/축소 |
| Alle nach vorne bringen | すべてを手前に移動 | 前置全部窗口 | 將此程式所有視窗移至最前 | 모두 앞으로 가져오기 |
| Vollbild / Vollbildmodus | フルスクリーン / フルスクリーンにする | 全屏幕 / 进入全屏幕 | 全螢幕 / 進入全螢幕 | 전체 화면 / 전체 화면 시작 |
| Tab, Tableiste | タブ、タブバー | 标签页、标签页栏 | 標籤頁、標籤頁列 | 탭, 탭 막대 |
| Wählen … / Auswählen | 選択… / 選択 | 选取… / 选取 | 選擇… / 選擇 | 선택… / 선택 |
| einblenden / ausblenden | 表示 / 非表示 | 显示 / 隐藏 | 顯示 / 隱藏 | 보기 / 가리기 |
| Vergrößern / Verkleinern | 拡大 / 縮小 | 放大 / 缩小 | 放大 / 縮小 | 확대 / 축소 |
| Einpassen (Zoom to Fit) | ウインドウに合わせる | 缩放至窗口大小 | 縮放到適當大小 | 크기에 맞게 확대/축소 |
| 100 % (Originalgröße) | 実際のサイズ | 实际大小 | 實際大小 | 실제 크기 |
| Exportieren … | 書き出す… | 导出… | 輸出… | 내보내기… |
| Zuschneiden | 切り取り | 裁剪 | 裁切 | 자르기 |
| Links/Rechts drehen | 反時計回りに回転 / 時計回りに回転 | 向左旋转 / 向右旋转 | 向左旋轉 / 向右旋轉 | 왼쪽으로 회전 / 오른쪽으로 회전 |
| Horizontal/Vertikal spiegeln | 水平方向に反転 / 垂直方向に反転 | 水平翻转 / 垂直翻转 | 水平翻轉 / 垂直翻轉 | 수평 뒤집기 / 수직 뒤집기 |
| Lupe | ルーペ | 放大镜 | 放大鏡 | 돋보기 |
| Diashow | スライドショー | 幻灯片放映 | 幻燈片秀 | 슬라이드쇼 |
| Nach Updates suchen … | アップデートを確認… | 检查更新… | 檢查更新項目… | 업데이트 확인… |
| Als Schreibtischhintergrund festlegen | 壁紙に設定 | 设为墙纸 | 設為背景圖片 | 배경화면으로 설정 |
| Ersetzen / Beide behalten / Überspringen | 置き換える / 両方とも残す / スキップ | 替换 / 保留两者 / 跳过 | 取代 / 兩者皆保留 / 略過 | 대치 / 둘 다 유지 / 건너뛰기 |
| Abbrechen / OK / Fertig | キャンセル / OK / 完了 | 取消 / 好 / 完成 | 取消 / 好 / 完成 | 취소 / 확인 / 완료 |
| Nicht mehr fragen | 次回から確認しない | 不再询问 | 不再詢問 | 다시 묻지 않음 |

### Allgemeine Begriffe

| Deutsch | ja | zh_CN | zh_TW | ko | Anmerkung |
|---|---|---|---|---|---|
| App | アプリ | App | App | 앱 | Apple 2026; nicht アプリケーション/应用程序/應用程式/응용 프로그램 |
| Standard-App (macOS) | デフォルトのアプリ | 默认App | 預設App | 기본 앱 | |
| Standard-App (Windows) | 既定のアプリ | 默认应用 | 預設應用程式 | 기본 앱 | Microsoft; nur in Windows-Texten |
| Einstellungen (Fenster) | 設定 | 设置 | 設定 | 설정 | Bereiche: 一般/通用/一般/일반, 詳細/高级/進階/고급 |
| Systemeinstellungen | システム設定 | 系统设置 | 系統設定 | 시스템 설정 | |
| Datenschutz & Sicherheit | プライバシーとセキュリティ | 隐私与安全 | 隱私權與安全性 | 개인정보 보호 및 보안 | |
| Festplattenvollzugriff | フルディスクアクセス | 完全磁盘访问权限 | 完全取用磁碟 | 전체 디스크 접근 권한 | |
| Tastenkürzel | キーボードショートカット | 键盘快捷键 | 鍵盤快速鍵 | 키보드 단축키 | nicht Kurzbefehle-App |
| Kurzbefehl (Apple Shortcuts) | ショートカット | 快捷指令 | 捷徑 | 단축어 | |
| Eigene Aktion | カスタムアクション | 自定义操作 | 自訂動作 | 사용자 지정 동작 | |
| Statusleiste / Titelleiste / Symbolleiste | ステータスバー / タイトルバー / ツールバー | 状态栏 / 标题栏 / 工具栏 | 狀態列 / 標題列 / 工具列 | 상태 막대 / 제목 막대 / 도구 막대 | |
| Infopanel | 情報パネル | 信息面板 | 資訊面板 | 정보 패널 | |
| Sammlung | コレクション | 集合 | 集合 | 모음 | **(?)** nur intern sichtbar |
| Ordner | フォルダ | 文件夹 | 檔案夾 | 폴더 | zh_TW Apple: 檔案夾, nicht 資料夾 |
| Endung | 拡張子 | 扩展名 | 副檔名 | 확장자 | |
| Passwort | パスワード | 密码 | 密碼 | 암호 | |
| Arbeitsspeicher | メモリ | 内存 | 記憶體 | 메모리 | |
| Speicherplatz | 空き領域 | 储存空间 | 儲存空間 | 저장 공간 | |
| Sicherungskopie | バックアップ | 备份 | 備份 | 백업 | |
| Mauszeiger / Mausrad | ポインタ / スクロールホイール | 指针 / 滚轮 | 指標 / 滾輪 | 포인터 / 스크롤 휠 | |
| Klick / Doppelklick | クリック / ダブルクリック | 点按 / 连按 | 按一下 / 按兩下 | 클릭 / 두 번 클릭 | Apple-Begriffe |
| ziehen (Drag & Drop) | ドラッグ | 拖移 | 拖移 | 드래그 | |
| Bildschirm | ディスプレイ (Gerät) / 画面 (Fläche) | 显示器 / 屏幕 | 顯示器 / 螢幕 | 디스플레이 / 화면 | |
| Desktophintergrund (Windows) | デスクトップの背景 | 桌面背景 | 桌面背景 | 바탕 화면 배경 | Microsoft |
| Programme (*.exe) (Windows) | プログラム | 程序 | 程式 | 프로그램 | Microsoft |
| Fehler | エラー | 错误 | 錯誤 | 오류 | |
| existiert bereits / nicht mehr | すでに存在します / もう存在しません | 已存在 / 已不存在 | 已存在 / 已不存在 | 이미 존재합니다 / 더 이상 존재하지 않습니다 | |

### Bildansicht, Bearbeiten, Fotografie

| Deutsch | ja | zh_CN | zh_TW | ko | Anmerkung |
|---|---|---|---|---|---|
| Bild | 画像 | 图像 | 影像 | 이미지 | Apple (Vorschau); „Foto“ nur für Fotos-App |
| Ausschnitt (sichtbarer Bildteil) | 表示範囲 | 显示区域 | 顯示範圍 | 표시 영역 | |
| Ausschnitt verschieben | 表示範囲を移動 | 移动显示区域 | 移動顯示範圍 | 표시 영역 이동 | |
| Zoom fixieren | ズームを固定 | 锁定缩放 | 鎖定縮放 | 확대/축소 고정 | |
| Filmstreifen | フィルムストリップ | 连续画面 | 膠片列 | 필름 스트립 | Apple Fotos |
| Raster | グリッド | 网格 | 格狀 | 격자 | Apple Fotos |
| Einzelbild (Animation) | フレーム | 帧 | 影格 | 프레임 | |
| Seite | ページ | 页面 | 頁面 | 페이지 | |
| Einblendungen | オーバーレイ | 叠层 | 覆疊 | 오버레이 | |
| Über-/Unterbelichtungswarnung | 白飛び/黒つぶれ警告 | 过曝/欠曝警告 | 過曝/欠曝警告 | 노출 과다/부족 경고 | **(?)** Lightroom: „クリッピング“ |
| Focus Peaking | フォーカスピーキング | 峰值对焦 | 峰值對焦 | 포커스 피킹 | |
| Fokuspunkte | AFポイント | 对焦点 | 對焦點 | 초점 포인트 | |
| Belichtung | 露出 | 曝光 | 曝光 | 노출 | |
| Bittiefe | ビット深度 | 位深度 | 位元深度 | 비트 심도 | |
| Ordnerkarte | フォルダの地図 | 文件夹地图 | 檔案夾地圖 | 폴더 지도 | „Karte“ allein: 地図/地图/地圖/지도 |
| Aufnahmeort | 撮影場所 | 拍摄地点 | 拍攝地點 | 촬영 위치 | |
| verlustfrei / neu kodieren | 無劣化 / 再エンコード | 无损 / 重新编码 | 無損 / 重新編碼 | 무손실 / 다시 인코딩 | |
| Zuschnitt (Ergebnis/Bereich) | 切り取り範囲 | 裁剪区域 | 裁切範圍 | 자르기 영역 | nicht „Auswahl“ (= Mehrfachauswahl) |
| Verschieben (Datei) | 移動 | 移动 | 移動 | 이동 | **(?)** Apple zh_TW mischt 搬移/移動 |
| Zusatzbilder | 補助画像 | 辅助图像 | 輔助影像 | 보조 이미지 | |
| XMP-Zusatzdatei | XMPサイドカーファイル | XMP附属文件 | XMP附屬檔案 | XMP 사이드카 파일 | |
| Schnellablage | クイック移動 | 快速归档 | 快速歸檔 | 빠른 이동 | |
| Bewertung / bewerten | 評価 / 評価する | 评分 / 评分 | 評分 / 評分 | 별점 / 별점 지정 | Apple Fotos |
| unbewertet | 評価なし | 未评分 | 未評分 | 별점 없음 | |
| Aussortieren / Aussortiermodus | 選別 / 選別モード | 选片 / 选片模式 | 選片 / 選片模式 | 선별 / 선별 모드 | **(?)** |
| Markieren (Pick, P) / markiert | 採用 / 採用 | 留用 / 留用 | 留用 / 留用 | 선택 / 선택됨 | Lightroom (ja: 採用フラグ, zh: 留用旗标) **(?)** ko |
| Ablehnen (Reject, X) / abgelehnt | 除外 / 除外 | 排除 / 排除 | 排除 / 排除 | 제외 / 제외됨 | Lightroom **(?)** zh_TW, ko |
| Markierung aufheben (U) | フラグを解除 | 取消旗标 | 取消旗標 | 플래그 해제 | Lightroom |
| Markierung (Flag allgemein) | フラグ | 旗标 | 旗標 | 플래그 | Lightroom |

Weitere Festlegungen aus dem Review (Oktober 2026): Statusmeldungen „X ein/aus“ in ja als
„X: オン“ / „X: オフ“; Tastennamen „Page Up“/„Page Down“ in zh_TW und ko (ja behält „Page↑/Page↓“
**(?)**); Einheiten-Suffixe der Drehfelder „ ミリ秒“, „ 毫秒“, „ 秒“, „ 초“ (führendes Leerzeichen
bleibt); der Standard-Dateiname einer zugeschnittenen Kopie ist in ja/zh_CN/zh_TW übersetzt
(„%1-切り取り.%2“, „%1-已裁剪.%2“, „%1-已裁切.%2“), in ko bleibt „%1-cropped.%2“ **(?)**.
„Festlegen“ (Knopf) ist in ja/zh_TW „設定“ und damit gleich lautend mit „Einstellungen“ **(?)**.

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

## Niederländisch, Schwedisch, Dänisch, Norwegisch (Bokmål), Finnisch (nl, sv, da, nb, fi)

Stand: Oktober 2026, maschinell gegen die Plattform-Lokalisierungen geprüft, **nicht** von
Muttersprachler:innen gegengelesen. Quellen, in dieser Reihenfolge:

1. **macOS 27** auf diesem Rechner: `AppKit.framework/Resources/MenuCommands.loctable`,
   `FunctionKeyNames.loctable`, `Preview.app/…/MainMenu.loctable`, Fotos, Finder-nahe
   Frameworks (Abschnitte `nl`, `sv`, `da`, `no` = Bokmål, `fi`). Werte in der Tabelle sind
   wörtlich von dort übernommen, wo „(macOS)“ steht.
2. **Microsoft-Terminologie** (Windows 11) für Texte, die nur unter Windows erscheinen.
3. **Fotografie:** Lightroom (Classic/Cloud) in der jeweiligen Sprache, soweit bekannt, sonst
   gebräuchliche Fachsprache. Lightroom Classic ist nach unserem Kenntnisstand nicht auf
   Dänisch, Norwegisch oder Finnisch lokalisiert und war hier nicht zum Nachsehen verfügbar;
   die Begriffe fürs Bewerten und Aussortieren sind daher weniger sicher (siehe Anmerkungen).

### Stil je Sprache

| Thema | nl | sv | da | nb | fi |
|---|---|---|---|---|---|
| Anrede | *je/jouw* (wie macOS), Imperativ ohne Pronomen: „Kies een andere app.“ | *du*, Imperativ: „Välj en annan app.“ | *du*, Imperativ: „Vælg en anden app.“ | *du*, Imperativ: „Velg en annen app.“ | Imperativ 2. Person Singular („Valitse toinen appi.“), sonst unpersönlich/Passiv wie macOS |
| Menübefehle, Knöpfe | Imperativ wie macOS („Bewaar“, „Open“, „Toon in Finder“, „Verplaats naar prullenmand“) | Imperativ („Spara“, „Öppna“) | Imperativ („Gem“, „Åbn“) | Imperativ („Lagre“, „Åpne“) | Imperativ („Tallenna“, „Avaa“) |
| Ein-/Ausblenden | Toon / Verberg | Visa / Göm | Vis / Skjul | Vis / Skjul | Näytä / Kätke |
| Auslassungspunkte | „…“ (U+2026) **ohne** Leerzeichen: „Open…“, „Bezig met laden…“ | „Öppna…“ | „Åbn…“ | „Åpne…“ | „Avaa…“ |
| Anführungszeichen | '…' (wie macOS: „Wijzig naam van '%1'“) | ”…” (U+201D beidseitig) | “…” | «…» | ”…” (U+201D beidseitig) |
| Prozent | „100%“ ohne Leerzeichen | „100 %“ | „100 %“ | „100 %“ | „100 %“ |
| Groß-/Kleinschreibung | nur erstes Wort groß (kein Title Case) | wie nl | wie nl | wie nl | wie nl |
| App | app, „de Opdrachten-app“ | app, „appen Genvägar“ | app, „appen Genveje“ | app, „Snarveier-appen“ | **appi** (wie macOS: „Avaa apilla“, „oletusappi“), nicht „sovellus“ |
| Tasten | Spatiebalk, Esc, Return, ← → ↑ ↓ | Mellanslag, Esc, Retur | Mellemrum, Esc, Retur | Mellomrom, Esc, Retur | Välilyönti, Esc, Return |

### Begriffe

| Begriff | Deutsch | nl | sv | da | nb | fi | Quelle / Anmerkung |
|---|---|---|---|---|---|---|---|
| Menü File | Ablage | Archief | Arkiv | Arkiv | Fil | Tiedosto | macOS (AppKit). fi: einige Apple-Apps sagen „Arkisto“, AppKit „Tiedosto“ |
| Menü Edit | Bearbeiten | Wijzig | Redigera | Rediger | Rediger | Muokkaa | macOS |
| Menü View | Darstellung | Weergave | Innehåll | Oversigt | Vis | Näytä | macOS (sv/da wörtlich so, nicht „Visa“/„Vis“) |
| Menü Window | Fenster | Venster | Fönster | Vindue | Vindu | Ikkuna | macOS |
| Menü Help | Hilfe | Help | Hjälp | Hjælp | Hjelp | Ohje | macOS |
| Settings… | Einstellungen … | Instellingen… | Inställningar… | Indstillinger… | Innstillinger… | Asetukset… | macOS |
| About / Quit | Über Lumaflip / Lumaflip beenden | Over Lumaflip / Stop Lumaflip | Om Lumaflip / Avsluta Lumaflip | Om Lumaflip / Slut Lumaflip | Om Lumaflip / Avslutt Lumaflip | Tietoja: Lumaflip / Lopeta Lumaflip | macOS |
| Open… / Open With | Öffnen … / Öffnen mit | Open… / Open met | Öppna… / Öppna med | Åbn… / Åbn med | Åpne… / Åpne med | Avaa… / Avaa apilla | macOS |
| Open Recent / Clear Menu | Benutzte Dokumente / Einträge löschen | Open recente bestanden / Wis menu | Öppna senaste / Rensa menyn | Åbn seneste / Ryd menu | Åpne sist brukte / Tøm meny | Avaa äskeinen / Tyhjennä valikko | macOS |
| Close / Close All | Schließen / Alle schließen | Sluit / Sluit alles | Stäng / Stäng alla | Luk / Luk alt | Lukk / Lukk alle | Sulje / Sulje kaikki | macOS |
| Save / Save As… | Sichern / Sichern unter … | Bewaar / Bewaar als… | Spara / Spara som… | Gem / Gem som… | Lagre / Lagre som… | Tallenna / Tallenna nimellä… | macOS |
| Undo / Redo | Widerrufen / Wiederholen | Herstel / Opnieuw | Ångra / Gör om | Fortryd / Gentag | Angre / Utfør likevel | Peru / Tee sittenkin | macOS. fi: „Kumoa“ ist in macOS *Abbrechen* – nie für Undo |
| Repeat (Diashow) | Wiederholen | Herhaal | Upprepa | Gentag | Gjenta | Toista uudelleen | macOS (Musik, Kurzbefehle) |
| Copy | Kopieren | Kopieer | Kopiera | Kopier | Kopier | Kopioi | macOS |
| Move to Trash | In den Papierkorb legen | Verplaats naar prullenmand | Flytta till papperskorgen | Flyt til papirkurv | Flytt til papirkurv | Siirrä roskakoriin | macOS (Vorschau) |
| Trash | Papierkorb | prullenmand | papperskorgen | papirkurv | papirkurv | roskakori | macOS |
| Show in Finder | Im Finder zeigen | Toon in Finder | Visa i Finder | Vis i Finder | Vis i Finder | Näytä Finderissa | macOS |
| Get Info / Info | Informationen | Toon info / Info | Visa info / Info | Vis info / Info | Vis info / Info | Näytä tiedot / Tiedot | macOS (Fotos) |
| Share | Teilen | Deel | Dela | Del | Del | Jaa | macOS |
| Rename… | Umbenennen … | Wijzig naam… | Byt namn… | Omdøb… | Endre navn… | Nimeä uudelleen… | macOS |
| Minimize / Zoom | Minimieren / Zoomen | Minimaliseer / Zoom | Minimera / Zooma | Minimer / Zoom | Minimer / Zoom | Pienennä / Zoomaa | macOS |
| Bring All to Front | Alle nach vorne bringen | Alles op voorgrond | Lägg alla överst | Anbring alle forrest | Legg alle øverst | Tuo kaikki eteen | macOS |
| Full Screen | Vollbild | Schermvullend (Zustand: schermvullende weergave) | Helskärm (Modus: helskärmsläge) | Fuld skærm | Fullskjerm (Modus: fullskjermmodus) | Koko näyttö (Modus: koko näytön tila) | macOS |
| Tab / Tab bar | Tab / Tableiste | tabblad / tabbladbalk | flik / flikfältet | fane / fanelinje | fane / fanelinje | välilehti / välilehtipalkki | macOS |
| Choose… | Wählen … | Kies… | Välj… | Vælg… | Velg… | Valitse… | macOS |
| Cancel / OK | Abbrechen / OK | Annuleer / OK | Avbryt / OK | Annuller / OK | Avbryt / OK | Kumoa / OK | macOS |
| Replace / Keep Both / Skip | Ersetzen / Beide behalten / Überspringen | Vervang / Behoud beide / Sla over | Ersätt / Behåll båda / Hoppa över | Erstat / Bevar begge / Spring over | Erstatt / Behold begge / Hopp over | Korvaa / Pidä molemmat / Ohita | macOS |
| Set as Desktop Picture | Als Schreibtischhintergrund festlegen | Gebruik als bureaubladafbeelding | Använd som skrivbordsbild | Brug som skrivebordsbillede | Bruk som skrivebordsbilde | Käytä työpöydän taustakuvana | macOS („Use Image as Desktop Picture“) |
| Desktop background (Windows) | Desktophintergrund | bureaubladachtergrond | skrivbordsbakgrund | skrivebordsbaggrund | skrivebordsbakgrunn | työpöydän tausta | Microsoft |
| Check for Updates… | Nach Updates suchen … | Zoek naar updates… | Sök efter uppdateringar… | Søg efter opdateringer… | Se etter oppdateringer… | Tarkista päivitykset… | Sparkle/übliche Form |
| System Settings | Systemeinstellungen | Systeeminstellingen | Systeminställningar | Systemindstillinger | Systeminnstillinger | Järjestelmäasetukset | macOS |
| Default app | Standard-App | standaardapp | förvald app (macOS) / standardapp (Windows) | standardapp | standardapp | oletusappi (macOS) / oletussovellus (Windows) | macOS, Microsoft („Standardapps“, „Standardappar“, „Oletussovellukset“) |
| Keyboard shortcut | Tastenkürzel | toetscombinatie | kortkommando | tastaturgenvej | tastatursnarvei | näppäinoikotie | macOS |
| Apple Shortcuts | Kurzbefehl, Kurzbefehle-App | opdracht, Opdrachten-app | genväg, appen Genvägar | genvej, appen Genveje | snarvei, Snarveier-appen | pikakomento, Pikakomennot-appi | macOS |
| Apple Maps | Apple Karten | Apple Kaarten | Kartor | Apple Kort | Kart | Applen Kartat | macOS |
| Quick Look | Quick Look | Snelle weergave | Överblick | Quick Look | Hurtigvisning | Pikakatselu | macOS-Namen; **da** bleibt „Quick Look“, weil Apples dänischer Name „Vis“ als Menüpunkt nicht von „Vis“ (zeigen) zu unterscheiden ist (unsicher) |
| Live Text | Live Text | Livetekst | Livetext | Livetekst | Tekst i objektivet | Liveteksti | macOS |
| Status bar | Statusleiste | statusbalk | statusfält | statuslinje | statuslinje | tilapalkki | macOS |
| Toolbar / Sidebar | Symbolleiste / Seitenleiste | knoppenbalk / navigatiekolom | verktygsfält / sidofält | værktøjslinje / indholdsoversigt | verktøylinje / sidepanel | työkalupalkki / sivupalkki | macOS |
| Folder | Ordner | map | mapp | mappe | mappe | kansio | macOS |
| File extension | Endung | extensie | filnamnstillägg (kurz: tillägg) | endelse | filendelse | tarkenne | Finder-Umbenennen-Dialoge („Vil du ændre endelsen …“, „endre filendelsen …“, „Muutetaanko tarkenne …“) |
| File (Datei) | Datei | bestand | fil | fil | fil | tiedosto | da: macOS sagt oft „arkiv“ – hier „fil“, weil Lumaflip auch ZIP/RAR-Archive („arkiv“) öffnet; zitierte macOS-Pfade bleiben wörtlich („Arkiv → Vis info“) |
| File format | Format | structuur (Einstellungsbereich „Structuren“) | format | format | format | muoto | nl: wie macOS nl („Structuur:“), für viele ungewohnt – prüfen |
| Tags (Finder) | Tags | tags | taggar | mærker | etiketter | tunnisteet | Finder |
| Password | Passwort | wachtwoord | lösenord | adgangskode | passord | salasana | macOS |
| Memory (RAM) | Arbeitsspeicher | werkgeheugen | arbetsminne | hukommelse (RAM) | minne | muisti | |
| Disk space | Speicherplatz | opslagruimte | lagringsutrymme | lagerplads | lagringsplass | tallennustila | macOS |
| Pointer | Mauszeiger | aanwijzer | pekare | markør | peker | osoitin | macOS |
| Display | Bildschirm | beeldscherm | skärm | skærm | skjerm | näyttö | macOS |
| Collection | Sammlung | verzameling | samling | samling | samling | kokoelma | |
| Custom action | Eigene Aktion | eigen actie | egen åtgärd | egen handling | egen handling | oma toiminto | |
| Image | Bild | afbeelding | bild | billede | bilde | kuva | macOS |
| Visible area / Pan | Ausschnitt / Ausschnitt verschieben | weergave / Verschuif weergave | utsnitt / Flytta utsnitt | udsnit / Flyt udsnit | utsnitt / Flytt utsnitt | näkymä / Siirrä näkymää | |
| Zoom to fit | Einpassen | Passend | Anpassa | Tilpas | Tilpass | Sovita ikkunaan | nl macOS (Vorschau); sonst kurze Form von „Anpassa till fönstret“, „Zoom til vindusstørrelse“ |
| Zoom in / out | Vergrößern / Verkleinern | Zoom in / Zoom uit | Zooma in / Zooma ut | Zoom ind / Zoom ud | Zoom inn / Zoom ut | Lähennä / Loitonna | macOS |
| Lock zoom | Zoom fixieren | Zet zoom vast | Lås zoom | Lås zoom | Lås zoom | Lukitse zoomaus | |
| Filmstrip | Filmstreifen | filmstrip | filmremsa | filmstrimmel | filmremse | filminauha | macOS |
| Grid | Raster | raster | rutnät | gitter | rutenett | ruudukko | macOS; da: Fotos sagt „Net“, hier „gitter“ (eindeutiger, gängig) – unsicher |
| Loupe | Lupe | loep | lupp | lup | lupe | luuppi | macOS |
| Slideshow | Diashow | diavoorstelling | bildspel | lysbilledshow | lysbildeserie | kuvaesitys | macOS |
| Thumbnails | Miniaturen | miniaturen | miniatyrer | miniaturer | miniatyrbilder | miniatyyrit | macOS (Vorschau) |
| Overlays | Einblendungen | overlays | överlägg | overlejringer | overlegg | peittokuvat | macOS-nahe (Überlagerungsleiste) |
| Crop | Zuschneiden / Zuschnitt | Snij bij / bijsnijding | Beskär / beskärning | Beskær / beskæring | Beskjær / beskjæring | Rajaa / rajaus | macOS |
| Rotate left/right | Links/Rechts drehen | Roteer linksom/rechtsom | Rotera åt vänster/höger | Roter til venstre/højre | Roter mot venstre/høyre | Pyöritä vasemmalle/oikealle | macOS (Vorschau) |
| Flip | Horizontal/Vertikal spiegeln | Spiegel horizontaal/verticaal | Vänd horisontellt/vertikalt | Spejlvend vandret/lodret | Vend horisontalt/vertikalt | Käännä vaakasuunnassa/pystysuunnassa | macOS (Vorschau, Fotos) |
| Export | Exportieren … | Exporteer… | Exportera… | Eksporter… | Eksporter… | Vie… | macOS |
| Metadata | Metadaten | metadata | metadata | metadata | metadata | metatiedot | macOS (Fotos) |
| Location (GPS) | Aufnahmeort | locatie | plats | lokalitet | sted | sijainti | macOS (Fotos/Karten) |
| Exposure | Belichtung | belichting | exponering | eksponering | eksponering | valotus | macOS (Fotos) |
| Lossless | verlustfrei | verliesvrij | förlustfri | tabsfri | tapsfri | häviötön | |
| Quick move | Schnellablage | Snel verplaatsen | Snabbflytt | Hurtigflytning | Hurtigflytting | Pikasiirto | eigene Funktion. Nicht „arkiv“ (= Menü Ablage in sv/da) |
| Rating | Bewertung | waardering | betyg | vurdering | vurdering | arvio | Lightroom nl/sv, macOS |
| Culling | Aussortieren / Aussortiermodus | Schiften / schiftmodus | Gallring / gallringsläge | Frasortering / frasorteringstilstand | Utsortering / utsorteringsmodus | Karsinta / karsintatila | unsicher; nl nicht „selecteren“ (= Auswahl), da/nb nicht „udvælgelse/utvelging“ |
| Pick (Befehl / Zustand) | Markieren / Markiert | Markeren / Gemarkeerd | Flagga / Flaggad | Marker / Markeret | Flagg / Flagget | Merkitse / Merkitty | sv/nb: „Markera/Marker“ heißt in macOS *Auswählen* („Markera allt“, „Marker alt“) – daher „Flagga/Flagg“ |
| Reject (Befehl / Zustand) | Ablehnen / Abgelehnt | Afwijzen / Afgewezen | Avvisa / Avvisad | Afvis / Afvist | Avvis / Avvist | Hylkää / Hylätty | Lightroom, macOS-nahe |
| Unflag | Markierung aufheben | Markering verwijderen | Ta bort flagga | Fjern markering | Fjern flagg | Poista merkintä | |
| Flag (allgemein) | Markierung | markering | flagga | markering | flagg | merkintä | |
| Unrated | unbewertet | zonder waardering | utan betyg | uden vurdering | uten vurdering | arvioimaton | |

Finder-Tag-Standardwerte („Pick“, „Reject“, „★★★“) und der Ordnername „_Rejected“ sind Daten
und bleiben unübersetzt – wie im Deutschen.

## Romanische Sprachen: fr, es, it, pt_BR, pt_PT

Durchgang vom 11.10.2026 (maschinell gegen Plattform-Glossare geprüft, **nicht** von
Muttersprachler:innen). Quellen, in dieser Reihenfolge:

1. **macOS 27**, Lokalisierungen auf dem Rechner: `AppKit.framework/Resources/*.loctable`
   (MenuCommands, FunctionKeyNames, Document …), Vorschau, Fotos, Finder/FinderKit und
   Systemeinstellungen (Abschnitte `fr`, `es`, `it`, `pt_BR`, `pt_PT`).
2. **Windows-only-Texte:** Microsoft-Terminologie der Sprache (Standard-Apps,
   Desktophintergrund, Batchdateien).
3. **Fotografie:** lokalisierte Adobe Lightroom/Apple Fotos (aus dem Gedächtnis, nicht live
   geprüft – siehe „Unsichere Entscheidungen“).

### Stilregeln je Sprache

(Auf Englisch, damit sie auch Übersetzer:innen ohne Deutschkenntnisse lesen können.)

#### Französisch (fr)

- Register: "vous" throughout, as in Apple's French macOS; instructions in vous-form imperative ("Choisissez…", "Saisissez…", "modifiez-en une"). Questions use "Voulez-vous…". "Veuillez" is kept where it was already there, but not added.
- Capitalization: sentence case everywhere (menus, buttons, headings), as Apple French does. Product/feature names keep Apple's localized capitalization ("Texte en direct", "Coup d’œil", "Réglages Système").
- Ellipsis: "…" (U+2026) glued to the preceding word ("Réglages…", "Ouvrir…"), only where the German source has "…"; never "...".
- Typographic apostrophe ’ (U+2019) everywhere (l’image, d’ajout).
- Spacing: U+00A0 before : ; ? ! and » and after «. Plain spaces elsewhere. "%1 : %2" also gets the nbsp, even where English/German have no space.
- Quotes: « » with U+00A0 inside, for file names, command/button names and setting names in running text. Straight " only in code/shell examples.
- Units: Ko/Mo/Go/To/Po, octet(s), "100 %" with U+00A0, "2 Go max.", "%1 bits", "f/%1", "%1 mm", "éq. 24×36".
- Menu paths: "Réglages → Général → Éditeur" with → and spaces. Menus are not quoted, the final command may be in « » (as in the German source).
- Key names: Espace, Esc, Entrée, Début / Fin, Pg↑ / Pg↓, arrows ← → ↑ ↓, macOS symbols ⌃⌥⇧⌘ with no space and no "+".
- Dashes: "–" with spaces inside sentences, as in the source; window titles keep "—"; ranges "1–9" with no spaces.
- Standard commands follow the French AppKit/Preview/Photos loctables (Placer dans la corbeille, Afficher dans le Finder, Lire les informations, Enregistrer, Annuler/Rétablir, Minimiser, Réduire/agrandir, Adapter à la fenêtre, Taille réelle, Diaporama, Recadrer, Faire pivoter, Retourner horizontalement). Windows-only strings use Microsoft terms (Applications par défaut, arrière-plan du Bureau, fichiers batch).
- Status/toast messages have no final period; full sentences do. Past participles agree with the implied noun "image" (Recadrée, Retenue, Rejetée).

#### Spanisch (es)

- **Register:** informal "tú", as in Apple's macOS Spanish ("¿Seguro que quieres…?", "Elige otro nombre.", "Puedes deshacerlo con ⌘Z."). Imperatives in tú form; no "por favor".
- **Questions/exclamations:** always with opening ¿ / ¡ ("¿Trasladar “%1” a la papelera?").
- **Capitalization:** sentence case everywhere (menus, buttons, labels), as in Apple's es localization ("Guardar como…", "Abrir recientes", "Tamaño real"). Product/feature names keep their Apple casing ("Texto en vivo", "Vista rápida", "Ajustes del Sistema").
- **Ellipsis:** single "…" (U+2026), glued to the preceding word ("Ajustes…", "Abrir…", "Cargando…"), only where the German source has it.
- **Quotes:** curly double quotes “ ” for file names, commands and buttons in running text (Apple es: Consultar “%@”); no « ». Straight quotes only in shell/code examples.
- **Punctuation for German " – " in sentences:** colon or semicolon ("No se ha guardado nada: vuelve a recortar."); en dash kept only in title/status separators where the source uses it as a separator; em dash " — " in window titles as in source.
- **Past-tense error messages:** perfect tense as in Apple es ("No se ha podido…", "se ha cancelado"), not preterite ("No se pudo").
- **Menu paths:** "Ajustes → General → Editor", menu names unquoted, final command may be quoted ("Ventana → “Vincular ventanas…”").
- **Key names:** Apple symbols ⌃⌥⇧⌘ glued to the key; named keys "Espacio", "Esc", "Intro", "Re Pág / Av Pág", "Inicio / Fin", arrows ← → ↑ ↓.
- **Units/numbers:** space before % and units ("100 %", "15 píxeles", "2 GB", "120 ms"); "×" with spaces for dimensions; SI units unchanged (KB, MB, GB); GPS West = "O".
- **Terminology:** archive = "archivo comprimido" (never bare "archivo", which means file); move = "mover" (M key), but "Trasladar a la papelera" (Apple); default app = "app por omisión" (Apple es), Windows-only strings use Microsoft "aplicación predeterminada", "fondo de escritorio".
- **Plurals/gender:** image = "imagen" (feminine) – participles agree ("Recortada", "Marcada", "Rechazada", "%n seleccionadas").

#### Italienisch (it)

- **Register:** informal "tu", as Apple macOS Italian uses it ("Vuoi sostituirlo?", "Scegli un altro nome."). Confirmation questions are phrased "Vuoi …?" instead of a bare infinitive ("Spostare …?"). Instructions use the tu imperative ("Riprova", "Consenti l’accesso").
- **Capitalization:** sentence case everywhere: menus, buttons, labels and column headers ("Salva col nome…", "Mostra nel Finder", "Dimensioni reali (100%)"). Proper names and Apple app names keep their capitals ("Cestino", "Comandi Rapidi", "Impostazioni di Sistema", "Testo attivo").
- **Ellipsis:** "…" (U+2026) attached to the previous word with no space ("Impostazioni…", "Caricamento…"), and only where the German source has "…".
- **Quotation marks:** curly “ ” for names of files, commands and buttons in running text, as Apple Italian does (AppKit: “%@” esiste già). All « » were replaced. Straight " stays only in shell/command examples.
- **Apostrophe:** typographic ’ in elisions ("l’immagine", "un’altra").
- **Error style:** "Impossibile + infinitive" for failures ("Impossibile salvare il ritaglio."), as in Apple Italian. Short status messages have no final period. Full sentences do.
- **Dashes and colons:** where German has " – " that joins two clauses, Italian mostly uses a colon ("…: non è stato salvato nulla."). " – " stays in short labels and toasts. Window titles keep " — ".
- **Units/numbers:** percent with no space ("100%", "%1%"). Other units take a space ("15 pixel", "2 GB", "120 ms", "1/250 s"). SI units (KB, MB, GB) are unchanged. GPS West is "O" (Ovest).
- **Menu paths:** "Impostazioni → Valutazione → Archiviazione rapida". Menu names have no quotes, and the final command may be in “ ” ("Finestra → “Collega finestre…”"). Menu names are File, Modifica, Vista, Finestra, Aiuto.
- **Key names:** modifier symbols are glued to the key with no "+" (⇧⌘H, ⌥←). Space is "Barra spaziatrice" (AppKit FunctionKeyNames), Esc is "Esc", Enter/Return is "Invio", Page Up/Down is "Pag↑ / Pag↓", Home/End is "Inizio / Fine".
- **Tabs vs. panels:** macOS Italian calls window tabs "pannelli". For that reason the Info panel became "Riquadro informazioni", so the two never collide.
- **Gender agreement:** status texts about images use the feminine ("Ritagliata", "Contrassegnata", "%n immagini rifiutate"). Windows-only strings use Microsoft Italian terminology ("App predefinite", "sfondo del desktop", "Programmi (*.exe)").

#### Portugiesisch, Brasilien (pt_BR)

- **Register:** "você" as in macOS pt_BR. Use the imperative for instructions ("Escolha outro nome.", "Digite um nome."). Confirmation questions follow AppKit: "Deseja substituí-lo?", "Tem certeza de que deseja …?", or a short infinitive question ("Mover %n imagens para o Lixo?"). Leave out "por favor".
- **Capitalization:** Title Case as in Apple pt_BR for **menu items, push buttons and command/action names** (MainMenu, MenuExtras, context menus, InputBindings action list, dialog buttons): "Salvar Como…", "Mover para o Lixo", "Ajustar à Janela", "Manter Ambos". Articles and prepositions (o, a, de, da, do, para, em, com, e, por, à) stay lowercase. Parenthetical qualifiers inside a menu item are lowercase: "Ajustar à Janela (permitir ampliação)", "100% (1:1 pixel)". Use sentence case everywhere else: checkboxes, radio buttons, settings labels, group titles, metadata field labels, tooltips, status and error messages.
- **Ellipsis:** Use "…" (U+2026) directly after the word, with no space ("Abrir…", "Carregando…"), and only where the German source has "…". Never write "...".
- **Quotation marks:** Use curly “ ” around file names, menu commands and button names in running text, as AppKit pt_BR does ("“%1” já existe.").
- **Menu paths:** Use "→" with spaces. Menu names have no quotes; a quoted final command is allowed: "Ajustes → Geral → Editor", "Janela → “Vincular Janelas…”". Apple's own pt_BR uses ">", but the app keeps the source's →.
- **Key names:** Espaço (AppKit FunctionKeyNames), Esc, Enter, Page Up / Page Down, Home / End, and the arrow symbols ← → ↑ ↓. Modifier symbols go without spaces or "+" (⇧⌘H, ⌥←). "tecla E" for single letters.
- **Numbers and units:** Percent sits next to the number with no space ("100%", "%1%"), as in Brazilian usage. Keep KB/MB/GB, "px", "s" and "ms". Write "0 bytes" (plural for zero). Write dimensions as "%1 × %2 pixels". Use an en dash with spaces " – " in sentences; window titles keep " — ".
- **Abbreviations:** Write "por exemplo" in full in sentences (no "p. ex."). Use "máx." only in compact option labels.
- **Gender and agreement:** "imagem" is feminine, so states agree with it ("Marcada", "Rejeitada", "%n selecionadas", "Recortada sem perdas"). "app" is masculine ("o app padrão").
- **Terminology anchors:** arquivo = file and arquivo compactado = archive (never bare "arquivo" for an archive). Lixo (never "Lixeira"). Ajustes / Ajustes do Sistema. app (not "aplicativo", except Microsoft's "Aplicativos padrão" on Windows). Windows-only strings use Microsoft terms ("Configurações", "plano de fundo da área de trabalho", "arquivos em lote").
- **Data values** stay untranslated: tag names "Pick" / "Reject", the folder "_Rejected", file filters "(*.exe)", and command-line samples ("powershell -File Script.ps1").

#### Portugiesisch, Portugal (pt_PT)

- **Variant:** genuine European Portuguese following Apple's macOS pt_PT loctables: ficheiro, ecrã, definições, guardar, rato, ligação, partilhar, palavra-passe, câmara, píxeis, banda desenhada, controlo, aplicação (never "app", also Microsoft pt-PT), gerund avoided ("A carregar…", "A exportar…").
- **Register:** impersonal / formal third person as in Apple pt_PT: questions "Pretende…?", "Tem a certeza de que pretende…?"; instructions in the formal imperative ("Selecione", "Escolha", "Introduza", "Clique", "Prima"); no "você", no "tu"; "por favor" omitted like Apple.
- **Capitalization:** sentence case everywhere (menus, buttons, titles, column headers), as Apple pt_PT ("Guardar como…", "Tamanho real", "Mover para o Lixo"). Product/app names keep capitals: Lixo, Finder, Definições do Sistema, Atalhos, Mapas, Vista rápida, Texto em direto.
- **Ellipsis:** single "…" (U+2026) glued to the word, no space ("Definições…", "A carregar…"), only where the German source has "…".
- **Quotes:** curly double quotes “ ” for file names, commands and button names in running text (Apple pt_PT uses “%@”); never « » or straight quotes (except shell/code examples).
- **Punctuation:** full sentences end with a period; short status/toast messages and labels without period (same as source). En dash with spaces " – " in sentences; window titles keep " — ". Comma after introductory "Por isso," / "Caso contrário,". Abbreviation "p. ex.,".
- **Menu paths:** "Definições → Geral → Editor", "Ficheiro → Informações"; menus unquoted, final command may be in “ ”.
- **Keys:** macOS symbols ⌃⌥⇧⌘ without "+" or spaces; key names Espaço, Esc, Enter, Pág↑/Pág↓, Início/Fim, arrows ← → ↑ ↓; "tecla E"; "manter premida".
- **Numbers/units:** percentages without space ("100%", Apple pt_PT style); "%1 × %2 píxeis", "px", KB/MB/GB unchanged, "%1 bits"; ranges with en dash "1–9", "0–5".
- **Photo terms:** Rating = classificação; Pick/Reject/Unflag = Marcar / Rejeitar / Remover marcação (states Marcada / Rejeitada); culling = triagem; crop = recortar/recorte; slideshow = diaporama; tags = identificadores (Finder pt_PT).
- **Data values** stay untranslated: tag names “Pick”, “Reject”, “★★★”, folder “_Rejected”, placeholders {name}/{n}/{format}, file filters.

### Begriffe

Ein Begriff des deutschen Glossars ergibt je Sprache genau einen Begriff. „–“ = in der
Sprache nicht gesondert festgelegt (gleich wie die übrigen Zeilen bzw. wie im Text).

#### Menüs und Standardbefehle (macOS)

| Begriff | Deutsch | fr | es | it | pt_BR | pt_PT |
|---|---|---|---|---|---|---|
| Menü File | Ablage | Fichier | Archivo | File | Arquivo | Ficheiro |
| Menü Edit | Bearbeiten | Édition | Edición | Modifica | Editar | Edição |
| Menü View | Darstellung | Présentation | Visualización | Vista | Visualizar | Visualização |
| Menü Window | Fenster | Fenêtre | Ventana | Finestra | Janela | Janela |
| Menü Help | Hilfe | Aide | Ayuda | Aiuto | Ajuda | Ajuda |
| Settings… | Einstellungen … | Réglages… | Ajustes… | Impostazioni… | Ajustes… | Definições… |
| About | Über Lumaflip | À propos de Lumaflip | Acerca de Lumaflip | Informazioni su Lumaflip | Sobre o Lumaflip | Acerca do Lumaflip |
| Quit | Lumaflip beenden | Quitter Lumaflip | Salir de Lumaflip | Esci da Lumaflip | Encerrar Lumaflip | Sair do Lumaflip |
| Open… | Öffnen … | Ouvrir… | Abrir… | Apri… | Abrir… | Abrir… |
| Open Recent | Benutzte Dokumente | Ouvrir l’élément récent | Abrir recientes | Apri recenti | Abrir Recente | Abrir documento recente |
| Clear Menu | Einträge löschen | Effacer le menu | Vaciar menú | Cancella menu | Limpar Menu | Limpar menu |
| Close / Close All | Schließen / Alle schließen | Fermer / Tout fermer | Cerrar / Cerrar todo | Chiudi / Chiudi tutto | Fechar / Fechar Todas | Fechar / Fechar todas |
| Save | **Sichern** | Enregistrer | Guardar | Salva | Salvar | Guardar |
| Save As | Sichern unter … | Enregistrer sous… | Guardar como… | Salva col nome… | Salvar Como… | Guardar como… |
| Undo / Redo | **Widerrufen** / Wiederholen | Annuler / Rétablir | Deshacer / Rehacer | Annulla / Ripristina | Desfazer / Refazer | Desfazer / Refazer |
| Repeat (Diashow) | Wiederholen | Répéter | Repetir | Ripeti | Repetir | Repetir |
| Copy | Kopieren | Copier | Copiar | Copia | Copiar | Copiar |
| Move to Trash | In den Papierkorb legen | Placer dans la corbeille | Trasladar a la papelera | Sposta nel Cestino | Mover para o Lixo | Mover para o Lixo |
| Show in Finder | Im Finder zeigen | Afficher dans le Finder | Mostrar en el Finder | Mostra nel Finder | Mostrar no Finder | Mostrar no Finder |
| Get Info | Informationen | Lire les informations | Obtener información | Ottieni informazioni | Obter Informações | Informações |
| Open With | Öffnen mit | Ouvrir avec | Abrir con | Apri con | Abrir com | Abrir com |
| Share | Teilen | Partager | Compartir | Condividi | Compartilhar | Partilhar |
| Rename | Umbenennen … | Renommer… | Renombrar… | Rinomina… | Renomear… | Alterar nome… |
| Minimize / Zoom | Minimieren / Zoomen | Minimiser / Réduire/agrandir | Minimizar / Zoom | Contrai / Ridimensiona | Minimizar / Zoom | Minimizar / Aumentar/reduzir |
| Bring All to Front | Alle nach vorne bringen | Tout ramener au premier plan | Traer todo al frente | Porta tutto in primo piano | Trazer Todas para a Frente | Passar tudo para a frente |
| Full screen | Vollbild (Befehl/Zustand), Vollbildmodus (macOS-Menübefehl) | Plein écran; Activer le mode plein écran (menu command) | Pantalla completa (state); Usar pantalla completa / Salir de pantalla completa (macOS commands) | A tutto schermo; menu "Attiva modalità a tutto schermo" | Tela Cheia; menu “Entrar em Tela Cheia” | Ecrã completo |
| Tabs | Tab, Tableiste, Vorherigen/Nächsten Tab anzeigen, Tab in ein neues Fenster bewegen, Alle Fenster zusammenführen | onglet, barre d’onglets, Afficher l’onglet précédent/suivant, Placer l’onglet dans une nouvelle fenêtre, Fusionner toutes les fenêtres | pestaña; Mostrar pestaña anterior/siguiente; Trasladar pestaña a una nueva ventana; Fusionar todas las ventanas; barra de pestañas | pannello; barra dei pannelli; Mostra pannello precedente/successivo; Sposta pannello in una nuova finestra; Unisci tutte le finestre | Aba; Mostrar Barra de Abas; Mostrar Aba Anterior/Seguinte; Mover Aba para Nova Janela; Combinar Todas as Janelas | separador; Mostrar separador anterior/seguinte; Mover separador para nova janela; Agrupar todas as janelas; barra de separadores |
| Choose… / Choose | Wählen … / Auswählen | Choisir… / Choisir | Seleccionar… / Seleccionar | Scegli… / Scegli | Escolher… / Escolher | Escolher… / Escolher |
| Set as Desktop Picture | Als Schreibtischhintergrund festlegen | Définir comme fond d’écran | Establecer como fondo de pantalla | Imposta come sfondo della scrivania | Definir como Imagem de Fundo | Definir como imagem de secretária |
| Check for Updates | Nach Updates suchen … | Rechercher des mises à jour… | Buscar actualizaciones… | Cerca aggiornamenti… | Buscar Atualizações… | Procurar atualizações… |

#### Allgemeine Begriffe

| Begriff | Deutsch | fr | es | it | pt_BR | pt_PT |
|---|---|---|---|---|---|---|
| Application | **App** | app | app | app | app (o app) | aplicação |
| App bundle | App-Paket | paquet d’app | paquete de app | pacchetto dell’app | pacote do app | pacote da aplicação |
| Default app | Standard-App | app par défaut | app por omisión | app predefinita | app padrão | aplicação predefinida |
| Editor | Editor („Im Editor öffnen“) | éditeur ("Ouvrir dans l’éditeur") | editor ("Abrir en el editor") | editor ("Apri nell’editor") | Editor (“Abrir no Editor”) | editor ("Abrir no editor") |
| Settings (Einstellungen-Fenster) | Einstellungen | Réglages | Ajustes | Impostazioni (panes: Generali, Finestre, Visualizzazione, Mouse e tastiera, Navigazione e ordinamento, Valutazione, Formati, Avanzate) | Ajustes | Definições |
| Keyboard shortcut | Tastenkürzel | raccourci clavier (column: Raccourci) | función rápida de teclado (column: "Función rápida") | abbreviazione da tastiera; column "Abbreviazione" | atalho de teclado (column: Atalho) | atalho de teclado (column: Atalho) |
| Shortcut (Apple Shortcuts) | Kurzbefehl, Kurzbefehle-App | raccourci, app Raccourcis | atajo; app Atajos | comando rapido; app Comandi Rapidi | atalho; app Atalhos | atalho; aplicação Atalhos |
| Custom action | Eigene Aktion | action personnalisée | acción personalizada | azione personalizzata | ação personalizada | ação personalizada |
| Status bar | Statusleiste | barre d’état | barra de estado | barra di stato | barra de status | barra de estado |
| Title bar | Titelleiste | barre de titre | barra de título | barra del titolo | barra de título | barra de título |
| Info panel | Infopanel (Befehl), Titel „Informationen“ | panneau d’informations; title "Informations" | Panel de información (command); title "Información" | Riquadro informazioni; title "Informazioni" | Painel de Informações; title “Informações” | Painel de informações (command); title "Informações" |
| Show/hide UI | einblenden / ausblenden | afficher / masquer | Mostrar / Ocultar | mostra / nascondi | Mostrar / Ocultar | mostrar / ocultar |
| Collection | Sammlung | collection | colección | raccolta | coleção | coleção |
| Folder | Ordner | dossier | carpeta | cartella | pasta | pasta |
| File extension | Endung | extension | extensión | estensione | extensão | extensão |
| Already exists | „%1“ existiert bereits. | « %1 » existe déjà. | “%1” ya existe. | “%1” esiste già. | “%1” já existe. | “%1” já existe. |
| No longer exists | existiert nicht mehr | n’existe plus | ya no existe | non esiste più | não existe mais | já não existe |
| Password | Passwort, passwortgeschützt | mot de passe, protégé par mot de passe | contraseña; protegido con contraseña | password; protetto da password | senha; protegido por senha | palavra-passe |
| Memory (RAM) | Arbeitsspeicher | mémoire ("Mémoire insuffisante") | memoria | memoria | memória | memória |
| Disk space | Speicherplatz | espace (disque) | espacio | spazio | espaço | espaço |
| Backup (für Widerrufen) | Sicherungskopie | copie de sauvegarde | copia de seguridad | copia di backup | cópia de segurança | cópia de segurança |
| Backup (Einstellungen) | Backup der Einstellungen | Sauvegarde des réglages | Copia de seguridad de los ajustes | Backup delle impostazioni | Backup dos Ajustes | Cópia de segurança das definições |
| Mouse pointer | Mauszeiger | pointeur | puntero | puntatore | ponteiro | ponteiro (do rato) |
| Scroll wheel | Mausrad | molette (de la souris) | rueda del ratón | rotella del mouse | roda do mouse | roda do rato |
| Click / double-click | Klick, Doppelklick, klicken | clic, double-clic, cliquer | clic, doble clic, hacer clic | clic, doppio clic, fare clic | clique, clique duplo, clicar | clique, duplo clique, clicar |
| Drag and drop | Drag & Drop; ziehen | glisser-déposer; faire glisser | arrastrar y soltar; arrastrar | trascinamento; trascinare | arrastar e soltar; arrastar | arrastar e largar |
| Internet connection | Internetverbindung | connexion Internet | conexión a internet | connessione Internet | conexão com a internet | ligação à Internet |
| Desktop environment (Linux) | Desktopumgebung | environnement de bureau | entorno de escritorio | ambiente desktop | ambiente de área de trabalho | ambiente de trabalho |
| Display | Bildschirm | écran | pantalla | schermo | tela | ecrã |
| Leave a mode | (Modus) beenden | quitter (le mode) | salir de (modo) | uscire (dalla modalità) | sair (do modo) | sair (do modo) / terminar |
| Restore defaults | Auf Standard zurücksetzen / Standard-… wiederherstellen | Restaurer les valeurs par défaut / Réinitialiser | Restaurar valores por omisión / Restablecer … | Ripristina valori predefiniti / Ripristina … predefinite | Restaurar Padrões / Redefinir | Repor predefinições / Repor … predefinidos |

#### Bildansicht

| Begriff | Deutsch | fr | es | it | pt_BR | pt_PT |
|---|---|---|---|---|---|---|
| Image | Bild | image | imagen | immagine | imagem | imagem |
| Visible area (viewport) | **Ausschnitt** | zone visible | vista | vista; area visibile | área visível | área visível |
| Pan | Ausschnitt verschieben | déplacer la zone visible | desplazar la vista | Sposta vista | mover a área visível | deslocar área visível |
| Zoom to fit | Einpassen | Adapter à la fenêtre (variants: Adapter (avec/sans agrandissement)) | Ajustar a la ventana | Adatta | Ajustar à Janela | Ajustar |
| Actual size | 100 % | 100 % (menu: Taille réelle (100 %, 1:1)) | 100 %; menu "100 % (tamaño real)" | 100%; menu "Dimensioni reali (100%)" | 100% (menu: “100% (1:1 pixel)”) | 100% (menu: "100% (1:1 píxeis)") |
| Zoom in / out | Vergrößern / Verkleinern | Zoom avant / Zoom arrière | Ampliar / Reducir | Ingrandisci / Riduci | Ampliar / Reduzir | Ampliar / Reduzir |
| Lock zoom | Zoom fixieren | Verrouiller le zoom | Fijar zoom | Blocca zoom | Travar Zoom | Bloquear zoom |
| Filmstrip | Filmstreifen | Bande de vignettes | Tira de imágenes | Pellicola | tira de filme | Tira de película |
| Grid | Raster | Grille | Cuadrícula | Griglia | grade | Grelha |
| Loupe | Lupe | loupe | Lupa | lente d’ingrandimento | lupa | Lupa |
| Slideshow | Diashow | Diaporama | Pase de diapositivas | Presentazione | apresentação de slides | Diaporama |
| Frame (animation) | Einzelbild | image d’animation | fotograma | fotogramma | quadro | fotograma |
| Page | Seite | page | página | pagina | página | página |
| Overlays (Foto-Hilfen) | Einblendungen | Superpositions | Superposiciones | Sovrapposizioni | Sobreposições | Sobreposições |
| Clipping warning | Über-/Unterbelichtungswarnung | alerte de sur-/sous-exposition | Aviso de sobre-/subexposición | Avviso di sovra-/sottoesposizione | aviso de superexposição/subexposição | Aviso de super-/subexposição |
| Focus peaking | Focus Peaking | focus peaking | Focus peaking | Focus peaking | Focus Peaking (menu) / focus peaking (text) | Focus peaking |
| AF points | Fokuspunkte | collimateurs AF | Puntos de enfoque | punti AF | pontos de foco | Pontos AF |
| Exposure | Belichtung, Belichtungsregler | exposition, curseur d’exposition | exposición; regulador de exposición | esposizione; cursore esposizione | exposição; controle de exposição | exposição; controlo de exposição |
| Bit depth | Bittiefe | profondeur de bits | profundidad de bits | profondità di bit | profundidade de bits | profundidade de bits |
| Compression | Komprimierung, Komprimierungsart | compression, méthode de compression | compresión; método de compresión | compressione; metodo di compressione | compressão | compressão |
| Folder map | Ordnerkarte | Carte du dossier | Mapa de la carpeta (menu: "Mapa") | Mappa della cartella; menu "Mappa" | Mapa da Pasta | Mapa da pasta (menu: "Mapa") |
| Location (GPS) | Aufnahmeort | lieu de prise de vue | ubicación | posizione | localização | localização |

#### Bearbeiten und Dateien

| Begriff | Deutsch | fr | es | it | pt_BR | pt_PT |
|---|---|---|---|---|---|---|
| Crop (action) | Zuschneiden | Recadrer | Recortar | Ritaglia | Recortar | Recortar |
| Crop (result/area) | Zuschnitt, Zuschnittbereich | recadrage, zone de recadrage | recorte; área de recorte | ritaglio; area di ritaglio | recorte; área de recorte | recorte; área de recorte |
| Rotate | Drehen, Links/Rechts drehen, Drehung sichern | Faire pivoter à gauche/droite; rotation; Enregistrer la rotation | Girar a la izquierda/derecha; rotación; Guardar rotación | Ruota; Ruota a sinistra/destra; Salva rotazione | Girar à Esquerda / Girar à Direita; Salvar Rotação | Rodar para a esquerda/direita; Guardar rotação |
| Flip | Horizontal/Vertikal spiegeln | Retourner horizontalement/verticalement | Voltear horizontalmente / verticalmente | Rifletti orizzontalmente / verticalmente | Virar Horizontal / Virar Vertical | Virar na horizontal / Virar na vertical |
| Lossless | verlustfrei | sans perte | sin pérdida | senza perdita | sem perdas | sem perdas |
| Re-encode | neu kodieren, Neukodieren | réencoder, réencodage | volver a codificar; codificada de nuevo | ricodificare; ricodifica | recodificar | recodificar |
| Auxiliary images | Zusatzbilder | images supplémentaires | imágenes adicionales | immagini aggiuntive | imagens auxiliares | imagens adicionais |
| Sidecar file | XMP-Zusatzdatei | fichier annexe XMP | archivo XMP complementario | file collaterale XMP | arquivo XMP associado | ficheiro XMP associado |
| Export | Exportieren, Export | Exporter, exportation | Exportar; exportación | Esporta; esportazione | Exportar; exportação | Exportar, exportação |
| Quick move | Schnellablage, Ziel der Schnellablage | Rangement rapide, cible du rangement rapide | Archivado rápido; destino del archivado rápido | Archiviazione rapida; destinazione dell’archiviazione rapida | Arquivamento Rápido | Arquivo rápido; destino do arquivo rápido |
| Name conflict | Namenskonflikt; Ersetzen / Beide behalten / Überspringen | Conflit de nom; Remplacer / Garder les deux / Ignorer | Conflicto de nombres; Reemplazar / Conservar ambos / Omitir | Conflitto di nomi; Sostituisci / Mantieni entrambi / Salta | conflito de nomes; Substituir / Manter Ambos / Ignorar | Conflito de nomes; Substituir / Manter ambos / Ignorar |
| Keep (unchanged) | beibehalten | conserver | conservar | mantieni | manter | manter |

#### Bewerten und Aussortieren

| Begriff | Deutsch | fr | es | it | pt_BR | pt_PT |
|---|---|---|---|---|---|---|
| Rating | Bewertung, bewerten, Sterne | note, noter, notation, étoiles | valoración, valorar, estrellas | valutazione; stelle | classificação, classificar, estrelas | classificação, classificar, estrelas |
| Culling | Aussortieren, Aussortiermodus | sélection, mode sélection | criba; Modo de criba | scrematura; Modalità scrematura | triagem; modo de triagem | triagem; modo de triagem |
| Flag (allgemein) | Markierung | marqueur | marca | contrassegno | marcação | marcação |
| Pick (Befehl, `P`) | **Markieren** | Retenir | Marcar | Contrassegna | Marcar | Marcar |
| Pick (Zustand) | **Markiert**, „nur markierte“ | Retenue, « retenues seulement » | Marcada; "solo marcadas" | Contrassegnata; "solo contrassegnate" | Marcada; “somente marcadas” | Marcada; "só marcadas" |
| Reject (Befehl, `X`) | **Ablehnen** | Rejeter | Rechazar | Rifiuta | Rejeitar | Rejeitar |
| Reject (Zustand) | **Abgelehnt**, „nur abgelehnte“ | Rejetée, « rejetées seulement » | Rechazada; "solo rechazadas" | Rifiutata; "solo rifiutate" | Rejeitada; “somente rejeitadas” | Rejeitada; "só rejeitadas" |
| Unflag (`U`) | Markierung aufheben | Retirer le marqueur | Quitar marca | Rimuovi contrassegno | Remover Marcação | Remover marcação |
| Unrated | unbewertet | non notée | sin valorar | non valutata | não classificada | sem classificação |
| `_Rejected` folder | Ordner „_Rejected“ | dossier « _Rejected » | carpeta “_Rejected” | cartella “_Rejected” | pasta “_Rejected” | pasta “_Rejected” |
| Finder tag names | „Pick“, „Reject“, „★★★“ | "Pick", "Reject", "★★★" | “Pick”, “Reject”, “★★★” | “Pick”, “Reject”, “★★★” | “Pick”, “Reject”, “★★★” | “Pick”, “Reject”, “★★★” |

#### Weitere Begriffe

| Begriff (englisch) | fr | es | it | pt_BR | pt_PT |
|---|---|---|---|---|---|
| Trash | corbeille | papelera | Cestino | Lixo | Lixo |
| System Settings | Réglages Système → Confidentialité et sécurité → Fichiers et dossiers / Automatisation | Ajustes del Sistema; Privacidad y seguridad → Automatización / Archivos y carpetas | Impostazioni di Sistema; Privacy e sicurezza | Ajustes do Sistema | Definições do Sistema |
| Quick Look | Coup d’œil | Vista rápida | Visualizzazione rapida | Visualização Rápida | Vista rápida |
| Live Text | Texte en direct | Texto en vivo | Testo attivo | Texto ao Vivo | Texto em direto |
| Apple Maps | Plans | Mapas de Apple; app Mapas | Apple Mappe; app Mappe | Mapas da Apple; app “Mapas” | Mapas ("Abrir em Mapas") |
| Keyboard key Space | Espace | Espacio | Barra spaziatrice | Espaço | Espaço |
| Keyboard key Esc | Esc | – | Esc | Esc | – |
| Keyboard key Enter | Entrée | Intro | Invio | – | – |
| Keyboard keys Page Up/Down | Pg↑ / Pg↓ | Re Pág / Av Pág | Pag↑ / Pag↓ | Page Up / Page Down | Pág↑ / Pág↓ |
| Keyboard keys Home/End | Début / Fin | Inicio / Fin | Inizio / Fine | Home / End | Início / Fim |
| Compare | Comparer, comparaison | Comparar; Comparación | Confronta; confronto | Comparar; comparação | Comparar / Comparação |
| Metadata | métadonnées | metadatos | metadati | metadados | metadados |
| HDR | HDR, aperçu SDR, plage dynamique étendue | – | HDR; anteprima SDR | HDR; pré-visualização SDR | HDR; gama dinâmica alargada |
| HDR gain map | carte de gain HDR | – | – | – | – |
| Culling mode | Mode sélection | – | – | – | – |
| Presets (Profilvorlagen) | Préréglages | – | – | – | – |
| Windows: Default apps | Applications par défaut | Configuración → Aplicaciones predeterminadas | App predefinite | Aplicativos padrão | Aplicações predefinidas |
| Windows: Desktop background | arrière-plan du Bureau | fondo de escritorio | sfondo del desktop | plano de fundo da área de trabalho | fundo do ambiente de trabalho |
| Windows: Programs (*.exe) | Programmes (*.exe) | – | Programmi (*.exe) | – | – |
| Batch files | fichiers batch | archivos por lotes | – | arquivos em lote | – |
| Tags (Finder) | tags | etiquetas | – | etiquetas | identificadores (Identificadores do Finder) |
| Comics | BD | – | – | – | – |
| Settings tab Fenster | Fenêtres | – | – | – | – |
| Quotation marks | – | “ ” | “ ” (was mixed « » / “ ”) | – | – |
| Archive | – | archivo comprimido | – | arquivo compactado | arquivo |
| Move (verschieben) | – | mover | – | – | – |
| Replace | – | Reemplazar | – | – | – |
| Link windows | – | Vincular / Desvincular ventanas | – | Vincular Janelas / Desvincular | Ligar janelas / Desligar janelas |
| Date Added | – | – | Data di aggiunta | Data da Adição | – |
| Date Taken | – | – | Data di acquisizione | – | – |
| Pixel dimensions | – | – | Dimensioni in pixel | – | – |
| Sort / Sort by | – | – | – | ordenação; Ordenar por | – |
| Date Modified / Created | – | – | – | Data de Modificação / Data de Criação | – |
| Subject (Live Text) | – | – | – | – | objeto |
| Digits (keys 0–9) | – | – | – | – | algarismos |
| Settings tabs | – | – | – | – | Geral, Janelas, Apresentação, Rato e teclado, Navegação e ordenação, Classificação, Formatos, Avançado |

#### Unsichere Entscheidungen (bitte von Muttersprachler:innen prüfen)

- **fr**
  - Filmstrip → Bande de vignettes: own choice; Lightroom FR "Film fixe"; "Pellicule" rejected (= Apple Camera Roll)
  - Overlays (Foto-Hilfen) → Superpositions: own choice
  - Sidecar file → fichier annexe XMP: own choice; (Lightroom FR "fichier annexe XMP" / "fichier sidecar")
  - Rating → note, noter, notation, étoiles: own choice; (Lightroom FR "classement")
  - Culling → sélection, mode sélection: own choice; ("tri" clashes with sorting)
  - Culling mode → Mode sélection: own choice
- **es**
  - Set as Desktop Picture → Establecer como fondo de pantalla: System Settings "Fondo de pantalla"; Windows: "fondo de escritorio"; exact Finder command wording
  - Visible area (viewport) → vista: own choice; also used for "Ansicht" (view), as in English
  - Filmstrip → Tira de imágenes: own choice; (Lightroom: "Tira de diapositivas")
  - Sidecar file → archivo XMP complementario: Lightroom
  - Quick move → Archivado rápido; destino del archivado rápido: own choice
  - Culling → criba; Modo de criba: own choice; (avoids "selección" = selection)
  - Pick (Befehl, `P`) → Marcar: Lightroom (ES: "Marcar como seleccionada")
- **it**
  - Zoom to fit → Adatta: own choice; Preview uses "Adatta alla finestra" (short form kept for consistency with "Adatta ↔ 100%")
  - Flip → Rifletti orizzontalmente / verticalmente: Preview (Photos uses "Capovolgi")
  - Culling → scrematura; Modalità scrematura: own choice (was "selezione", clashed with multi-selection/crop)
  - Keyboard keys Home/End → Inizio / Fine: own choice ()
- **pt_BR**
  - Quit → Encerrar Lumaflip: macOS AppKit (Apple app menus omit the article)
  - Check for Updates → Buscar Atualizações…: own choice (Sparkle pt_BR may use “Verificar Atualizações…”)
  - Status bar → barra de status: Finder pt_BR may say “Barra de Estado”
  - Sidecar file → arquivo XMP associado: Lightroom pt_BR wording not verified
  - Pick (Befehl, `P`) → Marcar: own choice (Lightroom pt_BR uses “Sinalizar”)
  - Apple Maps → Mapas da Apple; app “Mapas”: Apple uses the app name “Mapas”
- **pt_PT**
  - Shortcut (Apple Shortcuts) → atalho; aplicação Atalhos: macOS AppKit (Services: "Atalhos"); "atalho" collides with keyboard shortcut, context separates
  - Filmstrip → Tira de película: own choice
  - Sidecar file → ficheiro XMP associado: own choice
  - Quick move → Arquivo rápido; destino do arquivo rápido: own choice; "arquivo" also = archive
  - Pick (Befehl, `P`) → Marcar: own choice (mirrors German; Lightroom pt-BR "Escolhida")
  - Live Text → Texto em direto: macOS; (no loctable hit)
  - Keyboard keys Page Up/Down → Pág↑ / Pág↓: own choice
  - Keyboard keys Home/End → Início / Fim: own choice
