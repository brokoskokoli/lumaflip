// SPDX-License-Identifier: GPL-3.0-or-later
// Stable metadata keys and their German labels (F-INFO-02), see Decoder.h.

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
    {"shot", QT_TRANSLATE_NOOP("lf::Metadata", "Aufnahme")},
    {"gps", QT_TRANSLATE_NOOP("lf::Metadata", "GPS")},
};

constexpr Label kRows[] = {
    {"file.format", QT_TRANSLATE_NOOP("lf::Metadata", "Format")},
    {"file.dimensions", QT_TRANSLATE_NOOP("lf::Metadata", "Pixelmaße")},
    {"file.colorModel", QT_TRANSLATE_NOOP("lf::Metadata", "Farbraum")},
    {"file.profile", QT_TRANSLATE_NOOP("lf::Metadata", "Profil")},
    {"file.bitDepth", QT_TRANSLATE_NOOP("lf::Metadata", "Bittiefe")},
    {"file.hdr", QT_TRANSLATE_NOOP("lf::Metadata", "HDR")},
    {"file.alpha", QT_TRANSLATE_NOOP("lf::Metadata", "Transparenz")},
    {"file.frames", QT_TRANSLATE_NOOP("lf::Metadata", "Einzelbilder")},
    {"file.pages", QT_TRANSLATE_NOOP("lf::Metadata", "Seiten")},
    {"file.orientation", QT_TRANSLATE_NOOP("lf::Metadata", "Ausrichtung")},
    {"file.rating", QT_TRANSLATE_NOOP("lf::Metadata", "Bewertung")},
    {"camera.make", QT_TRANSLATE_NOOP("lf::Metadata", "Hersteller")},
    {"camera.model", QT_TRANSLATE_NOOP("lf::Metadata", "Modell")},
    {"camera.lens", QT_TRANSLATE_NOOP("lf::Metadata", "Objektiv")},
    {"shot.focalLength", QT_TRANSLATE_NOOP("lf::Metadata", "Brennweite")},
    {"shot.aperture", QT_TRANSLATE_NOOP("lf::Metadata", "Blende")},
    {"shot.exposureTime", QT_TRANSLATE_NOOP("lf::Metadata", "Belichtungszeit")},
    {"shot.iso", QT_TRANSLATE_NOOP("lf::Metadata", "ISO")},
    {"shot.exposureBias", QT_TRANSLATE_NOOP("lf::Metadata", "Belichtungskorrektur")},
    {"shot.flash", QT_TRANSLATE_NOOP("lf::Metadata", "Blitz")},
    {"shot.captured", QT_TRANSLATE_NOOP("lf::Metadata", "Aufnahmedatum")},
    {"gps.latitude", QT_TRANSLATE_NOOP("lf::Metadata", "Breite")},
    {"gps.longitude", QT_TRANSLATE_NOOP("lf::Metadata", "Länge")},
    {"gps.altitude", QT_TRANSLATE_NOOP("lf::Metadata", "Höhe")},
};

template <size_t N> QString lookup(const Label (&table)[N], const QString& key) {
    for (const Label& l : table)
        if (key == QLatin1StringView(l.key))
            return QCoreApplication::translate("lf::Metadata", l.label);
    return {};
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

void addMetadata(Metadata& md, const QString& key, const QVariant& value) {
    const QString label = metadataLabel(key);
    const QString groupLabel = metadataGroupLabel(key.section(QLatin1Char('.'), 0, 0));
    Q_ASSERT_X(!label.isEmpty() && !groupLabel.isEmpty(), "addMetadata", "unknown metadata key");
    if (label.isEmpty() || groupLabel.isEmpty())
        return;
    QVariantMap group = md.groups.value(groupLabel).toMap();
    group.insert(label, value);
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
