// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-VIEW-15: geometry and platform conventions of the title strip that fades in over the
// top of a borderless viewer window (the strip itself is app/TitleStrip). Pure functions,
// so that the layout, the side of the window buttons and the double-click preference are
// testable without windows.

#include <QPoint>
#include <QRect>
#include <QString>

#include <optional>

namespace lf {

enum class TitleButton { None, Menu, Minimize, Maximize, Close };

// Where the window buttons sit: macOS left, Windows right, Linux after the desktop setting.
enum class TitleButtonSide { Left, Right };

struct TitleStripSpec {
    int width = 0;
    int height = 0;
    // The strip draws minimize/maximize/close itself (Windows, Linux); on macOS the
    // system's buttons stay and `reservedLeft` keeps their place free.
    bool ownButtons = false;
    TitleButtonSide side = TitleButtonSide::Right;
    bool menuButton = false; // Windows: the window's menus behind one button (opposite side)
    int buttonWidth = 46;
    int reservedLeft = 0;
};

struct TitleStripLayout {
    QRect menu;
    QRect minimize;
    QRect maximize;
    QRect close;
    QRect titleArea;     // free space between the button groups
    QRect centeredTitle; // the same, symmetric (centered title); empty if too narrow
};

TitleStripLayout layoutTitleStrip(const TitleStripSpec& spec);
TitleButton titleButtonAt(const TitleStripLayout& layout, QPoint pos);

// GNOME `org.gnome.desktop.wm.preferences button-layout`, e.g. "'close,minimize:appmenu'"
// (buttons before the colon are on the left).
TitleButtonSide buttonSideFromGnomeLayout(const QString& value, TitleButtonSide fallback);
// KDE kwinrc [org.kde.kdecoration2] ButtonsOnLeft / ButtonsOnRight ("X" = close).
TitleButtonSide buttonSideFromKdeButtons(const QString& onLeft, const QString& onRight,
                                         TitleButtonSide fallback);

// What a double click on the title does (macOS: Systemeinstellungen → Schreibtisch & Dock →
// "Doppelklick auf Titelleiste": defaults AppleActionOnDoubleClick, older systems the bool
// AppleMiniaturizeOnDoubleClick). Windows and Linux maximize.
enum class TitleDoubleClick { Zoom, Minimize, Nothing };
TitleDoubleClick titleDoubleClickAction(const QString& appleActionOnDoubleClick,
                                        std::optional<bool> appleMiniaturizeOnDoubleClick);

} // namespace lf
