// SPDX-License-Identifier: GPL-3.0-or-later
// Preferences section "Bewertung" (M3): where ratings are stored (F-CULL-02), Finder tag
// names and colors, culling options (F-CULL-03/06) and the quick move targets (F-FILE-08).

#include "PreferencesExtras.h"
#include "core/QuickMove.h"
#include "core/Settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace lf::prefs {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("lf::PreferencesDialog", text);
}

Settings& st() {
    return Settings::instance();
}

QLabel* help(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() * 0.9);
    l->setFont(f);
    l->setForegroundRole(QPalette::PlaceholderText);
    return l;
}

// Keeps `fn` in sync with the setting `k` (now and on every change).
void follow(QWidget* w, const QString& k, std::function<void()> fn) {
    fn();
    QObject::connect(&st(), &Settings::changed, w, [k, fn](const QString& changed) {
        if (changed == k)
            fn();
    });
}

QCheckBox* check(const char* k, const QString& text, QWidget* parent) {
    auto* cb = new QCheckBox(text, parent);
    const QString key = QString::fromLatin1(k);
    cb->setObjectName(QStringLiteral("setting:") + key);
    QObject::connect(cb, &QCheckBox::toggled, cb, [key](bool on) { st().setValue(key, on); });
    follow(cb, key, [cb, key] {
        QSignalBlocker b(cb);
        cb->setChecked(st().value(key).toBool());
    });
    return cb;
}

QLineEdit* text(const char* k, QWidget* parent) {
    auto* e = new QLineEdit(parent);
    const QString key = QString::fromLatin1(k);
    e->setObjectName(QStringLiteral("setting:") + key);
    e->setMaximumWidth(160);
    QObject::connect(e, &QLineEdit::editingFinished, e, [e, key] {
        if (!e->text().trimmed().isEmpty())
            st().setValue(key, e->text().trimmed());
    });
    follow(e, key, [e, key] {
        QSignalBlocker b(e);
        e->setText(st().value(key).toString());
    });
    return e;
}

QComboBox* color(const char* k, QWidget* parent) {
    auto* c = new QComboBox(parent);
    const QString key = QString::fromLatin1(k);
    c->setObjectName(QStringLiteral("setting:") + key);
    // Finder label indices 0..7
    const QStringList names = {tr("Keine Farbe"), tr("Grau"), tr("Grün"),  tr("Lila"),
                               tr("Blau"),        tr("Gelb"), tr("Rot"),   tr("Orange")};
    for (int i = 0; i < names.size(); ++i)
        c->addItem(names.at(i), i);
    QObject::connect(c, &QComboBox::activated, c, [c, key](int i) { st().setValue(key, c->itemData(i)); });
    follow(c, key, [c, key] {
        QSignalBlocker b(c);
        c->setCurrentIndex(c->findData(st().value(key).toInt()));
    });
    return c;
}

QWidget* tagRow(const char* nameKey, const char* colorKey, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(text(nameKey, row));
    h->addWidget(color(colorKey, row));
    h->addStretch(1);
    return row;
}

QWidget* targetRow(int slot, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto* path = new QLineEdit(row);
    path->setReadOnly(true);
    path->setObjectName(QStringLiteral("quickTarget:%1").arg(slot));
    path->setPlaceholderText(tr("nicht festgelegt"));
    auto* choose = new QPushButton(tr("Wählen …"), row);
    choose->setObjectName(QStringLiteral("quickTargetChoose:%1").arg(slot));
    choose->setAutoDefault(false);
    auto* clear = new QPushButton(tr("Entfernen"), row);
    clear->setObjectName(QStringLiteral("quickTargetClear:%1").arg(slot));
    clear->setAutoDefault(false);
    h->addWidget(path, 1);
    h->addWidget(choose);
    h->addWidget(clear);
    const QString key = QString::fromLatin1(key::QuickTargetPrefix) + QString::number(slot);
    follow(row, key, [path, clear, slot] {
        const QString t = quickmove::target(slot);
        path->setText(QDir::toNativeSeparators(t));
        path->setToolTip(t);
        clear->setEnabled(!t.isEmpty());
    });
    QObject::connect(choose, &QPushButton::clicked, row, [row, slot] {
        const QString start = quickmove::target(slot);
        const QString dir = QFileDialog::getExistingDirectory(row, tr("Ziel %1 der Schnellablage").arg(slot), start);
        if (!dir.isEmpty())
            quickmove::setTarget(slot, dir);
    });
    QObject::connect(clear, &QPushButton::clicked, row, [slot] { quickmove::setTarget(slot, QString()); });
    return row;
}

} // namespace

