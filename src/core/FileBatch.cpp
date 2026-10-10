// SPDX-License-Identifier: GPL-3.0-or-later
// Several file operations as one undo step (see FileBatch.h).

#include "FileBatch.h"

#include "Platform.h"
#include "Rating.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <filesystem>
#include <system_error>

namespace lf {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("lf::FileBatch", text);
}

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

// Target name of a sidecar when its image goes to `imageTarget`.
QString sidecarTarget(const QString& sidecar, const QString& imageSource, const QString& imageTarget) {
    const QFileInfo target(imageTarget);
    const QString sideName = fileName(sidecar);
    const QString srcName = fileName(imageSource);
    if (sideName.startsWith(srcName + QLatin1Char('.'))) // "IMG.jpg.xmp"
        return target.dir().filePath(target.fileName() + sideName.mid(srcName.size()));
    // "IMG.xmp": base name of the new image, same extension as the sidecar.
    return target.dir().filePath(target.completeBaseName() + QLatin1Char('.') + QFileInfo(sidecar).suffix());
}

QString copyFile(const QString& from, const QString& to) {
    if (present(to))
        return tr("„%1“ existiert bereits.").arg(fileName(to));
    if (!QFile::copy(from, to))
        return tr("„%1“ konnte nicht kopiert werden.").arg(fileName(from));
    return {};
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

QStringList filebatch::conflicts(const QVector<TransferItem>& items, TransferKind kind) {
    QStringList out;
    if (kind == TransferKind::Trash)
        return out;
    for (const TransferItem& it : items) {
        const QString target = QDir(it.targetFolder).filePath(fileName(it.source));
        if (present(target) && !sameFile(it.source, target))
            out << it.source;
    }
    return out;
}

QStringList filebatch::ownSidecars(const QString& imagePath) {
    QStringList out;
    QSet<QString> seen;
    auto add = [&](const QString& p) {
        const QFileInfo fi(p);
        if (!fi.isFile())
            return;
        const QString key = fi.canonicalFilePath();
        if (seen.contains(key))
            return;
        seen.insert(key);
        out << fi.absoluteFilePath();
    };
    for (const QString& p : {imagePath + QStringLiteral(".xmp"), imagePath + QStringLiteral(".XMP")})
        if (QFileInfo::exists(p))
            add(p);
    const QFileInfo img(imagePath);
    const QString base = img.completeBaseName();
    const QDir dir = img.dir();
    // The Adobe sidecar ("IMG.xmp") only if no other file shares the base name.
    bool shared = false;
    for (const QString& other : dir.entryList({base + QStringLiteral(".*")}, QDir::Files | QDir::Hidden)) {
        const QString suffix = QFileInfo(other).suffix().toLower();
        if (other == img.fileName() || suffix == QLatin1String("xmp") ||
            QFileInfo(other).completeBaseName() != base)
            continue;
        shared = true;
        break;
    }
    if (!shared)
        for (const QString& p :
             {dir.filePath(base + QStringLiteral(".xmp")), dir.filePath(base + QStringLiteral(".XMP"))})
            if (QFileInfo::exists(p))
                add(p);
    return out;
}

// ---- FileBatch ----

FileBatch::FileBatch(TrashService& trash, TransferKind kind, QVector<TransferItem> items)
    : m_trash(trash), m_kind(kind), m_items(std::move(items)) {}

QVector<FileBatch::Step> FileBatch::steps() const {
    std::lock_guard g(m_mutex);
    return m_steps;
}

QStringList FileBatch::skipped() const {
    std::lock_guard g(m_mutex);
    return m_skipped;
}

QString FileBatch::doStep(const TransferItem& item, Step* step) {
    step->from = item.source;
    if (m_kind == TransferKind::Trash) {
        const QStringList sidecars = filebatch::ownSidecars(item.source);
        QString error;
        const auto loc = m_trash.moveToTrash(item.source, &error);
        if (!loc)
            return error;
        step->to = *loc;
        for (const QString& s : sidecars) {
            QString ignored;
            if (const auto sl = m_trash.moveToTrash(s, &ignored))
                step->sidecars.append({s, *sl});
        }
        return {};
    }
    const QString folder = item.targetFolder;
    if (!QFileInfo(folder).isDir()) {
        // Create the missing folders; remember the topmost new one for undo.
        QString top = folder;
        for (QString p = folder; !p.isEmpty() && !QFileInfo::exists(p); p = QFileInfo(p).absolutePath()) {
            top = p;
            if (QFileInfo(p).absolutePath() == p)
                break;
        }
        if (!QDir().mkpath(folder))
            return tr("Der Ordner „%1“ konnte nicht angelegt werden.").arg(folder);
        m_createdFolders << top;
    }
    QString target = QDir(folder).filePath(fileName(item.source));
    if (present(target)) {
        if (sameFile(item.source, target) && m_kind == TransferKind::Move)
            return QStringLiteral("skip"); // already there
        switch (item.onConflict) {
        case ConflictChoice::Skip:
            return QStringLiteral("skip");
        case ConflictChoice::KeepBoth:
            target = QDir(folder).filePath(filebatch::keepBothName(folder, fileName(item.source)));
            break;
        case ConflictChoice::Replace: {
            if (sameFile(item.source, target))
                return QStringLiteral("skip");
            QString error;
            const auto loc = m_trash.moveToTrash(target, &error);
            if (!loc)
                return error;
            step->replacedFrom = target;
            step->replacedTrash = *loc;
            break;
        }
        }
    }
    const QStringList sidecars = filebatch::ownSidecars(item.source);
    const QString e = m_kind == TransferKind::Move ? fileops::moveNoReplace(item.source, target)
                                                   : copyFile(item.source, target);
    if (!e.isEmpty()) {
        if (!step->replacedTrash.isEmpty()) {
            QString ignored;
            m_trash.restoreFromTrash(step->replacedTrash, step->replacedFrom, &ignored);
        }
        return e;
    }
    step->to = target;
    for (const QString& s : sidecars) {
        const QString t = sidecarTarget(s, item.source, target);
        if (present(t))
            continue; // keep the other file's sidecar; ours stays where it was
        const QString se = m_kind == TransferKind::Move ? fileops::moveNoReplace(s, t) : copyFile(s, t);
        if (se.isEmpty())
            step->sidecars.append({s, t});
    }
    return {};
}

QString FileBatch::redoStep(Step& step) {
    if (m_kind == TransferKind::Trash) {
        QString error;
        const auto loc = m_trash.moveToTrash(step.from, &error);
        if (!loc)
            return error;
        step.to = *loc;
        for (auto& [s, t] : step.sidecars) {
            QString ignored;
            if (const auto sl = m_trash.moveToTrash(s, &ignored))
                t = *sl;
        }
        return {};
    }
    QDir().mkpath(QFileInfo(step.to).absolutePath());
    if (!step.replacedFrom.isEmpty()) {
        QString error;
        const auto loc = m_trash.moveToTrash(step.replacedFrom, &error);
        if (!loc)
            return error;
        step.replacedTrash = *loc;
    }
    const QString e = m_kind == TransferKind::Move ? fileops::moveNoReplace(step.from, step.to)
                                                   : copyFile(step.from, step.to);
    if (!e.isEmpty())
        return e;
    for (const auto& [s, t] : std::as_const(step.sidecars)) {
        if (m_kind == TransferKind::Move)
            fileops::moveNoReplace(s, t);
        else
            copyFile(s, t);
    }
    return {};
}

QString FileBatch::undoStep(const Step& step) {
    QString error;
    if (m_kind == TransferKind::Trash) {
        if (!m_trash.restoreFromTrash(step.to, step.from, &error))
            return error;
        for (const auto& [s, t] : step.sidecars) {
            QString ignored;
            m_trash.restoreFromTrash(t, s, &ignored);
        }
        return {};
    }
    for (const auto& [s, t] : step.sidecars) {
        if (m_kind == TransferKind::Move)
            fileops::moveNoReplace(t, s);
        else
            QFile::remove(t);
    }
    if (m_kind == TransferKind::Move) {
        error = fileops::moveNoReplace(step.to, step.from);
    } else if (!QFile::remove(step.to)) {
        error = tr("Die Kopie „%1“ konnte nicht entfernt werden.").arg(fileName(step.to));
    }
    if (!error.isEmpty())
        return error;
    if (!step.replacedTrash.isEmpty() &&
        !m_trash.restoreFromTrash(step.replacedTrash, step.replacedFrom, &error))
        return error;
    return {};
}

QString FileBatch::apply() {
    std::lock_guard g(m_mutex);
    if (!m_planned) {
        m_planned = true;
        for (const TransferItem& item : std::as_const(m_items)) {
            if (!present(item.source)) {
                m_skipped << item.source;
                continue;
            }
            Step step;
            const QString e = doStep(item, &step);
            if (e == QLatin1String("skip")) {
                m_skipped << item.source;
                continue;
            }
            if (!e.isEmpty()) {
                // All or nothing: roll back what was done.
                for (auto it = m_steps.crbegin(); it != m_steps.crend(); ++it)
                    undoStep(*it);
                for (const QString& f : std::as_const(m_createdFolders))
                    QDir().rmdir(f);
                m_steps.clear();
                m_createdFolders.clear();
                return e;
            }
            m_steps.append(step);
        }
        return {};
    }
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
            if (!st.replacedFrom.isEmpty())
                emit ops.fileRemoved(st.replacedFrom);
            if (m_kind != TransferKind::Copy)
                emit ops.fileRemoved(st.from);
            if (m_kind != TransferKind::Trash)
                emit ops.fileAdded(st.to);
        }
        return;
    }
    for (auto it = s.crbegin(); it != s.crend(); ++it) {
        if (m_kind != TransferKind::Trash)
            emit ops.fileRemoved(it->to);
        if (m_kind != TransferKind::Copy)
            emit ops.fileAdded(it->from);
        if (!it->replacedFrom.isEmpty())
            emit ops.fileAdded(it->replacedFrom);
    }
}

void runBatchAsync(FileOps& ops, const QString& undoText, std::shared_ptr<FileBatch> batch,
                   FileOps::Completion done) {
    FileOps* o = &ops;
    ops.runAsync(
        undoText, [batch] { return batch->apply(); }, [batch] { return batch->revert(); },
        [o, batch](bool forward) { batch->notify(*o, forward); }, std::move(done));
}

} // namespace lf
