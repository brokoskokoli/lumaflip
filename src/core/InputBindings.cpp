// SPDX-License-Identifier: GPL-3.0-or-later
#include "InputBindings.h"

#include <QSet>

#include "Settings.h"

#include <QCoreApplication>
#include <QKeyCombination>

#include <algorithm>

namespace lf {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("lf::InputBindings", text);
}

struct ActionInfo {
    Action action;
    const char* id;
    const char* label;
};

// Order = order in the shortcut editor.
const ActionInfo kActions[] = {
    {Action::NextImage, "nextImage", "Nächstes Bild"},
    {Action::PreviousImage, "previousImage", "Vorheriges Bild"},
    {Action::FirstImage, "firstImage", "Erstes Bild"},
    {Action::LastImage, "lastImage", "Letztes Bild"},
    {Action::JumpForward, "jumpForward", "Mehrere Bilder vor"},
    {Action::JumpBackward, "jumpBackward", "Mehrere Bilder zurück"},
    {Action::NextFolder, "nextFolder", "Nächster Ordner"},
    {Action::PreviousFolder, "previousFolder", "Vorheriger Ordner"},
    {Action::NextPage, "nextPage", "Nächste Seite"},
    {Action::PreviousPage, "previousPage", "Vorherige Seite"},
    {Action::ZoomIn, "zoomIn", "Vergrößern"},
    {Action::ZoomOut, "zoomOut", "Verkleinern"},
    {Action::ZoomActual, "zoomActual", "100 %"},
    {Action::ZoomFit, "zoomFit", "Einpassen"},
    {Action::ZoomFillWidth, "zoomFillWidth", "Breite füllen"},
    {Action::ToggleFitActualAtCursor, "toggleFitActual", "Einpassen ↔ 100 % am Cursor"},
    {Action::ToggleZoomLock, "toggleZoomLock", "Zoom fixieren"},
    {Action::ScrollUp, "scrollUp", "Ausschnitt nach oben"},
    {Action::ScrollDown, "scrollDown", "Ausschnitt nach unten"},
    {Action::ScrollLeft, "scrollLeft", "Ausschnitt nach links"},
    {Action::ScrollRight, "scrollRight", "Ausschnitt nach rechts"},
    {Action::ScrollPageUp, "scrollPageUp", "Ausschnitt eine Seite nach oben"},
    {Action::ScrollPageDown, "scrollPageDown", "Ausschnitt eine Seite nach unten"},
    {Action::ScrollHome, "scrollHome", "Zum Bildanfang scrollen"},
    {Action::ScrollEnd, "scrollEnd", "Zum Bildende scrollen"},
    {Action::ToggleFullScreen, "toggleFullScreen", "Vollbild"},
    {Action::ExitModeOrNothing, "exitMode", "Vollbild bzw. Modus verlassen"},
    {Action::ExitModeOrCloseWindow, "exitModeOrClose", "Modus verlassen, sonst Fenster schließen"},
    {Action::CloseWindow, "closeWindow", "Fenster schließen"},
    {Action::CloseAllWindows, "closeAllWindows", "Alle Fenster schließen"},
    {Action::ToggleAnimation, "toggleAnimation", "Animation abspielen/pausieren"},
    {Action::NextFrame, "nextFrame", "Nächstes Einzelbild"},
    {Action::PreviousFrame, "previousFrame", "Vorheriges Einzelbild"},
    {Action::ToggleSlideshow, "toggleSlideshow", "Diashow"},
    {Action::RotateLeft, "rotateLeft", "Links drehen"},
    {Action::RotateRight, "rotateRight", "Rechts drehen"},
    {Action::FlipHorizontal, "flipHorizontal", "Horizontal spiegeln"},
    {Action::FlipVertical, "flipVertical", "Vertikal spiegeln"},
    {Action::SaveRotation, "saveRotation", "Drehung speichern"},
    {Action::Trash, "trash", "In den Papierkorb legen"},
    {Action::Undo, "undo", "Rückgängig"},
    {Action::Redo, "redo", "Wiederholen"},
    {Action::Rename, "rename", "Umbenennen"},
    {Action::RevealInFinder, "revealInFinder", "Im Finder zeigen"},
    {Action::CopyImage, "copyImage", "Bild kopieren"},
    {Action::CopyPath, "copyPath", "Pfad kopieren"},
    {Action::OpenInEditor, "openInEditor", "Im Editor öffnen"},
    {Action::ToggleInfoPanel, "toggleInfoPanel", "Infopanel"},
    {Action::ToggleFilmstrip, "toggleFilmstrip", "Filmstreifen"},
    {Action::ToggleGrid, "toggleGrid", "Raster"},
    {Action::ShowShortcuts, "showShortcuts", "Tastenkürzel-Übersicht"},
    {Action::OpenPreferences, "openPreferences", "Einstellungen"},
    {Action::NewWindow, "newWindow", "Neues Fenster"},
    {Action::DuplicateWindow, "duplicateWindow", "Fenster duplizieren"},
    {Action::Magnifier, "magnifier", "Lupe (gedrückt halten)"},
    {Action::DragOut, "dragOut", "Datei herausziehen"},
    {Action::StartPan, "startPan", "Ausschnitt verschieben"},
    {Action::Rate0, "rate0", "Bewertung entfernen (0 Sterne)"},
    {Action::Rate1, "rate1", "1 Stern"},
    {Action::Rate2, "rate2", "2 Sterne"},
    {Action::Rate3, "rate3", "3 Sterne"},
    {Action::Rate4, "rate4", "4 Sterne"},
    {Action::Rate5, "rate5", "5 Sterne"},
    {Action::RatePick, "ratePick", "Behalten (Pick)"},
    {Action::RateReject, "rateReject", "Verwerfen (Reject)"},
    {Action::RateClear, "rateClear", "Markierung zurücksetzen"},
};

