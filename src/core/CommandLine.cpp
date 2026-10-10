// SPDX-License-Identifier: GPL-3.0-or-later
#include "CommandLine.h"

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

bool isBatchFile(const QString& program) {
    const QString p = program.trimmed().toLower();
    return p.endsWith(QLatin1String(".bat")) || p.endsWith(QLatin1String(".cmd"));
}

} // namespace lf::cmdline
