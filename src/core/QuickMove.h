// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-FILE-08 quick move/copy ("Schnellablage"): `M` + `1`–`9` moves the current image into
// target folder n, `C` + `1`–`9` copies it; `M M` (`C C`) repeats the last target. The
// targets are configured in the preferences (key::QuickTargetPrefix + n). Pure logic: the
// key sequence state machine and the target list; the window runs the file operation
// (FileBatch, undoable, F-FILE-11 conflicts).

#include <QString>
#include <QStringList>
#include <Qt>

namespace lf {

enum class QuickOp { Move, Copy };

class QuickChord {
public:
    enum class Kind {
        NotHandled, // not part of a quick move sequence: handle the key normally
        Pending,    // waiting for the target digit
        Execute,    // run `op` into target `slot`
        Cancelled,  // sequence ended without a target (`passThrough`: handle the key normally)
    };
    struct Result {
        Kind kind = Kind::NotHandled;
        QuickOp op = QuickOp::Move;
        int slot = 0;              // 1..9 (Execute)
        bool repeat = false;       // Execute from `M M` / `C C`
        bool passThrough = false;  // Cancelled by a key that has its own meaning
        bool noLastTarget = false; // `M M` without a previous target
    };

    // The prefix key (Action::QuickMove / QuickCopy) was pressed.
    Result start(QuickOp op);
    // A key while isPending(); NotHandled when nothing is pending.
    Result feed(int key, Qt::KeyboardModifiers modifiers);
    bool isPending() const { return m_pending; }
    QuickOp pendingOp() const { return m_op; }
    void cancel() { m_pending = false; }

    // The last target used successfully (shared by all windows; 0 = none yet). Set by the
    // caller after the operation into that target started, not by feed().
    static int lastSlot();
    static void setLastSlot(int slot);

private:
    bool m_pending = false;
    QuickOp m_op = QuickOp::Move;
};

namespace quickmove {
inline constexpr int kSlots = 9;
// Target folders 1..9 (index 0 = slot 1) from the settings; "" = not set.
QStringList targets();
QString target(int slot);
void setTarget(int slot, const QString& folder);
// Menu text: "1  Urlaub" or "1  (nicht festgelegt)".
QString label(int slot, const QString& folder);
} // namespace quickmove

} // namespace lf
