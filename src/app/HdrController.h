// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// HDR in a viewer window (SPEC 4.5):
// - F-COL-04: the HDR toggle (key::HdrEnabled, ⇧⌘H, menu "Darstellung → HDR"): HDR images
//   with extended dynamic range or as SDR preview (tone-mapped); HDR images get a badge
//   in the status bar ("HDR", "HDR (SDR-Vorschau)", with the exposure if set).
// - F-COL-05: exposure (±EV, display only) for HDR/EXR images: ⌥⌘+ / ⌥⌘− / ⌥⌘0 and a
//   small HUD with a slider (⌥⌘E). Back to 0 EV on the next image unless "beibehalten".
// The rendering itself is ImageView/ImageCanvas (F-COL-03).

#include <QObject>
#include <QPointer>
#include <QString>

class QCheckBox;
class QFrame;
class QLabel;
class QSlider;
class QStatusBar;
class QToolButton;

namespace lf {

class ImageView;
class ViewerWindow;
enum class Action;

class HdrController : public QObject {
    Q_OBJECT
public:
    explicit HdrController(ViewerWindow& window);
    ~HdrController() override;

    static HdrController* of(ViewerWindow* window); // null if none

    // Toggle, exposure keys, HUD; true: handled.
    bool perform(Action action);

    bool hudRequested() const { return m_hudRequested; }
    void setHudRequested(bool on);
    bool isHudVisible() const;

    // Status bar badge (null until attached).
    void attachStatusBar(QStatusBar* bar);
    QString badgeText() const;    // empty for SDR images
    QString badgeToolTip() const; // why the image looks as it does
    void retranslate();

    QFrame* hud() const; // tests (null until first shown)

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refresh();
    void ensureHud();
    void layoutHud();
    QString exposureText() const;

    ViewerWindow& m_window;
    ImageView* m_view;
    QPointer<QLabel> m_badge;
    QFrame* m_hud = nullptr;
    QLabel* m_hudTitle = nullptr;
    QSlider* m_slider = nullptr;
    QLabel* m_value = nullptr;
    QToolButton* m_reset = nullptr;
    QCheckBox* m_lock = nullptr;
    bool m_hudRequested = false;
    bool m_updating = false;
};

} // namespace lf
