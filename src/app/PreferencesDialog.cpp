// SPDX-License-Identifier: GPL-3.0-or-later
#include "PreferencesDialog.h"

#include "core/Settings.h"

#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStyle>
#include <QTableWidget>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace lf {

namespace {

QPointer<PreferencesDialog> g_instance;

Settings& st() {
    return Settings::instance();
}

QString slotSettingsKey(const QString& slotId) {
    return slotId == QLatin1String("endOfCollection") ? QString::fromLatin1(key::WrapMode)
                                                      : QString::fromLatin1(key::InputPrefix) + slotId;
}

// Not implemented before version 1.0: shown, but disabled with this note.
QString laterNote() {
    return QCoreApplication::translate("lf::PreferencesDialog", "ab Version 1.0");
}

QLabel* helpLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() * 0.9);
    l->setFont(f);
    l->setForegroundRole(QPalette::PlaceholderText);
    return l;
}

// Actions that are not triggered by a free key shortcut (mouse-only or fixed slot keys).
bool editableAction(Action a) {
    switch (a) {
    case Action::DragOut:
    case Action::StartPan:
    case Action::ExitModeOrNothing:
    case Action::ExitModeOrCloseWindow:
        return false;
    default:
        return true;
    }
}

} // namespace

PreferencesDialog::PreferencesDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Einstellungen"));
    setModal(false);
    setSizeGripEnabled(true);
    resize(760, 640);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* toolbar = new QToolBar(this);
    toolbar->setObjectName(QStringLiteral("sectionToolbar"));
    toolbar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    toolbar->setMovable(false);
    layout->addWidget(toolbar);

    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack, 1);

    m_sectionActions = new QActionGroup(this);
    m_sectionActions->setExclusive(true);
    const QStyle::StandardPixmap icons[SectionCount] = {
        QStyle::SP_ComputerIcon,           QStyle::SP_TitleBarNormalButton, QStyle::SP_DesktopIcon,
        QStyle::SP_DialogHelpButton,       QStyle::SP_ArrowRight,           QStyle::SP_DialogApplyButton,
        QStyle::SP_FileDialogDetailedView, QStyle::SP_FileDialogInfoView};
    QWidget* (PreferencesDialog::* builders[SectionCount])() = {
        &PreferencesDialog::buildGeneral,    &PreferencesDialog::buildWindow,
        &PreferencesDialog::buildDisplay,    &PreferencesDialog::buildInput,
        &PreferencesDialog::buildNavigation, &PreferencesDialog::buildRating,
        &PreferencesDialog::buildFormats,    &PreferencesDialog::buildAdvanced};
    for (int i = 0; i < SectionCount; ++i) {
        // "&" would be taken as a mnemonic marker
        QString title = sectionTitle(i);
        QAction* act =
            toolbar->addAction(style()->standardIcon(icons[i]), title.replace('&', QStringLiteral("&&")));
        act->setCheckable(true);
        act->setData(i);
        m_sectionActions->addAction(act);
        connect(act, &QAction::triggered, this, [this, i] { showSection(i); });
        m_stack->addWidget(page((this->*builders[i])(), i));
    }

    m_messageLabel = new QLabel(this);
    m_messageLabel->setObjectName(QStringLiteral("messageLabel"));
    m_messageLabel->setContentsMargins(12, 4, 12, 8);
    m_messageLabel->hide();
    layout->addWidget(m_messageLabel);

    connect(&st(), &Settings::changed, this, &PreferencesDialog::onSettingChanged);
    showSection(General);
}

PreferencesDialog::~PreferencesDialog() = default;

void PreferencesDialog::showStatus(const QString& text) {
    m_messageLabel->setText(text);
    m_messageLabel->setVisible(!text.isEmpty());
}

QString PreferencesDialog::statusText() const {
    return m_messageLabel->isHidden() ? QString() : m_messageLabel->text();
}

bool PreferencesDialog::isChoiceAvailable(const QString& slotId, const QString& choiceId) {
    return InputBindings::isChoiceAvailable(slotId, choiceId);
}

PreferencesDialog* PreferencesDialog::showPreferences(QWidget* parent) {
    if (!g_instance)
        g_instance = new PreferencesDialog(parent);
    g_instance->show();
    g_instance->raise();
    g_instance->activateWindow();
    return g_instance;
}

PreferencesDialog* PreferencesDialog::existingInstance() {
    return g_instance;
}

QString PreferencesDialog::sectionTitle(int section) const {
    switch (section) {
    case General:
        return tr("Allgemein");
    case Window:
        return tr("Fenster");
    case Display:
        return tr("Anzeige");
    case Input:
        return tr("Maus & Tastatur");
    case Navigation:
        return tr("Navigation & Sortierung");
    case Rating:
        return tr("Bewertung");
    case Formats:
        return tr("Formate");
    case Advanced:
        return tr("Erweitert");
    default:
        return {};
    }
}

