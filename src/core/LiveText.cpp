// SPDX-License-Identifier: GPL-3.0-or-later
#include "LiveText.h"

#include <QCoreApplication>

namespace lf {

namespace {
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::LiveText)
};
} // namespace

LiveTextSession::LiveTextSession(ImageAnalysisBackend* backend, QObject* parent)
    : QObject(parent), m_backend(backend) {}

void LiveTextSession::setAllowed(bool allowed) {
    if (m_allowed == allowed)
        return;
    m_allowed = allowed;
    update();
}

void LiveTextSession::setBlocked(const QString& reason) {
    if (m_blocked == reason)
        return;
    m_blocked = reason;
    update();
}

void LiveTextSession::setActive(bool active) {
    if (m_active == active)
        return;
    m_active = active;
    update();
}

bool LiveTextSession::toggle() {
    setActive(!m_active);
    return m_active;
}

void LiveTextSession::setImage(const QString& path, const QImage& image) {
    if (path == m_path && image.isNull() == m_image.isNull())
        return; // a better stage of the same file: the analysis stays
    m_path = path;
    m_image = image;
    update();
}

void LiveTextSession::analysisFinished(quint64 token, bool hasResults, const QString& error) {
    if (!m_running || token != m_token)
        return; // cancelled or superseded
    m_running = false;
    m_error = error;
    setState(!error.isEmpty() ? State::Failed : hasResults ? State::Ready : State::NoResults);
}

void LiveTextSession::stop(State next) {
    if (m_running || !m_analyzedPath.isEmpty()) {
        ++m_token; // a late result of the old analysis is ignored
        m_running = false;
        m_analyzedPath.clear();
        if (m_backend)
            m_backend->cancel();
    }
    setState(next);
}

void LiveTextSession::update() {
    if (!m_active) {
        stop(State::Off);
        return;
    }
    if (!m_allowed || !m_blocked.isEmpty() || !m_backend || !m_backend->isSupported()) {
        stop(State::Unavailable);
        return;
    }
    if (m_path.isEmpty() || m_image.isNull()) {
        stop(State::Waiting);
        return;
    }
    if (m_path == m_analyzedPath)
        return; // running or done for this image
    stop(State::Analyzing);
    m_analyzedPath = m_path;
    m_running = true;
    m_error.clear();
    const quint64 token = ++m_token;
    setState(State::Analyzing);
    m_backend->analyze(token, m_image);
}

void LiveTextSession::setState(State s) {
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged();
}

QString LiveTextSession::statusText() const {
    switch (m_state) {
    case State::Off:
        return {};
    case State::Unavailable:
        if (!m_allowed)
            return Tr::tr("Live Text ist in den Einstellungen ausgeschaltet.");
        if (!m_blocked.isEmpty())
            return m_blocked;
        return Tr::tr("Live Text ist auf diesem Mac nicht verfügbar.");
    case State::Waiting:
    case State::Analyzing:
        return Tr::tr("Live Text: Bild wird analysiert …");
    case State::Ready:
        return Tr::tr("Live Text: Text und Motive auswählbar");
    case State::NoResults:
        return Tr::tr("Live Text: kein Text gefunden");
    case State::Failed:
        return Tr::tr("Live Text: Analyse fehlgeschlagen (%1)").arg(m_error);
    }
    return {};
}

} // namespace lf