const ActionInfo* info(Action a) {
    for (const auto& i : kActions)
        if (i.action == a)
            return &i;
    return nullptr;
}

// Default shortcuts per SPEC 5.2. In Qt on macOS "Ctrl" is the Command key (⌘) and
// "Meta" is the Control key (⌃).
QStringList defaultShortcutStrings(Action a) {
    switch (a) {
    case Action::NewWindow:
        return {"Ctrl+N"};
    case Action::DuplicateWindow:
        return {"Alt+Ctrl+N"};
    case Action::FirstImage:
        return {"Ctrl+Up"};
    case Action::LastImage:
        return {"Ctrl+Down"};
    case Action::ZoomIn:
        return {"Ctrl++", "Ctrl+=", "+"};
    case Action::ZoomOut:
        return {"Ctrl+-", "-"};
    case Action::ZoomActual:
        return {"Ctrl+0", "3"};
    case Action::ZoomFit:
        return {"Ctrl+9", "1"};
    case Action::ZoomFillWidth:
        return {"2"};
    case Action::ToggleFitActualAtCursor:
        return {"Z"};
    case Action::ToggleZoomLock:
        return {"Shift+Z"};
    case Action::ToggleFullScreen:
        return {"Meta+Ctrl+F", "F"};
    case Action::ToggleSlideshow:
        return {"Shift+Ctrl+F", "S"};
    case Action::RotateLeft:
        return {"Ctrl+L"};
    case Action::RotateRight:
        return {"Ctrl+R"};
    case Action::FlipHorizontal:
        return {"H"};
    case Action::FlipVertical:
        return {"Shift+H"};
    case Action::SaveRotation:
        return {"Ctrl+S"};
    case Action::ToggleInfoPanel:
        return {"Ctrl+I", "I"};
    case Action::ToggleFilmstrip:
        return {"T"};
    case Action::ToggleGrid:
        return {"G"};
    case Action::Trash:
        return {"Ctrl+Backspace", "Del"};
    case Action::Undo:
        return {"Ctrl+Z"};
    case Action::Redo:
        return {"Shift+Ctrl+Z"};
    case Action::Rename:
        return {"Return", "F2"};
    case Action::RevealInFinder:
        return {"Shift+Ctrl+R"};
    case Action::CopyImage:
        return {"Ctrl+C"};
    case Action::CopyPath:
        return {"Alt+Ctrl+C"};
    case Action::OpenInEditor:
        return {"E"};
    case Action::NextFrame:
        return {"."};
    case Action::PreviousFrame:
        return {","};
    case Action::NextPage:
        return {"Alt+Down"};
    case Action::PreviousPage:
        return {"Alt+Up"};
    case Action::OpenPreferences:
        return {"Ctrl+,"};
    case Action::ShowShortcuts:
        return {"?"};
    case Action::CloseWindow:
        return {"Ctrl+W"};
    case Action::CloseAllWindows:
        return {"Alt+Ctrl+W"};
    case Action::Magnifier:
        return {"L"};
    case Action::Rate0:
        return {"0"};
    case Action::Rate1:
        return {"1"};
    case Action::Rate2:
        return {"2"};
    case Action::Rate3:
        return {"3"};
    case Action::Rate4:
        return {"4"};
    case Action::Rate5:
        return {"5"};
    case Action::RatePick:
        return {"P"};
    case Action::RateReject:
        return {"X"};
    case Action::RateClear:
        return {"U"};
    default:
        return {};
    }
}

