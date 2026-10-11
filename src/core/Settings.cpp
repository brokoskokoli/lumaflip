// SPDX-License-Identifier: GPL-3.0-or-later
#include "Settings.h"

#include "Language.h"

#include "CustomActions.h"
#include "InputBindings.h"
#include "Platform.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>

#include <atomic>
#include <memory>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace lf {

namespace {

enum class Type { Bool, Int, String, KeyList };

struct KeyInfo {
    Type type = Type::String;
    QVariant def;
    QStringList allowed; // String: allowed values (empty = any)
    bool colorAllowed = false;
    int min = 0, max = 0;                   // Int
    QVariant (*dynamicDefault)() = nullptr; // default that follows a system setting
};

// F-WIN-06: the default of "restore windows" follows the system setting "Fenster beim
// Beenden schließen" (NSQuitAlwaysKeepsWindows); tests can pin it.
std::atomic<int> g_restoreDefaultOverride{-1}; // -1: system
QVariant restoreWindowsDefault() {
    const int o = g_restoreDefaultOverride.load();
    return o >= 0 ? o == 1 : systemRestoresWindows();
}

using Registry = QMap<QString, KeyInfo>; // sorted

const Registry& registry() {
    static const Registry reg = [] {
        Registry r;
        auto b = [&r](const char* k, bool d) {
            r.insert(QString::fromLatin1(k), {Type::Bool, d, {}, false, 0, 0});
        };
        auto i = [&r](const char* k, int d, int lo, int hi) {
            r.insert(QString::fromLatin1(k), {Type::Int, d, {}, false, lo, hi});
        };
        auto s = [&r](const char* k, const char* d, QStringList allowed, bool color = false) {
            r.insert(QString::fromLatin1(k), {Type::String, QString::fromLatin1(d), allowed, color, 0, 0});
        };
        s(key::OpenBehavior, "newWindow", {"newWindow", "replaceFront", "newUnlessOpen"});
        s(key::Language, kSystemLanguage,
          QStringList{QString::fromLatin1(kSystemLanguage)} + uiLanguageCodes());
        b(key::FitWindowToImage, false);
        b(key::RestoreWindows, false);
        r[QString::fromLatin1(key::RestoreWindows)].dynamicDefault = &restoreWindowsDefault;
        b(key::TitleShowFolder, false);
        b(key::ConfirmTrash, false);
        s(key::EditorApp, "", {}); // F-FILE-05
        b(key::AllowTabs, false);  // F-WIN-05
        // F-VIEW-14
        s(key::TitleBar, "autoHide", {"autoHide", "always"});
        s(key::Background, "dark", {"dark", "light", "system", "checker"}, true);
        s(key::ZoomMode, "fit", {"fit", "fitUpscale", "actual", "fillWidth", "fillHeight", "fill"});
        s(key::UpscaleFilter, "smooth", {"smooth", "nearest"});
        s(key::ActualSizeUnit, "devicePixels", {"devicePixels", "points"});
        b(key::ShowStatusBar, true);
        b(key::ShowInfoPanel, false);
        b(key::ShowFilmstrip, false);
        s(key::LoupeSize, "medium", {"small", "medium", "large"}); // F-VIEW-11
        s(key::LoupeZoom, "200", {"100", "200"});
        i(key::GridThumbSize, 160, 80, 400);         // F-GRID-02
        s(key::CoupleBy, "name", {"name", "index"}); // F-WIN-07
        b(key::HdrEnabled, true);                    // F-COL-04
        s(key::WrapMode, "stop", {"stop", "wrap", "nextFolder"});
        b(key::FollowFinder, true);
        b(key::FinderLiveQuery, false);
        s(key::ManualSortKey, "name",
          {"name", "dateModified", "dateCreated", "dateAdded", "dateCaptured", "size", "kind", "pixelCount",
           "rating"});
        b(key::ManualSortAscending, true);
        i(key::MemoryBudgetMB, 0, 0, 65536);
        i(key::PrefetchAhead, 3, 0, 20);
        i(key::PrefetchBehind, 1, 0, 20);
        b(key::RawEmbeddedOnly, false);
        b(key::PairRawJpeg, false); // F-FMT-06
        b(key::RatingStoreSidecar, true);
        b(key::RatingStoreEmbedded, false);
        b(key::RatingStoreFinderTags, false);
        // Culling and quick move (M3)
        r.insert(QString::fromLatin1(key::RatingTagStarSymbol),
                 {Type::String, QString(QChar(0x2605)), {}, false, 0, 0}); // "★"
        s(key::RatingTagPick, "Pick", {});
        s(key::RatingTagReject, "Reject", {});
        i(key::RatingTagStarColor, 0, 0, 7);
        i(key::RatingTagPickColor, 2, 0, 7);
        i(key::RatingTagRejectColor, 6, 0, 7);
        b(key::RatingKeepModDate, true);
        b(key::CullAutoAdvance, false);
        b(key::CullLockZoom, true);
        b(key::CullShowHelp, true);
        for (int n = 1; n <= 9; ++n)
            r.insert(QString::fromLatin1(key::QuickTargetPrefix) + QString::number(n),
                     {Type::String, QString(), {}, false, 0, 0});
        // Slideshow (F-SLIDE-01/02) and saving the rotation (F-EDIT-02)
        i(key::SlideshowIntervalSec, 3, 1, 60);
        b(key::SlideshowRepeat, true);
        b(key::SlideshowShuffle, false);
        b(key::SlideshowCrossfade, true);
        s(key::RotationSaveStrategy, "exif", {"exif", "lossless"});
        b(key::KeepModificationDate, false);
        // M4 (F-OPEN-10, F-MAC-06, F-FILE-09/10)
        b(key::ArchiveRightToLeft, false);
        b(key::LiveTextEnabled, true);
        b(key::WallpaperAllScreens, false);
        s(key::CustomActions, "[]", {});
        i(key::CustomActionTimeoutSec, 60, 1, 3600);
        // M4 (F-EDIT-03/04, F-INFO-07)
        s(key::CropAspect, "free", {"free", "original", "1:1", "4:3", "3:2", "16:9"});
        b(key::CropPortrait, false);
        b(key::CropGrid, true);
        s(key::ExportFormat, "jpeg", {"jpeg", "heic", "avif", "jxl", "png", "tiff"});
        i(key::ExportQuality, 90, 1, 100);
        i(key::ExportMaxEdge, 0, 0, 65535);
        s(key::ExportMetadata, "all", {"all", "noGps", "none"});
        s(key::ExportColor, "keep", {"keep", "srgb", "p3"});
        s(key::ExportHdr, "keep", {"keep", "sdr"});
        s(key::ExportPattern, "{name}-export", {});
        s(key::ExportFolder, "", {});
        b(key::OverlayClipping, false);
        b(key::OverlayPeaking, false);
        b(key::OverlayAfPoints, false);
        i(key::OverlayClipHighPercent, 98, 50, 100);
        i(key::OverlayClipLowPercent, 2, 0, 50);
        s(key::OverlayClipHdrReference, "headroom", {"headroom", "sdr"});
        i(key::OverlayPeakingSensitivity, 50, 0, 100);
        s(key::OverlayPeakingColor, "#ff2020", {"#ff2020"}, true);
        // Input (4.14)
        b(key::WheelInverted, false);
        i(key::WheelStepsPerImage, 1, 1, 5);
        i(key::WheelMinIntervalMs, 120, 0, 300);
        i(key::ZoomStepPercent, 15, 5, 50);
        i(key::JumpSize, 10, 2, 100);
        for (const auto& slot : InputBindings::inputSlots()) {
            if (slot.id == QLatin1String("endOfCollection"))
                continue; // stored as key::WrapMode
            r.insert(QString::fromLatin1(key::InputPrefix) + slot.id,
                     {Type::String, slot.defaultChoice, slot.choices, false, 0, 0});
        }
        for (Action a : InputBindings::allActions()) {
            QStringList d;
            for (const auto& seq : InputBindings::defaultShortcuts(a))
                d << seq.toString(QKeySequence::PortableText);
            r.insert(QString::fromLatin1(key::ShortcutPrefix) + actionId(a),
                     {Type::KeyList, d, {}, false, 0, 0});
        }
        return r;
    }();
    return reg;
}

const KeyInfo* findKey(const QString& k) {
    const Registry& r = registry();
    auto it = r.constFind(k);
    return it == r.constEnd() ? nullptr : &*it;
}

bool isColor(const QString& s) {
    static const QRegularExpression re(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    return re.match(s).hasMatch();
}

bool validKeySequence(const QString& s) {
    const QKeySequence seq = QKeySequence::fromString(s, QKeySequence::PortableText);
    return !seq.isEmpty() && seq[0].key() != Qt::Key_unknown;
}

// Normalizes `v` to the type of `info`; returns invalid QVariant if not acceptable.
QVariant normalize(const KeyInfo& info, const QVariant& v) {
    if (!v.isValid())
        return {};
    switch (info.type) {
    case Type::Bool: {
        if (v.typeId() == QMetaType::Bool)
            return v;
        const QString s = v.toString().toLower();
        if (s == QLatin1String("true") || s == QLatin1String("1"))
            return true;
        if (s == QLatin1String("false") || s == QLatin1String("0"))
            return false;
        return {};
    }
    case Type::Int: {
        bool ok = false;
        const int n = v.toInt(&ok);
        if (!ok || n < info.min || n > info.max)
            return {};
        return n;
    }
    case Type::String: {
        if (!v.canConvert<QString>())
            return {};
        const QString s = v.toString();
        if (!info.allowed.isEmpty() && !info.allowed.contains(s) && !(info.colorAllowed && isColor(s)))
            return {};
        return s;
    }
    case Type::KeyList: {
        QStringList list;
        if (v.typeId() == QMetaType::QString) {
            // INI stores single-element/empty lists as plain strings.
            if (!v.toString().isEmpty())
                list << v.toString();
        } else if (v.canConvert<QStringList>()) {
            list = v.toStringList();
        } else {
            return {};
        }
        for (const auto& s : list)
            if (!validKeySequence(s))
                return {};
        return list;
    }
    }
    return {};
}

QVariant toStorage(const KeyInfo* info, const QVariant& v) {
    // Empty lists are written as "" so every backend reads them back as "no shortcut".
    if (info && info->type == Type::KeyList && v.toStringList().isEmpty())
        return QString();
    return v;
}

QString typeName(Type t) {
    switch (t) {
    case Type::Bool:
        return QStringLiteral("bool");
    case Type::Int:
        return QStringLiteral("int");
    case Type::String:
        return QStringLiteral("string");
    case Type::KeyList:
        return QStringLiteral("list of shortcuts");
    }
    return {};
}

} // namespace

namespace {
// io.github.brokoskokoli.lumaflip (QSettings on macOS reverses the domain).
constexpr auto kOrgDomain = "brokoskokoli.github.io";
constexpr auto kSettingsApp = "lumaflip";
constexpr auto kLegacySettingsApp = "Lumaflip"; // 0.1: QSettings() with app name "Lumaflip"
constexpr auto kAppName = "Lumaflip";
constexpr auto kMigratedKey = "meta/migratedFrom01";
} // namespace

struct Settings::Private {
    mutable QMutex mutex;
    std::unique_ptr<QSettings> qs;
    QString folderFile;
    mutable bool foldersLoaded = false;
    mutable QJsonObject folders;

