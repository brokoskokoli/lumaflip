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
