// SPDX-License-Identifier: GPL-3.0-or-later
#include "Share.h"

#include "core/AppScoped.h"

#include <QAction>
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QHash>
#include <QIcon>
#include <QMenu>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>

namespace lf {

namespace {
QString tr(const char* text) {
    return QCoreApplication::translate("lf::Share", text);
}
} // namespace

QList<QUrl> shareItems(const QStringList& paths) {
    QList<QUrl> out;
    for (const QString& p : paths)
        if (!p.isEmpty())
            out << QUrl::fromLocalFile(p);
    return out;
}

struct ShareServiceCache::Private {
    Query query = &shareServices;
    QHash<QString, QList<ShareService>> cache; // type key -> services
    QSet<QString> running;
    quint64 generation = 0; // clearCache() drops results of queries still running
};

ShareServiceCache::ShareServiceCache(QObject* parent) : QObject(parent), d(std::make_shared<Private>()) {}

ShareServiceCache& ShareServiceCache::instance() {
    static AppScoped<ShareServiceCache> s([](QObject* parent) { return new ShareServiceCache(parent); });
    return s.get();
}

std::optional<QList<ShareService>> ShareServiceCache::cached(const QString& path) const {
    auto it = d->cache.constFind(shareTypeKey(path));
    if (it == d->cache.cend())
        return std::nullopt;
    return *it;
}

bool ShareServiceCache::isQuerying(const QString& path) const {
    return d->running.contains(shareTypeKey(path));
}

void ShareServiceCache::request(const QString& path) {
    if (path.isEmpty())
        return;
    const QString key = shareTypeKey(path);
    if (d->cache.contains(key) || d->running.contains(key))
        return;
    d->running.insert(key);
    const quint64 generation = d->generation;
    using Watcher = QFutureWatcher<QList<ShareService>>;
    auto* w = new Watcher(this);
    connect(w, &Watcher::finished, this, [this, w, key, generation] {
        const QList<ShareService> services = w->result();
        w->deleteLater();
        if (generation != d->generation)
            return;
        d->running.remove(key);
        d->cache.insert(key, services);
        emit servicesReady(key);
    });
    // Any file of the type stands for the type (the services look at the type only).
    w->setFuture(QtConcurrent::run([query = d->query, path] { return query({path}); }));
}

void ShareServiceCache::setQueryForTesting(Query query) {
    d->query = query ? std::move(query) : Query(&shareServices);
    clearCache();
}

void ShareServiceCache::clearCache() {
    ++d->generation;
    d->cache.clear();
    d->running.clear();
}

void populateShareMenu(QMenu* menu, const QStringList& paths, const std::function<void()>& showPicker) {
    menu->clear();
    QAction* picker = menu->addAction(tr("Teilen …"));
    picker->setObjectName(QStringLiteral("share:picker"));
    picker->setEnabled(!paths.isEmpty());
    QObject::connect(picker, &QAction::triggered, menu, [showPicker] {
        if (showPicker)
            showPicker();
    });
    if (paths.isEmpty())
        return;
    ShareServiceCache& cache = ShareServiceCache::instance();
    const std::optional<QList<ShareService>> services = cache.cached(paths.first());
    if (!services) {
        menu->addSeparator();
        QAction* wait = menu->addAction(tr("Wird geladen …"));
        wait->setObjectName(QStringLiteral("share:pending"));
        wait->setEnabled(false);
        // Filled when the list arrives (also while the menu is open: Qt updates the native
        // menu). One-shot: the connection goes with the first matching result.
        const QString key = shareTypeKey(paths.first());
        QPointer<QMenu> target(menu);
        QPointer<QAction> placeholder(wait); // gone if the menu was filled again meanwhile
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection =
            QObject::connect(&cache, &ShareServiceCache::servicesReady, menu,
                             [target, placeholder, key, paths, showPicker, connection](const QString& ready) {
                                 if (ready != key)
                                     return;
                                 QObject::disconnect(*connection);
                                 if (target && placeholder)
                                     populateShareMenu(target, paths, showPicker);
                             });
        cache.request(paths.first());
        return;
    }
    if (services->isEmpty())
        return;
    menu->addSeparator();
    for (const ShareService& s : *services) {
        QAction* act =
            menu->addAction(s.icon.isNull() ? QIcon() : QIcon(QPixmap::fromImage(s.icon)), s.title);
        act->setObjectName(QStringLiteral("share:") + s.title);
        const QString title = s.title;
        QObject::connect(act, &QAction::triggered, menu,
                         [title, paths] { performShareService(title, paths); });
    }
}

} // namespace lf