int PreferencesDialog::currentSection() const {
    return m_stack->currentIndex();
}

void PreferencesDialog::showSection(int section) {
    if (section < 0 || section >= SectionCount)
        return;
    m_stack->setCurrentIndex(section);
    const auto acts = m_sectionActions->actions();
    acts.at(section)->setChecked(true);
    setWindowTitle(sectionTitle(section));
}

QStringList PreferencesDialog::sectionKeys(int section) const {
    switch (section) {
    case General:
        return {key::OpenBehavior, key::RestoreWindows, key::ConfirmTrash, key::RotationSaveStrategy,
                key::KeepModificationDate};
    case Window:
        return {key::FitWindowToImage, key::TitleShowFolder, key::ShowStatusBar, key::ShowInfoPanel,
                key::ShowFilmstrip};
    case Display:
        return {
            key::Background,           key::ZoomMode,        key::UpscaleFilter,    key::ActualSizeUnit,
            key::SlideshowIntervalSec, key::SlideshowRepeat, key::SlideshowShuffle, key::SlideshowCrossfade};
    case Input:
        return {key::InputPrefix, key::WrapMode};
    case Navigation:
        return {key::WrapMode, key::FollowFinder, key::FinderLiveQuery, key::ManualSortKey,
                key::ManualSortAscending};
    case Rating:
        return {key::RatingStoreSidecar, key::RatingStoreEmbedded, key::RatingStoreFinderTags};
    case Formats:
        return {key::RawEmbeddedOnly};
    case Advanced:
        return {key::MemoryBudgetMB, key::PrefetchAhead, key::PrefetchBehind};
    default:
        return {};
    }
}

void PreferencesDialog::resetSection(int section) {
    for (const QString& k : sectionKeys(section))
        st().resetToDefaults(k);
    m_bindings.reload();
}

QWidget* PreferencesDialog::page(QWidget* content, int section) {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget;
    auto* v = new QVBoxLayout(inner);
    v->setContentsMargins(20, 16, 20, 16);
    v->addWidget(content);
    v->addStretch(1);
    auto* resetRow = new QHBoxLayout;
    resetRow->addStretch(1);
    auto* reset = new QPushButton(tr("Auf Standard zurücksetzen"), inner);
    reset->setObjectName(QStringLiteral("reset:%1").arg(section));
    reset->setAutoDefault(false);
    connect(reset, &QPushButton::clicked, this, [this, section] { resetSection(section); });
    resetRow->addWidget(reset);
    v->addLayout(resetRow);
    scroll->setWidget(inner);
    scroll->setObjectName(QStringLiteral("section:%1").arg(section));
    return scroll;
}

// ---- generic bindings ----

void PreferencesDialog::addRefresher(const QString& key, std::function<void()> fn) {
    fn();
    m_refreshers.insert(key, std::move(fn));
}

void PreferencesDialog::onSettingChanged(const QString& key) {
    if (key.startsWith(QLatin1String(key::InputPrefix)) || key == QLatin1String(key::WrapMode))
        m_bindings.reload();
    const auto fns = m_refreshers.values(key);
    for (const auto& fn : fns)
        fn();
    if (key.startsWith(QLatin1String(key::ShortcutPrefix))) {
        const Action a = actionFromId(key.mid(int(qstrlen(key::ShortcutPrefix))));
        if (auto it = m_rowOfAction.constFind(int(a)); it != m_rowOfAction.constEnd())
            refreshShortcutRow(*it);
        updateConflicts();
    }
}

namespace {
struct Binder {
    PreferencesDialog* dlg;
    std::function<void(const QString&, std::function<void()>)> addRefresher;

    QCheckBox* check(const QString& k, const QString& text, QWidget* parent) const {
        auto* cb = new QCheckBox(text, parent);
        cb->setObjectName(QStringLiteral("setting:") + k);
        QObject::connect(cb, &QCheckBox::toggled, dlg, [k](bool on) { st().setValue(k, on); });
        addRefresher(k, [cb, k] {
            QSignalBlocker b(cb);
            cb->setChecked(st().value(k).toBool());
        });
        return cb;
    }
    QComboBox* combo(const QString& k, const QStringList& values, const QStringList& labels,
                     QWidget* parent) const {
        auto* c = new QComboBox(parent);
        c->setObjectName(QStringLiteral("setting:") + k);
        for (int i = 0; i < values.size(); ++i)
            c->addItem(labels.value(i, values[i]), values[i]);
        QObject::connect(c, &QComboBox::activated, dlg, [c, k](int i) { st().setValue(k, c->itemData(i)); });
        addRefresher(k, [c, k] {
            QSignalBlocker b(c);
            c->setCurrentIndex(c->findData(st().value(k).toString()));
        });
        return c;
    }
    QSpinBox* spin(const QString& k, int lo, int hi, const QString& suffix, QWidget* parent) const {
        auto* s = new QSpinBox(parent);
        s->setObjectName(QStringLiteral("setting:") + k);
        s->setRange(lo, hi);
        s->setSuffix(suffix);
        QObject::connect(s, &QSpinBox::valueChanged, dlg, [k](int v) { st().setValue(k, v); });
        addRefresher(k, [s, k] {
            QSignalBlocker b(s);
            s->setValue(st().value(k).toInt());
        });
        return s;
    }
};
} // namespace

