// SPDX-License-Identifier: GPL-3.0-or-later
#include "AnimationPlayer.h"

#include "core/Decoder.h"

#include <QCoreApplication>
#include <QMutex>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace lf {

namespace {

// Animation decodes run beside the image provider's pool (they never wait for each other).
QThreadPool* animationPool() {
    static QPointer<QThreadPool> pool;
    if (!pool) {
        pool = new QThreadPool(QCoreApplication::instance()); // joined before Qt shuts down
        pool->setMaxThreadCount(2);
    }
    return pool;
}

int longEdge(QSize s) {
    return std::max(s.width(), s.height());
}

} // namespace

// Shared with the decode tasks: the open reader of one animation.
struct AnimationPlayer::Shared {
    QMutex mutex; // reader (held while a frame decodes)
    QString path;
    DecodeRequest request;
    Quality quality = Quality::Screen; // stage of the frames (that of the first frame)
    std::unique_ptr<FrameReader> reader;
    bool opened = false;

    // The player results go to. Reset (under ownerMutex) by the player when it stops this
    // animation or is destroyed, so a task never posts to a deleted object: it posts while
    // holding the lock, and Qt drops events of an object destroyed later.
    QMutex ownerMutex;
    AnimationPlayer* owner = nullptr;

    void detach() {
        QMutexLocker lock(&ownerMutex);
        owner = nullptr;
    }
};

AnimationPlayer::AnimationPlayer(QObject* parent) : QObject(parent), m_timer(new QTimer(this)) {
    m_timer->setSingleShot(true);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &AnimationPlayer::tick);
    m_clock.start();
}

AnimationPlayer::~AnimationPlayer() {
    if (m_shared)
        m_shared->detach(); // a running decode keeps `m_shared` alive but posts nothing
}

void AnimationPlayer::setMemoryBudget(qint64 bytes) {
    m_budget = std::max<qint64>(0, bytes);
}

void AnimationPlayer::setMaxDecodeBytes(qint64 bytes) {
    m_maxDecodeBytes = std::max<qint64>(0, bytes);
}

qint64 AnimationPlayer::ringBytes() const {
    // P-09: a share of the image cache budget, never more than kRingBytes.
    return m_budget > 0 ? std::min(kRingBytes, m_budget / 4) : kRingBytes;
}

qint64 AnimationPlayer::cacheAllBytes() const {
    return m_budget > 0 ? std::min(kCacheAllBytes, m_budget / 4) : kCacheAllBytes;
}

int AnimationPlayer::durationMs(const DecodedImage& frame) {
    return frame.frameDurationMs >= kMinDurationMs ? frame.frameDurationMs : kDefaultDurationMs;
}

void AnimationPlayer::open(const QString& path, const DecodedImagePtr& first, int frameCount, bool autoplay) {
    stop();
    if (path.isEmpty() || frameCount < 2)
        return;
    m_path = path;
    m_count = frameCount;
    m_current = 0;
    m_shownCount = 0;
    m_loopCount = first ? std::max(0, first->loopCount) : 0;
    m_playsDone = 0;
    m_finished = false;
    m_shared = std::make_shared<Shared>();
    m_shared->path = path;
    m_shared->owner = this;
    // Frames at the size of the shown first frame, so they replace it 1:1 - also in the
    // same buffer format (HDR) and within the provider's per-image cap (P-08).
    if (first && !first->fullSize.isEmpty() && first->image.size() != first->fullSize)
        m_shared->request.maxPixelSize = longEdge(first->image.size());
    m_shared->request.hdr = first && first->headroom > 1.0f;
    m_shared->request.maxBytes = m_maxDecodeBytes;
    if (first)
        m_shared->quality = first->quality;
    m_frameBytes = first ? first->image.sizeInBytes() : 0;
    m_cacheAll = m_frameBytes > 0 && m_frameBytes * m_count <= cacheAllBytes();
    if (first)
        m_frames.insert(0, first);
    if (autoplay)
        play();
    else
        scheduleDecode();
}

void AnimationPlayer::stop() {
    const bool was = isActive();
    ++m_generation; // results of running decodes are dropped
    m_timer->stop();
    m_frames.clear();
    if (m_shared)
        m_shared->detach();
    m_shared.reset();
    m_path.clear();
    m_count = 0;
    m_current = 0;
    m_pendingStep = -1;
    m_playing = m_waiting = m_busy = false;
    m_frameBytes = 0;
    m_cacheAll = false;
    m_finished = false;
    m_loopCount = m_playsDone = 0;
    if (was)
        emit stateChanged();
}

