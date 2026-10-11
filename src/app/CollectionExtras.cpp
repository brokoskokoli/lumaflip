// SPDX-License-Identifier: GPL-3.0-or-later
#include "CollectionExtras.h"

#include "Application.h"
#include "ViewerWindow.h"
#include "core/Collection.h"
#include "core/ImageFormats.h"
#include "core/Settings.h"

#include <QFileInfo>
#include <QLabel>
#include <QStatusBar>

namespace lf {

CollectionExtras::CollectionExtras(ViewerWindow& window, Application& app)
    : QObject(&window), m_window(window), m_app(app) {
    setObjectName(QStringLiteral("lfCollectionExtras"));
    Collection* c = window.collection();
    c->setPairRawJpeg(Settings::instance().value(key::PairRawJpeg).toBool()); // F-FMT-06
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString& k) {
        if (k == QLatin1String(key::PairRawJpeg))
            m_window.collection()->setPairRawJpeg(Settings::instance().value(key::PairRawJpeg).toBool());
    });
    connect(c, &Collection::loadProgress, this, [this](int folders, int images) {
        //: status bar while a folder with subfolders is read (F-OPEN-08)
        m_progress = tr("Unterordner werden gelesen … %1 Ordner, %n Bild(er)", nullptr, images).arg(folders);
        updateStatus();
    });
    connect(c, &Collection::treeLimitReached, this, [this](int images) {
        //: status bar: a folder with subfolders has more images than are read (F-OPEN-08)
        m_window.showMessage(
            tr("Unterordner: Nur die ersten %n Bilder werden angezeigt – öffne einen kleineren Ordner.",
               nullptr, images),
            8000);
    });
    connect(c, &Collection::loadingFinished, this, [this] {
        m_progress.clear();
        updateStatus();
    });
    connect(c, &Collection::currentChanged, this, &CollectionExtras::updateStatus);
    connect(c, &Collection::entriesChanged, this, &CollectionExtras::updateStatus);
}

bool CollectionExtras::perform(Action a) {
    switch (a) {
    case Action::ToggleSubfolders:
        setIncludeSubfolders(!includesSubfolders());
        return true;
    case Action::ToggleRawJpeg:
        toggleRawJpeg();
        return true;
    default:
        return false;
    }
}

bool CollectionExtras::includesSubfolders() const {
    return m_window.collection()->includesSubfolders();
}

void CollectionExtras::setIncludeSubfolders(bool on) {
    Collection* c = m_window.collection();
    if (c->includesSubfolders() == on)
        return;
    c->setIncludeSubfolders(on);
    if (!c->isFolderCollection())
        m_window.showMessage(tr("Unterordner gelten nur für Ordner, nicht für einzeln geöffnete Dateien"),
                             2500);
    else
        m_window.showMessage(on ? tr("Unterordner einbezogen") : tr("Nur dieser Ordner"), 1500);
    updateStatus();
    m_app.windowChanged(&m_window);
}

bool CollectionExtras::currentIsPair() const {
    const Collection* c = m_window.collection();
    const int i = c->currentIndex();
    return i >= 0 && !c->at(i).pairedPath.isEmpty();
}

bool CollectionExtras::currentShowsRaw() const {
    return currentIsPair() && isRawSuffix(QFileInfo(m_window.currentPath()).suffix().toLower());
}

void CollectionExtras::toggleRawJpeg() {
    if (!currentIsPair()) {
        m_window.showMessage(m_window.collection()->pairsRawJpeg()
                                 ? tr("Dieses Bild ist kein RAW+JPEG-Paar")
                                 : tr("RAW+JPEG-Paare sind ausgeschaltet (Einstellungen → Formate)"),
                             2500);
        return;
    }
    if (m_window.collection()->togglePairMember(m_window.currentPath()))
        m_window.showMessage(currentShowsRaw() ? tr("RAW-Datei") : tr("JPEG-Datei"), 1200);
    m_app.windowChanged(&m_window);
}

QString CollectionExtras::subfolderText() const {
    const Collection* c = m_window.collection();
    if (!c->includesSubfolders() || c->currentPath().isEmpty())
        return {};
    const QString rel = c->relativeFolder(c->currentPath());
    //: status bar: the subfolder of the current image (F-OPEN-08); %1 = path below the opened folder
    return rel.isEmpty() ? QString() : tr("in „%1“").arg(rel);
}

QString CollectionExtras::pairText() const {
    if (!currentIsPair())
        return {};
    return currentShowsRaw() ? tr("RAW+JPEG · RAW") : tr("RAW+JPEG");
}

QStringList CollectionExtras::statusParts() const {
    QStringList parts;
    for (const QString& s : {m_progress, subfolderText(), pairText()})
        if (!s.isEmpty())
            parts << s;
    return parts;
}

void CollectionExtras::attachStatusBar(QStatusBar* bar) {
    if (m_progressLabel || !bar)
        return;
    auto make = [bar](const char* name) {
        auto* l = new QLabel(bar);
        l->setObjectName(QString::fromLatin1(name));
        l->setContentsMargins(6, 0, 6, 0);
        bar->addWidget(l);
        l->hide();
        return l;
    };
    m_progressLabel = make("lfStatusSubfolderProgress");
    m_folderLabel = make("lfStatusSubfolder");
    m_pairLabel = make("lfStatusRawJpeg");
    // A small badge (F-FMT-06).
    m_pairLabel->setStyleSheet(QStringLiteral("QLabel { border: 1px solid palette(mid); border-radius: 4px; "
                                              "padding: 0px 4px; }"));
    updateStatus();
}

void CollectionExtras::updateStatus() {
    if (!m_progressLabel)
        return;
    m_progressLabel->setText(m_progress);
    m_progressLabel->setVisible(!m_progress.isEmpty());
    const QString folder = subfolderText();
    m_folderLabel->setText(folder);
    m_folderLabel->setVisible(!folder.isEmpty());
    const QString pair = pairText();
    m_pairLabel->setText(pair);
    m_pairLabel->setVisible(!pair.isEmpty());
    if (!pair.isEmpty()) {
        const Collection* c = m_window.collection();
        const QString other = QFileInfo(c->at(c->currentIndex()).pairedPath).fileName();
        m_pairLabel->setToolTip(tr("Zweite Datei: %1 – „RAW anzeigen“ wechselt").arg(other));
    }
}

} // namespace lf