#define LF_BINDER                                                                                            \
    Binder bind {                                                                                            \
        this, [this](const QString& k, std::function<void()> fn) { addRefresher(k, std::move(fn)); }         \
    }

// ---- sections ----

QWidget* PreferencesDialog::buildGeneral() {
    LF_BINDER;
    auto* w = new QWidget;
    auto* form = new QFormLayout(w);
    form->addRow(tr("Öffnen aus dem Finder:"),
                 bind.combo(key::OpenBehavior, {"newWindow", "replaceFront", "newUnlessOpen"},
                            {tr("Immer in neuem Fenster"), tr("Im vordersten Fenster ersetzen"),
                             tr("Neues Fenster, außer die Datei ist schon offen")},
                            w));
    form->addRow(QString(),
                 helpLabel(tr("Gilt für Doppelklick im Finder, „Öffnen mit“ und Drag & Drop auf das "
                              "Dock-Symbol."),
                           w));
    form->addRow(QString(),
                 bind.check(key::RestoreWindows, tr("Fenster beim nächsten Start wiederherstellen"), w));
    form->addRow(QString(),
                 helpLabel(tr("Ordner, Bild, Zoom und Position aller Fenster, die beim Beenden offen "
                              "waren."),
                           w));
    form->addRow(QString(), bind.check(key::ConfirmTrash, tr("Vor dem Löschen nachfragen"), w));
    form->addRow(QString(),
                 helpLabel(tr("Aus: ⌘⌫ legt das Bild sofort in den Papierkorb, ⌘Z holt es zurück."), w));
    // F-EDIT-02: saving the rotation (Bild → Drehung speichern, ⌘S)
    form->addRow(tr("Drehung speichern:"),
                 bind.combo(key::RotationSaveStrategy, {"exif", "lossless"},
                            {tr("EXIF-Ausrichtung setzen"), tr("JPEG verlustfrei drehen (DCT)")}, w));
    form->addRow(
        QString(),
        helpLabel(tr("Beides ändert keine Bildpixel. „EXIF-Ausrichtung“ ändert nur ein Kennzeichen "
                     "(JPEG, HEIC, TIFF, PNG); „verlustfrei drehen“ ordnet die JPEG-Blöcke neu an, "
                     "damit auch Programme ohne EXIF-Unterstützung das Bild richtig zeigen. RAW-Dateien "
                     "werden nie verändert; ⌘Z macht das Speichern rückgängig."),
                  w));
    form->addRow(QString(), bind.check(key::KeepModificationDate, tr("Änderungsdatum beibehalten"), w));
    return w;
}

QWidget* PreferencesDialog::buildWindow() {
    LF_BINDER;
    auto* w = new QWidget;
    auto* form = new QFormLayout(w);
    form->addRow(QString(), bind.check(key::FitWindowToImage, tr("Fenstergröße an Bild anpassen"), w));
    form->addRow(QString(), helpLabel(tr("Aus: Neue Fenster übernehmen die zuletzt benutzte Größe."), w));
    form->addRow(QString(), bind.check(key::TitleShowFolder, tr("Ordnername in der Titelleiste"), w));
    form->addRow(QString(), helpLabel(tr("Zum Beispiel „IMG_1234.jpg — 23 / 412 — Urlaub“."), w));
    form->addRow(tr("Neue Fenster zeigen:"), bind.check(key::ShowStatusBar, tr("Statuszeile"), w));
    QCheckBox* info = bind.check(key::ShowInfoPanel, tr("Infopanel (%1)").arg(laterNote()), w);
    QCheckBox* film = bind.check(key::ShowFilmstrip, tr("Filmstreifen (%1)").arg(laterNote()), w);
    info->setEnabled(false);
    film->setEnabled(false);
    form->addRow(QString(), info);
    form->addRow(QString(), film);
    return w;
}