bool isCullingAction(Action a) {
    switch (a) {
    case Action::Rate0:
    case Action::Rate1:
    case Action::Rate2:
    case Action::Rate3:
    case Action::Rate4:
    case Action::Rate5:
    case Action::RatePick:
    case Action::RateReject:
    case Action::RateClear:
        return true;
    default:
        return false;
    }
}

QString shortcutKey(Action a) {
    return QString::fromLatin1(key::ShortcutPrefix) + actionId(a);
}
QString slotKey(const QString& slotId) {
    return QString::fromLatin1(key::InputPrefix) + slotId;
}

constexpr Qt::KeyboardModifiers kRelevantMods =
    Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;

QList<QKeySequence> fromStrings(const QStringList& list) {
    QList<QKeySequence> out;
    for (const auto& s : list) {
        QKeySequence seq = QKeySequence::fromString(s, QKeySequence::PortableText);
        if (!seq.isEmpty())
            out.append(seq);
    }
    return out;
}

QStringList toStrings(const QList<QKeySequence>& list) {
    QStringList out;
    for (const auto& seq : list)
        if (!seq.isEmpty())
            out.append(seq.toString(QKeySequence::PortableText));
    return out;
}

int clampInt(const QVariant& v, int lo, int hi) {
    return std::clamp(v.toInt(), lo, hi);
}

} // namespace

QString actionId(Action a) {
    const ActionInfo* i = info(a);
    return i ? QString::fromLatin1(i->id) : QStringLiteral("none");
}

Action actionFromId(const QString& id) {
    for (const auto& i : kActions)
        if (id == QLatin1String(i.id))
            return i.action;
    return Action::None;
}

QString actionLabel(Action a) {
    const ActionInfo* i = info(a);
    return i ? tr(i->label) : tr("Nichts");
}

// ---- slots (SPEC 4.14) ----

