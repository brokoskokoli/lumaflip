// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Animation playback (F-FMT-03): GIF, APNG, animated WebP, HEICS, animated AVIF/JXL.
//
// GUI-thread object. Frames are decoded ahead of time on a worker (one FrameReader kept
// open per animation, frames decoded in order), at the size of the first frame the window
// shows, so a frame replaces the previous one 1:1 (texture reuse in ImageCanvas).
// Memory (P-09): animations whose frames together fit the cache-all bound (kCacheAllBytes,
// at most a quarter of the image cache budget) are kept completely (looping and stepping
// back cost nothing); longer ones keep a ring of frames ahead of the current one, hard
// capped by the ring bound (kRingBytes, at most a quarter of the budget) - but always the
// current and the next frame. Frames are decoded like the first one: same size, HDR
// format and per-image byte cap (P-08).
// Threads: decode tasks hold only a shared state object; results are posted to the player
// under a lock that its destructor also takes, so destroying a window while a frame
// decodes is safe.
// Timing: a precise single-shot timer aims at absolute due times (QElapsedTimer), so the
// error does not add up over frames; a frame that is not decoded in time delays playback
// (no frames are skipped). Durations of 10 ms or less (0 = unknown, common for AVIF/JXL; 1 cs
// in old GIFs) count as 100 ms, like in web browsers.
// Loops: the file's loop count (DecodedImage::loopCount) - playback stops on the last frame
// after that many plays; play() starts over. 0 plays endlessly.

#include "core/ImageProvider.h"

#include <QElapsedTimer>
#include <QMap>
#include <QObject>
#include <QString>

#include <memory>

class QTimer;

namespace lf {

class AnimationPlayer : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 kCacheAllBytes = qint64(64) << 20;
    static constexpr qint64 kRingBytes = qint64(48) << 20;
    static constexpr int kDefaultDurationMs = 100;
    static constexpr int kMinDurationMs = 11; // shorter durations play as kDefaultDurationMs
    static constexpr double kMinSpeed = 0.25;
    static constexpr double kMaxSpeed = 4.0;

    explicit AnimationPlayer(QObject* parent = nullptr);
    ~AnimationPlayer() override;

    // P-09 image cache budget (ImageProvider::memoryBudget()); 0 = only the fixed bounds.
    // P-08 per-frame byte cap (ImageProvider::maxDecodeBytes()); 0 = none. Both take
    // effect with the next open().
    void setMemoryBudget(qint64 bytes);
    void setMaxDecodeBytes(qint64 bytes);
    qint64 ringBytes() const;     // bound of the frame ring
    qint64 cacheAllBytes() const; // animations up to this size are kept completely

    // Takes over `path` with `frameCount` frames; `first` is frame 0 as currently shown.
    // Starts playing if `autoplay`. The loop count comes from `first`.
    void open(const QString& path, const DecodedImagePtr& first, int frameCount, bool autoplay = true);
    void stop(); // forgets the animation (another image is shown)
    bool isActive() const { return !m_path.isEmpty(); }
    QString path() const { return m_path; }

    void play();
    void pause();
    void togglePlay();
    bool isPlaying() const { return m_playing; }
    // The file's loop count is used up: stopped on the last frame (play() starts over).
    bool isFinished() const { return m_finished; }
    int loopCount() const { return m_loopCount; } // 0 = endlessly
    // Pauses and shows the next (+1) or previous (-1) frame (`,` / `.`).
    void step(int delta);
    void setSpeed(double factor); // clamped to kMinSpeed..kMaxSpeed
    double speed() const { return m_speed; }

    int currentFrame() const { return m_current; }
    int frameCount() const { return m_count; }
    DecodedImagePtr currentImage() const { return m_frames.value(m_current); }
    // Duration of a frame for playback (below kMinDurationMs -> kDefaultDurationMs).
    static int durationMs(const DecodedImage& frame);

    // Memory bookkeeping (tests, P-09): decoded frames held, their bytes, and the bound.
    int heldFrames() const { return int(m_frames.size()); }
    qint64 heldBytes() const;
    qint64 maxHeldBytes() const;
    bool cachesAllFrames() const { return m_cacheAll; }
    int framesShown() const { return m_shownCount; } // frame changes since open()

signals:
    // A new frame is current: the window hands it to the view.
    void frameChanged(int index, const lf::DecodedImagePtr& image);
    void stateChanged(); // playing/paused, speed
    void failed(const QString& error);

private:
    struct Shared;
    void tick();
    void showFrame(int index);
    void scheduleDecode();
    void decoded(quint64 generation, int index, const DecodedImagePtr& image, const QString& error);
    void evict();
    void armTimer();
    int wrap(int index) const;
    int framesAhead() const;
    int maxFrames() const; // hard cap of held frames

    std::shared_ptr<Shared> m_shared;
    QString m_path;
    int m_count = 0;
    int m_current = 0;
    int m_pendingStep = -1; // frame to show as soon as it is decoded (stepping)
    bool m_playing = false;
    bool m_waiting = false; // due, but the next frame is not decoded yet
    bool m_busy = false;    // a decode task runs
    bool m_cacheAll = false;
    bool m_finished = false; // loop count used up
    int m_loopCount = 0;     // 0 = endlessly
    int m_playsDone = 0;     // completed plays of the whole sequence
    double m_speed = 1.0;
    qint64 m_budget = 0;
    qint64 m_maxDecodeBytes = 0;
    qint64 m_frameBytes = 0;
    qint64 m_dueNs = 0; // absolute due time of the next frame (m_clock)
    int m_shownCount = 0;
    quint64 m_generation = 0;
    QMap<int, DecodedImagePtr> m_frames;
    QTimer* m_timer = nullptr;
    QElapsedTimer m_clock;
};

} // namespace lf