    // Created lazily so the default (native) backend picks up the organization and
    // application names set in main(). Caller holds `mutex`.
    QSettings& settings() {
        if (!qs)
            qs = makeNativeSettings();
        return *qs;
    }
    static std::unique_ptr<QSettings> makeNativeSettings() {
        // Explicit identity, independent of QCoreApplication's names (SPEC 8.6).
        return std::make_unique<QSettings>(QSettings::NativeFormat, QSettings::UserScope,
                                           QString::fromLatin1(kOrgDomain),
                                           QString::fromLatin1(kSettingsApp));
    }
};

namespace {
QString defaultFolderFile() {
    return Settings::appDataPath() + QStringLiteral("/folders.json");
}
} // namespace

void Settings::configureApplication() {
    // No organization name: AppDataLocation is "Application Support/<app name>" (SPEC 8.6).
    QCoreApplication::setOrganizationName(QString());
    QCoreApplication::setOrganizationDomain(QString::fromLatin1(kOrgDomain));
    QCoreApplication::setApplicationName(QString::fromLatin1(kAppName));
    QGuiApplication::setApplicationDisplayName(QString::fromLatin1(kAppName));
#ifndef Q_OS_MACOS
    // Wayland app id / X11 window class: matches the installed desktop entry
    // (packaging/linux), so the shell shows the right name and icon.
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.brokoskokoli.lumaflip"));
#endif
}

QString Settings::nativeSettingsPath() {
    return Private::makeNativeSettings()->fileName();
}

QString Settings::appDataPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

int Settings::migrateValues(QSettings& from, QSettings& to) {
    if (to.value(QString::fromLatin1(kMigratedKey)).toBool())
        return 0;
    int copied = 0;
    if (from.fileName() != to.fileName()) {
        const QStringList keys = from.allKeys();
        for (const QString& k : keys) {
            if (to.contains(k))
                continue;
            to.setValue(k, from.value(k));
            ++copied;
        }
    }
    // The old file is left alone: on case-insensitive volumes it may be the same file.
    to.setValue(QString::fromLatin1(kMigratedKey), true);
    to.sync();
    return copied;
}

bool Settings::migrateFile(const QString& oldPath, const QString& newPath) {
    if (oldPath == newPath || QFileInfo::exists(newPath) || !QFileInfo::exists(oldPath))
        return false;
    QDir().mkpath(QFileInfo(newPath).absolutePath());
    return QFile::copy(oldPath, newPath);
}

int Settings::migrateLegacyLocations() {
    int copied = 0;
    {
        // Every launch after the first: nothing to read from the old locations (P-01).
        Settings& s = instance();
        QMutexLocker lock(&s.d->mutex);
        if (s.d->settings().value(QString::fromLatin1(kMigratedKey)).toBool())
            return 0;
    }
    {
        QSettings legacy(QSettings::NativeFormat, QSettings::UserScope, QString::fromLatin1(kOrgDomain),
                         QString::fromLatin1(kLegacySettingsApp));
        Settings& s = instance();
        QMutexLocker lock(&s.d->mutex);
        QSettings& current = s.d->settings();
        bool sameFile = legacy.fileName() == current.fileName();
#ifdef _PC_CASE_SENSITIVE
        // Default macOS volumes are case-insensitive: "…Lumaflip.plist" and
        // "…lumaflip.plist" are one file, there is nothing to copy (and writing through two
        // preference domains into one file must be avoided).
        if (!sameFile && legacy.fileName().compare(current.fileName(), Qt::CaseInsensitive) == 0) {
            const QByteArray dir = QFile::encodeName(QFileInfo(current.fileName()).absolutePath());
            sameFile = ::pathconf(dir.constData(), _PC_CASE_SENSITIVE) != 1;
        }
#endif
        if (!sameFile)
            copied = migrateValues(legacy, current);
    }
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString legacyDir =
        base + QLatin1Char('/') + QLatin1String(kAppName) + QLatin1Char('/') + QLatin1String(kAppName);
    if (migrateFile(legacyDir + QStringLiteral("/folders.json"), defaultFolderFile()))
        ++copied;
    return copied;
}

Settings::Settings(QObject* parent) : QObject(parent), d(new Private) {}

Settings& Settings::instance() {
    static Settings* s = [] {
        auto* st = new Settings; // intentionally leaked: lives until process exit
        if (auto* app = QCoreApplication::instance(); app && st->thread() != app->thread())
            st->moveToThread(app->thread());
        return st;
    }();
    return *s;
}

void Settings::useTestingBackend(const QString& iniPath) {
    Settings& s = instance();
    QMutexLocker lock(&s.d->mutex);
    s.d->qs = std::make_unique<QSettings>(iniPath, QSettings::IniFormat);
    s.d->folderFile = iniPath + QStringLiteral(".folders.json");
    s.d->foldersLoaded = false;
    s.d->folders = {};
}

QStringList Settings::knownKeys() {
    return registry().keys();
}

QVariant Settings::defaultValue(const QString& key) const {
    const KeyInfo* info = findKey(key);
    if (info && info->dynamicDefault)
        return info->dynamicDefault();
    return info ? info->def : QVariant();
}

void Settings::setSystemRestoreWindowsForTesting(int value) {
    g_restoreDefaultOverride = value < 0 ? -1 : (value ? 1 : 0);
}

QVariant Settings::value(const QString& key) const {
    QVariant raw;
    {
        QMutexLocker lock(&d->mutex);
        raw = d->settings().value(key);
    }
    const KeyInfo* info = findKey(key);
    if (!info)
        return raw; // keys owned by other modules (e.g. window geometry)
    QVariant v = normalize(*info, raw);
    if (v.isValid())
        return v;
    return info->dynamicDefault ? info->dynamicDefault() : info->def;
}

void Settings::setValue(const QString& key, const QVariant& value) {
    const KeyInfo* info = findKey(key);
    QVariant v = value;
    if (info) {
        v = normalize(*info, value);
        if (!v.isValid()) {
            qWarning("lf::Settings: invalid value for %s ignored", qPrintable(key));
            return;
        }
    }
    const QVariant old = this->value(key);
    {
        QMutexLocker lock(&d->mutex);
        d->settings().setValue(key, toStorage(info, v));
    }
    if (old != v)
        emit changed(key);
}

void Settings::resetToDefaults(const QString& prefix) {
    QStringList affected;
    QHash<QString, QVariant> before;
    for (const QString& k : registry().keys())
        if (k.startsWith(prefix))
            before.insert(k, value(k));
    {
        QMutexLocker lock(&d->mutex);
        const QStringList all = d->settings().allKeys();
        for (const QString& k : all) {
            if (k.startsWith(prefix)) {
                d->settings().remove(k);
                if (!findKey(k))
                    affected << k;
            }
        }
    }
    for (auto it = before.cbegin(); it != before.cend(); ++it)
        if (value(it.key()) != it.value())
            affected << it.key();
    affected.sort();
    for (const QString& k : affected)
        emit changed(k);
}

// ---- export / import (F-SET-03) ----

QByteArray Settings::exportData() const {
    QJsonObject values;
    for (auto it = registry().cbegin(); it != registry().cend(); ++it) {
        const QVariant v = value(it.key());
        if (it->type == Type::KeyList)
            values.insert(it.key(), QJsonArray::fromStringList(v.toStringList()));
        else
            values.insert(it.key(), QJsonValue::fromVariant(v));
    }
    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("lumaflip-settings"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("settings"), values);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool Settings::exportTo(const QString& filePath, QString* error) const {
    return writeExport(filePath, exportData(), error);
}

bool Settings::writeExport(const QString& filePath, const QByteArray& data, QString* error) {
    QSaveFile f(filePath);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = tr("Datei kann nicht geschrieben werden: %1").arg(f.errorString());
        return false;
    }
    f.write(data);
    if (!f.commit()) {
        if (error)
            *error = tr("Datei kann nicht geschrieben werden: %1").arg(f.errorString());
        return false;
    }
    return true;
}

std::optional<QByteArray> Settings::readImport(const QString& filePath, QString* error) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = tr("Datei kann nicht gelesen werden: %1").arg(f.errorString());
        return std::nullopt;
    }
    return f.readAll();
}

