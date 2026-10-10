// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Maps input events (keys, wheel, mouse buttons, gestures) to actions according to
// the user settings (SPEC 4.14, 5.2). Pure logic, no widgets: the ImageView/window
// translates Qt events into InputEvent and executes the returned Action.

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

namespace lf {

enum class Action {
    None,
    NextImage,
    PreviousImage,
    FirstImage,
    LastImage,
    JumpForward,
    JumpBackward, // n images (setting)
    NextFolder,
    PreviousFolder, // M3
    NextPage,
    PreviousPage, // multi-page
    ZoomIn,
    ZoomOut,
    ZoomActual,
    ZoomFit,
    ZoomFillWidth,
    ToggleFitActualAtCursor,
    ToggleZoomLock,
    ScrollUp,
    ScrollDown,
    ScrollLeft,
    ScrollRight,
    ScrollPageUp,
    ScrollPageDown,
    ScrollHome,
    ScrollEnd,
    ToggleFullScreen,
    ExitModeOrNothing,
    ExitModeOrCloseWindow,
    CloseWindow,
    ToggleAnimation,
    NextFrame,
    PreviousFrame,
    ToggleSlideshow,
    RotateLeft,
    RotateRight,
    FlipHorizontal,
    FlipVertical,
    Trash,
    Rename,
    RevealInFinder,
    CopyImage,
    CopyPath,
    OpenInEditor,
    ToggleInfoPanel,
    ToggleFilmstrip,
    ToggleGrid,
    ShowShortcuts,
    NewWindow,
    DuplicateWindow,
    Magnifier, // hold
    DragOut,
    StartPan,
    // appended (keep order above stable)
    Rate0,
    Rate1,
    Rate2,
    Rate3,
    Rate4,
    Rate5,
    RatePick,
    RateReject,
    RateClear, // F-CULL-01
    Undo,
    Redo,
    OpenPreferences,
    CloseAllWindows,
    SaveRotation, // F-EDIT-02
    // M3 (appended, keep order above stable)
    ToggleCullingMode, // F-CULL-06
    QuickMove,         // F-FILE-08: M + 1..9 (prefix key)
    QuickCopy,         // F-FILE-08: C + 1..9 (prefix key)
    // M3 (appended, keep order above stable)
    GoToImage,           // F-NAV-06 (⌘G)
    ToggleShuffle,       // F-NAV-06
    FilterByName,        // F-NAV-08 (⌘F)
    CompareWithNext,     // F-GRID-03
    ToggleAlwaysOnTop,   // F-VIEW-12
    ToggleMinimalWindow, // F-VIEW-13
    CoupleWindows,       // F-WIN-07
    ToggleHdr,           // F-COL-04: HDR <-> SDR preview
    ExposureIncrease,    // F-COL-05: +1/3 EV (display only)
    ExposureDecrease,    // F-COL-05: -1/3 EV
    ExposureReset,       // F-COL-05: 0 EV
    ToggleExposureHud,   // F-COL-05: exposure slider over the image
};

QString actionId(Action a); // stable string id for settings ("nextImage")
Action actionFromId(const QString& id);
QString actionLabel(Action a); // German UI label

enum class InputKind { Key, Wheel, Click, DoubleClick, MiddleClick, BackButton, ForwardButton, Swipe, Drag };

struct InputEvent {
    InputKind kind = InputKind::Key;
    int key = 0; // Qt::Key for Key
    Qt::KeyboardModifiers modifiers;
    int wheelSteps = 0;        // +down/-up after accumulation (Wheel)
    int swipeDirection = 0;    // -1 left, +1 right (Swipe)
    bool zoomed = false;       // image larger than viewport
    bool animated = false;     // current image is animated
    bool atScrollEdge = false; // for "scroll then flip" (F-IN-07)
    bool inCullingMode = false;
    bool multiPage = false;  // current file has several pages (F-IN-04 "page")
    bool inLeftHalf = false; // click position (F-IN-09 "image": left half = previous)
};

// Configurable "slots" of SPEC 4.14 (F-IN-01..16). Each slot has a set of
// allowed choices; the choice id is persisted under Settings key "input/<slot>".
struct InputSlot {
    QString id;               // e.g. "wheel", "wheel.cmd", "pageUpDown", "space", ...
    QString label;            // German
    QStringList choices;      // choice ids
    QStringList choiceLabels; // German
    QStringList choiceHelp;   // German one-line explanation per choice (F-IN-17)
    QString defaultChoice;
};

class InputBindings {
public:
    InputBindings();                               // loads from Settings::instance()
    static const QVector<InputSlot>& inputSlots(); // ("slots" is a Qt keyword)
    // Presets (F-IN-18): "mac", "windows", "photographer". Choices that are not available
    // yet (isChoiceAvailable) are skipped: the slot gets its default instead.
    static void applyPreset(const QString& presetId);
    // Choices of input slots that are implemented (later ones are shown disabled with
    // "in einer späteren Version" and never set by a preset).
    static bool isChoiceAvailable(const QString& slotId, const QString& choiceId);
    // Actions the app can carry out (the others are planned: slideshow, info panel,
    // rating, rename …); the shortcut overview marks them.
    static bool isActionAvailable(Action a);

    QString choice(const QString& slotId) const;
    void setChoice(const QString& slotId, const QString& choiceId); // persists

    // Free key shortcuts (F-SET-02) for actions not covered by slots. An action can
    // have several alternative shortcuts (SPEC 5.2, e.g. Cmd+0 and "3").
    static QVector<Action> allActions(); // every action except None
    static QList<QKeySequence> defaultShortcuts(Action a);
    QKeySequence shortcut(Action a) const; // first (primary) shortcut
    QList<QKeySequence> shortcuts(Action a) const;
    void setShortcut(Action a, const QKeySequence& seq);          // replaces all with one (empty = none)
    void setShortcuts(Action a, const QList<QKeySequence>& seqs); // persists
    void resetShortcuts();
    // Actions whose shortcut collides with another action in the same context
    // (rating keys only apply in culling mode and never conflict with zoom digits).
    QVector<Action> conflicts() const;
    QVector<Action> conflictsWith(Action a) const;

    // Core: resolve an input event to an action.
    Action resolve(const InputEvent& ev) const;
    // Wheel tuning (F-IN-03)
    bool wheelInverted() const;
    int wheelStepsPerImage() const;
    int wheelMinIntervalMs() const;
    int zoomStepPercent() const;
    int jumpSize() const; // for JumpForward/JumpBackward

    void reload();

private:
    QHash<QString, QString> m_choices;
    QHash<int, QList<QKeySequence>> m_shortcuts;
};

} // namespace lf
