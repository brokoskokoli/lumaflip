// SPDX-License-Identifier: GPL-3.0-or-later
// Several file operations as one undo step (see FileBatch.h).

#include "FileBatch.h"

#include "Platform.h"
#include "Sidecars.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <filesystem>
#include <system_error>

namespace lf {

namespace {

// Tr::tr() for the code in this file outside QObject classes (lupdate takes the context from it).
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::FileBatch)
};

const QString kSkip = QStringLiteral("skip");

bool present(const QString& p) {
    const QFileInfo fi(p);
    return fi.exists() || fi.isSymLink();
}

bool sameFile(const QString& a, const QString& b) {
    std::error_code ec;
    return std::filesystem::equivalent(std::filesystem::path(a.toStdU16String()),
                                       std::filesystem::path(b.toStdU16String()), ec) &&
           !ec;
}

QString fileName(const QString& p) {
    return QFileInfo(p).fileName();
}

// "IMG_1" of "IMG_1.CR3" (everything before the last dot; the whole name without one).
QString baseOf(const QString& name) {
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    return dot <= 0 ? name : name.left(dot);
}

// ".CR3" of "IMG_1.CR3", empty without a suffix.
QString dotSuffixOf(const QString& name) {
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    return dot <= 0 ? QString() : name.mid(dot);
}

bool isSidecar(const QString& path) {
    return path.endsWith(QLatin1String(".xmp"), Qt::CaseInsensitive);
}

QString copyFile(const QString& from, const QString& to) {
    if (present(to))
        return Tr::tr("„%1“ existiert bereits.").arg(fileName(to));
    if (!QFile::copy(from, to))
        return Tr::tr("„%1“ konnte nicht kopiert werden.").arg(fileName(from));
    return {};
}

QString transferFile(TransferKind kind, const QString& from, const QString& to) {
    return kind == TransferKind::Move ? fileops::moveNoReplace(from, to) : copyFile(from, to);
}

// Takes a moved/copied file back (undo, rollback).
QString takeBack(TransferKind kind, const QString& from, const QString& to) {
    if (kind == TransferKind::Move)
        return fileops::moveNoReplace(to, from);
    if (!QFile::remove(to))
        return Tr::tr("Die Kopie „%1“ konnte nicht entfernt werden.").arg(fileName(to));
    return {};
}

// Folding of names in one folder (NFC; case-insensitive volumes also case-folded).
class Folding {
public:
    QString fold(const QString& folder, const QString& name) {
        auto it = m_insensitive.constFind(folder);
        if (it == m_insensitive.cend())
            it = m_insensitive.insert(folder, sidecars::isCaseInsensitiveVolume(folder));
        const QString n = name.normalized(QString::NormalizationForm_C);
        return *it ? n.toCaseFolded() : n;
    }

private:
    QHash<QString, bool> m_insensitive;
};

// Target names of a unit: the members' and their sidecars' (`newBase`: renamed, "IMG_1 2").
struct UnitTargets {
    QStringList files;    // file names, parallel to the members
    QStringList sidecars; // file names, parallel to the sidecars
};

UnitTargets unitTargets(const QStringList& members, const QStringList& sidecarPaths, const QString& newBase,
                        Folding& folding) {
    UnitTargets t;
    for (const QString& m : members) {
        const QString name = fileName(m);
        t.files << (newBase.isEmpty() ? name : newBase + dotSuffixOf(name));
    }
    for (const QString& s : sidecarPaths) {
        const QString sideName = fileName(s);
        const QString folder = QFileInfo(s).absolutePath();
        QString target;
        for (qsizetype i = 0; i < members.size() && target.isEmpty(); ++i) {
            const QString memberName = fileName(members[i]);
            // "IMG_1.JPG.xmp": the member's new name plus the rest.
            if (sideName.size() > memberName.size() + 1 && sideName[memberName.size()] == u'.' &&
                folding.fold(folder, sideName.left(memberName.size())) == folding.fold(folder, memberName))
                target = t.files[i] + sideName.mid(memberName.size());
        }
        if (target.isEmpty()) // "IMG_1.xmp": the unit's (new) base name, the sidecar's suffix
            target = newBase.isEmpty() ? sideName : newBase + dotSuffixOf(sideName);
        t.sidecars << target;
    }
    return t;
}

// F-FILE-11 "Beide behalten" for a unit: the first "<base> n" free for every target name.
QString keepBothBase(const QString& folder, const QStringList& members, const QStringList& sidecarPaths,
                     Folding& folding) {
    QString base = baseOf(fileName(members.first())); // not const: moved on return
    for (int n = 2; n < 100000; ++n) {
        const QString candidate = QStringLiteral("%1 %2").arg(base).arg(n);
        const UnitTargets t = unitTargets(members, sidecarPaths, candidate, folding);
        bool free = true;
        for (const QString& name : t.files + t.sidecars)
            free = free && !present(QDir(folder).filePath(name));
        if (free)
            return candidate;
    }
    return base;
}

} // namespace

