// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-MAC-04: registering the app as default viewer for many content types without
// blocking. Separate from MacShell so the bookkeeping (thread-safe error collection,
// "still running" notice, final result) is testable with a fake system call.

#include "core/Platform.h"

#include <QStringList>

#include <functional>

namespace lf::mac {

// Performs the system call for one content type; `completion` is called exactly once, on
// any thread, with an error text (empty = success).
using SetDefaultApp =
    std::function<void(const QString& type, std::function<void(const QString& error)> completion)>;

// Calls `setter` for every type `isKnownType` accepts (the others are reported as unknown).
// `done` runs once on the thread of `context` with the final result. If that takes longer
// than `pendingAfterMs` (macOS may be waiting for the user to confirm a system dialog),
// `pending` runs first with a note; it is not a failure.
void registerDefaultApp(const QStringList& types, const std::function<bool(const QString&)>& isKnownType,
                        const SetDefaultApp& setter, int pendingAfterMs, QObject* context,
                        Shell::DefaultViewerDone done, Shell::DefaultViewerPending pending);

// Text of the "still running" note.
QString defaultAppPendingMessage();

} // namespace lf::mac
