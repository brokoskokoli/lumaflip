// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-FILE-08 quick move/copy in a viewer window: `M` / `C` start the sequence (QuickChord),
// `1`–`9` pick the target folder from the preferences, `M M` / `C C` repeat the last one.
// F-FILE-11: a name taken in the target folder is asked about (Ersetzen / Beide behalten /
// Überspringen). The file work runs on the FileOps queue (FileBatch): asynchronous and
// undoable with ⌘Z; FileOps' notifications update every window.

#include "core/FileBatch.h"
#include "core/QuickMove.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class QTimer;
class QWidget;

namespace lf {

class Application;
class ViewerWindow;
struct InputEvent;

class QuickMoveController : public QObject {
    Q_OBJECT
public:
    QuickMoveController(ViewerWindow& window, Application& app);
    ~QuickMoveController() override;

    // The prefix key (Action::QuickMove / QuickCopy).
    void start(QuickOp op);
    // A key while the sequence waits for its target; true: consumed (do not handle it).
    bool interceptKey(const InputEvent& event);
    bool isPending() const { return m_chord.isPending(); }
    // Moves/copies the current image into target `slot` (1..9), e.g. from the menu - or the
    // images setSubjects() names (grid selection, F-GRID-02).
    void transferCurrent(QuickOp op, int slot);
    // M3: images the quick move/copy acts on instead of the current one (empty list: the
    // current image).
    void setSubjects(std::function<QStringList()> subjects) { m_subjects = std::move(subjects); }
    // Runs `items` as one undo step, asking about name conflicts first (`undoText`;
    // `doneMessage(count)` for the toast). Also used by the culling bulk move (F-CULL-05).
    void transfer(TransferKind kind, QVector<TransferItem> items, const QString& undoText,
                  std::function<QString(int done)> doneMessage);

    // F-FILE-11 dialog (non-blocking). `count` > 1 shows "Für alle anwenden". `done` gets the
    // choice (nullopt: cancelled) and whether it applies to all remaining conflicts.
    static void askConflict(QWidget* parent, const QString& fileName, const QString& folder, int count,
                            std::function<void(std::optional<ConflictChoice> choice, bool forAll)> done);

private:
    void runWithChoices(TransferKind kind, QVector<TransferItem> items, QStringList conflicts, int next,
                        bool forAll, ConflictChoice allChoice, const QString& undoText,
                        std::function<QString(int)> doneMessage);

    ViewerWindow& m_window;
    Application& m_app;
    QuickChord m_chord;
    QTimer* m_timeout = nullptr;
    std::function<QStringList()> m_subjects;
};

} // namespace lf
