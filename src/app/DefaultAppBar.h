// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-MAC-09: "Lumaflip ist nicht die Standard-App für den Dateityp „JPEG-Bild“." - an info
// bar at the top of the viewer window (below the access hint, never over the image), with
// "Festlegen" (this type, Shell::makeDefaultViewerAsync), "Alle Bildformate …" (the
// F-MAC-04 checklist), "Nicht mehr fragen" (global off) and a close button ("not now" for
// this type). At most once per type (core/DefaultAppOffer.h keeps the answers); never in
// full screen or a slideshow, never for archive members or PDF pages.
//
// The controller watches the windows of one Application. Once an image stayed on screen
// (after the first frame: defaultapp::kStartDelayMs, P-01; later defaultapp::kSettleMs) it
// asks the shell on a worker thread (LaunchServices can be slow) whether the app is the
// installed one and the default for the file's type, once per file suffix and session.
// main() turns it on (Application::enableDefaultAppOffer); tests build their Application
// without it, and LF_NO_DEFAULT_APP_OFFER=1 switches it off.

#include "core/Platform.h" // DefaultViewerStatus

#include <QElapsedTimer>
#include <QFrame>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QThreadPool>

class QLabel;
class QPushButton;
class QTimer;
class QToolButton;

namespace lf {

class Application;
class ViewerWindow;

class DefaultAppBar : public QFrame {
    Q_OBJECT
public:
    explicit DefaultAppBar(QWidget* parent = nullptr);

    void setType(const QString& type, const QString& typeName);
    QString type() const { return m_type; }
    QString typeName() const { return m_typeName; }
    QString text() const;
    // F-VIEW-15: room for the title strip that fades in over the top of the window.
    void setTopInset(int px);

signals:
    void setRequested();
    void allFormatsRequested();
    void neverRequested();
    void closeRequested();

protected:
    void changeEvent(QEvent* event) override;

private:
    void retranslate();

    QString m_type;
    QString m_typeName;
    QLabel* m_text = nullptr;
    QPushButton* m_set = nullptr;
    QPushButton* m_all = nullptr;
    QPushButton* m_never = nullptr;
    QToolButton* m_close = nullptr;
};

class DefaultAppOfferController : public QObject {
    Q_OBJECT
public:
    explicit DefaultAppOfferController(Application& app, int startDelayMs, int settleMs);
    ~DefaultAppOfferController() override; // waits for its worker thread

    // The bar currently shown (in any window), else nullptr.
    DefaultAppBar* visibleBar() const;
    // Number of system queries started and answered (tests).
    int queryCount() const { return m_queries; }
    int answeredQueryCount() const { return m_answered; }

signals:
    void barShown(lf::ViewerWindow* window);
    void queryFinished(); // a query came back, whatever it said (tests)

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void watchWindows();
    void imageShown(ViewerWindow* window, const QString& path);
    void evaluate();
    void showBar(ViewerWindow* window, const DefaultViewerStatus& status);
    void hideBar();
    void answer(const QString& type); // remembered as asked, bar hidden
    void setDefault(ViewerWindow* window);
    void updateInset(ViewerWindow* window);
    static bool fullScreenOrSlideshow(const ViewerWindow* window);

    Application& m_app;
    int m_startDelayMs;
    int m_settleMs;
    QElapsedTimer m_sinceFirstImage; // started with the first image shown
    QTimer* m_timer = nullptr;
    QPointer<ViewerWindow> m_pendingWindow;
    QString m_pendingPath;
    QSet<QString> m_seenSuffixes; // per session: a suffix is asked about once
    QString m_barSuffix;          // suffix of the file the bar was shown for
    bool m_queryRunning = false;
    int m_queries = 0;
    int m_answered = 0;
    QPointer<ViewerWindow> m_barWindow;
    QHash<ViewerWindow*, QPointer<DefaultAppBar>> m_bars;
    QThreadPool m_pool;
};

} // namespace lf