void AnimationPlayer::play() {
    if (!isActive() || m_count < 2 || m_playing)
        return;
    m_playing = true;
    m_pendingStep = -1;
    if (m_finished) { // loop count used up: play the sequence again from the start
        m_finished = false;
        // The step from the last frame back to frame 0 starts the new play, it completes none.
        m_playsDone = m_current == m_count - 1 ? -1 : 0;
    }
    const DecodedImagePtr cur = m_frames.value(m_current);
    const qint64 d = qint64(std::llround((cur ? durationMs(*cur) : kDefaultDurationMs) * 1e6 / m_speed));
    m_dueNs = m_clock.nsecsElapsed() + d;
    armTimer();
    scheduleDecode();
    emit stateChanged();
}

void AnimationPlayer::pause() {
    if (!m_playing)
        return;
    m_playing = false;
    m_waiting = false;
    m_timer->stop();
    emit stateChanged();
}

void AnimationPlayer::togglePlay() {
    m_playing ? pause() : play();
}

void AnimationPlayer::step(int delta) {
    if (!isActive() || m_count < 2 || delta == 0)
        return;
    pause();
    const int base = m_pendingStep >= 0 ? m_pendingStep : m_current;
    const int target = wrap(base + delta);
    if (m_frames.contains(target)) {
        m_pendingStep = -1;
        showFrame(target);
        return;
    }
    m_pendingStep = target;
    scheduleDecode();
}

void AnimationPlayer::setSpeed(double factor) {
    const double s = std::clamp(factor, kMinSpeed, kMaxSpeed);
    if (std::abs(s - m_speed) < 1e-9)
        return;
    // Keep the remaining time of the current frame proportional.
    if (m_playing) {
        const qint64 now = m_clock.nsecsElapsed();
        const qint64 left = std::max<qint64>(0, m_dueNs - now);
        m_dueNs = now + qint64(double(left) * m_speed / s);
    }
    m_speed = s;
    if (m_playing)
        armTimer();
    emit stateChanged();
}

qint64 AnimationPlayer::heldBytes() const {
    qint64 sum = 0;
    for (const auto& f : m_frames)
        if (f)
            sum += f->image.sizeInBytes();
    return sum;
}

qint64 AnimationPlayer::maxHeldBytes() const {
    return m_frameBytes * maxFrames();
}

int AnimationPlayer::maxFrames() const {
    if (m_count < 1)
        return 0;
    if (m_cacheAll)
        return m_count;
    // Hard cap: the ring budget, but at least the current and the next frame (playback
    // needs both; a single frame is already bounded by the decode cap, P-08).
    const qint64 n = ringBytes() / std::max<qint64>(1, m_frameBytes);
    return int(std::clamp<qint64>(n, std::min(2, m_count), m_count));
}

int AnimationPlayer::wrap(int index) const {
    return m_count > 0 ? ((index % m_count) + m_count) % m_count : 0;
}

int AnimationPlayer::framesAhead() const {
    if (m_count < 2)
        return 0;
    if (m_cacheAll)
        return m_count - 1;
    // Room for the current frame and one more (the previous or a stepped-to frame) if the
    // budget allows; at least one frame ahead.
    const int n = maxFrames();
    return std::clamp(n >= 3 ? n - 2 : n - 1, 1, m_count - 1);
}

