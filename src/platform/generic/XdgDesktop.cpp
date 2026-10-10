// SPDX-License-Identifier: GPL-3.0-or-later
// Freedesktop desktop entries, MIME associations and trash info files (XdgDesktop.h).

#include "XdgDesktop.h"

#include <QCollator>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMimeDatabase>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

#include <algorithm>

namespace lf::xdg {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::Shell)
};

QString env(const char* name) {
    return qEnvironmentVariable(name);
}

QStringList envDirs(const char* name, const QStringList& fallback) {
    const QString v = env(name);
    QStringList out;
    for (const QString& d : v.split(QLatin1Char(':'), Qt::SkipEmptyParts))
        if (QDir::isAbsolutePath(d))
            out << QDir::cleanPath(d);
    return out.isEmpty() ? fallback : out;
}

// General escapes of string values: \s \n \t \r \\ (and \; inside lists, kept for splitting).
QString unescape(const QString& v, bool keepSemicolonEscape = false) {
    QString out;
    out.reserve(v.size());
    for (qsizetype i = 0; i < v.size(); ++i) {
        const QChar c = v.at(i);
        if (c != QLatin1Char('\\') || i + 1 == v.size()) {
            out += c;
            continue;
        }
        const QChar n = v.at(++i);
        switch (n.unicode()) {
        case 's':
            out += QLatin1Char(' ');
            break;
        case 'n':
            out += QLatin1Char('\n');
            break;
        case 't':
            out += QLatin1Char('\t');
            break;
        case 'r':
            out += QLatin1Char('\r');
            break;
        case '\\':
            out += QLatin1Char('\\');
            break;
        case ';':
            out += keepSemicolonEscape ? QStringLiteral("\\;") : QStringLiteral(";");
            break;
        default:
            out += QLatin1Char('\\');
            out += n;
        }
    }
    return out;
}

QStringList splitList(const QString& raw) {
    QStringList out;
    QString cur;
    const QString v = unescape(raw, true);
    for (qsizetype i = 0; i < v.size(); ++i) {
        if (v.at(i) == QLatin1Char('\\') && i + 1 < v.size() && v.at(i + 1) == QLatin1Char(';')) {
            cur += QLatin1Char(';');
            ++i;
        } else if (v.at(i) == QLatin1Char(';')) {
            if (!cur.trimmed().isEmpty())
                out << cur.trimmed();
            cur.clear();
        } else {
            cur += v.at(i);
        }
    }
    if (!cur.trimmed().isEmpty())
        out << cur.trimmed();
    return out;
}

// Key/value pairs of each group of an INI-like freedesktop file (keys keep their locale
// suffix, the first occurrence wins).
using Groups = QHash<QString, QHash<QString, QString>>;

Groups readGroups(const QString& path) {
    Groups groups;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return groups;
    QString group;
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            group = line.mid(1, line.size() - 2);
            continue;
        }
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0 || group.isEmpty())
            continue;
        const QString key = line.left(eq).trimmed();
        auto& g = groups[group];
        if (!g.contains(key))
            g.insert(key, line.mid(eq + 1).trimmed());
    }
    return groups;
}

QString localized(const QHash<QString, QString>& g, const QString& key, const QString& locale) {
    if (!locale.isEmpty()) {
        // lang_COUNTRY@MODIFIER → lang_COUNTRY, lang (modifiers are rare for names).
        QString l = locale.section(QLatin1Char('.'), 0, 0).section(QLatin1Char('@'), 0, 0);
        for (const QString& cand : {l, l.section(QLatin1Char('_'), 0, 0)}) {
            const auto it = g.constFind(key + QLatin1Char('[') + cand + QLatin1Char(']'));
            if (it != g.cend())
                return unescape(*it);
        }
    }
    return unescape(g.value(key));
}

QStringList currentDesktops() {
    return env("XDG_CURRENT_DESKTOP").split(QLatin1Char(':'), Qt::SkipEmptyParts);
}

