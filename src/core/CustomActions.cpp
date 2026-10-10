// SPDX-License-Identifier: GPL-3.0-or-later
#include "CustomActions.h"

#include "CommandLine.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <QUuid>

#ifdef Q_OS_UNIX
#include <csignal>
#include <sys/types.h>
#endif
#ifdef Q_OS_WIN
#include <memory>
#include <string>

#include <windows.h>

#include <shellapi.h>
#endif

namespace lf {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::CustomActions)
};

QString kindId(CustomAction::Kind k) {
    return k == CustomAction::Kind::Shortcut ? QStringLiteral("shortcut") : QStringLiteral("shell");
}

} // namespace

namespace customactions {

QList<CustomAction> fromJson(const QString& json) {
    QList<CustomAction> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isArray())
        return out;
    for (const auto& v : doc.array()) {
        if (out.size() >= kMaxActions)
            break;
        const QJsonObject o = v.toObject();
        CustomAction a;
        a.id = o.value(QStringLiteral("id")).toString();
        a.name = o.value(QStringLiteral("name")).toString();
        a.kind = o.value(QStringLiteral("kind")).toString() == QLatin1String("shortcut")
                     ? CustomAction::Kind::Shortcut
                     : CustomAction::Kind::Shell;
        a.command = o.value(QStringLiteral("command")).toString();
        a.keys =
            QKeySequence::fromString(o.value(QStringLiteral("keys")).toString(), QKeySequence::PortableText);
        a.confirmed = o.value(QStringLiteral("confirmed")).toBool();
        if (a.id.isEmpty())
            a.id = newId();
        out.append(a);
    }
    return out;
}

QString toJson(const QList<CustomAction>& actions) {
    QJsonArray arr;
    for (const CustomAction& a : actions) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), a.id);
        o.insert(QStringLiteral("name"), a.name);
        o.insert(QStringLiteral("kind"), kindId(a.kind));
        o.insert(QStringLiteral("command"), a.command);
        o.insert(QStringLiteral("keys"), a.keys.toString(QKeySequence::PortableText));
        o.insert(QStringLiteral("confirmed"), a.confirmed);
        arr.append(o);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QList<CustomAction> load() {
    return fromJson(Settings::instance().value(key::CustomActions).toString());
}

void save(const QList<CustomAction>& actions) {
    Settings::instance().setValue(key::CustomActions, toJson(actions));
}

QString newId() {
    return QUuid::createUuid().toString(QUuid::Id128).left(12);
}

QString validate(const CustomAction& a) {
    if (a.name.trimmed().isEmpty())
        return Tr::tr("Die Aktion braucht einen Namen.");
    if (a.command.trimmed().isEmpty())
        return a.kind == CustomAction::Kind::Shell
                   ? Tr::tr("Die Aktion braucht einen Befehl.")
                   : Tr::tr("Die Aktion braucht den Namen eines Kurzbefehls.");
    if (a.kind == CustomAction::Kind::Shortcut && a.command.trimmed().startsWith(u'-'))
        return Tr::tr("Der Name eines Kurzbefehls darf nicht mit „-“ beginnen.");
#ifdef Q_OS_WIN
    if (a.kind == CustomAction::Kind::Shortcut)
        return Tr::tr("Kurzbefehle gibt es nur unter macOS.");
    const QStringList argv = splitCommand(a.command);
    if (argv.isEmpty())
        return Tr::tr("Die Aktion braucht einen Befehl.");
    if (cmdline::isBatchFile(argv.first()))
        return Tr::tr(
            "Batch-Dateien (.bat, .cmd) können nicht direkt ausgeführt werden: cmd.exe würde Dateinamen "
            "als Befehle lesen. Starte stattdessen ein Programm oder ein PowerShell-Skript "
            "(powershell -File Skript.ps1).");
#endif
    return {};
}

QStringList splitCommand(const QString& command) {
#ifdef Q_OS_WIN
    // The rules of CommandLineToArgvW, which programs apply to their own command line.
    const QString trimmed = command.trimmed();
    if (trimmed.isEmpty())
        return {};
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(reinterpret_cast<LPCWSTR>(trimmed.utf16()), &argc);
    QStringList out;
    for (int i = 0; argv && i < argc; ++i)
        out << QString::fromWCharArray(argv[i]);
    if (argv)
        LocalFree(argv);
    return out;
#else
    const QString trimmed = command.trimmed();
    return trimmed.isEmpty() ? QStringList() : QStringList{trimmed};
#endif
}

ProcessSpec processFor(const CustomAction& action, const QStringList& files, const QString& shortcutsTool) {
    ProcessSpec spec;
    if (action.kind == CustomAction::Kind::Shortcut) {
        spec.program = shortcutsTool;
        spec.arguments = {QStringLiteral("run"), action.command.trimmed()};
        if (!files.isEmpty()) {
            spec.arguments << QStringLiteral("--input-path");
            for (const QString& f : files)
                spec.arguments << QFileInfo(f).absoluteFilePath(); // never starts with "-"
        }
        return spec;
    }
#ifdef Q_OS_WIN
    // Windows: the command is a program with its own arguments; the files are appended as
    // further arguments, each quoted for CommandLineToArgvW (CustomActionRun::start builds the
    // command line, core/CommandLine.h) - no cmd.exe in between.
    const QStringList argv = splitCommand(action.command);
    if (!argv.isEmpty()) {
        spec.program = argv.first();
        spec.arguments = argv.mid(1);
        for (const QString& f : files)
            spec.arguments << QDir::toNativeSeparators(f);
    }
#else
    // The command is the script; the files are positional parameters ("$1", "$@") - the
    // shell never parses them.
    spec.program = QStringLiteral("/bin/sh");
    spec.arguments = {QStringLiteral("-c"), action.command, QString::fromLatin1(kShellArgv0)};
    spec.arguments << files;
#endif
    return spec;
}

} // namespace customactions

