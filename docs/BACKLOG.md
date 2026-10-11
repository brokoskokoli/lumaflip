<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Backlog

Wünsche für die Zeit nach dem aktuellen Release. Wenn ein Punkt eingeplant wird, kommt er mit
eigener Anforderungs-ID in [SPEC.md](../SPEC.md) und fällt hier heraus.

| # | Wunsch | Notizen | Aufgenommen |
|---|---|---|---|
| B-1 | **Zoom je Datei merken (Sitzung):** Wer ein Bild zoomt oder verschiebt, weiterblättert und zurückkommt, findet dieselbe Zoomstufe und denselben Ausschnitt wieder. Eine Taste (Vorschlag: Enter, frei belegbar) setzt das Bild auf den Standard-Zoommodus zurück. | Nur für die laufende Sitzung, nicht auf Platte. Abgrenzen gegen F-VIEW-05 „Zoom fixieren“, das den Zoom auf *alle* Bilder überträgt; bei aktivem Fixieren hat F-VIEW-05 Vorrang. Enter ist im Zuschneiden belegt (F-EDIT-03); dort gilt weiter das Speichern. | 2026-10-10 |
| B-2 | **Animierte GIFs und WebPs bewegen sich.** | Eigentlich spezifiziert und umgesetzt (F-FMT-03, M2, Formatgruppe „Animiert“). Erst prüfen, warum die Wiedergabe in der Testversion nicht läuft (ImageIO-Bildanzahl, Hilfsprozess, Autostart der Wiedergabe), dann beheben und einen Integrationstest mit echtem Abspielen ergänzen. | 2026-10-10 |
