<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Manuelle Prüfungen

Anforderungen, die sich nicht sinnvoll automatisch testen lassen. Jede Zeile nennt die
Anforderungs-ID, damit `tools/spec_coverage.py` sie als abgedeckt zählt. Die
Schritt-für-Schritt-Anleitung je Stufe steht in `docs/TESTING-M<n>.md`.

| ID | Prüfung | Warum manuell |
|---|---|---|
| P-07 | Zoomen (Pinch, ⌘+/−) und Verschieben (Ziehen, Trackpad mit Trägheit) eines 24-MP-Bildes auf einem ProMotion-Display: keine Ruckler (Quartz Debug / Instruments „Core Animation FPS“: 120 fps) | Bildrate des Fenster-Compositings ist im Test nicht messbar; die Render-Kosten pro Frame deckt `P_03_SwapPreloaded` ab |
| F-COL-01 | Display-P3-Testbild (z. B. WebKit-Logo „P3“) und sRGB-Bild nebeneinander in Lumaflip und Vorschau.app auf P3-Display und externem sRGB-Monitor vergleichen: identischer Eindruck, P3-Bild auf P3-Display sichtbar gesättigter | Visueller Abgleich mit Vorschau; die Pixelwerte im Framebuffer prüft `tst_imageview` automatisch |