QWidget* PreferencesDialog::buildDisplay() {
    LF_BINDER;
    auto* w = new QWidget;
    auto* form = new QFormLayout(w);

    // Background: fixed choices + custom colour (F-VIEW-09)
    auto* bgRow = new QWidget(w);
    auto* bgLayout = new QHBoxLayout(bgRow);
    bgLayout->setContentsMargins(0, 0, 0, 0);
    auto* bg = new QComboBox(bgRow);
    bg->setObjectName(QStringLiteral("setting:") + key::Background);
    const QStringList bgValues = {"dark", "light", "system", "checker", "custom"};
    const QStringList bgLabels = {tr("Dunkel"), tr("Hell"), tr("Wie Systemdarstellung"),
                                  tr("Schachbrett (Transparenz)"), tr("Eigene Farbe")};
    for (int i = 0; i < bgValues.size(); ++i)
        bg->addItem(bgLabels[i], bgValues[i]);
    auto* colorButton = new QPushButton(tr("Farbe wählen …"), bgRow);
    colorButton->setAutoDefault(false);
    bgLayout->addWidget(bg);
    bgLayout->addWidget(colorButton);
    bgLayout->addStretch(1);
    auto pickColor = [this] {
        QString cur = st().value(key::Background).toString();
        const QColor initial = cur.startsWith('#') ? QColor(cur) : QColor(40, 40, 40);
        const QColor c = QColorDialog::getColor(initial, this, tr("Hintergrundfarbe"));
        if (c.isValid())
            st().setValue(key::Background, c.name(QColor::HexRgb));
    };
    connect(bg, &QComboBox::activated, this, [bg, pickColor](int i) {
        const QString v = bg->itemData(i).toString();
        if (v != QLatin1String("custom")) {
            st().setValue(key::Background, v);
            return;
        }
        pickColor();
        // colour dialog cancelled: show the stored value again
        const QString cur = st().value(key::Background).toString();
        QSignalBlocker b(bg);
        bg->setCurrentIndex(bg->findData(cur.startsWith('#') ? QStringLiteral("custom") : cur));
    });
    connect(colorButton, &QPushButton::clicked, this, pickColor);
    addRefresher(key::Background, [bg, colorButton] {
        QSignalBlocker b(bg);
        const QString v = st().value(key::Background).toString();
        const bool custom = v.startsWith('#');
        bg->setCurrentIndex(bg->findData(custom ? QStringLiteral("custom") : v));
        colorButton->setEnabled(custom);
        colorButton->setToolTip(custom ? v : QString());
    });
    form->addRow(tr("Hintergrund:"), bgRow);

    form->addRow(tr("Zoom beim Öffnen:"),
                 bind.combo(key::ZoomMode, {"fit", "fitUpscale", "actual", "fillWidth", "fillHeight", "fill"},
                            {tr("Einpassen (nicht vergrößern)"), tr("Einpassen inkl. Vergrößern"),
                             tr("100 %"), tr("Breite füllen"), tr("Höhe füllen"), tr("Fenster füllen")},
                            w));
    form->addRow(tr("Vergrößern über 100 %:"),
                 bind.combo(key::UpscaleFilter, {"smooth", "nearest"}, {tr("Glatt"), tr("Pixel scharf")}, w));
    form->addRow(tr("100 % bedeutet:"),
                 bind.combo(key::ActualSizeUnit, {"devicePixels", "points"},
                            {tr("1 Bildpixel = 1 Gerätepixel (Retina-korrekt)"), tr("1 Bildpixel = 1 Punkt")},
                            w));
    // F-SLIDE-01/02
    form->addRow(tr("Diashow (⇧⌘F, S):"), bind.spin(key::SlideshowIntervalSec, 1, 60, tr(" s pro Bild"), w));
    form->addRow(QString(), bind.check(key::SlideshowRepeat, tr("Wiederholen"), w));
    form->addRow(QString(), bind.check(key::SlideshowShuffle, tr("Zufällige Reihenfolge"), w));
    form->addRow(QString(), bind.check(key::SlideshowCrossfade, tr("Überblenden"), w));
    form->addRow(QString(),
                 helpLabel(tr("Während der Diashow: Leertaste hält an, 1–9 setzt das Intervall in Sekunden, "
                              "←/→ blättert, Esc beendet."),
                           w));
    return w;
}

