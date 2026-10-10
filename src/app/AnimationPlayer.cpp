// SPDX-License-Identifier: GPL-3.0-or-later
#include "AnimationPlayer.h"

#include "core/Decoder.h"

#include <QCoreApplication>
#include <QMutex>
#include <QPointer>
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
    QMutex mutex;
    QString path;
    DecodeRequest request;
    std::unique_ptr<FrameReader> reader;
    bool opened = false;
};

AnimationPlayer::AnimationPlayer(QObject* parent) : QObject(parent), m_timer(new QTimer(this)) {
    m_timer->setSingleShot(true);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &AnimationPlayer::tick);
    m_clock.start();
}

AnimationPlayer::~AnimationPlayer() = default;

int AnimationPlayer::durationMs(const DecodedImage& frame) {
    return frame.frameDurationMs > 0 ? frame.frameDurationMs : kDefaultDurationMs;
}

void AnimationPlayer::open(const QString& path, const DecodedImagePtr& first, int frameCount, bool autoplay) {
    stop();
    if (path.isEmpty() || frameCount < 2)
        return;
    m_path = path;
    m_count = frameCount;
    m_current = 0;
    m_shownCount = 0;
    m_shared = std::make_shared<Shared>();
    m_shared->path = path;
    // Frames at the size of the shown first frame, so they replace it 1:1.
    if (first && !first->fullSize.isEmpty() && first->image.size() != first->fullSize)
        m_shared->request.maxPixelSize = longEdge(first->image.size());
    m_frameBytes = first ? first->image.sizeInBytes() : 0;
    m_cacheAll = m_frameBytes > 0 && m_frameBytes * m_count <= kCacheAllBytes;
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
    m_shared.reset();
    m_path.clear();
    m_count = 0;
    m_current = 0;
    m_pendingStep = -1;
    m_playing = m_waiting = m_busy = false;
    m_frameBytes = 0;
    m_cacheAll = false;
    if (was)
        emit stateChanged();
}

void AnimationPlayer::play() {
    if (!isActive() || m_count < 2 || m_playing)
        return;
    m_playing = true;
    m_pendingStep = -1;
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
    if (m_cacheAll)
        return m_frameBytes * m_count;
    return m_frameBytes * (framesAhead() + 3); // ahead + current + previous + a stepped-to frame
}

int AnimationPlayer::wrap(int index) const {
    return m_count > 0 ? ((index % m_count) + m_count) % m_count : 0;
}

int AnimationPlayer::framesAhead() const {
    if (m_count < 2)
        return 0;
    if (m_cacheAll)
        return m_count - 1;
    const qint64 n = kRingBytes / std::max<qint64>(1, m_frameBytes);
    return int(std::clamp<qint64>(n, 2, m_count - 1));
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
    const int ahead = framesAhead();
    for (auto it = m_frames.begin(); it != m_frames.end();) {
        const int i = it.key();
        const int dist = wrap(i - m_current); // frames ahead of the current one
        const bool keep = i == m_current || dist <= ahead || i == wrap(m_current - 1) || i == m_pendingStep;
        it = keep ? std::next(it) : m_frames.erase(it);
    }
}

void AnimationPlayer::scheduleDecode() {
    if (m_busy || !isActive() || !m_shared)
        return;
    int index = -1;
    if (m_pendingStep >= 0 && !m_frames.contains(m_pendingStep)) {
        index = m_pendingStep;
    } else {
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
    QPointer<AnimationPlayer> self(this);
    animationPool()->start([shared, gen, index, self] {
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
                result = std::make_shared<const DecodedImage>(std::move(*img));
            }
        }
        QMetaObject::invokeMethod(
            self.data(),
            [self, gen, index, result, error] {
                if (self)
                    self->decoded(gen, index, result, error);
            },
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
