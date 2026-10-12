// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Drives the real app into the states shown on the README and the website and saves them
// (tools/screenshots/main.cpp). Everything runs on the GUI thread of this process with the
// app's own classes: an Application with a viewer window per shot, opened on a temporary copy
// of the sample photos (ratings and Finder tags are written there, never to the originals).

#include "ShotFrame.h"

#include "core/Decoder.h"

#include <QHash>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <functional>
#include <memory>

class QWidget;

namespace lf {
class Application;
class ViewerWindow;
} // namespace lf

namespace lf::shots {

struct Options {
    QString photos; // folder with the sample photos (tools/fetch_sample_photos.py)
    QString out;    // output folder
    QString language = QStringLiteral("en");
    Appearance appearance = Appearance::Light;
    QStringList shots;
    QSize windowSize = QSize(1280, 800); // logical px
};

class ShotDriver {
public:
    explicit ShotDriver(Options options);
    ~ShotDriver();
    int run(); // 0 = every requested shot was saved

private:
    bool prepareFolder();
    void prepareRatings();
    QString path(const QString& name) const;
    ViewerWindow* openImage(const QString& name);
    bool ensureSize(QWidget* window, QSize size);
    bool waitImageReady(ViewerWindow* window, const QString& path);
    void closeWindows();
    // The window as drawn by Qt, including the GPU canvas (QRhiWidget).
    QImage grabWindow(QWidget* window);
    // Grabs until two grabs in a row are identical (thumbnails, metadata and fades done).
    QImage grabStable(QWidget* window, const std::function<void()>& prepare = {}, int timeoutMs = 20000);
    void revealTitle(ViewerWindow* window);
    FrameStyle mainWindowStyle(ViewerWindow* window) const;
    bool save(const QImage& content, const FrameStyle& style, const QString& file);

    bool shotViewer();
    bool shotInfo();
    bool shotCulling();
    bool shotGrid();
    bool shotCompare();
    bool shotMap();
    bool shotPreferences();
    bool shotDemo();

    Options m_o;
    QTemporaryDir m_work;
    QString m_folder;
    std::unique_ptr<Application> m_app;
    QHash<QString, Quality> m_quality; // best stage shown per path
    double m_dpr = 1.0;
};

} // namespace lf::shots