const QVector<InputSlot>& InputBindings::inputSlots() {
    static const QVector<InputSlot> slotTable = [] {
        QVector<InputSlot> v;
        auto add = [&v](const char* id, const char* label,
                        std::initializer_list<std::tuple<const char*, const char*, const char*>> choices,
                        const char* def) {
            InputSlot s;
            s.id = QString::fromLatin1(id);
            s.label = tr(label);
            for (const auto& [cid, clabel, chelp] : choices) {
                s.choices << QString::fromLatin1(cid);
                s.choiceLabels << tr(clabel);
                s.choiceHelp << tr(chelp);
            }
            s.defaultChoice = QString::fromLatin1(def);
            v.append(s);
        };
        // F-IN-01
        add("wheel", "Mausrad",
            {{"image", "Bild wechseln", "Nach unten drehen zeigt das nächste, nach oben das vorherige Bild."},
             {"zoom", "Zoomen (am Cursor)",
              "Nach oben drehen vergrößert, nach unten verkleinert – am Mauszeiger."},
             {"scrollOrImage", "Ausschnitt scrollen (sonst Bild wechseln)",
              "Gezoomt wird der Ausschnitt verschoben, sonst wird das Bild gewechselt."}},
            "image");
        // F-IN-02
        const auto wheelMod = {
            std::tuple<const char*, const char*, const char*>{
                "image", "Bild wechseln", "Bild wechseln, solange die Taste gehalten wird."},
            {"zoom", "Zoomen (am Cursor)", "Zoomen am Mauszeiger, solange die Taste gehalten wird."},
            {"scrollOrImage", "Ausschnitt scrollen (sonst Bild wechseln)",
             "Gezoomt wird der Ausschnitt verschoben, sonst wird das Bild gewechselt."},
            {"scrollHorizontal", "Horizontal scrollen", "Der Ausschnitt wird waagerecht verschoben."}};
        add("wheel.cmd", "Mausrad mit ⌘", wheelMod, "zoom");
        add("wheel.alt", "Mausrad mit ⌥", wheelMod, "scrollOrImage");
        add("wheel.shift", "Mausrad mit ⇧", wheelMod, "scrollHorizontal");
        // F-IN-04
        add("pageUpDown", "Bild↑ / Bild↓",
            {{"image", "Vorheriges/nächstes Bild", "Wechselt ein Bild zurück bzw. vor."},
             {"jump", "Mehrere Bilder zurück/vor", "Springt um die eingestellte Sprungweite (Standard 10)."},
             {"folder", "Vorheriger/nächster Ordner", "Wechselt in den benachbarten Ordner."},
             {"page", "Seite zurück/vor (sonst Bild)",
              "Bei mehrseitigen Dateien wird geblättert, sonst das Bild gewechselt."},
             {"scroll", "Ausschnitt seitenweise scrollen (sonst Bild)",
              "Gezoomt um eine Bildschirmhöhe scrollen, sonst Bild wechseln."}},
            "image");
        // F-IN-05
        add("homeEnd", "Pos1 / Ende",
            {{"image", "Erstes/letztes Bild", "Springt zum ersten bzw. letzten Bild der Sammlung."},
             {"scroll", "Bildanfang/-ende (sonst erstes/letztes Bild)",
              "Gezoomt an den Anfang bzw. das Ende des Bildes scrollen, sonst erstes/letztes Bild."}},
            "image");
        // F-IN-06
        add("space", "Leertaste / ⇧Leertaste",
            {{"image", "Nächstes/vorheriges Bild", "Leertaste: nächstes Bild, mit ⇧: vorheriges Bild."},
             {"animationOrImage", "Animation abspielen/pausieren, sonst Bild",
              "Bei animierten Bildern Wiedergabe umschalten, sonst Bild wechseln."},
             {"fitActual", "Einpassen ↔ 100 %", "Schaltet zwischen Einpassen und 100 % um."},
             {"slideshow", "Diashow starten/pausieren", "Startet bzw. pausiert die Diashow."}},
            "animationOrImage");
        // F-IN-07
        add("arrowsZoomed", "← / →, wenn gezoomt",
            {{"image", "Immer Bild wechseln",
              "Pfeiltasten wechseln das Bild; mit ⌥ wird der Ausschnitt verschoben."},
             {"scrollThenImage", "Erst bis zum Rand scrollen, dann Bild wechseln",
              "Der Ausschnitt wird verschoben; am Bildrand wechselt das Bild."},
             {"scroll", "Ausschnitt scrollen (Bild mit ⌥←/⌥→)",
              "Pfeiltasten verschieben den Ausschnitt; Bildwechsel mit ⌥←/⌥→."}},
            "image");
        // F-IN-08
        add("upDown", "↑ / ↓",
            {{"scrollOrImage", "Ausschnitt scrollen, sonst vorheriges/nächstes Bild",
              "Gezoomt wird der Ausschnitt verschoben, sonst das Bild gewechselt."},
             {"scrollOrNothing", "Ausschnitt scrollen (wenn gezoomt), sonst nichts",
              "Gezoomt wird der Ausschnitt verschoben, sonst passiert nichts."},
             {"image", "Vorheriges/nächstes Bild", "↑ zeigt das vorherige, ↓ das nächste Bild."},
             {"zoom", "Zoomen", "↑ vergrößert, ↓ verkleinert."}},
            "scrollOrImage");
        // F-IN-09
        add("click", "Einfacher Klick ins Bild",
            {{"none", "Nichts", "Ein Klick hat keine Wirkung (Ziehen verschiebt weiterhin)."},
             {"fitActual", "Einpassen ↔ 100 % am Cursor",
              "Ein Klick zoomt auf 100 % an der Klickstelle und zurück."},
             {"image", "Nächstes Bild (linke Hälfte: vorheriges)",
              "Klick in die rechte Hälfte: nächstes Bild, linke Hälfte: vorheriges."}},
            "none");
        // F-IN-10
        add("doubleClick", "Doppelklick ins Bild",
            {{"fullscreen", "Vollbild umschalten", "Doppelklick wechselt in das Vollbild und zurück."},
             {"fitActual", "Einpassen ↔ 100 %", "Doppelklick schaltet zwischen Einpassen und 100 % um."},
             {"close", "Fenster schließen", "Doppelklick schließt das Fenster."}},
            "fullscreen");
        // F-IN-11
        add("middleClick", "Mittlere Maustaste",
            {{"none", "Nichts", "Die mittlere Maustaste hat keine Wirkung."},
             {"fullscreen", "Vollbild", "Schaltet das Vollbild um."},
             {"close", "Fenster schließen", "Schließt das Fenster."},
             {"fitActual", "Einpassen ↔ 100 %",
              "Schaltet zwischen Einpassen und 100 % an der Klickstelle um."},
             {"magnifier", "Lupe (gedrückt halten)", "Zeigt eine Lupe, solange die Taste gedrückt ist."}},
            "fitActual");
        // F-IN-12
        add("backForward", "Maustasten 4/5 (Zurück/Vor)",
            {{"image", "Vorheriges/nächstes Bild",
              "Zurück-Taste: vorheriges Bild, Vor-Taste: nächstes Bild."},
             {"none", "Nichts", "Die Tasten haben keine Wirkung."}},
            "image");
        // F-IN-13
        add("swipe", "Trackpad-Wischen (zwei Finger, horizontal)",
            {{"image", "Bild wechseln (gezoomt: scrollen)",
              "Wischen wechselt das Bild; im gezoomten Zustand wird der Ausschnitt verschoben."},
             {"scroll", "Ausschnitt scrollen", "Wischen verschiebt immer nur den Ausschnitt."},
             {"off", "Aus", "Wischen wird ignoriert."}},
            "image");
        // F-IN-14
        add("drag", "Ziehen mit linker Maustaste",
            {{"pan", "Ausschnitt verschieben (⌥: Datei herausziehen)",
              "Ziehen verschiebt den Ausschnitt; mit ⌥ wird die Datei in andere Apps gezogen."},
             {"dragOut", "Datei herausziehen (⌥: Ausschnitt verschieben)",
              "Ziehen zieht die Datei in andere Apps; mit ⌥ wird der Ausschnitt verschoben."}},
            "pan");
        // F-IN-15
        add("esc", "Esc",
            {{"exitMode", "Vollbild bzw. Modus verlassen, sonst nichts",
              "Esc beendet Vollbild, Diashow oder andere Modi und tut sonst nichts."},
             {"exitModeOrClose", "Vollbild bzw. Modus verlassen, sonst Fenster schließen",
              "Esc beendet Modi; ist keiner aktiv, wird das Fenster geschlossen."}},
            "exitMode");
        // F-IN-16 (mirrors key::WrapMode / F-NAV-03)
        add("endOfCollection", "Ende der Sammlung",
            {{"stop", "Anhalten", "Am Ende bleibt das letzte Bild stehen (mit kurzem Hinweis)."},
             {"wrap", "Umlaufen", "Nach dem letzten Bild folgt wieder das erste."},
             {"nextFolder", "Zum nächsten Ordner",
              "Nach dem letzten Bild geht es im nächsten Ordner weiter."}},
            "stop");
        // Not in the 4.14 table, needed for the F-IN-18 preset "Fotograf:in" and the 5.2
        // conflict rule: digits/P/X/U rate only in culling mode unless enabled here.
        add("ratingKeys", "Bewertungstasten (0–5, P/X/U)",
            {{"cullingOnly", "Nur im Aussortiermodus", "Ziffern wählen sonst den Zoommodus (1/2/3)."},
             {"always", "Immer aktiv",
              "Ziffern vergeben immer Sterne; P/X/U markieren. Zoom-Ziffern sind aus."}},
            "cullingOnly");
        return v;
    }();
    return slotTable;
}

