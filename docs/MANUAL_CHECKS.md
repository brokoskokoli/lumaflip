<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Manuelle Prüfungen

Anforderungen, die sich nicht sinnvoll automatisch testen lassen. Jede Zeile nennt die
Anforderungs-ID, damit `tools/spec_coverage.py` sie als abgedeckt zählt. Die
Schritt-für-Schritt-Anleitung je Stufe steht in `docs/TESTING-M<n>.md`.

| ID | Prüfung | Warum manuell |
|---|---|---|
| P-07 | Zoomen (Pinch, ⌘+/−) und Verschieben (Ziehen, Trackpad mit Trägheit) eines 24-MP-Bildes auf einem ProMotion-Display: keine Ruckler (Quartz Debug / Instruments „Core Animation FPS“: 120 fps) | Bildrate des Fenster-Compositings ist im Test nicht messbar; die Render-Kosten pro Frame deckt `P_03_SwapPreloaded` ab |
| F-MAC-02 | Systemeinstellungen → Erscheinungsbild zwischen Hell und Dunkel umschalten, während Lumaflip läuft: Menüs, Statuszeile, Hinweisleiste, Einstellungsfenster und Fehlerkarte folgen sofort und sind in beiden Modi gut lesbar; Hintergrund „Wie System“ wechselt mit | Optischer Eindruck; Qt übernimmt das Systemaussehen, ein Test könnte nur Paletten vergleichen |
| F-OPEN-01 | Echter Doppelklick im Finder, „Öffnen mit → Lumaflip“, Ziehen von Datei/Ordner auf das Dock-Symbol und `open -a Lumaflip …` | Braucht Finder/Dock und LaunchServices; die daraus entstehenden `QFileOpenEvent`s, die 150-ms-Gruppierung, Kommandozeile und Drop aufs Fenster testet `tst_app` automatisch |
| F-MAC-01 | Menüleiste ansehen (App-Menü „Lumaflip“ mit Über/Einstellungen/Beenden, Ablage, Bearbeiten, Darstellung, Navigation, Bild, Fenster, Hilfe); Tastenkürzel mit ⌘ über das echte Menü auslösen (Menütitel blinkt); Einzeltasten wie `F`, `H`, `1` wirken im Bild, aber nicht, während im Einstellungsfenster ein Eingabefeld aktiv ist; Hilfe-Suchfeld im Menü „Hilfe“ findet Menübefehle | Native Menü-Tastenäquivalente laufen über AppKit; Inhalt, Rollen und Kürzel der Menüs prüft `tst_app` automatisch |
| F-WIN-10 | Rechtsklick auf das Dock-Symbol zeigt alle offenen Fenster mit Dateiname und Position; Auswahl holt das Fenster nach vorn | Das Dock-Menü ist nur über das Dock erreichbar; Inhalt prüft `tst_app` |
| F-PRIV-01 | Bild aus „Schreibtisch“ oder „Dokumente“ öffnen, wenn Lumaflip dafür noch keine Berechtigung hat: Systemabfrage erscheint einmal; bei „Nicht erlauben“ zeigt Lumaflip nur dieses Bild und die gelbe Hinweisleiste, „Zugriff erlauben…“ öffnet Systemeinstellungen → Datenschutz & Sicherheit → Dateien und Ordner | Echte TCC-Abfragen lassen sich nicht automatisieren; den gleichen UI-Pfad (Ordner nicht lesbar) testet `tst_app` mit einem `chmod`-geschützten Ordner |
| F-PRIV-02 | Nirgends in der App wird Festplattenvollzugriff verlangt oder die entsprechende Einstellungsseite geöffnet | Abwesenheit einer Systemabfrage; `tst_app_coverage` (`F_PRIV_02_neverDemandsFullDiskAccess`) prüft Texte, Menüs, Einstellungen und dass nur die Seite „Dateien und Ordner“ geöffnet wird |
| P-01 | Kaltstart mit Doppelklick auf ein 24-MP-JPEG im Finder (App vorher beendet): Bild steht in deutlich unter einer halben Sekunde | LaunchServices und Finder sind nicht Teil der Benchmarks `P_01_ColdOpenFirstImage24MP` (in-process, bis zur Bildschirmstufe) und `P_01_ProcessLaunchToFirstFrame24MP` (Prozessstart) |
| P-06 | → im Ordner mit 24-MP-Fotos gedrückt halten (Tastenwiederholung „schnell“): Bilder wechseln flüssig, Vorschauen erscheinen, keine Hänger | Gefühl beim Blättern; die Rate misst `P_06_HoldArrowKey24MP` |
| F-COL-01 | Display-P3-Testbild (z. B. WebKit-Logo „P3“) und sRGB-Bild nebeneinander in Lumaflip und Vorschau.app auf P3-Display und externem sRGB-Monitor vergleichen: identischer Eindruck, P3-Bild auf P3-Display sichtbar gesättigter | Visueller Abgleich mit Vorschau; die Pixelwerte im Framebuffer prüft `tst_imageview` automatisch |