// ---- helpers ----

QString filebatch::keepBothName(const QString& folder, const QString& name,
                                const std::function<bool(const QString&)>& exists) {
    const auto taken = [&](const QString& n) {
        const QString p = QDir(folder).filePath(n);
        return exists ? exists(p) : present(p);
    };
    if (!taken(name))
        return name;
    const QFileInfo fi(name);
    const QString base = fi.completeBaseName().isEmpty() ? name : fi.completeBaseName();
    const QString suffix =
        fi.completeBaseName().isEmpty() || fi.suffix().isEmpty() ? QString() : QLatin1Char('.') + fi.suffix();
    for (int n = 2; n < 100000; ++n) {
        const QString candidate = QStringLiteral("%1 %2%3").arg(base).arg(n).arg(suffix);
        if (!taken(candidate))
            return candidate;
    }
    return name;
}

QVector<QVector<int>> filebatch::units(const QVector<TransferItem>& items, TransferKind kind) {
    QVector<QVector<int>> out;
    QHash<QString, int> byKey;
    Folding folding;
    for (int i = 0; i < items.size(); ++i) {
        const QFileInfo fi(items[i].source);
        const QString name = fi.fileName();
        const QString base = baseOf(name);
        if (base == name || base.isEmpty()) {
            out.append({i});
            continue;
        }
        const QString folder = fi.absolutePath();
        const QString key =
            folding.fold(folder, folder + QLatin1Char('/') + base) + QLatin1Char('\n') +
            (kind == TransferKind::Trash ? QString() : QDir::cleanPath(items[i].targetFolder));
        const auto it = byKey.constFind(key);
        if (it != byKey.cend()) {
            out[*it].append(i);
            continue;
        }
        byKey.insert(key, int(out.size()));
        out.append({i});
    }
    return out;
}

QStringList filebatch::conflicts(const QVector<TransferItem>& items, TransferKind kind) {
    QStringList out;
    if (kind == TransferKind::Trash)
        return out;
    for (const QVector<int>& unit : units(items, kind)) {
        for (int i : unit) {
            const TransferItem& it = items[i];
            const QString target = QDir(it.targetFolder).filePath(fileName(it.source));
            if (present(target) && !sameFile(it.source, target)) {
                out << items[unit.first()].source;
                break;
            }
        }
    }
    return out;
}

QStringList filebatch::unitNames(const QVector<TransferItem>& items, TransferKind kind,
                                 const QString& source) {
    for (const QVector<int>& unit : units(items, kind)) {
        QStringList names;
        bool found = false;
        for (int i : unit) {
            names << fileName(items[i].source);
            found = found || items[i].source == source;
        }
        if (found)
            return names;
    }
    return {fileName(source)};
}

QStringList filebatch::ownSidecars(const QString& imagePath) {
    return sidecars::travelling({imagePath});
}

// ---- FileBatch ----

FileBatch::FileBatch(TrashService& trash, TransferKind kind, QVector<TransferItem> items)
    : m_trash(trash), m_kind(kind), m_items(std::move(items)) {}

QVector<FileBatch::Step> FileBatch::steps() const {
    std::lock_guard g(m_mutex);
    return m_steps;
}

int FileBatch::doneUnits() const {
    std::lock_guard g(m_mutex);
    return int(m_steps.size());
}

QStringList FileBatch::skipped() const {
    std::lock_guard g(m_mutex);
    return m_skipped;
}

