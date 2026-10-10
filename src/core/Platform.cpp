// SPDX-License-Identifier: GPL-3.0-or-later
// Default implementations of the platform interfaces (Platform.h).

#include "Platform.h"

#include <QFutureWatcher>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

namespace lf {

namespace {
struct DefaultViewerResult {
    bool ok = false;
    QString error;
};
} // namespace

void Shell::makeDefaultViewerAsync(const QStringList& types, QObject* context, DefaultViewerDone done) {
    // The blocking call waits for LaunchServices; keep it off the GUI thread.
    auto* watcher = new QFutureWatcher<DefaultViewerResult>(context);
    QObject::connect(watcher, &QFutureWatcher<DefaultViewerResult>::finished, context, [watcher, done] {
        const DefaultViewerResult r = watcher->result();
        watcher->deleteLater();
        if (done)
            done(r.ok, r.error);
    });
    watcher->setFuture(QtConcurrent::run([this, types] {
        DefaultViewerResult r;
        r.ok = makeDefaultViewer(types, &r.error);
        return r;
    }));
}

} // namespace lf