QWidget* PreferencesDialog::buildInput() {
    LF_BINDER;
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);

    // Presets (F-IN-18)
    auto* presetBox = new QGroupBox(tr("Profilvorlagen"), w);
    auto* presetRow = new QHBoxLayout(presetBox);
    const std::pair<const char*, QString> presets[] = {{"mac", tr("Mac-Standard")},
                                                       {"windows", tr("Windows-Viewer-Gewohnheiten")},
                                                       {"photographer", tr("Fotograf:in / Lightroom")}};
    for (const auto& [id, label] : presets) {
        auto* b = new QPushButton(label, presetBox);
        b->setObjectName(QStringLiteral("preset:") + QLatin1String(id));
        b->setAutoDefault(false);
        const QString pid = QString::fromLatin1(id);
        connect(b, &QPushButton::clicked, this, [this, pid] {
            InputBindings::applyPreset(pid);
            m_bindings.reload();
        });
        presetRow->addWidget(b);
    }
    presetRow->addStretch(1);
    v->addWidget(presetBox);

    // Slots (4.14) with help text (F-IN-17)
    auto* slotBox = new QGroupBox(tr("Maus, Trackpad und Tasten"), w);
    auto* grid = new QGridLayout(slotBox);
    grid->setColumnStretch(1, 1);
    int row = 0;
    for (const InputSlot& s : InputBindings::inputSlots()) {
        const QString k = slotSettingsKey(s.id);
        auto* label = new QLabel(s.label + QLatin1Char(':'), slotBox);
        auto* combo = new QComboBox(slotBox);
        combo->setObjectName(QStringLiteral("slot:") + s.id);
        for (int i = 0; i < s.choices.size(); ++i) {
            const bool available = isChoiceAvailable(s.id, s.choices[i]);
            combo->addItem(available ? s.choiceLabels.value(i)
                                     : tr("%1 (%2)").arg(s.choiceLabels.value(i), laterNote()),
                           s.choices[i]);
            if (!available) {
                if (auto* model = qobject_cast<QStandardItemModel*>(combo->model()))
                    model->item(i)->setEnabled(false);
            }
        }
        auto* help = helpLabel(QString(), slotBox);
        help->setObjectName(QStringLiteral("help:") + s.id);
        label->setBuddy(combo);
        connect(combo, &QComboBox::activated, this, [this, combo, sid = s.id](int i) {
            m_bindings.setChoice(sid, combo->itemData(i).toString());
        });
        addRefresher(k, [combo, help, s, k] {
            QSignalBlocker b(combo);
            QString c = st().value(k).toString();
            int idx = s.choices.indexOf(c);
            if (idx < 0)
                idx = s.choices.indexOf(s.defaultChoice);
            combo->setCurrentIndex(idx);
            help->setText(s.choiceHelp.value(idx));
        });
        grid->addWidget(label, row, 0, Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(combo, row, 1);
        grid->addWidget(help, row + 1, 1);
        row += 2;
    }
    v->addWidget(slotBox);

    // Wheel tuning (F-IN-03)
    auto* tuneBox = new QGroupBox(tr("Mausrad-Feinheiten"), w);
    auto* form = new QFormLayout(tuneBox);
    form->addRow(QString(), bind.check(key::WheelInverted, tr("Richtung umkehren"), tuneBox));
    form->addRow(tr("Raststufen pro Bildwechsel:"),
                 bind.spin(key::WheelStepsPerImage, 1, 5, QString(), tuneBox));
    form->addRow(tr("Mindestabstand zwischen Bildwechseln:"),
                 bind.spin(key::WheelMinIntervalMs, 0, 300, tr(" ms"), tuneBox));
    form->addRow(
        QString(),
        helpLabel(tr("Verhindert das Überspringen von Bildern bei Rädern ohne Rasterung."), tuneBox));
    form->addRow(tr("Zoomschritt pro Raste:"), bind.spin(key::ZoomStepPercent, 5, 50, tr(" %"), tuneBox));
    form->addRow(tr("Sprungweite (Bild↑/Bild↓):"), bind.spin(key::JumpSize, 2, 100, tr(" Bilder"), tuneBox));
    v->addWidget(tuneBox);

    v->addWidget(buildShortcutEditor());
    return w;
}