QString FileBatch::doStep(const QVector<int>& unit, Step* step) {
    QStringList members;
    for (int i : unit) {
        if (present(m_items[i].source))
            members << m_items[i].source;
        else
            m_skipped << m_items[i].source;
    }
    if (members.isEmpty())
        return kSkip;
    const TransferItem& first = m_items[unit.first()];
    // One listing per source folder for the whole batch: units differ in their base names,
    // so moving one unit does not change what another one owns.
    const QString sourceFolder = QFileInfo(members.first()).absolutePath();
    auto listing = m_listings.find(sourceFolder);
    if (listing == m_listings.end())
        listing = m_listings.insert(sourceFolder, sidecars::Folder::list(sourceFolder));
    const QStringList sidecarPaths = sidecars::travelling(members, &*listing);
    auto restoreReplaced = [&] {
        for (auto it = step->replaced.crbegin(); it != step->replaced.crend(); ++it) {
            QString ignored;
            m_trash.restoreFromTrash(it->second, it->first, &ignored);
        }
        step->replaced.clear();
    };

    if (m_kind == TransferKind::Trash) {
        for (const QString& m : std::as_const(members)) {
            QString error;
            const auto loc = m_trash.moveToTrash(m, &error);
            if (!loc) {
                for (auto it = step->files.crbegin(); it != step->files.crend(); ++it) {
                    QString ignored;
                    m_trash.restoreFromTrash(it->second, it->first, &ignored);
                }
                step->files.clear();
                return error;
            }
            step->files.append({m, *loc});
        }
        for (const QString& s : sidecarPaths) {
            QString ignored;
            if (const auto sl = m_trash.moveToTrash(s, &ignored))
                step->sidecars.append({s, *sl});
        }
        return {};
    }

    const QString folder = first.targetFolder;
    if (!QFileInfo(folder).isDir()) {
        // Create the missing folders; remember the topmost new one for undo.
        QString top = folder;
        for (QString p = folder; !p.isEmpty() && !QFileInfo::exists(p); p = QFileInfo(p).absolutePath()) {
            top = p;
            if (QFileInfo(p).absolutePath() == p)
                break;
        }
        if (!QDir().mkpath(folder))
            return Tr::tr("Der Ordner „%1“ konnte nicht angelegt werden.").arg(folder);
        m_createdFolders << top;
    }
    Folding folding;
    bool conflict = false;
    for (const QString& m : std::as_const(members)) {
        const QString target = QDir(folder).filePath(fileName(m));
        if (!present(target))
            continue;
        if (sameFile(m, target) && m_kind == TransferKind::Move)
            return kSkip; // already there
        conflict = true;
    }
    QString newBase;
    if (conflict) {
        switch (first.onConflict) {
        case ConflictChoice::Skip:
            return kSkip;
        case ConflictChoice::KeepBoth:
            newBase = keepBothBase(folder, members, sidecarPaths, folding);
            break;
        case ConflictChoice::Replace:
            for (const QString& m : std::as_const(members))
                if (sameFile(m, QDir(folder).filePath(fileName(m))))
                    return kSkip;
            break;
        }
    }
    const UnitTargets t = unitTargets(members, sidecarPaths, newBase, folding);
    if (conflict && first.onConflict == ConflictChoice::Replace) {
        // The old targets - and the sidecars they had under our sidecars' names - go.
        for (const QString& name : t.files + t.sidecars) {
            const QString target = QDir(folder).filePath(name);
            if (!present(target))
                continue;
            QString error;
            const auto loc = m_trash.moveToTrash(target, &error);
            if (!loc) {
                restoreReplaced();
                return error;
            }
            step->replaced.append({target, *loc});
        }
    }
    for (qsizetype i = 0; i < members.size(); ++i) {
        const QString target = QDir(folder).filePath(t.files[i]);
        if (const QString e = transferFile(m_kind, members[i], target); !e.isEmpty()) {
            for (auto it = step->files.crbegin(); it != step->files.crend(); ++it)
                takeBack(m_kind, it->first, it->second);
            step->files.clear();
            restoreReplaced();
            return e;
        }
        step->files.append({members[i], target});
    }
    for (qsizetype i = 0; i < sidecarPaths.size(); ++i) {
        const QString target = QDir(folder).filePath(t.sidecars[i]);
        if (present(target))
            continue; // keep the other file's sidecar; ours stays where it was
        if (transferFile(m_kind, sidecarPaths[i], target).isEmpty())
            step->sidecars.append({sidecarPaths[i], target});
    }
    return {};
}

QString FileBatch::redoStep(Step& step) {
    if (m_kind == TransferKind::Trash) {
        for (qsizetype i = 0; i < step.files.size(); ++i) {
            QString error;
            const auto loc = m_trash.moveToTrash(step.files[i].first, &error);
            if (!loc) {
                for (qsizetype k = i - 1; k >= 0; --k) {
                    QString ignored;
                    m_trash.restoreFromTrash(step.files[k].second, step.files[k].first, &ignored);
                }
                return error;
            }
            step.files[i].second = *loc;
        }
        for (auto& [s, t] : step.sidecars) {
            QString ignored;
            if (const auto sl = m_trash.moveToTrash(s, &ignored))
                t = *sl;
        }
        return {};
    }
    if (!step.files.isEmpty())
        QDir().mkpath(QFileInfo(step.files.first().second).absolutePath());
    auto restoreReplaced = [&](qsizetype count) {
        for (qsizetype k = count - 1; k >= 0; --k) {
            QString ignored;
            m_trash.restoreFromTrash(step.replaced[k].second, step.replaced[k].first, &ignored);
        }
    };
    for (qsizetype i = 0; i < step.replaced.size(); ++i) {
        QString error;
        const auto loc = m_trash.moveToTrash(step.replaced[i].first, &error);
        if (!loc) {
            restoreReplaced(i);
            return error;
        }
        step.replaced[i].second = *loc;
    }
    for (qsizetype i = 0; i < step.files.size(); ++i) {
        const auto& [from, to] = step.files[i];
        if (const QString e = transferFile(m_kind, from, to); !e.isEmpty()) {
            for (qsizetype k = i - 1; k >= 0; --k)
                takeBack(m_kind, step.files[k].first, step.files[k].second);
            restoreReplaced(step.replaced.size()); // the replaced targets come back
            return e;
        }
    }
    for (const auto& [s, t] : std::as_const(step.sidecars))
        transferFile(m_kind, s, t);
    return {};
}

