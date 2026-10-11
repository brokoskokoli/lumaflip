// SPDX-License-Identifier: GPL-3.0-or-later
#include "CommandLine.h"

#include <algorithm>
#include <iterator>

namespace lf::cmdline {

QString quoteArgument(const QString& arg) {
    const bool needsQuotes = arg.isEmpty() || arg.contains(u' ') || arg.contains(u'\t') ||
                             arg.contains(u'\n') || arg.contains(u'\v') || arg.contains(u'"');
    if (!needsQuotes)
        return arg;
    QString out;
    out.reserve(arg.size() + 2);
    out += u'"';
    qsizetype backslashes = 0;
    for (const QChar c : arg) {
        if (c == u'\\') {
            ++backslashes;
            continue;
        }
        if (c == u'"') {
            out += QString(backslashes * 2 + 1, u'\\'); // escape them and the quote
            out += u'"';
        } else {
            out += QString(backslashes, u'\\'); // literal: not before a quote
            out += c;
        }
        backslashes = 0;
    }
    out += QString(backslashes * 2, u'\\'); // before the closing quote
    out += u'"';
    return out;
}

QString quoteProgram(const QString& program) {
    if (program.contains(u'"'))
        return {};
    if (program.isEmpty() || program.contains(u' ') || program.contains(u'\t'))
        return u'"' + program + u'"';
    return program;
}

QString build(const QString& program, const QStringList& arguments) {
    QString out = quoteProgram(program);
    for (const QString& a : arguments) {
        out += u' ';
        out += quoteArgument(a);
    }
    return out;
}

QStringList splitArguments(const QString& line) {
    QStringList out;
    QString current;
    bool inArg = false, quoted = false;
    const qsizetype n = line.size();
    for (qsizetype i = 0; i < n;) {
        const QChar c = line.at(i);
        if (!quoted && (c == u' ' || c == u'\t')) {
            if (inArg) {
                out << current;
                current.clear();
                inArg = false;
            }
            ++i;
            continue;
        }
        inArg = true;
        if (c == u'\\') {
            qsizetype count = 0;
            while (i < n && line.at(i) == u'\\') {
                ++count;
                ++i;
            }
            if (i < n && line.at(i) == u'"') {
                current += QString(count / 2, u'\\');
                if (count % 2) {
                    current += u'"';
                    ++i;
                }
                // even: the quote is handled by the next round (toggles quoting)
            } else {
                current += QString(count, u'\\');
            }
            continue;
        }
        if (c == u'"') {
            // Inside quotes, "" is a literal quote (and quoting continues).
            if (quoted && i + 1 < n && line.at(i + 1) == u'"') {
                current += u'"';
                i += 2;
                continue;
            }
            quoted = !quoted;
            ++i;
            continue;
        }
        current += c;
        ++i;
    }
    if (inArg)
        out << current;
    return out;
}

QString effectiveFileName(const QString& program) {
    QString p = program.trimmed().toLower();
    p.replace(u'\\', u'/');
    p = p.mid(p.lastIndexOf(u'/') + 1);
    // "name.bat:stream" / "name.bat::$DATA": the file is name.bat (a drive colon is gone with
    // the folders above).
    if (const qsizetype colon = p.indexOf(u':'); colon >= 0)
        p.truncate(colon);
    while (p.endsWith(u'.') || p.endsWith(u' '))
        p.chop(1);
    return p;
}

bool isBatchFile(const QString& program) {
    const QString p = effectiveFileName(program);
    return p.endsWith(QLatin1String(".bat")) || p.endsWith(QLatin1String(".cmd"));
}

namespace {

// PowerShell takes unique prefixes of its parameter names.
bool isPrefixOf(const QString& name, const char* full, qsizetype minLength) {
    return name.size() >= minLength && QLatin1String(full).startsWith(name);
}

} // namespace

Reparse reparsesArguments(const QString& program, const QStringList& arguments) {
    if (isBatchFile(program))
        return Reparse::Batch;
    QString name = effectiveFileName(program);
    if (name.endsWith(QLatin1String(".exe")) || name.endsWith(QLatin1String(".com")))
        name.chop(4);
    static const char* const kShells[] = {"cmd", "conhost", "wsl", "bash"};
    for (const char* shell : kShells)
        if (name == QLatin1String(shell))
            return Reparse::Shell;
    if (name != QLatin1String("powershell") && name != QLatin1String("pwsh") &&
        name != QLatin1String("powershell_ise"))
        return Reparse::None;
    // Options with a value (the value is skipped); everything else that is not an option, or
    // any way to pass a command, makes PowerShell read script text.
    static const char* const kWithValue[] = {
        "executionpolicy", "windowstyle", "version",          "psconsolefile", "configurationname",
        "outputformat",    "inputformat", "workingdirectory", "settingsfile",  "custompipename"};
    for (qsizetype i = 0; i < arguments.size(); ++i) {
        const QString& a = arguments.at(i);
        if (!a.startsWith(u'-') && !a.startsWith(u'/'))
            return Reparse::PowerShellCommand; // a bare argument is a command (powershell.exe)
        const QString opt = a.mid(a.startsWith(QLatin1String("--")) ? 2 : 1).toLower();
        if (isPrefixOf(opt, "file", 1))
            return i + 1 < arguments.size() ? Reparse::None : Reparse::PowerShellCommand;
        if (isPrefixOf(opt, "command", 1) || isPrefixOf(opt, "commandwithargs", 1) ||
            opt == QLatin1String("e") || opt == QLatin1String("ec") || opt == QLatin1String("cwa") ||
            isPrefixOf(opt, "encodedcommand", 2) || isPrefixOf(opt, "encodedarguments", 2))
            return Reparse::PowerShellCommand;
        if (opt == QLatin1String("ep") || opt == QLatin1String("ex") || opt == QLatin1String("wd") ||
            opt == QLatin1String("w") || opt == QLatin1String("v") ||
            std::any_of(std::begin(kWithValue), std::end(kWithValue),
                        [&](const char* o) { return isPrefixOf(opt, o, 2); }))
            ++i; // its value
    }
    return Reparse::PowerShellCommand; // no -File: the appended file names would be commands
}

} // namespace lf::cmdline