QWidget* PreferencesDialog::buildShortcutEditor() {
    auto* box = new QGroupBox(tr("Tastenkürzel"));
    auto* v = new QVBoxLayout(box);
    v->addWidget(helpLabel(tr("Feld anklicken und neue Taste drücken. Bis zu drei Kürzel pro Aktion. "
                              "Ziffern und P/X/U für die Bewertung gelten nur im Aussortiermodus."),
                           box));
    m_conflictLabel = new QLabel(box);
    m_conflictLabel->setObjectName(QStringLiteral("conflictLabel"));
    m_conflictLabel->setStyleSheet(QStringLiteral("color: #d03030;"));
    v->addWidget(m_conflictLabel);

    QVector<Action> actions;
    for (Action a : InputBindings::allActions())
        if (editableAction(a))
            actions.append(a);

    m_shortcutTable = new QTableWidget(int(actions.size()), 2 + MaxShortcutsPerAction, box);
    m_shortcutTable->setObjectName(QStringLiteral("shortcutTable"));
    QStringList headers{tr("Aktion")};
    for (int i = 0; i < MaxShortcutsPerAction; ++i)
        headers << (i == 0 ? tr("Kürzel") : tr("Alternative"));
    headers << QString();
    m_shortcutTable->setHorizontalHeaderLabels(headers);
    m_shortcutTable->verticalHeader()->hide();
    m_shortcutTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_shortcutTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_shortcutTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_shortcutTable->setMinimumHeight(480);

    for (int row = 0; row < actions.size(); ++row) {
        const Action a = actions[row];
        m_rowOfAction.insert(int(a), row);
        auto* item = new QTableWidgetItem(actionLabel(a));
        item->setData(ActionRole, int(a));
        item->setData(ConflictRole, false);
        m_shortcutTable->setItem(row, 0, item);
        for (int c = 0; c < MaxShortcutsPerAction; ++c) {
            auto* edit = new QKeySequenceEdit(m_shortcutTable);
            edit->setObjectName(QStringLiteral("shortcut:%1:%2").arg(actionId(a)).arg(c));
            edit->setMaximumSequenceLength(1);
            edit->setClearButtonEnabled(true);
            if (auto* line = edit->findChild<QLineEdit*>())
                line->setPlaceholderText(tr("–"));
            connect(edit, &QKeySequenceEdit::keySequenceChanged, this, [this, row] {
                if (!m_updatingShortcuts)
                    applyShortcutRow(row);
            });
            m_shortcutTable->setCellWidget(row, 1 + c, edit);
        }
        auto* reset = new QToolButton(m_shortcutTable);
        reset->setText(QStringLiteral("↺"));
        reset->setToolTip(tr("Standardkürzel wiederherstellen"));
        reset->setObjectName(QStringLiteral("resetShortcut:") + actionId(a));
        connect(reset, &QToolButton::clicked, this,
                [this, a] { m_bindings.setShortcuts(a, InputBindings::defaultShortcuts(a)); });
        m_shortcutTable->setCellWidget(row, 1 + MaxShortcutsPerAction, reset);
        refreshShortcutRow(row);
    }
    m_shortcutTable->resizeColumnsToContents();
    m_shortcutTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    v->addWidget(m_shortcutTable);

    auto* row = new QHBoxLayout;
    row->addStretch(1);
    auto* resetAll = new QPushButton(tr("Alle Tastenkürzel zurücksetzen"), box);
    resetAll->setObjectName(QStringLiteral("resetAllShortcuts"));
    resetAll->setAutoDefault(false);
    connect(resetAll, &QPushButton::clicked, this, [this] { m_bindings.resetShortcuts(); });
    row->addWidget(resetAll);
    v->addLayout(row);
    updateConflicts();
    return box;
}

void PreferencesDialog::refreshShortcutRow(int row) {
    const Action a = Action(m_shortcutTable->item(row, 0)->data(ActionRole).toInt());
    const auto list = m_bindings.shortcuts(a);
    m_updatingShortcuts = true;
    for (int c = 0; c < MaxShortcutsPerAction; ++c) {
        auto* edit = static_cast<QKeySequenceEdit*>(m_shortcutTable->cellWidget(row, 1 + c));
        edit->setKeySequence(list.value(c));
    }
    m_updatingShortcuts = false;
}

void PreferencesDialog::applyShortcutRow(int row) {
    const Action a = Action(m_shortcutTable->item(row, 0)->data(ActionRole).toInt());
    QList<QKeySequence> list;
    for (int c = 0; c < MaxShortcutsPerAction; ++c) {
        auto* edit = static_cast<QKeySequenceEdit*>(m_shortcutTable->cellWidget(row, 1 + c));
        if (!edit->keySequence().isEmpty())
            list.append(edit->keySequence());
    }
    // Keep alternatives the editor cannot show (more than MaxShortcutsPerAction).
    const auto current = m_bindings.shortcuts(a);
    for (int i = MaxShortcutsPerAction; i < current.size(); ++i)
        list.append(current[i]);
    m_bindings.setShortcuts(a, list); // emits changed -> updateConflicts()
    updateConflicts();
}

void PreferencesDialog::updateConflicts() {
    if (!m_shortcutTable)
        return;
    m_conflicts = 0;
    for (int row = 0; row < m_shortcutTable->rowCount(); ++row) {
        QTableWidgetItem* item = m_shortcutTable->item(row, 0);
        const Action a = Action(item->data(ActionRole).toInt());
        const auto others = m_bindings.conflictsWith(a);
        const bool conflict = !others.isEmpty();
        item->setData(ConflictRole, conflict);
        if (conflict) {
            ++m_conflicts;
            QStringList names;
            for (Action o : others)
                names << actionLabel(o);
            item->setBackground(QColor(220, 60, 60, 90));
            item->setToolTip(tr("Konflikt mit: %1").arg(names.join(QStringLiteral(", "))));
        } else {
            item->setBackground(QBrush());
            item->setToolTip(QString());
        }
    }
    m_conflictLabel->setText(
        m_conflicts
            ? tr("%n Aktion(en) mit doppelt belegten Tasten – bitte eine davon ändern.", nullptr, m_conflicts)
            : QString());
    m_conflictLabel->setVisible(m_conflicts > 0);
}

