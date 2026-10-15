// SPDX-License-Identifier: GPL-3.0-or-later
// Default implementations of the platform interfaces (Platform.h).

#include "Platform.h"

#include <QFuture>
#include <QFutureWatcher>
#include <QList>
#include <QMutex>
#include <QtConcurrent/QtConcurrentRun>

namespace lf {

namespace {
struct DefaultViewerResult {
    bool ok = false;
    QString error;
};

class PlainPreparedNames final : public PreparedNames {
public:
    PlainPreparedNames(const NameCollator& collator, QVector<QString> names)
        : m_collator(collator), m_names(std::move(names)) {}
    int compare(qsizetype i, qsizetype j) const override {
        return m_collator.compare(m_names[i], m_names[j]);
    }

private:
    const NameCollator& m_collator;
    QVector<QString> m_names;
};
} // namespace

std::unique_ptr<PreparedNames> NameCollator::prepare(QVector<QString> names) const {
    return std::make_unique<PlainPreparedNames>(*this, std::move(names));
}

// Worker threads of makeDefaultViewerAsync() that use this shell.
struct Shell::AsyncCalls {
    QMutex mutex;
    QList<QFuture<DefaultViewerResult>> running;
};

Shell::Shell() : m_async(std::make_unique<AsyncCalls>()) {}

Shell::~Shell() {
    // Too late to be safe for derived classes (owners call waitForPendingCalls() first),
    // but never leave a thread running on freed memory of this part.
    waitForPendingCalls();
}

void Shell::waitForPendingCalls() {
    for (;;) {
        QFuture<DefaultViewerResult> f;
        {
            QMutexLocker lock(&m_async->mutex);
            if (m_async->running.isEmpty())
                return;
            f = m_async->running.takeFirst();
        }
        f.waitForFinished();
    }
}

DefaultViewerStatus Shell::defaultViewerStatus(const QString& /*filePath*/) {
    return {};
}

bool Shell::isInstalledApp() {
    return false;
}

// The signature is the virtual one (overrides keep the callbacks); this version ignores `pending`.
// NOLINTBEGIN(performance-unnecessary-value-param)
void Shell::makeDefaultViewerAsync(const QStringList& types, QObject* context, DefaultViewerDone done,
                                   DefaultViewerPending /*pending*/) {
    // NOLINTEND(performance-unnecessary-value-param)
    // The blocking call waits for LaunchServices; keep it off the GUI thread. The future is
    // remembered so the owner can wait for it before the shell goes away.
    const QFuture<DefaultViewerResult> future = QtConcurrent::run([this, types] {
        DefaultViewerResult r;
        r.ok = makeDefaultViewer(types, &r.error);
        return r;
    });
    {
        QMutexLocker lock(&m_async->mutex);
        m_async->running.removeIf([](const QFuture<DefaultViewerResult>& f) { return f.isFinished(); });
        m_async->running.append(future);
    }
    auto* watcher = new QFutureWatcher<DefaultViewerResult>(context);
    QObject::connect(watcher, &QFutureWatcher<DefaultViewerResult>::finished, context,
                     [watcher, done = std::move(done)] {
                         const DefaultViewerResult r = watcher->result();
                         watcher->deleteLater();
                         if (done)
                             done(r.ok, r.error);
                     });
    watcher->setFuture(future);
}

} // namespace lf