QString FileBatch::undoStep(const Step& step) {
    QString first;
    if (m_kind == TransferKind::Trash) {
        for (const auto& [from, loc] : step.files) {
            QString error;
            if (!m_trash.restoreFromTrash(loc, from, &error) && first.isEmpty())
                first = error;
        }
        for (const auto& [s, t] : step.sidecars) {
            QString ignored;
            m_trash.restoreFromTrash(t, s, &ignored);
        }
        return first;
    }
    for (const auto& [s, t] : step.sidecars)
        takeBack(m_kind, s, t);
    for (auto it = step.files.crbegin(); it != step.files.crend(); ++it)
        if (const QString e = takeBack(m_kind, it->first, it->second); !e.isEmpty() && first.isEmpty())
            first = e;
    if (!first.isEmpty())
        return first; // the replaced files cannot come back while ours are in the way
    for (auto it = step.replaced.crbegin(); it != step.replaced.crend(); ++it) {
        QString error;
        if (!m_trash.restoreFromTrash(it->second, it->first, &error) && first.isEmpty())
            first = error;
    }
    return first;
}

QString FileBatch::apply() {
    std::lock_guard g(m_mutex);
    if (!m_planned) {
        m_planned = true;
        for (const QVector<int>& unit : filebatch::units(m_items, m_kind)) {
            Step step;
            const QString e = doStep(unit, &step);
            if (e == kSkip) {
                for (int i : unit)
                    if (!m_skipped.contains(m_items[i].source))
                        m_skipped << m_items[i].source;
                continue;
            }
            if (!e.isEmpty()) {
                // All or nothing: roll back what was done.
                for (auto it = m_steps.crbegin(); it != m_steps.crend(); ++it)
                    undoStep(*it);
                for (const QString& f : std::as_const(m_createdFolders))
                    QDir().rmdir(f);
                m_steps.clear();
                m_listings.clear();
                m_createdFolders.clear();
                return e;
            }
            m_steps.append(step);
        }
        m_listings.clear();
        return {};
    }
    for (const QString& f : std::as_const(m_createdFolders))
        QDir().mkpath(f);
    for (qsizetype i = 0; i < m_steps.size(); ++i) {
        if (const QString e = redoStep(m_steps[i]); !e.isEmpty()) {
            for (qsizetype k = i - 1; k >= 0; --k)
                undoStep(m_steps[k]);
            return e;
        }
    }
    return {};
}

QString FileBatch::revert() {
    std::lock_guard g(m_mutex);
    QString first;
    for (auto it = m_steps.crbegin(); it != m_steps.crend(); ++it) {
        const QString e = undoStep(*it);
        if (!e.isEmpty() && first.isEmpty())
            first = e;
    }
    // Folders this batch created (e.g. "_Rejected") go again if they are empty now.
    for (const QString& f : std::as_const(m_createdFolders)) {
        QDir dir(f);
        if (dir.exists() &&
            dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty())
            QDir().rmdir(f);
    }
    return first;
}

void FileBatch::notify(FileOps& ops, bool forward) const {
    const QVector<Step> s = steps();
    if (forward) {
        for (const Step& st : s) {
            for (const auto& r : st.replaced)
                if (!isSidecar(r.first))
                    emit ops.fileRemoved(r.first);
            for (const auto& [from, to] : st.files) {
                if (m_kind != TransferKind::Copy)
                    emit ops.fileRemoved(from);
                if (m_kind != TransferKind::Trash)
                    emit ops.fileAdded(to);
            }
        }
        return;
    }
    for (auto it = s.crbegin(); it != s.crend(); ++it) {
        for (auto f = it->files.crbegin(); f != it->files.crend(); ++f) {
            if (m_kind != TransferKind::Trash)
                emit ops.fileRemoved(f->second);
            if (m_kind != TransferKind::Copy)
                emit ops.fileAdded(f->first);
        }
        for (const auto& r : it->replaced)
            if (!isSidecar(r.first))
                emit ops.fileAdded(r.first);
    }
}

void runBatchAsync(FileOps& ops, const QString& undoText, const std::shared_ptr<FileBatch>& batch,
                   FileOps::Completion done) {
    FileOps* o = &ops;
    ops.runAsync(
        undoText, [batch] { return batch->apply(); }, [batch] { return batch->revert(); },
        [o, batch](bool forward) { batch->notify(*o, forward); }, std::move(done));
}

} // namespace lf
