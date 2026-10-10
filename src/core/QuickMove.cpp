// SPDX-License-Identifier: GPL-3.0-or-later
// F-FILE-08 quick move/copy, see QuickMove.h.

#include "QuickMove.h"

#include "Settings.h"

#include <QCoreApplication>
#include <QDir>

#include <atomic>

namespace lf {

namespace {

std::atomic<int> g_lastSlot{0};

constexpr Qt::KeyboardModifiers kRelevant =
    Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;

bool isModifierKey(int key) {
    return key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta ||
           key == Qt::Key_CapsLock || key == Qt::Key_AltGr;
}

} // namespace

QuickChord::Result QuickChord::start(QuickOp op) {
    m_pending = true;
    m_op = op;
    Result r;
    r.kind = Kind::Pending;
    r.op = op;
    return r;
}

QuickChord::Result QuickChord::feed(int key, Qt::KeyboardModifiers modifiers) {
    Result r;
    if (!m_pending)
        return r;
    r.op = m_op;
    if (isModifierKey(key)) {
        r.kind = Kind::Pending;
        return r;
    }
    const Qt::KeyboardModifiers mods = modifiers & kRelevant;
    m_pending = false;
    if (mods == Qt::NoModifier && key >= Qt::Key_1 && key <= Qt::Key_9) {
        r.kind = Kind::Execute;
        r.slot = key - Qt::Key_0;
        setLastSlot(r.slot);
        return r;
    }
    const int prefixKey = m_op == QuickOp::Move ? Qt::Key_M : Qt::Key_C;
    if (mods == Qt::NoModifier && key == prefixKey) {
        if (lastSlot() <= 0) {
            r.kind = Kind::Cancelled;
            r.noLastTarget = true;
            return r;
        }
        r.kind = Kind::Execute;
        r.slot = lastSlot();
        r.repeat = true;
        return r;
    }
    r.kind = Kind::Cancelled;
    r.passThrough = key != Qt::Key_Escape;
    return r;
}

int QuickChord::lastSlot() {
    return g_lastSlot.load();
}

void QuickChord::setLastSlot(int slot) {
    if (slot >= 1 && slot <= quickmove::kSlots)
        g_lastSlot.store(slot);
}

QStringList quickmove::targets() {
    QStringList out;
    for (int n = 1; n <= kSlots; ++n)
        out << target(n);
    return out;
}

QString quickmove::target(int slot) {
    if (slot < 1 || slot > kSlots)
        return {};
    return Settings::instance()
        .value(QString::fromLatin1(key::QuickTargetPrefix) + QString::number(slot))
        .toString();
}

void quickmove::setTarget(int slot, const QString& folder) {
    if (slot < 1 || slot > kSlots)
        return;
    Settings::instance().setValue(QString::fromLatin1(key::QuickTargetPrefix) + QString::number(slot),
                                  folder);
}

QString quickmove::label(int slot, const QString& folder) {
    if (folder.isEmpty())
        return QCoreApplication::translate("lf::QuickMove", "%1  (nicht festgelegt)").arg(slot);
    return QStringLiteral("%1  %2").arg(slot).arg(QDir(folder).dirName());
}

} // namespace lf
