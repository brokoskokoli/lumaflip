// SPDX-License-Identifier: GPL-3.0-or-later
#include "FinderViewSettings.h"

#include "BinaryPlist.h"

#include <QDir>
#include <QFileInfo>
#include <QVariantList>

namespace lf::finder {
namespace {

QVariantMap decodeBlob(const QVariant& value) {
    const QByteArray blob = value.toByteArray();
    if (!bplist::looksLikeBinaryPlist(blob))
        return {};
    const auto v = bplist::parse(blob);
    if (!v || v->typeId() != QMetaType::QVariantMap)
        return {};
    return v->toMap();
}

// Booleans in plists may come as bool or int.
std::optional<bool> boolValue(const QVariant& v) {
    if (!v.isValid())
        return std::nullopt;
    switch (v.typeId()) {
    case QMetaType::Bool:
        return v.toBool();
    case QMetaType::LongLong:
    case QMetaType::Int:
    case QMetaType::ULongLong:
    case QMetaType::UInt:
    case QMetaType::Double:
        return v.toLongLong() != 0;
    case QMetaType::QString:
        if (v.toString() == QLatin1String("1") ||
            v.toString().compare(QLatin1String("true"), Qt::CaseInsensitive) == 0)
            return true;
        if (v.toString() == QLatin1String("0") ||
            v.toString().compare(QLatin1String("false"), Qt::CaseInsensitive) == 0)
            return false;
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

// Default direction of a column when the plist does not say (Finder's defaults).
bool defaultAscending(SortKey key) {
    switch (key) {
    case SortKey::DateModified:
    case SortKey::DateCreated:
    case SortKey::DateAdded:
    case SortKey::DateCaptured:
    case SortKey::Size:
        return false;
    default:
        return true;
    }
}

// Columns we cannot reproduce; Finder's secondary order within them is by name.
bool isNameOnlyColumn(const QString& id) {
    static const QStringList ids = {QStringLiteral("label"),      QStringLiteral("version"),
                                    QStringLiteral("comments"),   QStringLiteral("ubiquity"),
                                    QStringLiteral("shareOwner"), QStringLiteral("shareLastEditor"),
                                    QStringLiteral("tags")};
    return ids.contains(id);
}

QVariantMap mapValue(const QVariantMap& m, const char* key) {
    return m.value(QLatin1String(key)).toMap();
}

std::optional<SortSpec> withSource(std::optional<SortSpec> s, SortSource source) {
    if (s)
        s->source = source;
    return s;
}

QString cleanDir(const QString& p) {
    QString c = QDir::cleanPath(p);
    const QFileInfo fi(c);
    const QString canon = fi.canonicalFilePath();
    return canon.isEmpty() ? c : canon;
}

} // namespace

ViewStyle viewStyleFromCode(const QString& code) {
    if (code == QLatin1String("icnv"))
        return ViewStyle::Icon;
    if (code == QLatin1String("Nlsv"))
        return ViewStyle::List;
    if (code == QLatin1String("clmv"))
        return ViewStyle::Column;
    if (code == QLatin1String("glyv") || code == QLatin1String("Flwv")) // Flwv: Cover Flow (old)
        return ViewStyle::Gallery;
    return ViewStyle::Unknown;
}

FolderRecords extractFolderRecords(const QVector<DsStoreRecord>& records, const QString& folderName) {
    FolderRecords r;
    // Record names are NFC (normalized by the parser); so is the name we look for.
    const QString name = folderName.normalized(QString::NormalizationForm_C);
    for (const DsStoreRecord& rec : records) {
        if (rec.fileName != name)
            continue;
        if (rec.code == QLatin1String("vstl")) {
            const ViewStyle s = viewStyleFromCode(rec.value.toString());
            if (s != ViewStyle::Unknown) {
                r.style = s;
                r.found = true;
            }
            continue;
        }
        QVariantMap* target = nullptr;
        if (rec.code == QLatin1String("lsvC"))
            target = &r.lsvC;
        else if (rec.code == QLatin1String("lsvp"))
            target = &r.lsvp;
        else if (rec.code == QLatin1String("lsvP"))
            target = &r.lsvP;
        else if (rec.code == QLatin1String("icvp"))
            target = &r.icvp;
        else if (rec.code == QLatin1String("glvp"))
            target = &r.glvp;
        if (!target || rec.type != QLatin1String("blob"))
            continue;
        *target = decodeBlob(rec.value);
        if (!target->isEmpty())
            r.found = true;
    }
    return r;
}

FolderRecords mergeRecords(const FolderRecords& primary, const FolderRecords& secondary) {
    FolderRecords r = primary;
    if (!r.style)
        r.style = secondary.style;
    if (r.lsvC.isEmpty())
        r.lsvC = secondary.lsvC;
    if (r.lsvp.isEmpty())
        r.lsvp = secondary.lsvp;
    if (r.lsvP.isEmpty())
        r.lsvP = secondary.lsvP;
    if (r.icvp.isEmpty())
        r.icvp = secondary.icvp;
    if (r.glvp.isEmpty())
        r.glvp = secondary.glvp;
    r.found = primary.found || secondary.found;
    return r;
}

std::optional<SortKey> keyForColumn(const QString& id) {
    if (id == QLatin1String("name"))
        return SortKey::Name;
    if (id == QLatin1String("dateModified"))
        return SortKey::DateModified;
    if (id == QLatin1String("dateCreated"))
        return SortKey::DateCreated;
    if (id == QLatin1String("dateAdded"))
        return SortKey::DateAdded;
    if (id == QLatin1String("dateLastOpened"))
        return SortKey::DateModified; // last-opened date is not available; closest match
    if (id == QLatin1String("size"))
        return SortKey::Size;
    if (id == QLatin1String("kind"))
        return SortKey::Kind;
    if (isNameOnlyColumn(id))
        return SortKey::Name;
    return std::nullopt;
}

std::optional<SortSpec> specForArrangeBy(const QString& a) {
    SortSpec s;
    if (a == QLatin1String("none") || a == QLatin1String("grid")) {
        s.key = SortKey::FinderIconPosition;
        s.ascending = true;
        return s;
    }
    if (a == QLatin1String("dateLastOpened")) {
        s.key = SortKey::DateModified;
        s.ascending = false;
        return s;
    }
    const auto key = keyForColumn(a);
    if (!key)
        return std::nullopt;
    s.key = *key;
    s.ascending = defaultAscending(*key);
    return s;
}

std::optional<SortSpec> specForListSettings(const QVariantMap& settings) {
    const QString column = settings.value(QLatin1String("sortColumn")).toString();
    if (column.isEmpty())
        return std::nullopt;
    const auto key = keyForColumn(column);
    if (!key)
        return std::nullopt;
    SortSpec s;
    s.key = *key;
    s.ascending = defaultAscending(*key);
    const QVariant columns = settings.value(QLatin1String("columns"));
    if (columns.typeId() == QMetaType::QVariantList) {
        // lsvC / ExtendedListViewSettingsV2: [{identifier, ascending, ...}, ...]
        for (const QVariant& c : columns.toList()) {
            const QVariantMap m = c.toMap();
            if (m.value(QLatin1String("identifier")).toString() == column) {
                if (const auto asc = boolValue(m.value(QLatin1String("ascending"))))
                    s.ascending = *asc;
                break;
            }
        }
    } else if (columns.typeId() == QMetaType::QVariantMap) {
        // lsvp / ListViewSettings: {identifier: {ascending, index, ...}, ...}
        const QVariantMap m = columns.toMap().value(column).toMap();
        if (const auto asc = boolValue(m.value(QLatin1String("ascending"))))
            s.ascending = *asc;
    }
    if (isNameOnlyColumn(column))
        s.ascending = true; // unsupported column -> plain name order
    return s;
}

std::optional<SortSpec> specForColumnArrangeBy(const QString& code) {
    SortSpec s;
    if (code == QLatin1String("dnam") || code == QLatin1String("none") || code.isEmpty())
        s.key = SortKey::Name;
    else if (code == QLatin1String("kipl"))
        s.key = SortKey::Kind;
    else if (code == QLatin1String("modd"))
        s.key = SortKey::DateModified;
    else if (code == QLatin1String("ascd"))
        s.key = SortKey::DateCreated;
    else if (code == QLatin1String("logs") || code == QLatin1String("phys"))
        s.key = SortKey::Size;
    else if (code == QLatin1String("dadd"))
        s.key = SortKey::DateAdded;
    else if (code == QLatin1String("labl"))
        s.key = SortKey::Name;
    else
        return std::nullopt;
    s.ascending = defaultAscending(s.key);
    return s;
}

namespace {

std::optional<SortSpec> globalListSpec(const QVariantMap& settings) {
    if (auto s = specForListSettings(mapValue(settings, "ExtendedListViewSettingsV2")))
        return s;
    if (auto s = specForListSettings(mapValue(settings, "ExtendedListViewSettings")))
        return s;
    return specForListSettings(mapValue(settings, "ListViewSettings"));
}

std::optional<SortSpec> globalSpec(const GlobalContext& g, ViewStyle style) {
    const QVariantMap& prefs = g.finderPrefs;
    QVector<QVariantMap> sources;
    if (!g.specialKey.isEmpty())
        sources.append(mapValue(prefs, g.specialKey.toLatin1().constData()));
    sources.append(mapValue(prefs, "StandardViewSettings"));
    for (const QVariantMap& settings : sources) {
        if (settings.isEmpty())
            continue;
        std::optional<SortSpec> s;
        switch (style) {
        case ViewStyle::List:
            s = globalListSpec(settings);
            break;
        case ViewStyle::Icon:
        case ViewStyle::Unknown:
            s = specForArrangeBy(
                mapValue(settings, "IconViewSettings").value(QLatin1String("arrangeBy")).toString());
            break;
        case ViewStyle::Gallery:
            s = specForArrangeBy(
                mapValue(settings, "GalleryViewSettings").value(QLatin1String("arrangeBy")).toString());
            if (s && s->key == SortKey::FinderIconPosition) // gallery "none" = by name
                s = SortSpec{SortKey::Name, true, SortSource::FinderGlobal};
            break;
        case ViewStyle::Column:
            break;
        }
        if (s)
            return s;
    }
    if (style == ViewStyle::Column) {
        const QVariantMap col = mapValue(mapValue(prefs, "StandardViewOptions"), "ColumnViewOptions");
        if (col.contains(QLatin1String("ArrangeBy")))
            return specForColumnArrangeBy(col.value(QLatin1String("ArrangeBy")).toString());
    }
    return std::nullopt;
}

} // namespace

SortSpec resolve(const FolderRecords& folder, const GlobalContext& global) {
    // Desktop is always shown as an icon view.
    const bool desktop = global.specialKey == QLatin1String("DesktopViewSettings");
    ViewStyle style = ViewStyle::Unknown;
    if (folder.style)
        style = *folder.style;
    else if (desktop)
        style = ViewStyle::Icon;
    else
        style = viewStyleFromCode(global.finderPrefs.value(QLatin1String("FXPreferredViewStyle")).toString());
    if (style == ViewStyle::Unknown)
        style = ViewStyle::Icon; // Finder's factory default

    // Tier 1: per-folder settings for the effective view style.
    std::optional<SortSpec> tier1;
    switch (style) {
    case ViewStyle::List:
        for (const QVariantMap* m : {&folder.lsvC, &folder.lsvp, &folder.lsvP}) {
            if ((tier1 = specForListSettings(*m)))
                break;
        }
        break;
    case ViewStyle::Icon:
    case ViewStyle::Unknown:
        tier1 = specForArrangeBy(folder.icvp.value(QLatin1String("arrangeBy")).toString());
        break;
    case ViewStyle::Gallery:
        tier1 = specForArrangeBy(folder.glvp.value(QLatin1String("arrangeBy")).toString());
        if (tier1 && tier1->key == SortKey::FinderIconPosition)
            tier1 = SortSpec{SortKey::Name, true, SortSource::FinderDsStore};
        break;
    case ViewStyle::Column:
        break; // column view has no per-folder sort settings
    }
    if (tier1)
        return *withSource(tier1, SortSource::FinderDsStore);

    // Tier 2: global defaults for that view style.
    if (auto s = globalSpec(global, style))
        return *withSource(s, SortSource::FinderGlobal);

    // Tier 4.
    return SortSpec{SortKey::Name, true, SortSource::Fallback};
}

FolderRecords readFolderRecords(const QString& folderPath) {
    const QString folder = QDir::cleanPath(folderPath);
    const QFileInfo fi(folder);
    FolderRecords fromParent;
    if (!fi.isRoot()) {
        if (auto recs = dsstore::readFile(fi.absolutePath() + QLatin1String("/.DS_Store")))
            fromParent = extractFolderRecords(*recs, fi.fileName());
    }
    FolderRecords own;
    if (auto recs = dsstore::readFile(folder + QLatin1String("/.DS_Store")))
        own = extractFolderRecords(*recs, QStringLiteral("."));
    return mergeRecords(fromParent, own);
}

QHash<QString, QPointF> iconPositions(const QVector<DsStoreRecord>& records) {
    QHash<QString, QPointF> out;
    for (const DsStoreRecord& rec : records) {
        if (rec.code != QLatin1String("Iloc") || rec.fileName == QLatin1String("."))
            continue;
        const QByteArray b = rec.value.toByteArray();
        if (b.size() < 8)
            continue;
        auto be32 = [&](int i) {
            const auto* u = reinterpret_cast<const uchar*>(b.constData()) + i;
            return qint32((quint32(u[0]) << 24) | (quint32(u[1]) << 16) | (quint32(u[2]) << 8) |
                          quint32(u[3]));
        };
        out.insert(rec.fileName, QPointF(be32(0), be32(4)));
    }
    return out;
}

QHash<QString, QPointF> readIconPositions(const QString& folderPath) {
    if (auto recs = dsstore::readFile(QDir::cleanPath(folderPath) + QLatin1String("/.DS_Store")))
        return iconPositions(*recs);
    return {};
}

std::optional<SortSpec> parseLiveQueryOutput(const QString& output, const QString& folderPath) {
    const QStringList lines = output.trimmed().split(QLatin1Char('\n'));
    if (lines.size() < 2)
        return std::nullopt;
    QString target = lines[0].trimmed();
    if (target.isEmpty() || cleanDir(target) != cleanDir(folderPath))
        return std::nullopt;
    const QString view = lines[1].trimmed();
    const QString what = lines.value(2).trimmed();
    if (view == QLatin1String("list")) {
        static const QHash<QString, SortKey> columns = {
            {QStringLiteral("name column"), SortKey::Name},
            {QStringLiteral("modification date column"), SortKey::DateModified},
            {QStringLiteral("creation date column"), SortKey::DateCreated},
            {QStringLiteral("date added column"), SortKey::DateAdded},
            {QStringLiteral("size column"), SortKey::Size},
            {QStringLiteral("kind column"), SortKey::Kind},
            {QStringLiteral("label column"), SortKey::Name},
            {QStringLiteral("version column"), SortKey::Name},
            {QStringLiteral("comment column"), SortKey::Name},
        };
        const auto it = columns.find(what);
        if (it == columns.end())
            return std::nullopt;
        const QString dir = lines.value(3).trimmed();
        // "normal" is the column's natural direction (dates/sizes: newest/largest first).
        bool asc = defaultAscending(*it);
        if (dir == QLatin1String("reversed"))
            asc = !asc;
        else if (dir != QLatin1String("normal"))
            return std::nullopt;
        if (it.key() == QLatin1String("label column") || it.key() == QLatin1String("version column") ||
            it.key() == QLatin1String("comment column"))
            asc = true;
        return SortSpec{*it, asc, SortSource::FinderLive};
    }
    if (view == QLatin1String("icon")) {
        static const QHash<QString, QString> arrangements = {
            {QStringLiteral("not arranged"), QStringLiteral("none")},
            {QStringLiteral("snap to grid"), QStringLiteral("grid")},
            {QStringLiteral("arranged by name"), QStringLiteral("name")},
            {QStringLiteral("arranged by modification date"), QStringLiteral("dateModified")},
            {QStringLiteral("arranged by creation date"), QStringLiteral("dateCreated")},
            {QStringLiteral("arranged by date added"), QStringLiteral("dateAdded")},
            {QStringLiteral("arranged by size"), QStringLiteral("size")},
            {QStringLiteral("arranged by kind"), QStringLiteral("kind")},
            {QStringLiteral("arranged by label"), QStringLiteral("label")},
        };
        const auto it = arrangements.find(what);
        if (it == arrangements.end())
            return std::nullopt;
        return withSource(specForArrangeBy(*it), SortSource::FinderLive);
    }
    return std::nullopt; // column/gallery view: not queryable, fall through
}

} // namespace lf::finder
