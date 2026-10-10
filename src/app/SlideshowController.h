// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Slideshow logic (F-SLIDE-01/02), independent of widgets: the order of the images
// (collection order from the current image on, or shuffled with a seed), the interval
// timer, pause, repeat and manual stepping. The window shows what showRequested() names
// and prefetches upcoming() (SPEC 7.1: prefetch follows the slideshow order).

#include <QObject>
#include <QStringList>

#include <random>

class QTimer;

namespace lf {

class SlideshowController : public QObject {
    Q_OBJECT
public:
    struct Options {
        int intervalMs = 3000;
        bool repeat = true;
        bool shuffle = false;
        quint32 seed = 0; // shuffle order (same seed, same order)
    };
    static constexpr int kMinIntervalMs = 200;

    explicit SlideshowController(QObject* parent = nullptr);
    ~SlideshowController() override;

    // Starts with `current` (shown already); the timer runs at once.
    void start(const QStringList& paths, const QString& current, const Options& options);
    void stop();
    bool isRunning() const { return m_running; }
    bool isPaused() const { return m_paused; }
    void setPaused(bool paused);
    void togglePause() { setPaused(!m_paused); }
    void setIntervalMs(int ms); // restarts the countdown
    int intervalMs() const { return m_options.intervalMs; }
    const Options& options() const { return m_options; }

    QStringList order() const { return m_order; }
    int position() const { return m_pos; }
    QString currentPath() const;
    // The next `n` paths in slideshow order (wrapping with `repeat`; a new shuffle round
    // is not known yet and ends the list).
    QStringList upcoming(int n) const;

    // Manual stepping (←/→): next/previous in slideshow order; false at the end without
    // repeat (next) or at the start (previous). Restarts the countdown.
    bool next();
    bool previous();
    // The window's current image changed by other means: continue from there.
    void sync(const QString& path);
    // The collection changed (files added/removed): removed paths leave the order, new
    // ones are appended (shuffled in with shuffle on). The position stays on its path.
    void updatePaths(const QStringList& paths);

signals:
    void showRequested(const QString& path);
    void finished(); // the end was reached without repeat
    void stateChanged();

private:
    void advance(); // timer
    void restartTimer();
    void reshuffle(const QString& first);

    QTimer* m_timer = nullptr;
    QStringList m_order;
    int m_pos = 0;
    Options m_options;
    bool m_running = false;
    bool m_paused = false;
    std::mt19937 m_rng;
};

} // namespace lf
