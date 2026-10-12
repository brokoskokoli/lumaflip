// SPDX-License-Identifier: GPL-3.0-or-later
#include "InputBindings.h"

#include "Language.h"
#include "Platform.h"

#include <QSet>

#include "Settings.h"

#include <QCoreApplication>
#include <QKeyCombination>
#include <QMutex>

#include <algorithm>
#include <optional>

namespace lf {

namespace {

// tr() for the code in this file outside QObject classes (lupdate takes the context from it).
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::InputBindings)
};

struct ActionInfo {
    Action action;
    const char* id;
    const char* label;
};

// Order = order in the shortcut editor.
const ActionInfo kActions[] = {
    {Action::NextImage, "nextImage", QT_TRANSLATE_NOOP("lf::InputBindings", "Nächstes Bild")},
    {Action::PreviousImage, "previousImage", QT_TRANSLATE_NOOP("lf::InputBindings", "Vorheriges Bild")},
    {Action::FirstImage, "firstImage", QT_TRANSLATE_NOOP("lf::InputBindings", "Erstes Bild")},
    {Action::LastImage, "lastImage", QT_TRANSLATE_NOOP("lf::InputBindings", "Letztes Bild")},
    {Action::JumpForward, "jumpForward", QT_TRANSLATE_NOOP("lf::InputBindings", "Mehrere Bilder vor")},
    {Action::JumpBackward, "jumpBackward", QT_TRANSLATE_NOOP("lf::InputBindings", "Mehrere Bilder zurück")},
    {Action::NextFolder, "nextFolder", QT_TRANSLATE_NOOP("lf::InputBindings", "Nächster Ordner")},
    {Action::PreviousFolder, "previousFolder", QT_TRANSLATE_NOOP("lf::InputBindings", "Vorheriger Ordner")},
    {Action::NextPage, "nextPage", QT_TRANSLATE_NOOP("lf::InputBindings", "Nächste Seite")},
    {Action::PreviousPage, "previousPage", QT_TRANSLATE_NOOP("lf::InputBindings", "Vorherige Seite")},
    {Action::ZoomIn, "zoomIn", QT_TRANSLATE_NOOP("lf::InputBindings", "Vergrößern")},
    {Action::ZoomOut, "zoomOut", QT_TRANSLATE_NOOP("lf::InputBindings", "Verkleinern")},
    {Action::ZoomActual, "zoomActual", QT_TRANSLATE_NOOP("lf::InputBindings", "100 %")},
    {Action::ZoomFit, "zoomFit", QT_TRANSLATE_NOOP("lf::InputBindings", "Einpassen")},
    {Action::ZoomFillWidth, "zoomFillWidth", QT_TRANSLATE_NOOP("lf::InputBindings", "Breite füllen")},
    {Action::ToggleFitActualAtCursor, "toggleFitActual",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Einpassen ↔ 100 % am Mauszeiger")},
    {Action::ToggleZoomLock, "toggleZoomLock", QT_TRANSLATE_NOOP("lf::InputBindings", "Zoom fixieren")},
    {Action::ResetZoom, "resetZoom",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Standard-Zoom wiederherstellen")},
    {Action::ScrollUp, "scrollUp", QT_TRANSLATE_NOOP("lf::InputBindings", "Ausschnitt nach oben")},
    {Action::ScrollDown, "scrollDown", QT_TRANSLATE_NOOP("lf::InputBindings", "Ausschnitt nach unten")},
    {Action::ScrollLeft, "scrollLeft", QT_TRANSLATE_NOOP("lf::InputBindings", "Ausschnitt nach links")},
    {Action::ScrollRight, "scrollRight", QT_TRANSLATE_NOOP("lf::InputBindings", "Ausschnitt nach rechts")},
    {Action::ScrollPageUp, "scrollPageUp",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Ausschnitt eine Seite nach oben")},
    {Action::ScrollPageDown, "scrollPageDown",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Ausschnitt eine Seite nach unten")},
    {Action::ScrollHome, "scrollHome", QT_TRANSLATE_NOOP("lf::InputBindings", "Zum Bildanfang scrollen")},
    {Action::ScrollEnd, "scrollEnd", QT_TRANSLATE_NOOP("lf::InputBindings", "Zum Bildende scrollen")},
    {Action::ToggleFullScreen, "toggleFullScreen", QT_TRANSLATE_NOOP("lf::InputBindings", "Vollbild")},
    {Action::ExitModeOrNothing, "exitMode",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Vollbild oder Modus beenden")},
    {Action::ExitModeOrCloseWindow, "exitModeOrClose",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Modus beenden, sonst Fenster schließen")},
    {Action::CloseWindow, "closeWindow", QT_TRANSLATE_NOOP("lf::InputBindings", "Fenster schließen")},
    {Action::CloseAllWindows, "closeAllWindows",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Alle Fenster schließen")},
    {Action::ToggleAnimation, "toggleAnimation",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Animation abspielen/pausieren")},
    {Action::NextFrame, "nextFrame", QT_TRANSLATE_NOOP("lf::InputBindings", "Nächstes Einzelbild")},
    {Action::PreviousFrame, "previousFrame", QT_TRANSLATE_NOOP("lf::InputBindings", "Vorheriges Einzelbild")},
    {Action::ToggleSlideshow, "toggleSlideshow", QT_TRANSLATE_NOOP("lf::InputBindings", "Diashow")},
    {Action::RotateLeft, "rotateLeft", QT_TRANSLATE_NOOP("lf::InputBindings", "Links drehen")},
    {Action::RotateRight, "rotateRight", QT_TRANSLATE_NOOP("lf::InputBindings", "Rechts drehen")},
    {Action::FlipHorizontal, "flipHorizontal", QT_TRANSLATE_NOOP("lf::InputBindings", "Horizontal spiegeln")},
    {Action::FlipVertical, "flipVertical", QT_TRANSLATE_NOOP("lf::InputBindings", "Vertikal spiegeln")},
    {Action::SaveRotation, "saveRotation", QT_TRANSLATE_NOOP("lf::InputBindings", "Drehung sichern")},
    {Action::Trash, "trash", QT_TRANSLATE_NOOP("lf::InputBindings", "In den Papierkorb legen")},
    {Action::Undo, "undo", QT_TRANSLATE_NOOP("lf::InputBindings", "Widerrufen")},
    {Action::Redo, "redo", QT_TRANSLATE_NOOP("lf::InputBindings", "Wiederholen")},
    {Action::Rename, "rename", QT_TRANSLATE_NOOP("lf::InputBindings", "Umbenennen")},
    {Action::RevealInFinder, "revealInFinder", QT_TRANSLATE_NOOP("lf::InputBindings", "Im Finder zeigen")},
    {Action::CopyImage, "copyImage", QT_TRANSLATE_NOOP("lf::InputBindings", "Bild kopieren")},
    {Action::CopyPath, "copyPath", QT_TRANSLATE_NOOP("lf::InputBindings", "Pfad kopieren")},
    {Action::OpenInEditor, "openInEditor", QT_TRANSLATE_NOOP("lf::InputBindings", "Im Editor öffnen")},
    {Action::ToggleInfoPanel, "toggleInfoPanel", QT_TRANSLATE_NOOP("lf::InputBindings", "Infopanel")},
    {Action::ToggleFilmstrip, "toggleFilmstrip", QT_TRANSLATE_NOOP("lf::InputBindings", "Filmstreifen")},
    {Action::ToggleGrid, "toggleGrid", QT_TRANSLATE_NOOP("lf::InputBindings", "Raster")},
    {Action::ShowShortcuts, "showShortcuts",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Tastenkürzel-Übersicht")},
    {Action::OpenPreferences, "openPreferences", QT_TRANSLATE_NOOP("lf::InputBindings", "Einstellungen")},
    {Action::NewWindow, "newWindow", QT_TRANSLATE_NOOP("lf::InputBindings", "Neues Fenster")},
    {Action::DuplicateWindow, "duplicateWindow",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Fenster duplizieren")},
    {Action::Magnifier, "magnifier", QT_TRANSLATE_NOOP("lf::InputBindings", "Lupe (gedrückt halten)")},
    {Action::DragOut, "dragOut", QT_TRANSLATE_NOOP("lf::InputBindings", "Datei herausziehen")},
    {Action::StartPan, "startPan", QT_TRANSLATE_NOOP("lf::InputBindings", "Ausschnitt verschieben")},
    {Action::Rate0, "rate0", QT_TRANSLATE_NOOP("lf::InputBindings", "Bewertung entfernen (0 Sterne)")},
    {Action::Rate1, "rate1", QT_TRANSLATE_NOOP("lf::InputBindings", "1 Stern")},
    {Action::Rate2, "rate2", QT_TRANSLATE_NOOP("lf::InputBindings", "2 Sterne")},
    {Action::Rate3, "rate3", QT_TRANSLATE_NOOP("lf::InputBindings", "3 Sterne")},
    {Action::Rate4, "rate4", QT_TRANSLATE_NOOP("lf::InputBindings", "4 Sterne")},
    {Action::Rate5, "rate5", QT_TRANSLATE_NOOP("lf::InputBindings", "5 Sterne")},
    {Action::RatePick, "ratePick", QT_TRANSLATE_NOOP("lf::InputBindings", "Markieren")},
    {Action::RateReject, "rateReject", QT_TRANSLATE_NOOP("lf::InputBindings", "Ablehnen")},
    {Action::RateClear, "rateClear", QT_TRANSLATE_NOOP("lf::InputBindings", "Markierung aufheben")},
    {Action::ToggleCullingMode, "toggleCullingMode",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Aussortiermodus")},
    {Action::QuickMove, "quickMove",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Schnellablage: verschieben (M, dann 1–9)")},
    {Action::QuickCopy, "quickCopy",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Schnellablage: kopieren (C, dann 1–9)")},
    {Action::GoToImage, "goToImage", QT_TRANSLATE_NOOP("lf::InputBindings", "Gehe zu Bild …")},
    {Action::ToggleShuffle, "toggleShuffle", QT_TRANSLATE_NOOP("lf::InputBindings", "Zufällige Reihenfolge")},
    {Action::FilterByName, "filterByName", QT_TRANSLATE_NOOP("lf::InputBindings", "Nach Dateinamen filtern")},
    {Action::CompareWithNext, "compareWithNext",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Mit nächstem vergleichen")},
    {Action::ToggleAlwaysOnTop, "toggleAlwaysOnTop",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Immer im Vordergrund")},
    {Action::ToggleMinimalWindow, "toggleMinimalWindow",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Minimaler Fenstermodus")},
    {Action::CoupleWindows, "coupleWindows", QT_TRANSLATE_NOOP("lf::InputBindings", "Fenster koppeln …")},
    {Action::ToggleHdr, "toggleHdr", QT_TRANSLATE_NOOP("lf::InputBindings", "HDR anzeigen")},
    {Action::ExposureIncrease, "exposureIncrease",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Belichtung erhöhen")},
    {Action::ExposureDecrease, "exposureDecrease",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Belichtung verringern")},
    {Action::ExposureReset, "exposureReset",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Belichtung zurücksetzen")},
    {Action::ToggleExposureHud, "toggleExposureHud",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Belichtungsregler einblenden")},
    {Action::ToggleSubfolders, "toggleSubfolders",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Unterordner einbeziehen")},
    {Action::ToggleRawJpeg, "toggleRawJpeg",
     QT_TRANSLATE_NOOP("lf::InputBindings", "RAW/JPEG des Paares anzeigen")},
    {Action::ToggleFolderMap, "toggleFolderMap", QT_TRANSLATE_NOOP("lf::InputBindings", "Ordnerkarte")},
    {Action::QuickLook, "quickLook", QT_TRANSLATE_NOOP("lf::InputBindings", "Quick Look")},
    {Action::ToggleLiveText, "toggleLiveText", QT_TRANSLATE_NOOP("lf::InputBindings", "Live Text")},
    {Action::SetWallpaper, "setWallpaper",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Als Schreibtischhintergrund festlegen")},
    {Action::ShowActionLog, "showActionLog",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Protokoll der Aktionen")},
    {Action::ToggleRightToLeft, "toggleRightToLeft",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Comics von rechts nach links lesen")},
    {Action::CropImage, "cropImage", QT_TRANSLATE_NOOP("lf::InputBindings", "Zuschneiden")},
    {Action::ExportImage, "exportImage", QT_TRANSLATE_NOOP("lf::InputBindings", "Exportieren")},
    {Action::ToggleClipping, "toggleClipping",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Über-/Unterbelichtungswarnung")},
    {Action::ToggleFocusPeaking, "toggleFocusPeaking",
     QT_TRANSLATE_NOOP("lf::InputBindings", "Focus Peaking")},
    {Action::ToggleAfPoints, "toggleAfPoints", QT_TRANSLATE_NOOP("lf::InputBindings", "Fokuspunkte")},
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
    case Action::NextFolder: // F-OPEN-09: ⌥→ / ⌥← (only while not zoomed, see resolve())
        return {"Alt+Right"};
    case Action::PreviousFolder:
        return {"Alt+Left"};
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
    // F-VIEW-14: Enter (Return and the keypad's Enter) while the zoom of the image was
    // changed; otherwise Enter keeps renaming (F-FILE-04), see resolve().
    case Action::ResetZoom:
        return {"Return", "Enter"};
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
    case Action::ToggleCullingMode:
        return {"Ctrl+K", "K"};
    case Action::QuickMove:
        return {"M"};
    case Action::QuickCopy:
        return {"C"};
    case Action::GoToImage:
        return {"Ctrl+G"};
    case Action::FilterByName:
        return {"Ctrl+F"};
    // F-COL-04/05 (SPEC 5.2 has no keys for them; these collide with nothing there and are
    // free in macOS: ⇧⌘H, ⌥⌘+ / ⌥⌘− / ⌥⌘0, ⌥⌘E).
    case Action::ToggleHdr:
        return {"Shift+Ctrl+H"};
    case Action::ExposureIncrease:
        return {"Alt+Ctrl+=", "Alt+Ctrl++"};
    case Action::ExposureDecrease:
        return {"Alt+Ctrl+-"};
    case Action::ExposureReset:
        return {"Alt+Ctrl+0"};
    case Action::ToggleExposureHud:
        return {"Alt+Ctrl+E"};
    // F-INFO-08 (not in SPEC 5.2): ⇧⌘M is free in macOS and in the table above.
    case Action::ToggleFolderMap:
        return {"Shift+Ctrl+M"};
    // M4 (not in SPEC 5.2): ⌘Y is Quick Look in Finder (Space flips images here); ⇧⌘L is
    // free in macOS and in the table above (⌘L rotates, L is the magnifier).
    case Action::QuickLook:
        return {"Ctrl+Y"};
    case Action::ToggleLiveText:
        return {"Shift+Ctrl+L"};
    // M4 part 2 (not in SPEC 5.2), free in macOS and above: ⇧C crop (C alone is the quick
    // copy prefix), ⇧⌘E export (⌥⌘E is the exposure slider), J / ⇧J clipping and focus
    // peaking (J as in common photo tools), ⇧A AF points.
    case Action::CropImage:
        return {"Shift+C"};
    case Action::ExportImage:
        return {"Shift+Ctrl+E"};
    case Action::ToggleClipping:
        return {"J"};
    case Action::ToggleFocusPeaking:
        return {"Shift+J"};
    case Action::ToggleAfPoints:
        return {"Shift+A"};
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
    if (a == Action::RevealInFinder) {
        // Named after the platform's file manager (menu, error card, shortcut list).
        switch (platformFileManager()) {
        case FileManager::Explorer:
            return Tr::tr("Im Explorer zeigen");
        case FileManager::Other:
            return Tr::tr("Im Dateimanager zeigen");
        case FileManager::Finder:
            break;
        }
    }
    const ActionInfo* i = info(a);
    if (!i)
        return Tr::tr("Nichts");
    return Tr::tr(i->label);
}

// ---- slots (SPEC 4.14) ----

QVector<InputSlot> InputBindings::inputSlots() {
    // Rebuilt when the UI language changed (labels are translated, F-I18N-03). Returned as
    // a (shared) copy: a reference would outlive the mutex while another thread rebuilds.
    static QMutex mutex;
    static QVector<InputSlot> slotTable;
    static int builtFor = -1;
    QMutexLocker lock(&mutex);
    const int generation = languageGeneration();
    if (builtFor == generation)
        return slotTable;
    builtFor = generation;
    slotTable = [] {
        QVector<InputSlot> v;
        auto add = [&v](const char* id, const char* label,
                        std::initializer_list<std::tuple<const char*, const char*, const char*>> choices,
                        const char* def) {
            InputSlot s;
            s.id = QString::fromLatin1(id);
            s.label = Tr::tr(label);
            for (const auto& [cid, clabel, chelp] : choices) {
                s.choices << QString::fromLatin1(cid);
                s.choiceLabels << Tr::tr(clabel);
                s.choiceHelp << Tr::tr(chelp);
            }
            s.defaultChoice = QString::fromLatin1(def);
            v.append(s);
        };
        // F-IN-01
        add("wheel", QT_TRANSLATE_NOOP("lf::InputBindings", "Mausrad"),
            {{"image", QT_TRANSLATE_NOOP("lf::InputBindings", "Bild wechseln"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Nach unten drehen zeigt das nächste, nach oben das vorherige Bild.")},
             {"zoom", QT_TRANSLATE_NOOP("lf::InputBindings", "Zoomen (am Mauszeiger)"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Nach oben drehen vergrößert, nach unten verkleinert – am Mauszeiger.")},
             {"scrollOrImage",
              QT_TRANSLATE_NOOP("lf::InputBindings", "Ausschnitt verschieben (sonst Bild wechseln)"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Gezoomt wird der Ausschnitt verschoben, sonst wird das Bild gewechselt.")}},
            "image");
        // F-IN-02
        const auto wheelMod = {
            std::tuple<const char*, const char*, const char*>{
                "image", QT_TRANSLATE_NOOP("lf::InputBindings", "Bild wechseln"),
                QT_TRANSLATE_NOOP("lf::InputBindings", "Bild wechseln, solange die Taste gehalten wird.")},
            {"zoom", QT_TRANSLATE_NOOP("lf::InputBindings", "Zoomen (am Mauszeiger)"),
             QT_TRANSLATE_NOOP("lf::InputBindings",
                               "Zoomen am Mauszeiger, solange die Taste gehalten wird.")},
            {"scrollOrImage",
             QT_TRANSLATE_NOOP("lf::InputBindings", "Ausschnitt verschieben (sonst Bild wechseln)"),
             QT_TRANSLATE_NOOP("lf::InputBindings",
                               "Gezoomt wird der Ausschnitt verschoben, sonst wird das Bild gewechselt.")},
            {"scrollHorizontal", QT_TRANSLATE_NOOP("lf::InputBindings", "Horizontal scrollen"),
             QT_TRANSLATE_NOOP("lf::InputBindings", "Der Ausschnitt wird waagerecht verschoben.")}};
        add("wheel.cmd", QT_TRANSLATE_NOOP("lf::InputBindings", "Mausrad mit ⌘"), wheelMod, "zoom");
        add("wheel.alt", QT_TRANSLATE_NOOP("lf::InputBindings", "Mausrad mit ⌥"), wheelMod, "scrollOrImage");
        add("wheel.shift", QT_TRANSLATE_NOOP("lf::InputBindings", "Mausrad mit ⇧"), wheelMod,
            "scrollHorizontal");
        // F-IN-04
        add("pageUpDown", QT_TRANSLATE_NOOP("lf::InputBindings", "Bild↑ / Bild↓"),
            {{"image", QT_TRANSLATE_NOOP("lf::InputBindings", "Vorheriges/nächstes Bild"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Wechselt ein Bild zurück bzw. vor.")},
             {"jump", QT_TRANSLATE_NOOP("lf::InputBindings", "Mehrere Bilder zurück/vor"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Springt um die eingestellte Sprungweite (Standard 10).")},
             {"folder", QT_TRANSLATE_NOOP("lf::InputBindings", "Vorheriger/nächster Ordner"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Wechselt in den benachbarten Ordner.")},
             {"page", QT_TRANSLATE_NOOP("lf::InputBindings", "Seite zurück/vor (sonst Bild)"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Bei mehrseitigen Dateien wird geblättert, sonst das Bild gewechselt.")},
             {"scroll",
              QT_TRANSLATE_NOOP("lf::InputBindings", "Ausschnitt seitenweise verschieben (sonst Bild)"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Gezoomt um eine Bildschirmhöhe scrollen, sonst Bild wechseln.")}},
            "image");
        // F-IN-05
        add("homeEnd", QT_TRANSLATE_NOOP("lf::InputBindings", "Pos1 / Ende"),
            {{"image", QT_TRANSLATE_NOOP("lf::InputBindings", "Erstes/letztes Bild"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Springt zum ersten bzw. letzten Bild der Sammlung.")},
             {"scroll",
              QT_TRANSLATE_NOOP("lf::InputBindings", "Bildanfang/-ende (sonst erstes/letztes Bild)"),
              QT_TRANSLATE_NOOP(
                  "lf::InputBindings",
                  "Gezoomt an den Anfang bzw. das Ende des Bildes scrollen, sonst erstes/letztes Bild.")}},
            "image");
        // F-IN-06
        add("space", QT_TRANSLATE_NOOP("lf::InputBindings", "Leertaste / ⇧Leertaste"),
            {{"image", QT_TRANSLATE_NOOP("lf::InputBindings", "Nächstes/vorheriges Bild"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Leertaste: nächstes Bild, mit ⇧: vorheriges Bild.")},
             {"animationOrImage",
              QT_TRANSLATE_NOOP("lf::InputBindings", "Animation abspielen/pausieren, sonst Bild"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Bei animierten Bildern Wiedergabe umschalten, sonst Bild wechseln.")},
             {"fitActual", QT_TRANSLATE_NOOP("lf::InputBindings", "Einpassen ↔ 100 %"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Schaltet zwischen Einpassen und 100 % um.")},
             {"slideshow", QT_TRANSLATE_NOOP("lf::InputBindings", "Diashow starten/pausieren"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Startet bzw. pausiert die Diashow.")}},
            "animationOrImage");
        // F-IN-07
        add("arrowsZoomed", QT_TRANSLATE_NOOP("lf::InputBindings", "← / →, wenn gezoomt"),
            {{"image", QT_TRANSLATE_NOOP("lf::InputBindings", "Immer Bild wechseln"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Pfeiltasten wechseln das Bild; mit ⌥ wird der Ausschnitt verschoben.")},
             {"scrollThenImage",
              QT_TRANSLATE_NOOP("lf::InputBindings", "Erst bis zum Rand verschieben, dann Bild wechseln"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Der Ausschnitt wird verschoben; am Bildrand wechselt das Bild.")},
             {"scroll", QT_TRANSLATE_NOOP("lf::InputBindings", "Ausschnitt verschieben (Bild mit ⌥←/⌥→)"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Pfeiltasten verschieben den Ausschnitt; Bildwechsel mit ⌥←/⌥→.")}},
            "image");
        // F-IN-08
        add("upDown", QT_TRANSLATE_NOOP("lf::InputBindings", "↑ / ↓"),
            {{"scrollOrImage",
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Ausschnitt verschieben, sonst vorheriges/nächstes Bild"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Gezoomt wird der Ausschnitt verschoben, sonst das Bild gewechselt.")},
             {"scrollOrNothing",
              QT_TRANSLATE_NOOP("lf::InputBindings", "Ausschnitt verschieben (wenn gezoomt), sonst nichts"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Gezoomt wird der Ausschnitt verschoben, sonst passiert nichts.")},
             {"image", QT_TRANSLATE_NOOP("lf::InputBindings", "Vorheriges/nächstes Bild"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "↑ zeigt das vorherige, ↓ das nächste Bild.")},
             {"zoom", QT_TRANSLATE_NOOP("lf::InputBindings", "Zoomen"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "↑ vergrößert, ↓ verkleinert.")}},
            "scrollOrImage");
        // F-IN-09
        add("click", QT_TRANSLATE_NOOP("lf::InputBindings", "Einfacher Klick ins Bild"),
            {{"none", QT_TRANSLATE_NOOP("lf::InputBindings", "Nichts"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Ein Klick hat keine Wirkung (Ziehen verschiebt weiterhin).")},
             {"fitActual", QT_TRANSLATE_NOOP("lf::InputBindings", "Einpassen ↔ 100 % am Mauszeiger"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Ein Klick zoomt auf 100 % an der Klickstelle und zurück.")},
             {"image", QT_TRANSLATE_NOOP("lf::InputBindings", "Nächstes Bild (linke Hälfte: vorheriges)"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Klick in die rechte Hälfte: nächstes Bild, linke Hälfte: vorheriges.")}},
            "none");
        // F-IN-10
        add("doubleClick", QT_TRANSLATE_NOOP("lf::InputBindings", "Doppelklick ins Bild"),
            {{"fullscreen", QT_TRANSLATE_NOOP("lf::InputBindings", "Vollbild umschalten"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Doppelklick wechselt in das Vollbild und zurück.")},
             {"fitActual", QT_TRANSLATE_NOOP("lf::InputBindings", "Einpassen ↔ 100 %"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Doppelklick schaltet zwischen Einpassen und 100 % um.")},
             {"close", QT_TRANSLATE_NOOP("lf::InputBindings", "Fenster schließen"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Doppelklick schließt das Fenster.")}},
            "fullscreen");
        // F-IN-11
        add("middleClick", QT_TRANSLATE_NOOP("lf::InputBindings", "Mittlere Maustaste"),
            {{"none", QT_TRANSLATE_NOOP("lf::InputBindings", "Nichts"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Die mittlere Maustaste hat keine Wirkung.")},
             {"fullscreen", QT_TRANSLATE_NOOP("lf::InputBindings", "Vollbild"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Schaltet das Vollbild um.")},
             {"close", QT_TRANSLATE_NOOP("lf::InputBindings", "Fenster schließen"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Schließt das Fenster.")},
             {"fitActual", QT_TRANSLATE_NOOP("lf::InputBindings", "Einpassen ↔ 100 %"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Schaltet zwischen Einpassen und 100 % an der Klickstelle um.")},
             {"magnifier", QT_TRANSLATE_NOOP("lf::InputBindings", "Lupe (gedrückt halten)"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Zeigt eine Lupe, solange die Taste gedrückt ist.")}},
            "fitActual");
        // F-IN-12
        add("backForward", QT_TRANSLATE_NOOP("lf::InputBindings", "Maustasten 4/5 (Zurück/Vor)"),
            {{"image", QT_TRANSLATE_NOOP("lf::InputBindings", "Vorheriges/nächstes Bild"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Zurück-Taste: vorheriges Bild, Vor-Taste: nächstes Bild.")},
             {"none", QT_TRANSLATE_NOOP("lf::InputBindings", "Nichts"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Die Tasten haben keine Wirkung.")}},
            "image");
        // F-IN-13
        add("swipe", QT_TRANSLATE_NOOP("lf::InputBindings", "Trackpad-Wischen (zwei Finger, horizontal)"),
            {{"image",
              QT_TRANSLATE_NOOP("lf::InputBindings", "Bild wechseln (gezoomt: Ausschnitt verschieben)"),
              QT_TRANSLATE_NOOP(
                  "lf::InputBindings",
                  "Wischen wechselt das Bild; im gezoomten Zustand wird der Ausschnitt verschoben.")},
             {"scroll", QT_TRANSLATE_NOOP("lf::InputBindings", "Nur Ausschnitt verschieben"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Wischen verschiebt immer nur den Ausschnitt.")},
             {"off", QT_TRANSLATE_NOOP("lf::InputBindings", "Aus"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Wischen wird ignoriert.")}},
            "image");
        // F-IN-14
        add("drag", QT_TRANSLATE_NOOP("lf::InputBindings", "Ziehen mit linker Maustaste"),
            {{"pan", QT_TRANSLATE_NOOP("lf::InputBindings", "Ausschnitt verschieben (⌥: Datei herausziehen)"),
              QT_TRANSLATE_NOOP(
                  "lf::InputBindings",
                  "Ziehen verschiebt den Ausschnitt; mit ⌥ wird die Datei in andere Apps gezogen.")},
             {"dragOut",
              QT_TRANSLATE_NOOP("lf::InputBindings", "Datei herausziehen (⌥: Ausschnitt verschieben)"),
              QT_TRANSLATE_NOOP(
                  "lf::InputBindings",
                  "Ziehen zieht die Datei in andere Apps; mit ⌥ wird der Ausschnitt verschoben.")}},
            "pan");
        // F-IN-15
        add("esc", QT_TRANSLATE_NOOP("lf::InputBindings", "Esc"),
            {{"exitMode", QT_TRANSLATE_NOOP("lf::InputBindings", "Vollbild oder Modus beenden, sonst nichts"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Esc beendet Vollbild, Diashow oder andere Modi und tut sonst nichts.")},
             {"exitModeOrClose",
              QT_TRANSLATE_NOOP("lf::InputBindings", "Vollbild oder Modus beenden, sonst Fenster schließen"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Esc beendet Modi; ist keiner aktiv, wird das Fenster geschlossen.")}},
            "exitMode");
        // F-IN-16 (mirrors key::WrapMode / F-NAV-03)
        add("endOfCollection", QT_TRANSLATE_NOOP("lf::InputBindings", "Ende der Sammlung"),
            {{"stop", QT_TRANSLATE_NOOP("lf::InputBindings", "Anhalten"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Am Ende bleibt das letzte Bild stehen (mit kurzem Hinweis).")},
             {"wrap", QT_TRANSLATE_NOOP("lf::InputBindings", "Umlaufen"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Nach dem letzten Bild folgt wieder das erste.")},
             {"nextFolder", QT_TRANSLATE_NOOP("lf::InputBindings", "Zum nächsten Ordner"),
              QT_TRANSLATE_NOOP("lf::InputBindings",
                                "Nach dem letzten Bild geht es im nächsten Ordner weiter.")}},
            "stop");
        // Not in the 4.14 table, needed for the F-IN-18 preset QT_TRANSLATE_NOOP("lf::InputBindings",
        // "Fotograf:in") and the 5.2 conflict rule: digits/P/X/U rate only in culling mode unless enabled
        // here.
        add("ratingKeys", QT_TRANSLATE_NOOP("lf::InputBindings", "Bewertungstasten (0–5, P/X/U)"),
            {{"cullingOnly", QT_TRANSLATE_NOOP("lf::InputBindings", "Nur im Aussortiermodus"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Ziffern wählen sonst den Zoommodus (1/2/3).")},
             {"always", QT_TRANSLATE_NOOP("lf::InputBindings", "Immer aktiv"),
              QT_TRANSLATE_NOOP("lf::InputBindings", "Ziffern vergeben immer Sterne, P/X/U setzen "
                                                     "Markierungen. Die Zoom-Ziffern sind dann aus.")}},
            "cullingOnly");
        return v;
    }();
    return slotTable;
}

namespace {
std::optional<InputSlot> findSlot(const QString& id) {
    for (const auto& s : InputBindings::inputSlots())
        if (s.id == id)
            return s;
    return std::nullopt;
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
    const std::optional<InputSlot> s = findSlot(slotId);
    return s ? s->defaultChoice : QString();
}

void InputBindings::setChoice(const QString& slotId, const QString& choiceId) {
    const std::optional<InputSlot> s = findSlot(slotId);
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
        // Lightroom-like: digits = stars, P/X/U flag (F-IN-18, M3)
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
    // M3: the magnifier (F-IN-11 "Lupe", F-VIEW-11) was the last planned choice. Choices of
    // later stages go into this set.
    static const QSet<QString> later = {};
    return !later.contains(slotId + QLatin1Char(':') + choiceId);
}

bool InputBindings::isActionAvailable(Action a) {
    // M3: the grid (F-GRID-02) and the magnifier (F-VIEW-11) were the last planned actions.
    return a != Action::None;
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
        // F-VIEW-14: ResetZoom acts only while the zoom was changed, else the key goes on.
        if (a == Action::ResetZoom || other == Action::ResetZoom)
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
    // fromCombined(): any key code (not every int is a Qt::Key enumerator).
    const QKeySequence seq(QKeyCombination::fromCombined(mods.toInt() | ev.key));
    // Printable non-letter keys like "?" or "+" arrive with Shift on many layouts.
    const bool shiftedSymbol = (mods & Qt::ShiftModifier) && ev.key >= 0x20 && ev.key < 0x7f &&
                               !(ev.key >= Qt::Key_A && ev.key <= Qt::Key_Z);
    const QKeySequence seqNoShift(
        QKeyCombination::fromCombined((mods & ~Qt::ShiftModifier).toInt() | ev.key));
    auto matchShortcut = [&](bool culling) {
        for (Action a : allActions()) {
            if (isCullingAction(a) != culling || a == Action::ResetZoom)
                continue;
            const auto list = shortcuts(a);
            if (list.contains(seq) || (shiftedSymbol && list.contains(seqNoShift)))
                return a;
        }
        return Action::None;
    };
    // 0. F-VIEW-14: while the zoom of the image was changed its keys reset it (Enter); else
    // they keep their other meaning (rename, full screen) below.
    if (ev.zoomChanged) {
        const auto reset = shortcuts(Action::ResetZoom);
        if (reset.contains(seq) || (shiftedSymbol && reset.contains(seqNoShift)))
            return Action::ResetZoom;
    }
    // 1. Conflict rule (5.2): in culling mode (or if enabled) rating keys win over zoom digits.
    const bool ratingActive =
        ev.inCullingMode || choice(QStringLiteral("ratingKeys")) == QLatin1String("always");
    if (ratingActive) {
        if (Action a = matchShortcut(true); a != Action::None)
            return a;
    }
    // 2. Free shortcuts (user overrides win over the fixed slot keys).
    Action free = matchShortcut(false);
    // F-OPEN-09: the folder shortcuts on ⌥←/⌥→ apply while not zoomed; zoomed, the arrow
    // slot decides (F-IN-07: pan or flip, F-VIEW-04).
    if ((free == Action::NextFolder || free == Action::PreviousFolder) && ev.zoomed &&
        (ev.key == Qt::Key_Left || ev.key == Qt::Key_Right))
        free = Action::None;
    if (Action a = free; a != Action::None) {
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
            return alt ? Action::None : img; // ⌥←/⌥→: the (configurable) folder shortcuts above
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