CustomActionRun::CustomActionRun(const CustomAction& action, const QStringList& files, int timeoutMs,
                                 QObject* parent, const QString& shortcutsTool)
    : QObject(parent), m_action(action), m_files(files), m_timeoutMs(timeoutMs), m_tool(shortcutsTool) {}

CustomActionRun::~CustomActionRun() {
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->disconnect(this);
        killGroup();
        m_process->waitForFinished(1000);
    }
}

bool CustomActionRun::isRunning() const {
    return m_process && m_process->state() != QProcess::NotRunning;
}

void CustomActionRun::start() {
    if (m_process || m_done)
        return;
    if (const QString problem = customactions::validate(m_action); !problem.isEmpty()) {
        m_result.error = problem;
        QTimer::singleShot(0, this, &CustomActionRun::finish);
        return;
    }
    const customactions::ProcessSpec spec = customactions::processFor(m_action, m_files, m_tool);
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->setStandardInputFile(QProcess::nullDevice());
#ifdef Q_OS_UNIX
    // Own session (and process group): a timeout ends the command with everything it started.
    QProcess::UnixProcessParameters params;
    params.flags = QProcess::UnixProcessFlag::CreateNewSession |
                   QProcess::UnixProcessFlag::ResetSignalHandlers |
                   QProcess::UnixProcessFlag::CloseFileDescriptors;
    params.lowestFileDescriptorToClose = 3;
    m_process->setUnixProcessParameters(params);
#endif
#ifdef Q_OS_WIN
    // The command line exactly as core/CommandLine.h builds it (CommandLineToArgvW rules).
    // CreateProcessW may modify the buffer: a writable copy that lives as long as the modifier.
    auto commandLine = std::make_shared<std::wstring>(
        cmdline::build(QDir::toNativeSeparators(spec.program), spec.arguments).toStdWString());
    m_process->setCreateProcessArgumentsModifier([commandLine](QProcess::CreateProcessArguments* args) {
        args->arguments = commandLine->data();
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    if (!m_files.isEmpty()) // the folder of the (first) file, if it is there
        if (const QFileInfo dir(QFileInfo(m_files.first()).absolutePath()); dir.isDir())
            m_process->setWorkingDirectory(dir.absoluteFilePath());
    connect(m_process, &QProcess::readyReadStandardOutput, this,
            [this] { append(m_process->readAllStandardOutput()); });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        append(m_process->readAllStandardOutput());
        m_result.exitCode = status == QProcess::NormalExit ? code : -1;
        if (m_result.timedOut)
            m_result.error = Tr::tr("Die Aktion wurde nach %n Sekunde(n) abgebrochen.", nullptr,
                                    (m_timeoutMs + 999) / 1000);
        else if (status != QProcess::NormalExit)
            m_result.error = Tr::tr("Die Aktion wurde abgebrochen.");
        else if (code != 0)
            m_result.error = Tr::tr("Die Aktion endete mit Fehlercode %1.").arg(code);
        finish();
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return; // the others end in finished()
        m_result.error = m_action.kind == CustomAction::Kind::Shortcut
                             ? Tr::tr("Die Kurzbefehle-App konnte nicht gestartet werden.")
                             : Tr::tr("Der Befehl konnte nicht gestartet werden.");
        finish();
    });
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (!isRunning())
            return;
        m_result.timedOut = true;
        killGroup();
    });
    m_result.started = true;
    m_process->start(spec.program, spec.arguments, QIODevice::ReadOnly);
    if (m_timeoutMs > 0)
        m_timer->start(m_timeoutMs);
}

void CustomActionRun::cancel() {
    if (isRunning())
        killGroup();
}

void CustomActionRun::killGroup() {
#ifdef Q_OS_UNIX
    if (const qint64 pid = m_process->processId(); pid > 0)
        ::kill(-pid_t(pid), SIGKILL); // the session leader's group: the shell and its children
#endif
    m_process->kill();
}

void CustomActionRun::append(const QByteArray& bytes) {
    if (m_output.size() >= customactions::kMaxOutputBytes)
        return;
    m_output += bytes.left(customactions::kMaxOutputBytes - m_output.size());
}

void CustomActionRun::finish() {
    if (m_done)
        return;
    m_done = true;
    if (m_timer)
        m_timer->stop();
    if (m_result.error.isEmpty() && !m_result.started)
        m_result.error = Tr::tr("Der Befehl konnte nicht gestartet werden.");
    if (m_process && m_process->error() == QProcess::FailedToStart)
        m_result.started = false;
    m_result.output = QString::fromUtf8(m_output);
    emit finished(m_result);
}

} // namespace lf