// All installed application entries by id (the first directory wins).
QList<DesktopEntry> allApplications(const QString& locale) {
    QList<DesktopEntry> out;
    QSet<QString> seen;
    for (const QString& dir : applicationDirs()) {
        QDirIterator it(dir, {QStringLiteral("*.desktop")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            QString id = QDir(dir).relativeFilePath(path);
            id.replace(QLatin1Char('/'), QLatin1Char('-'));
            if (seen.contains(id))
                continue;
            seen.insert(id);
            if (auto e = parseDesktopFile(path, id, locale))
                out << *e;
        }
    }
    return out;
}

// mimeapps.list files in lookup order.
QStringList mimeappsFiles() {
    QStringList names;
    for (const QString& d : currentDesktops())
        names << d.toLower() + QStringLiteral("-mimeapps.list");
    names << QStringLiteral("mimeapps.list");
    QStringList dirs{configHome()};
    dirs << configDirs();
    dirs << dataHome() + QStringLiteral("/applications");
    for (const QString& d : dataDirs())
        dirs << d + QStringLiteral("/applications");
    QStringList out;
    for (const QString& dir : dirs)
        for (const QString& n : names)
            out << dir + QLatin1Char('/') + n;
    return out;
}

QStringList withAncestors(const QString& mimeType) {
    QStringList out{mimeType};
    const QMimeType t = QMimeDatabase().mimeTypeForName(mimeType);
    if (t.isValid()) {
        if (t.name() != mimeType)
            out << t.name(); // alias → canonical name
        for (const QString& a : t.allAncestors())
            if (a != QLatin1String("application/octet-stream"))
                out << a;
    }
    out.removeDuplicates();
    return out;
}

} // namespace

QString dataHome() {
    const QString v = env("XDG_DATA_HOME");
    return QDir::isAbsolutePath(v) ? QDir::cleanPath(v) : QDir::homePath() + QStringLiteral("/.local/share");
}

QStringList dataDirs() {
    return envDirs("XDG_DATA_DIRS", {QStringLiteral("/usr/local/share"), QStringLiteral("/usr/share")});
}

QString configHome() {
    const QString v = env("XDG_CONFIG_HOME");
    return QDir::isAbsolutePath(v) ? QDir::cleanPath(v) : QDir::homePath() + QStringLiteral("/.config");
}

QStringList configDirs() {
    return envDirs("XDG_CONFIG_DIRS", {QStringLiteral("/etc/xdg")});
}

QStringList applicationDirs() {
    QStringList out{dataHome() + QStringLiteral("/applications")};
    for (const QString& d : dataDirs())
        out << d + QStringLiteral("/applications");
    out.removeDuplicates();
    return out;
}

std::optional<DesktopEntry> parseDesktopFile(const QString& path, const QString& id, const QString& locale) {
    const Groups groups = readGroups(path);
    const auto git = groups.constFind(QStringLiteral("Desktop Entry"));
    if (git == groups.cend())
        return std::nullopt;
    const auto& g = *git;
    if (g.value(QStringLiteral("Type")) != QLatin1String("Application"))
        return std::nullopt;
    DesktopEntry e;
    e.path = QFileInfo(path).absoluteFilePath();
    e.id = id.isEmpty() ? QFileInfo(path).fileName() : id;
    e.name = localized(g, QStringLiteral("Name"), locale);
    e.exec = unescape(g.value(QStringLiteral("Exec")));
    e.icon = unescape(g.value(QStringLiteral("Icon")));
    e.tryExec = unescape(g.value(QStringLiteral("TryExec")));
    e.mimeTypes = splitList(g.value(QStringLiteral("MimeType")));
    e.onlyShowIn = splitList(g.value(QStringLiteral("OnlyShowIn")));
    e.notShowIn = splitList(g.value(QStringLiteral("NotShowIn")));
    e.noDisplay = g.value(QStringLiteral("NoDisplay")) == QLatin1String("true");
    e.hidden = g.value(QStringLiteral("Hidden")) == QLatin1String("true");
    e.terminal = g.value(QStringLiteral("Terminal")) == QLatin1String("true");
    if (e.name.isEmpty() || e.exec.isEmpty())
        return std::nullopt;
    return e;
}

std::optional<DesktopEntry> findApplication(const QString& desktopId, const QString& locale) {
    if (desktopId.isEmpty())
        return std::nullopt;
    for (const QString& dir : applicationDirs()) {
        // "vendor-app.desktop" may live in dir/vendor/app.desktop.
        QString rel = desktopId;
        if (QFileInfo::exists(dir + QLatin1Char('/') + rel))
            return parseDesktopFile(dir + QLatin1Char('/') + rel, desktopId, locale);
        for (qsizetype i = rel.indexOf(QLatin1Char('-')); i > 0; i = rel.indexOf(QLatin1Char('-'), i + 1)) {
            QString cand = rel;
            cand[i] = QLatin1Char('/');
            if (QFileInfo::exists(dir + QLatin1Char('/') + cand))
                return parseDesktopFile(dir + QLatin1Char('/') + cand, desktopId, locale);
        }
    }
    return std::nullopt;
}