QWidget* PreferencesDialog::buildNavigation() {
    LF_BINDER;
    auto* w = new QWidget;
    auto* form = new QFormLayout(w);
    form->addRow(tr("Am Ende der Sammlung:"),
                 bind.combo(key::WrapMode, {"stop", "wrap", "nextFolder"},
                            {tr("Anhalten"), tr("Umlaufen"), tr("Zum nächsten Ordner")}, w));
    form->addRow(QString(), bind.check(key::FollowFinder, tr("Sortierung wie im Finder"), w));
    form->addRow(QString(),
                 helpLabel(tr("Liest die Darstellungsoptionen des Ordners (.DS_Store) und die globalen "
                              "Finder-Einstellungen."),
                           w));
    form->addRow(QString(), bind.check(key::FinderLiveQuery, tr("Vorderstes Finder-Fenster abfragen"), w));
    form->addRow(
        QString(),
        helpLabel(tr("Fragt per AppleScript die aktuelle Sortierung des Finder-Fensters ab. macOS bittet "
                     "dafür einmalig um die Erlaubnis, den Finder zu steuern (Automatisierung)."),
                  w));
    form->addRow(tr("Manuelle Sortierung:"),
                 bind.combo(key::ManualSortKey,
                            {"name", "dateModified", "dateCreated", "dateAdded", "dateCaptured", "size",
                             "kind", "pixelCount", "rating"},
                            {tr("Name"), tr("Änderungsdatum"), tr("Erstellungsdatum"), tr("Hinzugefügt am"),
                             tr("Aufnahmedatum"), tr("Größe"), tr("Art"), tr("Pixelzahl"), tr("Bewertung")},
                            w));
    form->addRow(QString(), bind.check(key::ManualSortAscending, tr("Aufsteigend"), w));
    form->addRow(QString(),
                 helpLabel(tr("Gilt, wenn „Sortierung wie im Finder“ aus ist oder keine Finder-Information "
                              "gefunden wird. Eine Wahl im Menü Darstellung → Sortieren nach wird pro Ordner "
                              "gemerkt; „Für alle Ordner verwenden“ macht sie zur Vorgabe hier."),
                           w));
    return w;
}

QWidget* PreferencesDialog::buildRating() {
    LF_BINDER;
    auto* w = new QWidget;
    auto* form = new QFormLayout(w);
    form->addRow(tr("Bewertungen speichern:"),
                 bind.check(key::RatingStoreSidecar, tr("XMP-Zusatzdatei neben dem Bild (Bild.xmp)"), w));
    form->addRow(QString(), helpLabel(tr("Kompatibel mit Lightroom, Capture One und darktable."), w));
    form->addRow(QString(),
                 bind.check(key::RatingStoreEmbedded, tr("Im Bild selbst (eingebettetes XMP)"), w));
    form->addRow(QString(),
                 helpLabel(tr("Nur JPEG, HEIC, TIFF, PNG und DNG; sonst wird die Zusatzdatei benutzt. "
                              "Die Bildpixel werden nicht neu kodiert."),
                           w));
    form->addRow(QString(), bind.check(key::RatingStoreFinderTags, tr("Finder-Tags"), w));
    form->addRow(QString(), helpLabel(tr("Im Finder sichtbar und per Spotlight durchsuchbar."), w));
    // Rating (F-CULL-01/02) is not implemented yet: the choices are visible but disabled.
    for (QCheckBox* cb : w->findChildren<QCheckBox*>())
        cb->setEnabled(false);
    form->insertRow(0, QString(),
                    helpLabel(tr("Bewertungen und der Aussortiermodus folgen %1.").arg(laterNote()), w));
    return w;
}

QWidget* PreferencesDialog::buildFormats() {
    LF_BINDER;
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(new QLabel(tr("Unterstützte Formate (ohne Nachinstallation):"), w));
    auto* list = new QTableWidget(0, 2, w);
    list->setObjectName(QStringLiteral("formatList"));
    list->setHorizontalHeaderLabels({tr("Gruppe"), tr("Formate")});
    list->verticalHeader()->hide();
    list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list->setSelectionMode(QAbstractItemView::NoSelection);
    const std::pair<QString, QString> groups[] = {
        {tr("Standard"), QStringLiteral("JPEG, PNG, GIF, BMP, TIFF, WebP, ICO/CUR, ICNS")},
        {tr("Modern"), QStringLiteral("HEIC/HEIF, AVIF, JPEG XL")},
        {tr("RAW"), QStringLiteral("CR2, CR3, CRW, NEF, NRW, ARW, SR2, RAF, ORF, RW2, DNG, PEF, IIQ, 3FR …")},
        {tr("Pro/HDR"), QStringLiteral("PSD, OpenEXR, Radiance HDR, JPEG 2000, TGA, DDS, KTX")},
        // F-FMT-03: played back (Leertaste, , / .); F-FMT-04: pages with ⌥↑/⌥↓.
        {tr("Animiert"), tr("GIF, APNG, WebP, AVIF/HEICS, JXL – mit Wiedergabe")},
        {tr("Vektor"), QStringLiteral("SVG")},
    };
    for (const auto& [g, f] : groups) {
        const int r = list->rowCount();
        list->insertRow(r);
        list->setItem(r, 0, new QTableWidgetItem(g));
        list->setItem(r, 1, new QTableWidgetItem(f));
    }
    list->resizeColumnToContents(0);
    list->horizontalHeader()->setStretchLastSection(true);
    list->setMinimumHeight(200);
    v->addWidget(list);
    auto* row = new QHBoxLayout;
    auto* makeDefault = new QPushButton(tr("Als Standard-App für Bilder festlegen"), w);
    makeDefault->setObjectName(QStringLiteral("makeDefault"));
    makeDefault->setAutoDefault(false);
    connect(makeDefault, &QPushButton::clicked, this, &PreferencesDialog::makeDefaultRequested);
    row->addWidget(makeDefault);
    row->addStretch(1);
    v->addLayout(row);
    v->addSpacing(12);
    v->addWidget(
        bind.check(key::RawEmbeddedOnly, tr("RAW: nur eingebettete Vorschau anzeigen (schneller)"), w));
    v->addWidget(helpLabel(
        tr("Aus: Zuerst die eingebettete Vorschau, danach wird die volle Entwicklung nachgeladen."), w));
    return w;
}

