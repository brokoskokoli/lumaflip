// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-VIEW-13/15: borderless viewer window with a title strip that fades in.
//
// Without a title bar the image reaches the top edge of the window. When the mouse moves
// (or the window becomes active, for keyboard users) a semi-transparent black strip fades
// in at the top: the window title in white and the window buttons - macOS keeps its own
// buttons (they fade with the strip), Windows and Linux get buttons drawn by the strip
// (Windows right, Linux on the desktop's side). After kIdleMs without a mouse move it fades
// out again; while the mouse is over it, it stays. Dragging the strip moves the window,
// a double click zooms/maximizes (macOS: the system setting), files can still be dropped
// onto the window. In full screen and in a window with native tabs (macOS) the strip is off
// (full screen has its own chrome, F-VIEW-10; the tab bar needs the title bar).
//
// The strip is a widget above the image view (later sibling in the central widget): the
// EDR surface grabs it like the quick filter bar (EdrSurface.h).

#include "app/WindowModes.h" // TitleStripHit
#include "core/TitleStripLayout.h"

#include <QPointer>
#include <QWidget>

class QMenu;
class QTimer;
class QVariantAnimation;
class QWindow;

namespace lf {

class ViewerWindow;

class TitleStrip : public QWidget {
    Q_OBJECT
public:
    explicit TitleStrip(QWidget* parent);

    void setTitle(const QString& title);
    QString title() const { return m_title; }
    void setButtonSide(TitleButtonSide side);
    void setMenuButton(bool on); // Windows: the window's menus (its menu bar is collapsed)
    void setReservedLeft(int px);

    // Fades in (true) or out; `ms` 0 = at once. The widget is hidden when faded out.
    void fadeTo(bool shown, int ms);
    bool shownTarget() const { return m_target; }
    qreal opacity() const { return m_opacity; }
    // Fully faded in: only then the strip takes the mouse (a click right after the first
    // mouse move still reaches the image).
    bool takesMouse() const { return isVisible() && m_target && m_opacity >= 1.0; }

    TitleStripLayout stripLayout() const;
    TitleButton buttonAt(QPoint pos) const;
    // What the native frame does at `pos` (strip coordinates): move the window, the
    // maximize button (Windows Snap Layouts) or nothing (Qt handles it).
    TitleStripHit nativeHit(QPoint pos) const;
    void setHover(TitleButton button);

signals:
    void opacityChanged();
    void menuRequested(QPoint globalPos);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void setOpacity(qreal value);
    void activate(TitleButton button);
    void paintGlyph(QPainter& p, TitleButton button, const QRect& r) const;

    QString m_title;
    TitleButtonSide m_side = TitleButtonSide::Right;
    bool m_menuButton = false;
    int m_reservedLeft = 0;
    bool m_target = false;
    qreal m_opacity = 0.0;
    QVariantAnimation* m_fade = nullptr;
    TitleButton m_hover = TitleButton::None;
    TitleButton m_pressed = TitleButton::None;
    QPoint m_pressPos;
    bool m_dragPending = false;
};

// Owns the strip of one window and the window's borderless state (per window; new windows
// follow the setting key::TitleBar).
class TitleStripController : public QObject {
    Q_OBJECT
public:
    static constexpr int kIdleMs = 1500; // SPEC 5.1: like the full screen chrome
    static constexpr int kFadeInMs = 150;
    static constexpr int kFadeOutMs = 350;

    TitleStripController(ViewerWindow& window, QWidget* host);

    bool borderless() const { return m_borderless; }
    void setBorderless(bool on);
    // The strip is in use: borderless, not in full screen, not a native tab.
    bool active() const { return m_active; }
    TitleStrip* strip() const { return m_strip; }
    // Windows: the window's menu bar is collapsed into the strip's menu button.
    bool holdsMenuBar() const;

    void reveal();             // fade in, fade out after kIdleMs without a mouse move
    void windowStateChanged(); // shown, full screen left, tabs changed: native state again

signals:
    void raised(); // the strip was raised: widgets that must stay above it raise now

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void apply();
    void place();
    void watchNativeWindow();
    void onIdle();
    void showMenu(QPoint globalPos);
    bool handleResizeEdges(QEvent* event); // Linux: frameless windows resize at the edges
    bool wantActive() const;

    ViewerWindow& m_window;
    QPointer<QWidget> m_host;
    TitleStrip* m_strip = nullptr;
    QTimer* m_idle = nullptr;
    QPointer<QWindow> m_handle; // QWindow the mouse filter is installed on
    QPointer<QMenu> m_menu;
    bool m_borderless = false;
    bool m_active = false;
    bool m_edgeCursor = false;
};

} // namespace lf