bool isShown(const DesktopEntry& e) {
    if (e.hidden || e.noDisplay)
        return false;
    const QStringList desktops = currentDesktops();
    if (!e.onlyShowIn.isEmpty() && std::none_of(desktops.begin(), desktops.end(),
                                                [&](const QString& d) { return e.onlyShowIn.contains(d); }))
        return false;
    if (std::any_of(desktops.begin(), desktops.end(),
                    [&](const QString& d) { return e.notShowIn.contains(d); }))
        return false;
    if (!e.tryExec.isEmpty()) {
        if (QDir::isAbsolutePath(e.tryExec) ? !QFileInfo(e.tryExec).isExecutable()
                                            : QStandardPaths::findExecutable(e.tryExec).isEmpty())
            return false;
    }
    return true;
}

QStringList expandExec(const DesktopEntry& e, const QStringList& files) {
    // 1. Split into arguments (quoting rules of the Exec key).
    QStringList raw;
    QString cur;
    bool inQuotes = false, have = false;
    const QString& s = e.exec;
    for (qsizetype i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (inQuotes) {
            if (c == QLatin1Char('\\') && i + 1 < s.size() &&
                QStringLiteral("\"`$\\").contains(s.at(i + 1))) {
                cur += s.at(++i);
            } else if (c == QLatin1Char('"')) {
                inQuotes = false;
            } else {
                cur += c;
            }
        } else if (c == QLatin1Char('"')) {
            inQuotes = true;
            have = true;
        } else if (c == QLatin1Char(' ') || c == QLatin1Char('\t')) {
            if (have || !cur.isEmpty())
                raw << cur;
            cur.clear();
            have = false;
        } else {
            cur += c;
        }
    }
    if (inQuotes)
        return {};
    if (have || !cur.isEmpty())
        raw << cur;
    if (raw.isEmpty())
        return {};
    // 2. Field codes.
    auto url = [](const QString& f) { return QUrl::fromLocalFile(f).toString(QUrl::FullyEncoded); };
    QStringList out;
    bool usedFiles = false;
    for (const QString& arg : std::as_const(raw)) {
        if (arg == QLatin1String("%F") || arg == QLatin1String("%U")) {
            for (const QString& f : files)
                out << (arg == QLatin1String("%U") ? url(f) : f);
            usedFiles = true;
            continue;
        }
        if (arg == QLatin1String("%i")) {
            if (!e.icon.isEmpty())
                out << QStringLiteral("--icon") << e.icon;
            continue;
        }
        QString a;
        bool hadCode = false; // an argument made of field codes only that expands to nothing goes
        for (qsizetype i = 0; i < arg.size(); ++i) {
            if (arg.at(i) != QLatin1Char('%') || i + 1 == arg.size()) {
                a += arg.at(i);
                continue;
            }
            const QChar code = arg.at(++i);
            hadCode = hadCode || code != QLatin1Char('%');
            switch (code.unicode()) {
            case '%':
                a += QLatin1Char('%');
                break;
            case 'f':
            case 'u':
                usedFiles = true;
                if (!files.isEmpty())
                    a += code == QLatin1Char('u') ? url(files.first()) : files.first();
                break;
            case 'F':
            case 'U':
                usedFiles = true;
                if (!files.isEmpty())
                    a += code == QLatin1Char('U') ? url(files.first()) : files.first();
                break;
            case 'c':
                a += e.name;
                break;
            case 'k':
                a += e.path;
                break;
            default: // %d %D %n %N %v %m and unknown codes: removed
                break;
            }
        }
        if (!(hadCode && a.isEmpty()))
            out << a;
    }
    if (!usedFiles)
        out << files;
    return out;
}

bool launch(const DesktopEntry& e, const QStringList& files, QString* error) {
    QStringList argv = expandExec(e, files);
    if (argv.isEmpty()) {
        if (error)
            *error = Tr::tr("„%1“ kann nicht gestartet werden.").arg(e.name);
        return false;
    }
    const QString program = argv.takeFirst();
    if (!QProcess::startDetached(program, argv)) {
        if (error)
            *error = Tr::tr("„%1“ kann nicht gestartet werden.").arg(e.name);
        return false;
    }
    return true;
}

QString defaultApplication(const QString& mimeType) {
    const QStringList types = withAncestors(mimeType);
    QSet<QString> removed;
    for (const QString& type : types) {
        for (const QString& file : mimeappsFiles()) {
            const Groups g = readGroups(file);
            for (const QString& id : splitList(g.value(QStringLiteral("Removed Associations")).value(type)))
                removed.insert(id);
            for (const QString& id : splitList(g.value(QStringLiteral("Default Applications")).value(type)))
                if (!removed.contains(id) && findApplication(id))
                    return id;
        }
        for (const QString& file : mimeappsFiles()) {
            const Groups g = readGroups(file);
            for (const QString& id : splitList(g.value(QStringLiteral("Added Associations")).value(type)))
                if (!removed.contains(id) && findApplication(id))
                    return id;
        }
    }
    return {};
}

