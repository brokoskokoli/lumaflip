// SPDX-License-Identifier: GPL-3.0-or-later
// Culling and rating in a viewer window (F-CULL-01..06), see CullingController.h.

#include "CullingController.h"

#include "Application.h"
#include "ImageView.h"
#include "QuickMoveController.h"
#include "ViewerWindow.h"
#include "core/BulkRejects.h"
#include "core/Collection.h"
#include "core/FileBatch.h"
#include "core/FileOps.h"
#include "core/InputBindings.h"
#include "core/RatingStore.h"
#include "core/Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStatusBar>
#include <QtConcurrent/QtConcurrentRun>

namespace lf {

namespace {

// F-CULL-06: rating badge (top left) and the key help (bottom) over the image.
class CullingOverlay : public QWidget {
public:
    explicit CullingOverlay(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("lfCullingOverlay"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }
    void set(const std::optional<Rating>& rating, bool help) {
        m_rating = rating;
        m_help = help;
        update();
    }
    QString badgeText() const {
        if (!m_rating)
            return QCoreApplication::translate("lf::CullingController", "Bewertung wird gelesen …");
        return rating::describe(*m_rating);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        // Mode label + rating, top left.
        const QString mode = QCoreApplication::translate("lf::CullingController", "AUSSORTIEREN");
        QFont small = font();
        small.setPointSizeF(small.pointSizeF() * 0.9);
        small.setBold(true);
        QFont big = font();
        big.setPointSizeF(big.pointSizeF() * 1.8);
        const QString text = badgeText();
        const QFontMetrics fs(small), fb(big);
        const int pad = 10;
        const int w = std::max(fs.horizontalAdvance(mode), fb.horizontalAdvance(text)) + 2 * pad;
        const int h = fs.height() + fb.height() + 2 * pad;
        QRect badge(16, 16, w, h);
        QColor fill(20, 20, 20, 190);
        if (m_rating && m_rating->isPick())
            fill = QColor(30, 120, 50, 220);
        else if (m_rating && m_rating->isReject())
            fill = QColor(150, 30, 30, 220);
        QPainterPath path;
        path.addRoundedRect(badge, 8, 8);
        p.fillPath(path, fill);
        p.setPen(QColor(240, 240, 240));
        p.setFont(small);
        p.drawText(badge.adjusted(pad, pad, -pad, 0), Qt::AlignLeft | Qt::AlignTop, mode);
        p.setFont(big);
        p.drawText(badge.adjusted(pad, pad + fs.height(), -pad, -pad), Qt::AlignLeft | Qt::AlignVCenter,
                   text);
        if (!m_help)
            return;
        // Large key help, bottom center.
        const QStringList lines = {
            QCoreApplication::translate("lf::CullingController",
                                        "0–5  Sterne     P  Behalten     X  Verwerfen     U  Markierung weg"),
            QCoreApplication::translate(
                "lf::CullingController",
                "← →  Blättern     M/C + 1–9  Verschieben/Kopieren     ⌘Z  Rückgängig     Esc  Beenden"),
        };
        QFont hf = font();
        hf.setPointSizeF(hf.pointSizeF() * 1.25);
        p.setFont(hf);
        const QFontMetrics fh(hf);
        int hw = 0;
        for (const QString& l : lines)
            hw = std::max(hw, fh.horizontalAdvance(l));
        hw = std::min(hw + 2 * pad + 8, width() - 32);
        const int hh = int(lines.size()) * fh.height() + 2 * pad;
        QRect help((width() - hw) / 2, height() - hh - 20, hw, hh);
        QPainterPath hp;
        hp.addRoundedRect(help, 8, 8);
        p.fillPath(hp, QColor(20, 20, 20, 170));
        p.setPen(QColor(235, 235, 235));
        for (int i = 0; i < lines.size(); ++i)
            p.drawText(QRect(help.left() + pad, help.top() + pad + i * fh.height(), help.width() - 2 * pad,
                             fh.height()),
                       Qt::AlignCenter, lines.at(i));
    }

private:
    std::optional<Rating> m_rating;
    bool m_help = true;
};

bool autoAdvance() {
    return Settings::instance().value(key::CullAutoAdvance).toBool();
}

} // namespace

CullingController::CullingController(ViewerWindow& window, Application& app)
    : QObject(&window), m_window(window), m_app(app) {
    setObjectName(QStringLiteral("lfCullingController"));
    static bool storeConfigured = false;
    RatingStore& store = RatingStore::instance();
    if (!storeConfigured) {
        storeConfigured = true;
        store.followSettings(); // key::Rating* -> RatingOptions (F-CULL-02)
    }
    m_quick = new QuickMoveController(window, app);
    Collection* c = window.collection();
    c->setRatingReader(store.reader()); // F-SORT-02 / F-CULL-04
    connect(&store, &RatingStore::ratingChanged, this, &CullingController::onRatingChanged);
    connect(c, &Collection::currentChanged, this, &CullingController::onCurrentChanged);
    connect(c, &Collection::entriesChanged, this, &CullingController::updateStatus);
    FileOps* ops = &app.fileOps();
    connect(ops, &FileOps::fileRemoved, this, [](const QString& p) { RatingStore::instance().forget(p); });
    connect(ops, &FileOps::fileChanged, this, [](const QString& p) { RatingStore::instance().forget(p); });
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString& k) {
        if (k == QLatin1String(key::CullShowHelp))
            updateOverlay();
        if (k.startsWith(QLatin1String("rating/")))
            onCurrentChanged(); // other stores: read the current rating again
    });
}