QWidget* createRatingPage(QWidget* parent) {
    auto* w = new QWidget(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);

    // ---- F-CULL-02: stores ----
    auto* stores = new QGroupBox(tr("Bewertungen speichern"), w);
    auto* form = new QFormLayout(stores);
    form->addRow(check(key::RatingStoreSidecar, tr("XMP-Zusatzdatei neben dem Bild (Bild.xmp)"), stores));
    form->addRow(help(tr("Kompatibel mit Lightroom, Capture One und darktable: Sterne in xmp:Rating, "
                         "„Verwerfen“ als -1. Vorhandene Inhalte der Datei bleiben erhalten."),
                      stores));
    form->addRow(check(key::RatingStoreEmbedded, tr("Im Bild selbst (eingebettetes XMP)"), stores));
    form->addRow(help(tr("Nur JPEG, HEIC, TIFF, PNG und DNG; die Bildpixel werden nicht neu kodiert. RAW-Dateien "
                         "werden nie verändert – dafür wird die Zusatzdatei benutzt."),
                      stores));
    form->addRow(check(key::RatingKeepModDate, tr("Änderungsdatum beim Schreiben ins Bild beibehalten"), stores));
    form->addRow(check(key::RatingStoreFinderTags, tr("Finder-Tags"), stores));
    form->addRow(help(tr("Im Finder sichtbar und per Spotlight durchsuchbar. Sind mehrere Speicherorte gewählt, "
                         "werden alle beschrieben; beim Lesen gilt die Reihenfolge Zusatzdatei, Bild, Tags."),
                      stores));
    form->addRow(tr("Stern-Symbol:"), tagRow(key::RatingTagStarSymbol, key::RatingTagStarColor, stores));
    form->addRow(tr("Tag für Pick:"), tagRow(key::RatingTagPick, key::RatingTagPickColor, stores));
    form->addRow(tr("Tag für Verworfen:"), tagRow(key::RatingTagReject, key::RatingTagRejectColor, stores));
    form->addRow(help(tr("Drei Sterne werden zum Tag „★★★“ (Symbol dreimal)."), stores));
    v->addWidget(stores);

    // ---- F-CULL-03/06 ----
    auto* culling = new QGroupBox(tr("Aussortieren"), w);
    auto* cf = new QFormLayout(culling);
    cf->addRow(check(key::CullAutoAdvance, tr("Nach Bewertung automatisch weiter"), culling));
    cf->addRow(check(key::CullLockZoom, tr("Zoom im Aussortiermodus fixieren"), culling));
    cf->addRow(check(key::CullShowHelp, tr("Tastenhilfe im Aussortiermodus zeigen"), culling));
    cf->addRow(help(tr("Aussortiermodus: ⌘K oder K. Ziffern vergeben dort Sterne statt Zoomstufen; "
                       "„Bewertungstasten immer aktiv“ gibt es unter Maus & Tastatur."),
                    culling));
    v->addWidget(culling);

    // ---- F-FILE-08 ----
    auto* quick = new QGroupBox(tr("Schnellablage"), w);
    auto* qf = new QFormLayout(quick);
    qf->addRow(help(tr("M und dann 1–9 verschiebt das Bild in den Zielordner, C und dann 1–9 kopiert es. "
                       "M M bzw. C C wiederholt das letzte Ziel. ⌘Z macht es rückgängig."),
                    quick));
    for (int n = 1; n <= quickmove::kSlots; ++n)
        qf->addRow(QStringLiteral("%1").arg(n), targetRow(n, quick));
    v->addWidget(quick);
    return w;
}

QStringList ratingPageKeys() {
    QStringList keys{key::RatingStoreSidecar,   key::RatingStoreEmbedded, key::RatingStoreFinderTags,
                     key::RatingKeepModDate,    key::RatingTagStarSymbol, key::RatingTagPick,
                     key::RatingTagReject,      key::RatingTagStarColor,  key::RatingTagPickColor,
                     key::RatingTagRejectColor, key::CullAutoAdvance,     key::CullLockZoom,
                     key::CullShowHelp};
    for (int n = 1; n <= quickmove::kSlots; ++n)
        keys << QString::fromLatin1(key::QuickTargetPrefix) + QString::number(n);
    return keys;
}

} // namespace lf::prefs