QList<DesktopEntry> applicationsForMimeType(const QString& mimeType, const QString& locale) {
    const QStringList types = withAncestors(mimeType);
    QSet<QString> added, removed;
    for (const QString& file : mimeappsFiles()) {
        const Groups g = readGroups(file);
        for (const QString& type : types) {
            for (const QString& id : splitList(g.value(QStringLiteral("Added Associations")).value(type)))
                added.insert(id);
            for (const QString& id : splitList(g.value(QStringLiteral("Removed Associations")).value(type)))
                removed.insert(id);
        }
    }
    const QString def = defaultApplication(mimeType);
    QList<DesktopEntry> out;
    for (const DesktopEntry& e : allApplications(locale)) {
        if (removed.contains(e.id) && e.id != def)
            continue;
        const bool matches = added.contains(e.id) || e.id == def ||
                             std::any_of(types.begin(), types.end(),
                                         [&](const QString& t) { return e.mimeTypes.contains(t); });
        if (matches && isShown(e))
            out << e;
    }
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(out.begin(), out.end(), [&](const DesktopEntry& a, const DesktopEntry& b) {
        if ((a.id == def) != (b.id == def))
            return a.id == def;
        const int c = collator.compare(a.name, b.name);
        return c != 0 ? c < 0 : a.id < b.id;
    });
    return out;
}

bool setDefaultApplication(const QString& desktopId, const QStringList& mimeTypes, QString* error) {
    const QString path = configHome() + QStringLiteral("/mimeapps.list");
    QStringList lines;
    {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&f);
            in.setEncoding(QStringConverter::Utf8);
            while (!in.atEnd())
                lines << in.readLine();
        }
    }
    const QString section = QStringLiteral("[Default Applications]");
    qsizetype start = -1;
    for (qsizetype i = 0; i < lines.size(); ++i)
        if (lines.at(i).trimmed() == section) {
            start = i;
            break;
        }
    if (start < 0) {
        if (!lines.isEmpty() && !lines.last().trimmed().isEmpty())
            lines << QString();
        lines << section;
        start = lines.size() - 1;
    }
    qsizetype end = start + 1;
    while (end < lines.size() && !lines.at(end).trimmed().startsWith(QLatin1Char('[')))
        ++end;
    for (const QString& type : mimeTypes) {
        const QString entry = type + QLatin1Char('=') + desktopId + QLatin1Char(';');
        bool replaced = false;
        for (qsizetype i = start + 1; i < end; ++i) {
            const QString l = lines.at(i).trimmed();
            if (l.section(QLatin1Char('='), 0, 0).trimmed() == type) {
                lines[i] = entry;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            // After the last non-empty line of the section.
            qsizetype at = end;
            while (at > start + 1 && lines.at(at - 1).trimmed().isEmpty())
                --at;
            lines.insert(at, entry);
            ++end;
        }
    }
    QDir().mkpath(configHome());
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = Tr::tr("„%1“ kann nicht geschrieben werden.").arg(path);
        return false;
    }
    out.write(lines.join(QLatin1Char('\n')).toUtf8());
    out.write("\n");
    if (!out.commit()) {
        if (error)
            *error = Tr::tr("„%1“ kann nicht geschrieben werden.").arg(path);
        return false;
    }
    return true;
}

QString trashInfoPath(const QString& trashedFile) {
    const QFileInfo fi(trashedFile);
    return QDir::cleanPath(fi.absolutePath() + QStringLiteral("/../info/") + fi.fileName() +
                           QStringLiteral(".trashinfo"));
}

std::optional<QString> trashInfoOriginalPath(const QString& trashedFile) {
    const Groups g = readGroups(trashInfoPath(trashedFile));
    const QString raw = g.value(QStringLiteral("Trash Info")).value(QStringLiteral("Path"));
    if (raw.isEmpty())
        return std::nullopt;
    const QString decoded = QUrl::fromPercentEncoding(raw.toUtf8());
    if (QDir::isAbsolutePath(decoded))
        return QDir::cleanPath(decoded);
    // Relative paths belong to a $topdir trash: $topdir/.Trash-$uid or $topdir/.Trash/$uid.
    const QDir trash(QFileInfo(trashedFile).absolutePath() + QStringLiteral("/.."));
    QString top = QDir::cleanPath(trash.absolutePath() + QStringLiteral("/.."));
    if (QFileInfo(top).fileName() == QLatin1String(".Trash"))
        top = QDir::cleanPath(top + QStringLiteral("/.."));
    return QDir::cleanPath(top + QLatin1Char('/') + decoded);
}

} // namespace lf::xdg
