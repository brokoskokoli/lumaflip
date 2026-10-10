// SPDX-License-Identifier: GPL-3.0-or-later
// MainMenu, M2 media entries: slideshow (F-SLIDE-01/02), pages (F-FMT-04), animation and
// its speed (F-FMT-03), saving the rotation (F-EDIT-02).

#include "AnimationPlayer.h"
#include "Application.h"
#include "MainMenu.h"
#include "ViewerWindow.h"
#include "core/Language.h"
#include "core/Settings.h"

#include <QAction>
#include <QActionGroup>
#include <QLocale>
#include <QMenu>

#include <cmath>

namespace lf {

namespace {
const double kSpeeds[] = {0.25, 0.5, 1.0, 1.5, 2.0, 4.0};

QString speedId(double s) {
    return QStringLiteral("speed:") + QString::number(s);
}

struct SlideshowOption {
    const char* id;
    const char* key;
    struct {
        const char* source;
        const char* comment; // disambiguation ("Wiederholen" is also Redo)
    } text;
};
const SlideshowOption kSlideshowOptions[] = {
    {"slideshow:repeat", key::SlideshowRepeat,
     QT_TRANSLATE_NOOP3("lf::MainMenu", "Wiederholen", "slideshow option: start again after the last image")},
    {"slideshow:shuffle", key::SlideshowShuffle,
     QT_TRANSLATE_NOOP3("lf::MainMenu", "Zufällige Reihenfolge", "")},
    {"slideshow:crossfade", key::SlideshowCrossfade, QT_TRANSLATE_NOOP3("lf::MainMenu", "Überblenden", "")},
};
} // namespace

void MainMenu::buildMedia(QMenu* view, QMenu* nav, QMenu* image) {
    auto front = [this] { return m_app.frontWindow(); };

    // ---- Darstellung: slideshow ----
    view->addSeparator();
    add(view, Action::ToggleSlideshow, tr("Diashow"))->setCheckable(true);
    QMenu* options = view->addMenu(tr("Diashow-Optionen"));
    options->setObjectName(QStringLiteral("menu:slideshowOptions"));
    for (const auto& o : kSlideshowOptions) {
        QAction* act = addCommand(options, QString::fromLatin1(o.id),
                                  tr(o.text.source, *o.text.comment ? o.text.comment : nullptr));
        act->setCheckable(true);
        const QString k = QString::fromLatin1(o.key);
        connect(act, &QAction::triggered, this, [k](bool on) { Settings::instance().setValue(k, on); });
    }
    connect(options, &QMenu::aboutToShow, this, &MainMenu::refreshState);

    // ---- Navigation: pages ----
    nav->addSeparator();
    add(nav, Action::NextPage, tr("Nächste Seite"));
    add(nav, Action::PreviousPage, tr("Vorherige Seite"));

    // ---- Bild: save rotation, animation ----
    image->addSeparator();
    add(image, Action::SaveRotation, tr("Drehung speichern"));
    image->addSeparator();
    QMenu* anim = image->addMenu(tr("Animation"));
    anim->setObjectName(QStringLiteral("menu:animation"));
    add(anim, Action::ToggleAnimation, tr("Abspielen/Pause"));
    add(anim, Action::NextFrame, tr("Nächstes Einzelbild"));
    add(anim, Action::PreviousFrame, tr("Vorheriges Einzelbild"));
    anim->addSeparator();
    QMenu* speed = anim->addMenu(tr("Geschwindigkeit"));
    speed->setObjectName(QStringLiteral("menu:animationSpeed"));
    auto* group = new QActionGroup(this);
    for (double s : kSpeeds) {
        QAction* act = addCommand(speed, speedId(s), QStringLiteral("%1×").arg(formatLocale().toString(s)));
        act->setCheckable(true);
        group->addAction(act);
        connect(act, &QAction::triggered, this, [front, s] {
            if (auto* w = front())
                w->setAnimationSpeed(s);
        });
    }
    for (QMenu* m : {nav, image, anim, speed})
        connect(m, &QMenu::aboutToShow, this, &MainMenu::refreshState);
}

void MainMenu::refreshMediaState() {
    ViewerWindow* w = m_app.frontWindow();
    const bool hasWindow = w != nullptr;
    for (Action a : {Action::ToggleSlideshow, Action::NextPage, Action::PreviousPage, Action::SaveRotation,
                     Action::ToggleAnimation, Action::NextFrame, Action::PreviousFrame})
        if (QAction* act = action(a))
            act->setEnabled(hasWindow);
    if (QAction* show = action(Action::ToggleSlideshow))
        show->setChecked(w && w->isSlideshowRunning());
    Settings& st = Settings::instance();
    for (const auto& o : kSlideshowOptions)
        if (QAction* act = command(QString::fromLatin1(o.id)))
            act->setChecked(st.value(QString::fromLatin1(o.key)).toBool());
    const double current = w && w->animation() ? w->animation()->speed() : 1.0;
    for (double s : kSpeeds) {
        if (QAction* act = command(speedId(s))) {
            act->setEnabled(hasWindow);
            act->setChecked(std::abs(s - current) < 1e-6);
        }
    }
}

} // namespace lf
