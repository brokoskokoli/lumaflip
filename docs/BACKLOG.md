<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Backlog

Wünsche für die Zeit nach dem aktuellen Release. Wenn ein Punkt eingeplant wird, kommt er mit
eigener Anforderungs-ID in [SPEC.md](../SPEC.md) und fällt hier heraus.

| # | Wunsch | Notizen | Aufgenommen |
|---|---|---|---|
| B-3 | **Randloses Fenster mit einblendender Titelleiste:** Das Bild reicht bis an den oberen Rand, ohne sichtbare Titelleiste. Bewegt man die Maus, blendet oben ein halbtransparent schwarzer Streifen ein: Fenstertitel in Weiß, die Fensterknöpfe (macOS links: Schließen/Minimieren/Zoomen; Windows rechts: Minimieren/Maximieren/Schließen; Linux nach Desktop-Einstellung). Nach kurzer Ruhe blendet der Streifen wieder aus. Ziehen am Streifen bewegt das Fenster, Doppelklick zoomt bzw. maximiert, Drag & Drop von Dateien bleibt erlaubt. | Erweitert F-VIEW-13 (minimaler Fenstermodus, heute: transparente Titelleiste, Knöpfe immer sichtbar). macOS: `NSWindowStyleMaskFullSizeContentView` + versteckter Titel, Standardknöpfe ein-/ausblenden, Streifen als eigenes Overlay. Windows: rahmenloses Fenster mit eigenen Knöpfen, `WM_NCHITTEST` für Ziehen, Aero Snap und Snap-Layouts. Linux: rahmenlos, `startSystemMove`/`startSystemResize`. Gleiches Verhalten wie die einblendenden Bedienelemente im Vollbild (F-VIEW-10); soll der Standard für neue Fenster werden? (Nutzer fragen) | 2026-10-11 |
