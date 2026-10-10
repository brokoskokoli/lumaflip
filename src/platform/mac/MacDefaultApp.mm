// SPDX-License-Identifier: GPL-3.0-or-later
#include "MacDefaultApp.h"

#include <QCoreApplication>
#include <QMutex>
#include <QPointer>

#include <dispatch/dispatch.h>
#include <memory>

namespace lf::mac {

namespace {

// Shared by all completion handlers (any thread) and the delivery on the GUI thread.
struct Registration {
    QMutex mutex;
    QStringList errors;
    bool finished = false;
};

} // namespace

QString defaultAppPendingMessage() {
    return QCoreApplication::translate(
        "lf::Shell",
        "Lumaflip wird noch als Standard-App festgelegt – bitte bestätige die Rückfrage von macOS, "
        "falls eine erscheint.");
}

void registerDefaultApp(const QStringList& types, const std::function<bool(const QString&)>& isKnownType,
                        const SetDefaultApp& setter, int pendingAfterMs, QObject* context,
                        // By value: the dispatch blocks below keep copies (a reference would dangle).
                        // NOLINTNEXTLINE(performance-unnecessary-value-param)
                        Shell::DefaultViewerDone done, Shell::DefaultViewerPending pending) {
    auto state = std::make_shared<Registration>();
    QPointer<QObject> guard(context);
    // Runs `fn` on the thread of `context`, unless it is gone.
    auto post = [guard](const std::function<void()>& fn) {
        if (QObject* c = guard.data())
            QMetaObject::invokeMethod(
                c,
                [guard, fn] {
                    if (guard)
                        fn();
                },
                Qt::QueuedConnection);
    };

    dispatch_group_t group = dispatch_group_create();
    for (const QString& t : types) {
        if (!isKnownType(t)) {
            QMutexLocker lock(&state->mutex);
            state->errors << QStringLiteral("%1: %2").arg(
                t, QCoreApplication::translate("lf::Shell", "unbekannter Typ"));
            continue;
        }
        dispatch_group_enter(group);
        setter(t, [state, group, t](const QString& error) {
            if (!error.isEmpty()) {
                QMutexLocker lock(&state->mutex);
                state->errors << QStringLiteral("%1: %2").arg(t, error);
            }
            dispatch_group_leave(group);
        });
    }
    dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    // The final result, however long the system takes (no timeout: a late success is a
    // success, F-MAC-04).
    dispatch_group_notify(group, queue, ^{
      QStringList messages;
      {
          QMutexLocker lock(&state->mutex);
          state->finished = true;
          messages = state->errors;
      }
      post([done, messages] {
          if (done)
              done(messages.isEmpty(), messages.join(QLatin1Char('\n')));
      });
    });
    if (pending && pendingAfterMs >= 0) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(pendingAfterMs) * int64_t(NSEC_PER_MSEC)),
                       queue, ^{
                         {
                             QMutexLocker lock(&state->mutex);
                             if (state->finished)
                                 return;
                         }
                         post([pending, state] {
                             {
                                 QMutexLocker lock(&state->mutex);
                                 if (state->finished)
                                     return; // the result was faster than this note after all
                             }
                             pending(defaultAppPendingMessage());
                         });
                       });
    }
}

} // namespace lf::mac
