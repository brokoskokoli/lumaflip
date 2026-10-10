// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The decoder helper process on Windows (SPEC 8.5; core/DecoderHelper.h).
//
// What the sandbox achieves (docs/WINDOWS.md):
//  - Low integrity level (a restricted copy of the app's token without privileges, mandatory
//    label Low): the helper cannot write to the user's files, folders or registry keys
//    (Windows' "no write up" policy), cannot send messages to the app's or other windows
//    (UIPI) and cannot open the app's process.
//  - Job object: the helper dies with the app (KILL_ON_JOB_CLOSE, also if the app
//    crashes), may not start any process (active process limit 1 and the child process
//    creation policy), no clipboard, desktop, global atoms or system settings (UI limits).
//  - Process mitigations: no DLLs from remote locations or with a low label, no extension
//    points (AppInit DLLs, IMEs), strict handle checks; after start-up no dynamic code.
//  - Only the two pipe ends (and stderr) are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST).
// Not achieved: network access is NOT blocked (that needs an AppContainer, which in turn
// cannot read arbitrary user files by path - the decoders get paths), and reading files is
// allowed (by design: the helper reads the image).

#include <QString>
#include <QStringList>

namespace lf::win {

struct HelperProcess {
    qint64 pid = 0;
    void* process = nullptr; // HANDLE
    void* job = nullptr;     // HANDLE
    int toChild = -1;        // app's write end (overlapped)
    int fromChild = -1;      // app's read end (overlapped)
};

// Starts `exe args` with the protocol pipes on its stdin/stdout, sandboxed as described
// above. Falls back to an unsandboxed start only if `allowUnsandboxed` (the helper then
// reports sandboxed = false and the app refuses it unless the pool allows that).
bool startDecoderHelper(const QString& exe, const QStringList& args, const QStringList& extraEnvironment,
                        HelperProcess* out, QString* error);
// Closes the pipes, terminates the process and waits for it.
void killDecoderHelper(HelperProcess* p);

// Helper side: checks that this process runs at low integrity inside a job and enables the
// mitigations that can only be set from inside (no dynamic code). False + why otherwise.
bool verifyDecoderSandbox(QString* error);

// Whether the process `pid` still runs (tests).
bool processAlive(qint64 pid);

} // namespace lf::win
