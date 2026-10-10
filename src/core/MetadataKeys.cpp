// SPDX-License-Identifier: GPL-3.0-or-later
// Stable metadata keys and their German labels (F-INFO-02, translatable: F-I18N-01), see Decoder.h.

#include "Decoder.h"

#include <QCoreApplication>

namespace lf {

namespace {

struct Label {
    const char* key;
    const char* label; // German source text, translated in context "lf::Metadata"
};

// Groups and rows in display order. The only place that names them.
constexpr Label kGroups[] = {
    {"file", QT_TRANSLATE_NOOP("lf::Metadata", "Datei")},
    {"camera", QT_TRANSLATE_NOOP("lf::Metadata", "Kamera")},
    //: group: exposure/shooting data
    {"shot", QT_TRANSLATE_NOOP("lf::Metadata", "Aufnahme")},
    {"gps", QT_TRANSLATE_NOOP("lf::Metadata", "GPS")},
};

constexpr Label kRows[] = {
    {"file.format", QT_TRANSLATE_NOOP("lf::Metadata", "Format")},
    //: image size in pixels (width × height)
    {"file.dimensions", QT_TRANSLATE_NOOP("lf::Metadata", "Pixelmaße")},
    {"file.colorModel", QT_TRANSLATE_NOOP("lf::Metadata", "Farbraum")},
    {"file.profile", QT_TRANSLATE_NOOP("lf::Metadata", "Profil")},
    {"file.bitDepth", QT_TRANSLATE_NOOP("lf::Metadata", "Bittiefe")},
    {"file.hdr", QT_TRANSLATE_NOOP("lf::Metadata", "HDR")},
    {"file.alpha", QT_TRANSLATE_NOOP("lf::Metadata", "Transparenz")},
    //: number of animation frames
    {"file.frames", QT_TRANSLATE_NOOP("lf::Metadata", "Einzelbilder")},
    {"file.pages", QT_TRANSLATE_NOOP("lf::Metadata", "Seiten")},
    {"file.orientation", QT_TRANSLATE_NOOP("lf::Metadata", "Ausrichtung")},
    //: star rating
    {"file.rating", QT_TRANSLATE_NOOP("lf::Metadata", "Bewertung")},
    {"camera.make", QT_TRANSLATE_NOOP("lf::Metadata", "Hersteller")},
    {"camera.model", QT_TRANSLATE_NOOP("lf::Metadata", "Modell")},
    {"camera.lens", QT_TRANSLATE_NOOP("lf::Metadata", "Objektiv")},
    {"shot.focalLength", QT_TRANSLATE_NOOP("lf::Metadata", "Brennweite")},
    //: aperture (f-number)
    {"shot.aperture", QT_TRANSLATE_NOOP("lf::Metadata", "Blende")},
    {"shot.exposureTime", QT_TRANSLATE_NOOP("lf::Metadata", "Belichtungszeit")},
    {"shot.iso", QT_TRANSLATE_NOOP("lf::Metadata", "ISO")},
    {"shot.exposureBias", QT_TRANSLATE_NOOP("lf::Metadata", "Belichtungskorrektur")},
    {"shot.flash", QT_TRANSLATE_NOOP("lf::Metadata", "Blitz")},
    {"shot.captured", QT_TRANSLATE_NOOP("lf::Metadata", "Aufnahmedatum")},
    //: GPS latitude
    {"gps.latitude", QT_TRANSLATE_NOOP("lf::Metadata", "Breite")},
    //: GPS longitude
    {"gps.longitude", QT_TRANSLATE_NOOP("lf::Metadata", "Länge")},
    //: GPS altitude
    {"gps.altitude", QT_TRANSLATE_NOOP("lf::Metadata", "Höhe")},
};

template <size_t N> const char* source(const Label (&table)[N], const QString& key) {
    for (const Label& l : table)
        if (key == QLatin1StringView(l.key))
            return l.label;
    return nullptr;
}

template <size_t N> QString lookup(const Label (&table)[N], const QString& key) {
    const char* s = source(table, key);
    return s ? QCoreApplication::translate("lf::Metadata", s) : QString();
}

} // namespace

QString metadataLabel(const QString& key) {
    return lookup(kRows, key);
}

QString metadataGroupLabel(const QString& group) {
    return lookup(kGroups, group);
}

QStringList metadataKeys() {
    QStringList keys;
    for (const Label& l : kRows)
        keys << QString::fromLatin1(l.key);
    return keys;
}

QString translateMetadataLabel(const QString& germanLabel) {
    return QCoreApplication::translate("lf::Metadata", germanLabel.toUtf8().constData());
}

void addMetadata(Metadata& md, const QString& key, const QVariant& value) {
    const char* rowSource = source(kRows, key);
    const char* groupSource = source(kGroups, key.section(QLatin1Char('.'), 0, 0));
    Q_ASSERT_X(rowSource && groupSource, "addMetadata", "unknown metadata key");
    if (!rowSource || !groupSource)
        return;
    // `groups` uses the German labels (stable, also when the UI language changes later).
    const QString groupLabel = QString::fromUtf8(groupSource);
    const QString label = metadataLabel(key);
    QVariantMap group = md.groups.value(groupLabel).toMap();
    group.insert(QString::fromUtf8(rowSource), value);
    md.groups.insert(groupLabel, group);
    for (MetadataEntry& e : md.entries) {
        if (e.key == key) {
            e.value = value;
            return;
        }
    }
    md.entries.append({key, label, value});
}

} // namespace lf