CullingController::~CullingController() = default;

CullingController* CullingController::of(ViewerWindow* window) {
    return window ? window->findChild<CullingController*>(QStringLiteral("lfCullingController"),
                                                          Qt::FindDirectChildrenOnly)
                  : nullptr;
}

void CullingController::setActive(bool on) {
    if (on == m_active)
        return;
    m_active = on;
    ImageView* view = m_window.view();
    if (Settings::instance().value(key::CullLockZoom).toBool()) {
        // F-CULL-06 "gesperrte Zoomstufe": zoom and position stay while flipping.
        if (on) {
            m_zoomWasLocked = view->zoomLocked();
            view->setZoomLocked(true);
        } else {
            view->setZoomLocked(m_zoomWasLocked);
        }
    }
    if (on && !m_overlay) {
        m_overlay = new CullingOverlay(view);
        view->installEventFilter(this);
    }
    if (on)
        onCurrentChanged();
    updateOverlay();
    m_window.showMessage(on ? tr("Aussortiermodus: Ziffern vergeben Sterne, P/X/U markieren")
                            : tr("Aussortiermodus beendet"),
                         1800);
    emit activeChanged(on);
    m_app.windowChanged(&m_window);
}

bool CullingController::interceptKey(const InputEvent& event) {
    return m_quick->interceptKey(event);
}

bool CullingController::perform(Action a) {
    if (rating::isRatingAction(a)) {
        rate(a);
        return true;
    }
    switch (a) {
    case Action::ToggleCullingMode:
        setActive(!m_active);
        return true;
    case Action::QuickMove:
        m_quick->start(QuickOp::Move);
        return true;
    case Action::QuickCopy:
        m_quick->start(QuickOp::Copy);
        return true;
    case Action::ExitModeOrNothing:
    case Action::ExitModeOrCloseWindow:
        if (!m_active)
            return false;
        setActive(false); // Esc leaves the culling mode first (F-IN-15 "Modus verlassen")
        return true;
    default:
        return false;
    }
}

std::optional<Rating> CullingController::currentRating() const {
    const QString cur = m_window.currentPath();
    return cur.isEmpty() ? std::nullopt : RatingStore::instance().cached(cur);
}

void CullingController::rate(Action a) {
    const QString cur = m_window.currentPath();
    if (cur.isEmpty())
        return;
    QPointer<CullingController> self(this);
    const QString name = QFileInfo(cur).fileName();
    // F-FMT-06: the second file of a RAW+JPEG pair gets the same change (its own sidecar),
    // in the same undo step.
    QStringList paths{cur};
    if (const QString other = m_window.collection()->pairedPath(cur); !other.isEmpty())
        paths << other;
    RatingStore::instance().changeRatings(
        m_app.fileOps(), paths, [a](const Rating& stored) { return rating::apply(stored, a); },
        [self, name](bool ok, const QString& error, const QString& notice, const Rating& now) {
            if (!self)
                return;
            if (!ok) {
                self->m_window.showMessage(error, 5000);
                return;
            }
            if (!notice.isEmpty() && !self->m_noticeShown) {
                self->m_noticeShown = true; // e.g. RAW: sidecar instead of the file (once per window)
                self->m_window.showMessage(notice, 6000);
                return;
            }
            if (!self->m_active) // without the overlay: a short confirmation
                self->m_window.showMessage(tr("%1: %2").arg(name, rating::describe(now)), 1500);
        });
    // F-CULL-03: on to the next image right away (the write runs in the background).
    if (rating::cullingStep(Rating(), a, autoAdvance()).advance)
        m_window.perform(Action::NextImage);
}

void CullingController::setFilter(const RatingFilter& filter) {
    m_window.collection()->setRatingFilter(filter);
    updateStatus();
    m_app.windowChanged(&m_window);
}

RatingFilter CullingController::filter() const {
    return m_window.collection()->ratingFilter();
}

QString CullingController::filterStatus() const {
    const Collection* c = m_window.collection();
    const RatingFilter f = c->ratingFilter();
    if (!f.isActive())
        return {};
    return tr("Filter: %1 — %2 / %3").arg(f.describe()).arg(c->count()).arg(c->totalCount());
}

