// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Maps input events (keys, wheel, mouse buttons, gestures) to actions according to
// the user settings (SPEC 4.14, 5.2). Pure logic, no widgets: the ImageView/window
// translates Qt events into InputEvent and executes the returned Action.

#include <QHash>
#include <QKeySequence>
#include <QString>
#include <QStringList>
#include <QVector>

namespace lf {

enum class Action {
    None,
    NextImage, PreviousImage, FirstImage, LastImage,
    JumpForward, JumpBackward,            // n images (setting)
    NextFolder, PreviousFolder,           // M3
    NextPage, PreviousPage,               // multi-page
    ZoomIn, ZoomOut, ZoomActual, ZoomFit, ZoomFillWidth, ToggleFitActualAtCursor, ToggleZoomLock,
    ScrollUp, ScrollDown, ScrollLeft, ScrollRight, ScrollPageUp, ScrollPageDown, ScrollHome, ScrollEnd,
    ToggleFullScreen, ExitModeOrNothing, ExitModeOrCloseWindow, CloseWindow,
    ToggleAnimation, NextFrame, PreviousFrame,
    ToggleSlideshow,
    RotateLeft, RotateRight, FlipHorizontal, FlipVertical,
    Trash, Rename, RevealInFinder, CopyImage, CopyPath, OpenInEditor,
    ToggleInfoPanel, ToggleFilmstrip, ToggleGrid, ShowShortcuts,
    NewWindow, DuplicateWindow,
    Magnifier,                            // hold
    DragOut,
    StartPan,
};

QString actionId(Action a);          // stable string id for settings ("nextImage")
Action actionFromId(const QString& id);
QString actionLabel(Action a);       // German UI label

enum class InputKind { Key, Wheel, Click, DoubleClick, MiddleClick, BackButton, ForwardButton, Swipe, Drag };

struct InputEvent {
    InputKind kind = InputKind::Key;
    int key = 0;                 // Qt::Key for Key
    Qt::KeyboardModifiers modifiers;
    int wheelSteps = 0;          // +down/-up after accumulation (Wheel)
    int swipeDirection = 0;      // -1 left, +1 right (Swipe)
    bool zoomed = false;         // image larger than viewport
    bool animated = false;       // current image is animated
    bool atScrollEdge = false;   // for "scroll then flip" (F-IN-07)
    bool inCullingMode = false;
};

// Configurable "slots" of SPEC 4.14 (F-IN-01..16). Each slot has a set of
// allowed choices; the choice id is persisted under Settings key "input/<slot>".
struct InputSlot {
    QString id;                  // e.g. "wheel", "wheel.cmd", "pageUpDown", "space", ...
    QString label;               // German
    QStringList choices;         // choice ids
    QStringList choiceLabels;    // German
    QString defaultChoice;
};

class InputBindings {
public:
    InputBindings();                        // loads from Settings::instance()
    static const QVector<InputSlot>& inputSlots();  // ("slots" is a Qt keyword)
    // Presets (F-IN-18): "mac", "windows", "photographer".
    static void applyPreset(const QString& presetId);

    QString choice(const QString& slotId) const;
    void setChoice(const QString& slotId, const QString& choiceId);  // persists

    // Free key shortcuts (F-SET-02) for actions not covered by slots.
    QKeySequence shortcut(Action a) const;
    void setShortcut(Action a, const QKeySequence& seq);
    QVector<Action> conflicts() const;

    // Core: resolve an input event to an action.
    Action resolve(const InputEvent& ev) const;
    // Wheel tuning (F-IN-03)
    bool wheelInverted() const;
    int wheelStepsPerImage() const;
    int wheelMinIntervalMs() const;
    int zoomStepPercent() const;
    int jumpSize() const;  // for JumpForward/JumpBackward

    void reload();

private:
    QHash<QString, QString> m_choices;
    QHash<int, QKeySequence> m_shortcuts;
};

} // namespace lf
