// SPDX-License-Identifier: GPL-3.0-or-later
#include "OpenWith.h"

#include "core/AppScoped.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
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

constexpr auto kPreviewBundleId = "com.apple.Preview";

QIcon iconOf(const AppEntry& app) {
    if (app.icon.isNull())
        return {};
    return QIcon(QPixmap::fromImage(app.icon));
}

} // namespace

bool isOwnApplication(const AppEntry& app) {
    if (app.bundleId == QLatin1String(kLumaflipBundleId))
        return true;
    const QString ownId = ownBundleId();
    if (!ownId.isEmpty() && app.bundleId == ownId)
        return true;
    const QString path = QDir::cleanPath(app.path);
    const QString ownPath = ownBundlePath();
    if (!ownPath.isEmpty() && path == QDir::cleanPath(ownPath))
        return true;
    return QFileInfo(path).fileName().compare(QLatin1String("Lumaflip.app"), Qt::CaseInsensitive) == 0;
}

QString chooseEditor(const QList<AppEntry>& apps, const QString& configuredAppPath) {
    if (!configuredAppPath.isEmpty()) {
        AppEntry configured;
        configured.path = configuredAppPath;
        for (const AppEntry& a : apps)
            if (QDir::cleanPath(a.path) == QDir::cleanPath(configuredAppPath))
                configured.bundleId = a.bundleId;
        if (!isOwnApplication(configured))
            return configuredAppPath;
    }
    for (const AppEntry& a : apps)
        if (a.isDefault && !isOwnApplication(a))
            return a.path;
    // Lumaflip is the default viewer: Preview is the natural editor on a Mac, then any other.
    for (const AppEntry& a : apps)
        if (a.bundleId == QLatin1String(kPreviewBundleId))
            return a.path;
    for (const AppEntry& a : apps)
        if (!isOwnApplication(a))
            return a.path;
    return {};
}

struct OpenWithService::Private {
    Query query = &queryApplicationsForFile;
    QHash<QString, QList<AppEntry>> cache;
    QSet<QString> running;
    quint64 generation = 0; // a reset drops results of queries still running
};

OpenWithService::OpenWithService(QObject* parent) : QObject(parent), d(new Private) {}

OpenWithService& OpenWithService::instance() {
    // Owned by the application object; created again for a new one (AppScoped).
    static AppScoped<OpenWithService> s([](QObject* parent) { return new OpenWithService(parent); });
    return s.get();
}

QString OpenWithService::typeKey(const QString& filePath) {
    return QFileInfo(filePath).suffix().toLower();
}

std::optional<QList<AppEntry>> OpenWithService::cached(const QString& filePath) const {
    auto it = d->cache.constFind(typeKey(filePath));
    if (it == d->cache.constEnd())
        return std::nullopt;
    return *it;
}

bool OpenWithService::isQuerying(const QString& filePath) const {
    return d->running.contains(typeKey(filePath));
}

void OpenWithService::request(const QString& filePath) {
    const QString key = typeKey(filePath);
    if (filePath.isEmpty() || d->cache.contains(key) || d->running.contains(key))
        return;
    d->running.insert(key);
    const quint64 generation = d->generation;
    using Watcher = QFutureWatcher<QList<AppEntry>>;
    auto* w = new Watcher(this);
    connect(w, &Watcher::finished, this, [this, w, key, generation] {
        const QList<AppEntry> apps = w->result();
        w->deleteLater();
        if (generation != d->generation)
            return;
        d->running.remove(key);
        d->cache.insert(key, apps);
        emit applicationsReady(key);
    });
    w->setFuture(QtConcurrent::run([query = d->query, filePath] { return query(filePath); }));
}

void OpenWithService::withApplications(const QString& filePath, QObject* context,
                                       std::function<void(const QList<AppEntry>&)> done) {
    if (auto apps = cached(filePath)) {
        done(*apps);
        return;
    }
    const QString key = typeKey(filePath);
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(this, &OpenWithService::applicationsReady, context,
                          [this, key, connection, done](const QString& readyKey) {
                              if (readyKey != key)
                                  return;
                              disconnect(*connection);
                              done(d->cache.value(key));
                          });
    request(filePath);
}

void OpenWithService::openInEditor(const QString& filePath, Shell& shell, QObject* context,
                                   std::function<void(const QString& message)> report) {
    if (filePath.isEmpty())
        return;
    const QString configured = Settings::instance().value(key::EditorApp).toString();
    Shell* sh = &shell;
    withApplications(filePath, context, [sh, filePath, configured, report](const QList<AppEntry>& apps) {
        const QString app = chooseEditor(apps, configured);
        if (app.isEmpty()) {
            if (report)
                report(tr("Kein Programm zum Bearbeiten gefunden. Wähle eines unter Einstellungen → "
                          "Allgemein → Editor."));
            return;
        }
        sh->openWith(filePath, app);
    });
}

void OpenWithService::populateMenu(QMenu* menu, const QString& filePath, Shell& shell,
                                   QWidget* dialogParent) {
    menu->clear();
    if (filePath.isEmpty()) {
        menu->addAction(tr("Kein Bild geöffnet"))->setEnabled(false);
        return;
    }
    Shell* sh = &shell;
    QPointer<QWidget> parent(dialogParent);
    auto addOther = [menu, sh, filePath, parent] {
        menu->addSeparator();
        QAction* other = menu->addAction(tr("Andere …"));
        other->setObjectName(QStringLiteral("openWith:other"));
        QObject::connect(other, &QAction::triggered, menu, [sh, filePath, parent] {
            const QString app = QFileDialog::getOpenFileName(
                parent, tr("Programm wählen"), QStringLiteral("/Applications"), tr("Programme (*.app)"));
            if (!app.isEmpty())
                sh->openWith(filePath, app);
        });
    };
    const std::optional<QList<AppEntry>> apps = cached(filePath);
    if (!apps) {
        QAction* wait = menu->addAction(tr("Programme werden gesucht …"));
        wait->setObjectName(QStringLiteral("openWith:pending"));
        wait->setEnabled(false);
        addOther();
        QPointer<QMenu> target(menu);
        withApplications(filePath, menu, [this, target, filePath, sh, parent](const QList<AppEntry>&) {
            if (target) // filled while open: Qt updates the native menu
                populateMenu(target, filePath, *sh, parent);
        });
        return;
    }
    bool any = false;
    bool defaultDone = false;
    for (const AppEntry& app : *apps) {
        if (isOwnApplication(app))
            continue;
        if (any && defaultDone) {
            menu->addSeparator(); // Finder style: the default app first, then the others
            defaultDone = false;
        }
        QAction* act =
            menu->addAction(iconOf(app), app.isDefault ? tr("%1 (Standard)").arg(app.name) : app.name);
        act->setObjectName(QStringLiteral("openWith:") + app.path);
        act->setData(app.path);
        act->setToolTip(app.path);
        const QString appPath = app.path;
        QObject::connect(act, &QAction::triggered, menu,
                         [sh, filePath, appPath] { sh->openWith(filePath, appPath); });
        any = true;
        defaultDone = app.isDefault;
    }
    if (!any)
        menu->addAction(tr("Keine passenden Programme"))->setEnabled(false);
    addOther();
}

void OpenWithService::setQueryForTesting(Query query) {
    d->query = query ? std::move(query) : Query(&queryApplicationsForFile);
    clearCache();
}

void OpenWithService::clearCache() {
    ++d->generation;
    d->cache.clear();
    d->running.clear();
}

} // namespace lf