bool Settings::importFrom(const QString& filePath, QString* error) {
    const std::optional<QByteArray> data = readImport(filePath, error);
    return data && importData(*data, error);
}

bool Settings::importData(const QByteArray& json, QString* error) {
    auto fail = [error](const QString& msg) {
        if (error)
            *error = msg;
        return false;
    };
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError)
        return fail(tr("Keine gültige Einstellungsdatei (JSON-Fehler: %1).").arg(pe.errorString()));
    const QJsonObject root = doc.object();
    if (!doc.isObject() ||
        root.value(QStringLiteral("format")).toString() != QLatin1String("lumaflip-settings") ||
        !root.value(QStringLiteral("settings")).isObject())
        return fail(tr("Keine Lumaflip-Einstellungsdatei."));
    const QJsonObject values = root.value(QStringLiteral("settings")).toObject();

    QList<QPair<QString, QVariant>> accepted;
    QStringList problems;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        const KeyInfo* info = findKey(it.key());
        if (!info)
            continue; // unknown keys (newer/older versions) are ignored
        const QJsonValue jv = it.value();
        bool typeOk = false;
        switch (info->type) {
        case Type::Bool:
            typeOk = jv.isBool();
            break;
        case Type::Int:
            typeOk = jv.isDouble() && jv.toDouble() == double(jv.toInt());
            break;
        case Type::String:
            typeOk = jv.isString();
            break;
        case Type::KeyList: {
            typeOk = jv.isArray();
            for (const auto& e : jv.toArray())
                typeOk = typeOk && e.isString();
            break;
        }
        }
        const QVariant v = typeOk ? normalize(*info, jv.toVariant()) : QVariant();
        if (!v.isValid()) {
            problems << tr("%1: ungültiger Wert (erwartet: %2)").arg(it.key(), typeName(info->type));
            continue;
        }
        accepted.append({it.key(), v});
    }
    if (!problems.isEmpty())
        return fail(
            tr("Import abgebrochen, nichts wurde geändert:\n%1").arg(problems.join(QLatin1Char('\n'))));
    for (auto& [k, v] : accepted) {
        if (k == QLatin1String(key::CustomActions)) {
            // F-FILE-10: an imported command never counts as confirmed by the user here.
            v = customactions::toJson(customactions::withConfirmationFrom(
                customactions::fromJson(v.toString()),
                customactions::fromJson(value(key::CustomActions).toString())));
        }
        setValue(k, v);
    }
    return true;
}