namespace {
const InputSlot* findSlot(const QString& id) {
    for (const auto& s : InputBindings::inputSlots())
        if (s.id == id)
            return &s;
    return nullptr;
}
} // namespace

// ---- construction / persistence ----

InputBindings::InputBindings() {
    reload();
}

void InputBindings::reload() {
    Settings& st = Settings::instance();
    m_choices.clear();
    for (const auto& s : inputSlots()) {
        const QString k =
            s.id == QLatin1String("endOfCollection") ? QString::fromLatin1(key::WrapMode) : slotKey(s.id);
        QString c = st.value(k).toString();
        if (!s.choices.contains(c))
            c = s.defaultChoice;
        m_choices.insert(s.id, c);
    }
    m_shortcuts.clear();
    for (Action a : allActions())
        m_shortcuts.insert(int(a), fromStrings(st.value(shortcutKey(a)).toStringList()));
}

QString InputBindings::choice(const QString& slotId) const {
    auto it = m_choices.constFind(slotId);
    if (it != m_choices.constEnd())
        return *it;
    const InputSlot* s = findSlot(slotId);
    return s ? s->defaultChoice : QString();
}

void InputBindings::setChoice(const QString& slotId, const QString& choiceId) {
    const InputSlot* s = findSlot(slotId);
    if (!s || !s->choices.contains(choiceId))
        return;
    m_choices.insert(slotId, choiceId);
    const QString k =
        slotId == QLatin1String("endOfCollection") ? QString::fromLatin1(key::WrapMode) : slotKey(slotId);
    Settings::instance().setValue(k, choiceId);
}

