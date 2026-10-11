# Prüfstatus der Übersetzungen (F-I18N-05)

Erstübersetzungen sind maschinell entstanden (Oktober 2026) und vor einem Release von
Muttersprachler:innen gegenzulesen (Beiträge über GitHub, siehe [README.md](README.md)).
Status: **maschinell, ungeprüft** → **geprüft** (Name/Handle, Datum).

| Sprache | Code | Status | Geprüft von / am | Anmerkungen |
|---|---|---|---|---|
| Deutsch | `de` | Ausgangssprache | – | `lumaflip_de.ts` enthält nur die Pluralformen |
| Englisch | `en` | maschinell durchgesehen, ungeprüft | | Okt. 2026: Apple-Begriffe und HIG-Schreibweise (Title Case/Sentence case, Kurzformen, Geviertstrich), Glossar en/pl/cs |
| Französisch | `fr` | maschinell, ungeprüft | | Einheiten Ko/Mo/Go; Filmstreifen = „Pellicule“ prüfen |
| Spanisch | `es` | maschinell, ungeprüft | | |
| Italienisch | `it` | maschinell, ungeprüft | | Tabs = „pannelli“ prüfen |
| Portugiesisch (Brasilien) | `pt_BR` | maschinell, ungeprüft | | |
| Portugiesisch (Portugal) | `pt_PT` | maschinell, ungeprüft | | Qt-eigene Texte kommen aus `qtbase_pt_BR` (Qt liefert kein pt_PT) |
| Niederländisch | `nl` | maschinell, ungeprüft | | „Format“ = „Structuur“ prüfen |
| Polnisch | `pl` | maschinell durchgesehen, ungeprüft | | Okt. 2026: macOS-pl-Begriffe (Zachowaj, Kosz, Pokaż w Finderze), Pluralformen; prüfen: „Szybki przegląd“ statt Quick Look, „Oznacz jako wybrane“ (lang), „Kurtyna“ (Wischen) |
| Schwedisch | `sv` | maschinell, ungeprüft | | Einheit „kB“ prüfen |
| Dänisch | `da` | maschinell, ungeprüft | | |
| Norwegisch (Bokmål) | `nb` | maschinell, ungeprüft | | Qt-eigene Texte kommen aus `qtbase_nn` (Qt liefert kein nb) |
| Finnisch | `fi` | maschinell, ungeprüft | | |
| Tschechisch | `cs` | maschinell durchgesehen, ungeprüft | | Okt. 2026: macOS-cs-Begriffe (vykání, Odvolat akci, Přesunout do koše), Pluralformen; prüfen: Zvětšit vs. Přiblížit, „Mapy Apple“, feste Genitive („složek: %1“) |
| Japanisch | `ja` | maschinell, Terminologie-Review | Claude (KI), 2026-10-11 | Begriffe/Satzzeichen nach macOS 27 (GLOSSARY.md); “” statt 「」; Pick/Reject = 採用/除外 (Lightroom); Review durch Muttersprachler:innen steht aus |
| Chinesisch (vereinfacht) | `zh_CN` | maschinell, Terminologie-Review | Claude (KI), 2026-10-11 | Finder = „访达“, Begriffe nach macOS 27 (GLOSSARY.md), keine Leerzeichen CJK↔Latein; Pick/Reject = 留用/排除; Review durch Muttersprachler:innen steht aus |
| Chinesisch (traditionell) | `zh_TW` | maschinell, Terminologie-Review | Claude (KI), 2026-10-11 | Begriffe nach macOS 27 (檔案夾, 影像, 按一下); „…“ statt Apples „⋯“ (Test); Pick/Reject = 留用/排除 prüfen; Review durch Muttersprachler:innen steht aus |
| Koreanisch | `ko` | maschinell, Terminologie-Review | Claude (KI), 2026-10-11 | Begriffe und Register (-습니다/-하십시오) nach macOS 27; Pick/Reject = 선택/제외 prüfen; Review durch Muttersprachler:innen steht aus |

Die Texte der `Info.plist` (`packaging/macos/lproj/*/InfoPlist.strings`) haben denselben Status
wie die Sprache.