// ---- per-folder values (F-SORT-06) ----

namespace {
QString folderHash(const QString& folderPath) {
    QFileInfo fi(folderPath);
    QString canonical = fi.canonicalFilePath();
    if (canonical.isEmpty())
        canonical = QDir::cleanPath(fi.absoluteFilePath());
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha256).toHex().left(32));
}
} // namespace

QVariant Settings::folderValue(const QString& folderPath, const QString& key) const {
    QMutexLocker lock(&d->mutex);
    if (d->folderFile.isEmpty())
        d->folderFile = defaultFolderFile();
    if (!d->foldersLoaded) {
        d->foldersLoaded = true;
        QFile f(d->folderFile);
        if (f.open(QIODevice::ReadOnly))
            d->folders = QJsonDocument::fromJson(f.readAll()).object();
    }
    const QJsonObject entry = d->folders.value(folderHash(folderPath)).toObject();
    const QJsonValue v = entry.value(QStringLiteral("values")).toObject().value(key);
    return v.isUndefined() ? QVariant() : v.toVariant();
}

void Settings::setFolderValue(const QString& folderPath, const QString& key, const QVariant& value) {
    (void)folderValue(folderPath, key); // ensure loaded
    QMutexLocker lock(&d->mutex);
    const QString h = folderHash(folderPath);
    QJsonObject entry = d->folders.value(h).toObject();
    QJsonObject values = entry.value(QStringLiteral("values")).toObject();
    if (value.isValid())
        values.insert(key, QJsonValue::fromVariant(value));
    else
        values.remove(key);
    if (values.isEmpty()) {
        d->folders.remove(h);
    } else {
        entry.insert(QStringLiteral("values"), values);
        d->folders.insert(h, entry);
    }
    QDir().mkpath(QFileInfo(d->folderFile).absolutePath());
    QSaveFile f(d->folderFile);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(d->folders).toJson(QJsonDocument::Compact));
        if (!f.commit())
            qWarning("lf::Settings: cannot write %s", qPrintable(d->folderFile));
    } else {
        qWarning("lf::Settings: cannot write %s", qPrintable(d->folderFile));
    }
}

} // namespace lf