void InputBindings::applyPreset(const QString& presetId) {
    // Every slot is set: preset value or the slot's default. Wheel tuning and
    // shortcuts not touched by the preset stay as they are.
    QHash<QString, QString> p;
    QList<QKeySequence> fullScreen = defaultShortcuts(Action::ToggleFullScreen);
    QList<QKeySequence> rename = defaultShortcuts(Action::Rename);
    if (presetId == QLatin1String("windows")) {
        // IrfanView / FastStone habits
        p = {{"wheel", "image"},          {"wheel.cmd", "zoom"},
             {"pageUpDown", "image"},     {"homeEnd", "image"},
             {"space", "image"},          {"arrowsZoomed", "image"},
             {"upDown", "scrollOrImage"}, {"doubleClick", "fullscreen"},
             {"esc", "exitModeOrClose"}};
        fullScreen.append(QKeySequence(Qt::Key_Return));
        fullScreen.append(QKeySequence(Qt::Key_Enter));
        rename = {QKeySequence(Qt::Key_F2)};
    } else if (presetId == QLatin1String("photographer")) {
        // Lightroom-like
        p = {{"wheel", "zoom"}, {"wheel.cmd", "image"}, {"click", "fitActual"}, {"ratingKeys", "always"}};
    } else if (presetId != QLatin1String("mac")) {
        return;
    }
    InputBindings b;
    for (const auto& s : inputSlots()) {
        if (s.id == QLatin1String("endOfCollection"))
            continue; // navigation setting, not part of the input profile
        const QString wanted = p.value(s.id, s.defaultChoice);
        b.setChoice(s.id, isChoiceAvailable(s.id, wanted) ? wanted : s.defaultChoice);
    }
    b.setShortcuts(Action::ToggleFullScreen, fullScreen);
    b.setShortcuts(Action::Rename, rename);
}

bool InputBindings::isChoiceAvailable(const QString& slotId, const QString& choiceId) {
    static const QSet<QString> later = {
        QStringLiteral("middleClick:magnifier"), // F-IN-11 Lupe
        QStringLiteral("ratingKeys:always"),     // Bewertungen (F-CULL-01)
    };
    return !later.contains(slotId + QLatin1Char(':') + choiceId);
}

bool InputBindings::isActionAvailable(Action a) {
    switch (a) {
    case Action::ToggleGrid:
    case Action::Magnifier:
    case Action::Rate0:
    case Action::Rate1:
    case Action::Rate2:
    case Action::Rate3:
    case Action::Rate4:
    case Action::Rate5:
    case Action::RatePick:
    case Action::RateReject:
    case Action::RateClear:
        return false;
    default:
        return a != Action::None;
    }
}

// ---- shortcuts ----

QVector<Action> InputBindings::allActions() {
    QVector<Action> v;
    for (const auto& i : kActions)
        v.append(i.action);
    return v;
}

QList<QKeySequence> InputBindings::defaultShortcuts(Action a) {
    return fromStrings(defaultShortcutStrings(a));
}

QKeySequence InputBindings::shortcut(Action a) const {
    const auto list = shortcuts(a);
    return list.isEmpty() ? QKeySequence() : list.first();
}

QList<QKeySequence> InputBindings::shortcuts(Action a) const {
    auto it = m_shortcuts.constFind(int(a));
    return it != m_shortcuts.constEnd() ? *it : defaultShortcuts(a);
}

void InputBindings::setShortcut(Action a, const QKeySequence& seq) {
    setShortcuts(a, seq.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{seq});
}

void InputBindings::setShortcuts(Action a, const QList<QKeySequence>& seqs) {
    if (a == Action::None)
        return;
    QList<QKeySequence> clean;
    for (const auto& s : seqs)
        if (!s.isEmpty() && !clean.contains(s))
            clean.append(s);
    m_shortcuts.insert(int(a), clean);
    Settings::instance().setValue(shortcutKey(a), toStrings(clean));
}

void InputBindings::resetShortcuts() {
    Settings::instance().resetToDefaults(QString::fromLatin1(key::ShortcutPrefix));
    reload();
}

QVector<Action> InputBindings::conflictsWith(Action a) const {
    QVector<Action> out;
    const auto mine = shortcuts(a);
    if (mine.isEmpty())
        return out;
    for (Action other : allActions()) {
        if (other == a || isCullingAction(other) != isCullingAction(a))
            continue;
        const auto theirs = shortcuts(other);
        for (const auto& s : mine)
            if (theirs.contains(s)) {
                out.append(other);
                break;
            }
    }
    return out;
}

QVector<Action> InputBindings::conflicts() const {
    QVector<Action> out;
    for (Action a : allActions())
        if (!conflictsWith(a).isEmpty())
            out.append(a);
    return out;
}

// ---- wheel tuning (F-IN-03) ----

bool InputBindings::wheelInverted() const {
    return Settings::instance().value(key::WheelInverted).toBool();
}
int InputBindings::wheelStepsPerImage() const {
    return clampInt(Settings::instance().value(key::WheelStepsPerImage), 1, 5);
}
int InputBindings::wheelMinIntervalMs() const {
    return clampInt(Settings::instance().value(key::WheelMinIntervalMs), 0, 300);
}
int InputBindings::zoomStepPercent() const {
    return clampInt(Settings::instance().value(key::ZoomStepPercent), 5, 50);
}
int InputBindings::jumpSize() const {
    return clampInt(Settings::instance().value(key::JumpSize), 2, 100);
}

