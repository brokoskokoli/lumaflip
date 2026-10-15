// SPDX-License-Identifier: GPL-3.0-or-later
#include "SlideshowController.h"

#include <QSet>
#include <QTimer>

#include <algorithm>

namespace lf {

// m_rng is seeded in start() from Options::seed (same seed, same order).
// NOLINTNEXTLINE(bugprone-random-generator-seed)
SlideshowController::SlideshowController(QObject* parent) : QObject(parent), m_timer(new QTimer(this)) {
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &SlideshowController::advance);
}

SlideshowController::~SlideshowController() = default;

void SlideshowController::start(const QStringList& paths, const QString& current, const Options& options) {
    m_options = options;
    m_options.intervalMs = std::max(kMinIntervalMs, options.intervalMs);
    m_rng.seed(options.seed);
    m_order = paths;
    m_pos = std::max(0, int(m_order.indexOf(current)));
    if (m_options.shuffle)
        reshuffle(m_order.value(m_pos));
    m_running = !m_order.isEmpty();
    m_paused = false;
    restartTimer();
    emit stateChanged();
}

void SlideshowController::stop() {
    if (!m_running)
        return;
    m_running = false;
    m_paused = false;
    m_timer->stop();
    emit stateChanged();
}

void SlideshowController::setPaused(bool paused) {
    if (!m_running || paused == m_paused)
        return;
    m_paused = paused;
    restartTimer();
    emit stateChanged();
}

void SlideshowController::setIntervalMs(int ms) {
    m_options.intervalMs = std::max(kMinIntervalMs, ms);
    restartTimer();
    emit stateChanged();
}

QString SlideshowController::currentPath() const {
    return m_order.value(m_pos);
}

QStringList SlideshowController::upcoming(int n) const {
    QStringList out;
    const int size = int(m_order.size());
    for (int k = 1; k <= n && k < size; ++k) {
        int i = m_pos + k;
        if (i >= size) {
            if (!m_options.repeat || m_options.shuffle)
                break; // end, or the next shuffle round is not drawn yet
            i -= size;
        }
        out << m_order[i];
    }
    return out;
}

bool SlideshowController::next() {
    if (!m_running || m_order.isEmpty())
        return false;
    if (m_pos + 1 >= m_order.size()) {
        if (!m_options.repeat)
            return false;
        if (m_options.shuffle) {
            // A new round; it does not start with the image just shown.
            const QString last = m_order.value(m_pos);
            reshuffle(QString());
            if (m_order.size() > 1 && m_order.first() == last)
                m_order.swapItemsAt(0, 1 + int(m_rng() % quint32(m_order.size() - 1)));
        }
        m_pos = 0;
    } else {
        ++m_pos;
    }
    restartTimer();
    emit showRequested(m_order[m_pos]);
    return true;
}

bool SlideshowController::previous() {
    if (!m_running || m_order.isEmpty())
        return false;
    if (m_pos == 0) {
        if (!m_options.repeat || m_options.shuffle)
            return false;
        m_pos = int(m_order.size()) - 1;
    } else {
        --m_pos;
    }
    restartTimer();
    emit showRequested(m_order[m_pos]);
    return true;
}

void SlideshowController::sync(const QString& path) {
    if (!m_running || path == currentPath())
        return;
    const qsizetype i = m_order.indexOf(path);
    if (i < 0)
        return;
    m_pos = int(i);
    restartTimer();
}

void SlideshowController::updatePaths(const QStringList& paths) {
    if (!m_running)
        return;
    const QString cur = currentPath();
    const QSet<QString> now(paths.cbegin(), paths.cend());
    QStringList order;
    for (const QString& p : std::as_const(m_order))
        if (now.contains(p))
            order << p;
    const QSet<QString> known(order.cbegin(), order.cend());
    for (const QString& p : paths) {
        if (known.contains(p))
            continue;
        if (m_options.shuffle && !order.isEmpty())
            order.insert(qsizetype(m_rng() % quint32(order.size() + 1)), p);
        else
            order << p;
    }
    m_order = order;
    const qsizetype i = m_order.indexOf(cur);
    m_pos = i >= 0 ? int(i) : std::clamp(m_pos, 0, std::max(0, int(m_order.size()) - 1));
    if (m_order.isEmpty())
        stop();
}

void SlideshowController::advance() {
    if (!m_running || m_paused)
        return;
    if (!next()) {
        stop();
        emit finished();
    }
}

void SlideshowController::restartTimer() {
    m_timer->stop();
    if (m_running && !m_paused)
        m_timer->start(m_options.intervalMs);
}

void SlideshowController::reshuffle(const QString& first) {
    // Fisher-Yates with the seeded generator (modulo: the same order on every platform).
    for (int i = int(m_order.size()) - 1; i > 0; --i)
        m_order.swapItemsAt(i, int(m_rng() % quint32(i + 1)));
    if (!first.isEmpty()) {
        const qsizetype at = m_order.indexOf(first);
        if (at > 0)
            m_order.move(at, 0);
    }
    m_pos = 0;
}

} // namespace lf