void AnimationPlayer::armTimer() {
    const qint64 left = m_dueNs - m_clock.nsecsElapsed();
    m_timer->start(int(std::max<qint64>(0, (left + 999'999) / 1'000'000)));
}

void AnimationPlayer::tick() {
    if (!m_playing)
        return;
    const qint64 now = m_clock.nsecsElapsed();
    if (now < m_dueNs - 500'000) { // woke up early
        armTimer();
        return;
    }
    const int next = wrap(m_current + 1);
    if (next == 0 && m_loopCount > 0 && ++m_playsDone >= m_loopCount) {
        // The file's loop count is used up: stay on the last frame.
        m_playing = false;
        m_waiting = false;
        m_finished = true;
        emit stateChanged();
        return;
    }
    const DecodedImagePtr img = m_frames.value(next);
    if (!img) {
        m_waiting = true; // shown as soon as it is decoded
        scheduleDecode();
        return;
    }
    showFrame(next);
    const qint64 d = qint64(std::llround(durationMs(*img) * 1e6 / m_speed));
    m_dueNs += d;
    if (m_dueNs < now) // far behind (e.g. the machine was busy): start over from now
        m_dueNs = now + d;
    armTimer();
}

void AnimationPlayer::showFrame(int index) {
    m_current = index;
    ++m_shownCount;
    evict();
    emit frameChanged(index, m_frames.value(index));
    scheduleDecode();
}

void AnimationPlayer::evict() {
    if (m_cacheAll)
        return;
    // Kept, in this order, while there is room (maxFrames): the current frame, the frames
    // ahead, a stepped-to frame, the previous frame.
    const int ahead = framesAhead();
    const int cap = maxFrames();
    QList<int> keep{m_current};
    for (int k = 1; k <= ahead; ++k)
        keep << wrap(m_current + k);
    if (m_pendingStep >= 0)
        keep << m_pendingStep;
    keep << wrap(m_current - 1);
    QSet<int> kept;
    for (int i : std::as_const(keep)) {
        if (kept.size() >= cap)
            break;
        if (m_frames.contains(i) || i == m_pendingStep || i == wrap(m_current + 1))
            kept.insert(i); // also reserves room for a frame still being decoded
    }
    for (auto it = m_frames.begin(); it != m_frames.end();)
        it = kept.contains(it.key()) ? std::next(it) : m_frames.erase(it);
}

void AnimationPlayer::scheduleDecode() {
    if (m_busy || !isActive() || !m_shared)
        return;
    int index = -1;
    if (m_pendingStep >= 0 && !m_frames.contains(m_pendingStep)) {
        index = m_pendingStep;
    } else if (m_cacheAll || int(m_frames.size()) < maxFrames()) { // P-09: never above the cap
        const int ahead = framesAhead();
        for (int k = 1; k <= ahead; ++k) {
            const int i = wrap(m_current + k);
            if (!m_frames.contains(i)) {
                index = i;
                break;
            }
        }
    }
    if (index < 0)
        return;
    m_busy = true;
    const std::shared_ptr<Shared> shared = m_shared;
    const quint64 gen = m_generation;
    animationPool()->start([shared, gen, index] {
        DecodedImagePtr result;
        QString error;
        {
            QMutexLocker lock(&shared->mutex);
            if (!shared->opened) {
                shared->reader = DecoderRegistry::instance().openFrames(shared->path);
                shared->opened = true;
            }
            if (!shared->reader) {
                error = DecoderRegistry::messageUnreadable();
            } else if (auto img = shared->reader->readFrame(index, shared->request, &error)) {
                img->frame = index;
                img->quality = shared->quality;
                result = std::make_shared<const DecodedImage>(std::move(*img));
            }
        }
        // Posted while holding ownerMutex: the player cannot be destroyed in between (its
        // destructor detaches under the same lock), and Qt discards the event if the player
        // is destroyed before it is delivered.
        QMutexLocker lock(&shared->ownerMutex);
        if (AnimationPlayer* owner = shared->owner)
            QMetaObject::invokeMethod(
                owner, [owner, gen, index, result, error] { owner->decoded(gen, index, result, error); },
                Qt::QueuedConnection);
    });
}

void AnimationPlayer::decoded(quint64 generation, int index, const DecodedImagePtr& image,
                              const QString& error) {
    if (generation != m_generation)
        return; // another animation (or none) by now
    m_busy = false;
    if (!image) {
        pause();
        m_pendingStep = -1;
        emit failed(error);
        return;
    }
    m_frames.insert(index, image);
    if (m_pendingStep == index) {
        m_pendingStep = -1;
        showFrame(index);
        return;
    }
    if (m_waiting && m_playing && index == wrap(m_current + 1)) {
        m_waiting = false;
        showFrame(index);
        m_dueNs = m_clock.nsecsElapsed() + qint64(std::llround(durationMs(*image) * 1e6 / m_speed));
        armTimer();
        return;
    }
    evict();
    scheduleDecode();
}

} // namespace lf