QWidget* PreferencesDialog::buildAdvanced() {
    LF_BINDER;
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    auto* perf = new QGroupBox(tr("Leistung"), w);
    auto* form = new QFormLayout(perf);
    QSpinBox* budget = bind.spin(key::MemoryBudgetMB, 0, 65536, tr(" MB"), perf);
    budget->setSingleStep(256);
    budget->setSpecialValueText(tr("Automatisch (25 % des Arbeitsspeichers, max. 2 GB)"));
    form->addRow(tr("Speicherbudget:"), budget);
    form->addRow(tr("Bilder im Voraus laden:"), bind.spin(key::PrefetchAhead, 0, 20, QString(), perf));
    form->addRow(tr("Bilder zurück behalten:"), bind.spin(key::PrefetchBehind, 0, 20, QString(), perf));
    v->addWidget(perf);

    auto* io = new QGroupBox(tr("Einstellungen sichern"), w);
    auto* row = new QHBoxLayout(io);
    auto* exp = new QPushButton(tr("Exportieren …"), io);
    exp->setObjectName(QStringLiteral("exportSettings"));
    exp->setAutoDefault(false);
    auto* imp = new QPushButton(tr("Importieren …"), io);
    imp->setObjectName(QStringLiteral("importSettings"));
    imp->setAutoDefault(false);
    auto* all = new QPushButton(tr("Alle Einstellungen zurücksetzen"), io);
    all->setObjectName(QStringLiteral("resetAll"));
    all->setAutoDefault(false);
    row->addWidget(exp);
    row->addWidget(imp);
    row->addStretch(1);
    row->addWidget(all);
    const QString filter = tr("Lumaflip-Einstellungen (*.json)");
    connect(exp, &QPushButton::clicked, this, [this, filter] {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        const QString path =
            QFileDialog::getSaveFileName(this, tr("Einstellungen exportieren"),
                                         dir + QStringLiteral("/Lumaflip-Einstellungen.json"), filter);
        if (!path.isEmpty())
            exportSettingsTo(path);
    });
    connect(imp, &QPushButton::clicked, this, [this, filter] {
        const QString path =
            QFileDialog::getOpenFileName(this, tr("Einstellungen importieren"), QString(), filter);
        if (!path.isEmpty())
            importSettingsFrom(path);
    });
    connect(all, &QPushButton::clicked, this, [this] {
        const auto answer =
            QMessageBox::question(this, tr("Alle Einstellungen zurücksetzen?"),
                                  tr("Alle Einstellungen einschließlich der Tastenkürzel werden auf "
                                     "die Standardwerte zurückgesetzt."));
        if (answer == QMessageBox::Yes) {
            for (const QString& k : Settings::knownKeys())
                st().resetToDefaults(k);
            m_bindings.reload();
        }
    });
    v->addWidget(io);
    return w;
}

// ---- export / import (F-SET-03) ----

bool PreferencesDialog::exportSettingsTo(const QString& filePath) {
    QString error;
    const bool ok = st().exportTo(filePath, &error);
    m_messageLabel->setText(ok ? tr("Einstellungen exportiert: %1").arg(filePath)
                               : tr("Export fehlgeschlagen: %1").arg(error));
    m_messageLabel->show();
    if (!ok && isVisible())
        QMessageBox::warning(this, tr("Export fehlgeschlagen"), error);
    return ok;
}

bool PreferencesDialog::importSettingsFrom(const QString& filePath) {
    QString error;
    const bool ok = st().importFrom(filePath, &error);
    m_bindings.reload();
    m_messageLabel->setText(ok ? tr("Einstellungen importiert: %1").arg(filePath)
                               : tr("Import fehlgeschlagen: %1").arg(error));
    m_messageLabel->show();
    if (!ok && isVisible())
        QMessageBox::warning(this, tr("Import fehlgeschlagen"), error);
    return ok;
}

} // namespace lf