// ---- resolve ----

Action InputBindings::resolve(const InputEvent& ev) const {
    const Qt::KeyboardModifiers mods = ev.modifiers & kRelevantMods;
    auto pick = [](bool forward, Action fwd, Action back) { return forward ? fwd : back; };

    switch (ev.kind) {
    case InputKind::Wheel: {
        if (ev.wheelSteps == 0)
            return Action::None;
        QString slot = QStringLiteral("wheel");
        if (mods & Qt::ControlModifier)
            slot = QStringLiteral("wheel.cmd");
        else if (mods & Qt::AltModifier)
            slot = QStringLiteral("wheel.alt");
        else if (mods & Qt::ShiftModifier)
            slot = QStringLiteral("wheel.shift");
        const bool down = wheelInverted() ? ev.wheelSteps < 0 : ev.wheelSteps > 0;
        const QString c = choice(slot);
        if (c == QLatin1String("zoom"))
            return pick(down, Action::ZoomOut, Action::ZoomIn);
        if (c == QLatin1String("scrollOrImage") && ev.zoomed)
            return pick(down, Action::ScrollDown, Action::ScrollUp);
        if (c == QLatin1String("scrollHorizontal"))
            return pick(down, Action::ScrollRight, Action::ScrollLeft);
        return pick(down, Action::NextImage, Action::PreviousImage);
    }
    case InputKind::Click: {
        const QString c = choice(QStringLiteral("click"));
        if (c == QLatin1String("fitActual"))
            return Action::ToggleFitActualAtCursor;
        if (c == QLatin1String("image"))
            return ev.inLeftHalf ? Action::PreviousImage : Action::NextImage;
        return Action::None;
    }
    case InputKind::DoubleClick: {
        const QString c = choice(QStringLiteral("doubleClick"));
        if (c == QLatin1String("fitActual"))
            return Action::ToggleFitActualAtCursor;
        if (c == QLatin1String("close"))
            return Action::CloseWindow;
        return Action::ToggleFullScreen;
    }
    case InputKind::MiddleClick: {
        const QString c = choice(QStringLiteral("middleClick"));
        if (c == QLatin1String("fullscreen"))
            return Action::ToggleFullScreen;
        if (c == QLatin1String("close"))
            return Action::CloseWindow;
        if (c == QLatin1String("fitActual"))
            return Action::ToggleFitActualAtCursor;
        if (c == QLatin1String("magnifier"))
            return Action::Magnifier;
        return Action::None;
    }
    case InputKind::BackButton:
    case InputKind::ForwardButton:
        if (choice(QStringLiteral("backForward")) == QLatin1String("none"))
            return Action::None;
        return ev.kind == InputKind::BackButton ? Action::PreviousImage : Action::NextImage;
    case InputKind::Swipe: {
        if (ev.swipeDirection == 0)
            return Action::None;
        const QString c = choice(QStringLiteral("swipe"));
        const bool forward = ev.swipeDirection < 0; // fingers move left -> next (like Preview)
        if (c == QLatin1String("off"))
            return Action::None;
        if (c == QLatin1String("scroll") || ev.zoomed)
            return pick(forward, Action::ScrollRight, Action::ScrollLeft);
        return pick(forward, Action::NextImage, Action::PreviousImage);
    }
    case InputKind::Drag: {
        const bool dragOut = choice(QStringLiteral("drag")) == QLatin1String("dragOut");
        const bool alt = mods & Qt::AltModifier;
        return dragOut != alt ? Action::DragOut : Action::StartPan;
    }
    case InputKind::Key:
        break;
    }

    // ---- keys ----
    if (ev.key == 0 || ev.key == Qt::Key_unknown)
        return Action::None;
    const QKeySequence seq(QKeyCombination(mods, Qt::Key(ev.key)));
    // Printable non-letter keys like "?" or "+" arrive with Shift on many layouts.
    const bool shiftedSymbol = (mods & Qt::ShiftModifier) && ev.key >= 0x20 && ev.key < 0x7f &&
                               !(ev.key >= Qt::Key_A && ev.key <= Qt::Key_Z);
    const QKeySequence seqNoShift(QKeyCombination(mods & ~Qt::ShiftModifier, Qt::Key(ev.key)));
    auto matchShortcut = [&](bool culling) {
        for (Action a : allActions()) {
            if (isCullingAction(a) != culling)
                continue;
            const auto list = shortcuts(a);
            if (list.contains(seq) || (shiftedSymbol && list.contains(seqNoShift)))
                return a;
        }
        return Action::None;
    };
    // 1. Conflict rule (5.2): in culling mode (or if enabled) rating keys win over zoom digits.
    const bool ratingActive =
        ev.inCullingMode || choice(QStringLiteral("ratingKeys")) == QLatin1String("always");
    if (ratingActive) {
        if (Action a = matchShortcut(true); a != Action::None)
            return a;
    }
    // 2. Free shortcuts (user overrides win over the fixed slot keys).
    if (Action a = matchShortcut(false); a != Action::None) {
        if (ratingActive && (a == Action::ZoomActual || a == Action::ZoomFit || a == Action::ZoomFillWidth) &&
            mods == Qt::NoModifier && ev.key >= Qt::Key_0 && ev.key <= Qt::Key_9)
            return Action::None; // digits belong to ratings while rating keys are active
        return a;
    }
    // 3. Keys configured through slots (4.14).
    const bool noMods = mods == Qt::NoModifier;
    switch (ev.key) {
    case Qt::Key_Left:
    case Qt::Key_Right: {
        const bool fwd = ev.key == Qt::Key_Right;
        const bool alt = mods == Qt::AltModifier;
        if (!noMods && !alt)
            return Action::None;
        const Action img = pick(fwd, Action::NextImage, Action::PreviousImage);
        const Action scroll = pick(fwd, Action::ScrollRight, Action::ScrollLeft);
        if (!ev.zoomed)
            return alt ? pick(fwd, Action::NextFolder, Action::PreviousFolder) : img;
        const QString c = choice(QStringLiteral("arrowsZoomed"));
        if (c == QLatin1String("scrollThenImage"))
            return alt ? img : (ev.atScrollEdge ? img : scroll);
        if (c == QLatin1String("scroll"))
            return alt ? img : scroll;
        return alt ? scroll : img; // "image"; ⌥ pans (F-VIEW-04)
    }
    case Qt::Key_Up:
    case Qt::Key_Down: {
        if (!noMods)
            return Action::None;
        const bool fwd = ev.key == Qt::Key_Down;
        const QString c = choice(QStringLiteral("upDown"));
        const Action scroll = pick(fwd, Action::ScrollDown, Action::ScrollUp);
        if (c == QLatin1String("image"))
            return pick(fwd, Action::NextImage, Action::PreviousImage);
        if (c == QLatin1String("zoom"))
            return pick(fwd, Action::ZoomOut, Action::ZoomIn);
        if (ev.zoomed)
            return scroll;
        if (c == QLatin1String("scrollOrNothing"))
            return Action::None;
        return pick(fwd, Action::NextImage, Action::PreviousImage);
    }
    case Qt::Key_PageUp:
    case Qt::Key_PageDown: {
        if (!noMods)
            return Action::None;
        const bool fwd = ev.key == Qt::Key_PageDown;
        const QString c = choice(QStringLiteral("pageUpDown"));
        if (c == QLatin1String("jump"))
            return pick(fwd, Action::JumpForward, Action::JumpBackward);
        if (c == QLatin1String("folder"))
            return pick(fwd, Action::NextFolder, Action::PreviousFolder);
        if (c == QLatin1String("page") && ev.multiPage)
            return pick(fwd, Action::NextPage, Action::PreviousPage);
        if (c == QLatin1String("scroll") && ev.zoomed)
            return pick(fwd, Action::ScrollPageDown, Action::ScrollPageUp);
        return pick(fwd, Action::NextImage, Action::PreviousImage);
    }
    case Qt::Key_Home:
    case Qt::Key_End: {
        if (!noMods)
            return Action::None;
        const bool end = ev.key == Qt::Key_End;
        if (choice(QStringLiteral("homeEnd")) == QLatin1String("scroll") && ev.zoomed)
            return pick(end, Action::ScrollEnd, Action::ScrollHome);
        return pick(end, Action::LastImage, Action::FirstImage);
    }
    case Qt::Key_Space: {
        if (!noMods && mods != Qt::ShiftModifier)
            return Action::None;
        const bool fwd = noMods;
        const QString c = choice(QStringLiteral("space"));
        if (c == QLatin1String("fitActual"))
            return Action::ToggleFitActualAtCursor;
        if (c == QLatin1String("slideshow"))
            return Action::ToggleSlideshow;
        if (c == QLatin1String("animationOrImage") && ev.animated)
            return Action::ToggleAnimation;
        return pick(fwd, Action::NextImage, Action::PreviousImage);
    }
    case Qt::Key_Escape:
        if (!noMods)
            return Action::None;
        return choice(QStringLiteral("esc")) == QLatin1String("exitModeOrClose")
                   ? Action::ExitModeOrCloseWindow
                   : Action::ExitModeOrNothing;
    default:
        break;
    }
    return Action::None;
}

} // namespace lf