## Auslieferung: Signierung, Notarisierung, Lizenzen (SPEC 10.1, 10.2, 11)

Automatisch prüft `tools/package.sh`: Bundle enthält keine Verweise auf Homebrew (`otool -L`/`LC_RPATH`),
`codesign --verify --deep --strict` ist gültig; `tst_about` prüft Über-Fenster und Lizenzliste.
Release-Schritte: [RELEASING.md](RELEASING.md).

| Bereich | Prüfung | Warum manuell |
|---|---|---|
| Gatekeeper | Release-DMG im Browser herunterladen (Quarantäne-Attribut), öffnen, Lumaflip nach „Programme“ ziehen, starten: nur die normale Rückfrage „aus dem Internet geladen“, **kein** Hinweis „nicht verifizierter Entwickler“. `spctl -a -vvv -t exec /Applications/Lumaflip.app` meldet `accepted` und `source=Notarized Developer ID` | Braucht ein notarisiertes Release (Apple-Dienst, Zugangsdaten) und einen echten Download mit Quarantäne |
| Notarisierung offline | `xcrun stapler validate` für DMG und App meldet „The validate action worked!“; auf einem zweiten Mac ohne Netzwerk startet die frisch heruntergeladene App trotzdem | Ticket-Abfrage bei Apple bzw. zweiter Rechner nötig |
| Signatur | `codesign -dv --verbose=4 Lumaflip.app`: `Authority=Developer ID Application: …`, `flags=0x10000(runtime)`, Zeitstempel vorhanden; `codesign -d --entitlements - Lumaflip.app` zeigt nur `com.apple.security.automation.apple-events` (keine Sandbox) | Lokale Builds sind ad-hoc signiert; die Developer-ID-Signatur entsteht nur im Release mit Zertifikat |
| TCC-Texte | Bild aus Schreibtisch, Dokumente, Downloads, von einem USB-Stick und von einem Netzlaufwerk öffnen: Systemabfrage zeigt jeweils den deutschen Text „Lumaflip liest den Ordner des geöffneten Bildes, damit du mit den Pfeiltasten blättern kannst.“ | Echte TCC-Abfragen lassen sich nicht automatisieren (Info.plist-Schlüssel siehe `packaging/macos/Info.plist.in`) |
| Über/Lizenzen | Im gepackten App: Lumaflip → Über Lumaflip zeigt Symbol, Version, GPL-Hinweis; „Lizenzen von Drittanbietern…“ listet Qt und alle mitgelieferten Bibliotheken (u. a. libwebp, libtiff, libjpeg-turbo, ICU, FreeType, HarfBuzz) mit lesbarem Lizenztext | Die Lizenzliste entsteht erst beim Paketieren; `tst_about` prüft die Anzeige mit Testdaten |
| App-Symbol | Symbol in Finder (alle Ansichtsgrößen), Dock, App-Umschalter (⌘⇥) und im Über-Fenster scharf und ohne Ränder | Optischer Eindruck |
| Homebrew Cask | Nach einem Release: Vorlage `packaging/homebrew/lumaflip.rb` mit Version/SHA-256 ausfüllen, `brew install --cask ./lumaflip.rb` installiert und startet die App, `brew uninstall --zap --cask lumaflip` entfernt sie samt Einstellungen | Braucht ein veröffentlichtes Release |
