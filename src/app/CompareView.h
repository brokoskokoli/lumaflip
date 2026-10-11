// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Compare mode (F-GRID-03): 2–4 images side by side, each in its own ImageView ("pane")
// with name and rating below. Zoom and visible part are synchronized (ViewSyncBinding);
// with ⌥ a pane moves on its own. One pane has the focus (highlighted frame): rating keys
// and zoom commands of the window apply to it; Tab / ⇧Tab cycle the focus, a click focuses
// a pane, Esc closes the mode. Images come from the shared ImageProvider (this view is its
// own client, so the window's prefetch is not disturbed).

#include "core/CompareBlend.h"
#include "core/ImageProvider.h"
#include "core/InputBindings.h"

#include <QFrame>
#include <QStringList>
#include <QVector>

#include <functional>

class QComboBox;
class QLabel;
class QSlider;

namespace lf {

class CompareBlendOverlay;
class ImageView;
class ViewSyncBinding;

class CompareView : public QFrame {
    Q_OBJECT
public:
    static constexpr int kMinPanes = 2;
    static constexpr int kMaxPanes = 4;

    explicit CompareView(ImageProvider& provider, QWidget* parent = nullptr);
    ~CompareView() override;

    // Shows these images (the first kMaxPanes); `focus` = pane with the focus.
    void setPaths(const QStringList& paths, int focus = 0);
    QStringList paths() const;
    int paneCount() const { return int(m_panes.size()); }
    ImageView* paneView(int pane) const;
    QString paneCaption(int pane) const; // "name — ★★★☆☆" (tests)

    int focusedPane() const { return m_focus; }
    void setFocusedPane(int pane);
    void cycleFocus(int direction);
    QString focusedPath() const;
    ImageView* focusedView() const { return paneView(m_focus); }

    ViewSyncBinding* sync() const { return m_sync; }
    void updateCaptions(); // ratings changed
    void stop();           // releases the provider client (hidden)
    // F-I18N-03: accessible name, captions, and panes with an error card decode again (their
    // decoder texts are in the old language). Called on QEvent::LanguageChange.
    void retranslateUi();

    // Same settings as the window's view.
    void applyViewSettings(const QString& background, bool upscaleNearest, bool devicePixels);
    // F-GRID-04: difference / blend / wipe of the first pane and a partner pane (default the
    // second; with 3–4 panes chosen in the bar) over the first pane (Off = side by side).
    // `value`: blend amount or split position (0..1); the difference is amplified by up to
    // 16x from the same slider.
    void setBlendMode(compare::BlendMode mode);
    compare::BlendMode blendMode() const { return m_blend; }
    void setBlendValue(double value);
    double blendValue() const { return m_blendValue; }
    void setBlendPartner(int pane); // 1 .. paneCount()-1
    int blendPartner() const { return m_partner; }
    CompareBlendOverlay* blendOverlay() const { return m_overlay; }
    QWidget* blendBar() const { return m_bar; }

    // Keys taken from the menu shortcuts while the compare view has the focus (rating digits).
    void setExtraKeys(std::function<bool(const QKeyEvent*)> wanted) { m_extraKeys = std::move(wanted); }

signals:
    void closeRequested();
    void focusChanged(int pane);
    // Wheel, clicks … in a pane (the pane got the focus); the window maps them to actions.
    void paneInput(int pane, const lf::InputEvent& event, QPointF position);

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool focusNextPrevChild(bool next) override;

private:
    struct Pane {
        QFrame* frame = nullptr;
        ImageView* view = nullptr;
        QLabel* caption = nullptr;
        QString path;
        Quality shown = Quality::Preview;
        bool hasImage = false;
    };
    void onImageReady(const QString& path, Quality quality, const DecodedImagePtr& image);
    void onImageFailed(const QString& path, const QString& error);
    void request(int pane, Quality quality);
    void showStage(int pane, Quality quality, const DecodedImagePtr& image);
    void updateFrames();
    QSize decodeTarget() const;
    void updateBlend();
    void retranslateBar();
    void fillPartnerBox();
    double overlayValue() const; // m_blendValue in the overlay's terms (difference: amount)

    ImageProvider& m_provider;
    QVector<Pane> m_panes;
    ViewSyncBinding* m_sync = nullptr;
    int m_focus = 0;
    std::function<bool(const QKeyEvent*)> m_extraKeys;
    QWidget* m_paneArea = nullptr;
    QWidget* m_bar = nullptr;
    QComboBox* m_modeBox = nullptr;
    QComboBox* m_partnerBox = nullptr; // 3–4 panes: which pane is compared with the first
    QSlider* m_slider = nullptr;
    QLabel* m_valueLabel = nullptr;
    CompareBlendOverlay* m_overlay = nullptr;
    compare::BlendMode m_blend = compare::BlendMode::Off;
    double m_blendValue = 0.5;
    int m_partner = 1;
};

} // namespace lf
