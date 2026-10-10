// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-MAC-06 (M4): Live Text (select and copy text in the image), Visual Look Up and subject
// lifting via VisionKit - the request logic, independent of VisionKit (tests use a fake).
//
// Analysis runs only on demand: while the user has Live Text switched on for a window, for
// the image shown (once per file, not per pipeline stage), never for prefetched images.
// Flipping to another image, switching it off, the setting key::LiveTextEnabled going off or
// a blocking state (setBlocked(), e.g. the EDR surface, see app/LiveTextOverlay.h) cancel
// the running analysis; a late result of a cancelled analysis is ignored (token).

#include <QImage>
#include <QObject>
#include <QString>

namespace lf {

class ImageAnalysisBackend {
public:
    virtual ~ImageAnalysisBackend() = default;
    virtual bool isSupported() const = 0;
    // Starts analysing `image`; the result goes to LiveTextSession::analysisFinished(token, …).
    virtual void analyze(quint64 token, const QImage& image) = 0;
    // Stops the running analysis and removes the overlay content.
    virtual void cancel() = 0;
};

class LiveTextSession : public QObject {
    Q_OBJECT
public:
    enum class State {
        Off,         // not switched on
        Unavailable, // switched on, but not possible (setting off, unsupported, blocked)
        Waiting,     // switched on, no image yet
        Analyzing,
        Ready,     // overlay shows the analysis
        NoResults, // nothing found in the image
        Failed,
    };

    explicit LiveTextSession(ImageAnalysisBackend* backend, QObject* parent = nullptr); // non-owning
    void setAllowed(bool allowed);          // key::LiveTextEnabled
    void setBlocked(const QString& reason); // empty = not blocked
    void setActive(bool active);
    bool toggle(); // returns the new isActive()
    bool isActive() const { return m_active; }
    // The image shown changed. A null image (still loading, error card) waits.
    void setImage(const QString& path, const QImage& image);
    // From the backend; stale tokens are ignored.
    void analysisFinished(quint64 token, bool hasResults, const QString& error);

    State state() const { return m_state; }
    bool overlayVisible() const { return m_active && m_state == State::Ready; }
    quint64 currentToken() const { return m_token; }
    QString analyzedPath() const { return m_analyzedPath; }
    // Short user facing text for the state ("Live Text: kein Text gefunden"), empty for Off.
    QString statusText() const;
    QString blockedReason() const { return m_blocked; }

signals:
    void stateChanged();

private:
    void update();
    void stop(State next);
    void setState(State s);

    ImageAnalysisBackend* m_backend;
    bool m_allowed = true;
    bool m_active = false;
    QString m_blocked;
    QString m_path;
    QImage m_image;
    QString m_analyzedPath; // path of the running/finished analysis
    quint64 m_token = 0;
    bool m_running = false;
    State m_state = State::Off;
    QString m_error;
};

} // namespace lf