void CullingController::bulkRejects(bool toTrash) {
    // Only what the window shows under its filters (name filter, rating filter); the other
    // file of a shown RAW+JPEG pair goes along (BulkRejects.h).
    QVector<QPair<QString, QString>> shown;
    for (const FileEntry& e : m_window.collection()->entries())
        shown.append({e.path, e.pairedPath});
    if (shown.isEmpty())
        return;
    // Ratings fresh from the disk on a worker.
    const RatingOptions o = RatingStore::instance().options();
    QPointer<CullingController> self(this);
    auto* w = new QFutureWatcher<bulkrejects::Plan>(this);
    connect(w, &QFutureWatcher<bulkrejects::Plan>::finished, this, [self, w, toTrash] {
        const bulkrejects::Plan plan = w->result();
        w->deleteLater();
        if (!self)
            return;
        ViewerWindow& win = self->m_window;
        if (plan.images == 0) {
            win.showMessage(tr("Keine verworfenen Bilder"), 2500);
            return;
        }
        auto* box = new QMessageBox(&win);
        box->setObjectName(QStringLiteral("lfBulkRejectsDialog"));
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setIcon(QMessageBox::Question);
        box->setText(toTrash ? tr("%n verworfene(s) Bild(er) in den Papierkorb legen?", nullptr, plan.images)
                             : tr("%n verworfene(s) Bild(er) in den Ordner „_Rejected“ verschieben?", nullptr,
                                  plan.images));
        box->setInformativeText(tr("Nur die angezeigten Bilder (aktuelle Filter). XMP-Zusatzdateien und "
                                   "die zweite Datei von RAW+JPEG-Paaren gehen mit. ⌘Z macht den ganzen "
                                   "Schritt rückgängig."));
        QPushButton* go =
            box->addButton(toTrash ? tr("In den Papierkorb") : tr("Verschieben"), QMessageBox::AcceptRole);
        go->setObjectName(QStringLiteral("lfBulkRejectsConfirm"));
        box->addButton(QMessageBox::Cancel);
        box->setDefaultButton(go);
        connect(box, &QMessageBox::finished, self, [self, box, go, plan, toTrash](int) {
            if (!self || box->clickedButton() != go)
                return;
            const QString undo =
                toTrash ? tr("Verworfene in den Papierkorb") : tr("Verworfene nach _Rejected");
            self->m_quick->transfer(
                toTrash ? TransferKind::Trash : TransferKind::Move, plan.items, undo, [toTrash](int n) {
                    return toTrash ? tr("%n Bild(er) in den Papierkorb gelegt", nullptr, n)
                                   : tr("%n Bild(er) nach „_Rejected“ verschoben", nullptr, n);
                });
        });
        box->open();
    });
    w->setFuture(QtConcurrent::run([shown, o, toTrash] {
        QStringList paths;
        for (const auto& s : shown)
            paths << s.first;
        return bulkrejects::plan(shown, ratingio::readMany(paths, o), toTrash);
    }));
}

void CullingController::attachStatusBar(QStatusBar* bar) {
    if (m_filterLabel || !bar)
        return;
    m_filterLabel = new QLabel(bar);
    m_filterLabel->setObjectName(QStringLiteral("lfStatusFilter"));
    m_filterLabel->setContentsMargins(6, 0, 6, 0);
    bar->addWidget(m_filterLabel);
    updateStatus();
}

void CullingController::updateStatus() {
    if (!m_filterLabel)
        return;
    const QString t = filterStatus();
    m_filterLabel->setText(t);
    m_filterLabel->setVisible(!t.isEmpty());
}

QWidget* CullingController::overlay() const {
    return m_overlay;
}

void CullingController::onCurrentChanged() {
    const QString cur = m_window.currentPath();
    if (m_active && !cur.isEmpty())
        RatingStore::instance().request(cur); // fresh from the disk; the overlay follows
    updateOverlay();
    updateStatus();
}

void CullingController::onRatingChanged(const QString& path, const Rating& r) {
    m_window.collection()->updateRating(path, r); // F-CULL-04 filter, F-SORT-02 values
    if (path == m_window.currentPath())
        updateOverlay();
    updateStatus();
}

void CullingController::updateOverlay() {
    if (!m_overlay)
        return;
    auto* o = static_cast<CullingOverlay*>(m_overlay.data());
    o->setVisible(m_active && !m_window.currentPath().isEmpty());
    o->set(currentRating(), Settings::instance().value(key::CullShowHelp).toBool());
    layoutOverlay();
}

void CullingController::layoutOverlay() {
    if (m_overlay)
        m_overlay->setGeometry(m_window.view()->rect());
    if (m_overlay && m_overlay->isVisible())
        m_overlay->raise();
}

bool CullingController::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_window.view() && event->type() == QEvent::Resize)
        layoutOverlay();
    return QObject::eventFilter(watched, event);
}

} // namespace lf
