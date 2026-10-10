// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-FILE-10 (M4): user-defined actions - a shell command or a Shortcuts shortcut that gets
// the current file as argument, from the menu or a key of its own.
//
// Windows: there is no POSIX shell. A command is a program with arguments ("C:\Tools\x.exe"
// --flag); the files are appended as separate arguments, each quoted by the rules of
// CommandLineToArgvW (core/CommandLine.h), and the program is started with CreateProcess -
// never through cmd.exe. Batch files are refused (cmd would parse the file names);
// Shortcuts actions do not exist.
//
// Safety: file paths are never put into a command line. A shell command runs as
//     /bin/sh -c <command> lumaflip-action <file> …
// so the command reads the files as "$1" / "$@" (quotes, spaces, "$(…)", backquotes in file
// names stay data); a shortcut runs as
//     /usr/bin/shortcuts run <name> --input-path <file> …
// Both start without a shell around them (QProcess with an argument list). Every action runs
// asynchronously with a timeout (killed after it) and its output is collected (bounded) for
// the log. The first run of an action is confirmed by the user (CustomAction::confirmed).
//
// Stored as JSON in the setting key::CustomActions.

#include <QByteArray>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;
class QTimer;

namespace lf {

struct CustomAction {
    enum class Kind { Shell, Shortcut };
    QString id;   // stable, generated ("a1b2c3…")
    QString name; // menu text
    Kind kind = Kind::Shell;
    QString command; // shell command (Shell) or the shortcut's name (Shortcut)
    QKeySequence keys;
    bool confirmed = false; // the user allowed the first run
    friend bool operator==(const CustomAction&, const CustomAction&) = default;
};

namespace customactions {

inline constexpr int kMaxActions = 50;
inline constexpr qsizetype kMaxOutputBytes = 64 * 1024;
inline constexpr char kShellArgv0[] = "lumaflip-action";

QList<CustomAction> fromJson(const QString& json);
QString toJson(const QList<CustomAction>& actions);
QList<CustomAction> load(); // from Settings
void save(const QList<CustomAction>& actions);
QString newId();

// Why `action` cannot run (empty = it can): no name, no command, a shortcut name that would
// be read as an option ("-…").
QString validate(const CustomAction& action);
// Windows: the command split into program and arguments (CommandLineToArgvW rules; empty if
// there is no command). Elsewhere the trimmed command as the only element.
QStringList splitCommand(const QString& command);

struct ProcessSpec {
    QString program;
    QStringList arguments;
};
// The program and arguments for `files` (see above); `shortcutsTool` replaces
// /usr/bin/shortcuts (tests).
ProcessSpec processFor(const CustomAction& action, const QStringList& files,
                       const QString& shortcutsTool = QStringLiteral("/usr/bin/shortcuts"));

} // namespace customactions

// Runs one action invocation. Emits finished() exactly once (also after a timeout or a
// failed start); deletes nothing itself.
class CustomActionRun : public QObject {
    Q_OBJECT
public:
    struct Result {
        bool started = false;
        bool timedOut = false;
        int exitCode = -1;
        QString output; // stdout + stderr, at most kMaxOutputBytes (UTF-8, lossy)
        QString error;  // user facing, empty on success
        bool ok() const { return started && !timedOut && exitCode == 0; }
    };

    CustomActionRun(const CustomAction& action, const QStringList& files, int timeoutMs,
                    QObject* parent = nullptr,
                    const QString& shortcutsTool = QStringLiteral("/usr/bin/shortcuts"));
    ~CustomActionRun() override;
    void start();
    void cancel(); // kills the process; finished() follows
    const CustomAction& action() const { return m_action; }
    const Result& result() const { return m_result; }
    bool isRunning() const;

signals:
    void finished(const lf::CustomActionRun::Result& result);

private:
    void finish();
    void killGroup();
    void append(const QByteArray& bytes);

    CustomAction m_action;
    QStringList m_files;
    int m_timeoutMs;
    QString m_tool;
    QProcess* m_process = nullptr;
    QTimer* m_timer = nullptr;
    QByteArray m_output;
    Result m_result;
    bool m_done = false;
};

} // namespace lf
