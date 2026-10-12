// SPDX-License-Identifier: GPL-3.0-or-later
// lf_screenshots: the README/website screenshots and the frames of the demo animation, taken
// from real Lumaflip windows (docs/RELEASING.md "Screenshots").
//
//   lf_screenshots --photos build/sample-photos --out build/screenshots \
//                  --lang en --appearance dark --shots viewer,culling,demo [--scale 2]
//
// Every shot is a PNG "<shot>-<lang>-<appearance>.png" (the demo: a folder of frames and
// frames.txt) with the window frame drawn around it (ShotFrame.h). --scale renders at that
// device pixel ratio: if the screen has another one, the tool starts itself again with
// QT_SCALE_FACTOR set accordingly. Opens windows: run it in CI or on a machine nobody uses.

#include "ShotDriver.h"
#include "ShotFrame.h"

#include "app/RecentItems.h"
#include "app/Translations.h"
#include "core/ImageProvider.h"
#include "core/InputBindings.h"
#include "core/Settings.h"
#include "core/SortSpec.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QProcess>
#include <QScreen>
#include <QStyleHints>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

Q_DECLARE_METATYPE(lf::InputEvent)
Q_DECLARE_METATYPE(lf::SortSpec)

namespace {

int fail(const QString& text) {
    std::fprintf(stderr, "lf_screenshots: %s\n", qPrintable(text));
    return 2;
}

} // namespace

int main(int argc, char* argv[]) {
    // Like the integration tests: no EDR surface over the canvas (it is not in a widget grab)
    // and no "make Lumaflip the default app" bar.
    qputenv("LF_NO_EDR", "1");
    qputenv("LF_NO_DEFAULT_APP_OFFER", "1");
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Lumaflip Screenshots"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Screenshots of Lumaflip for the README and the website."));
    parser.addHelpOption();
    const QCommandLineOption photosOpt(
        QStringLiteral("photos"), QStringLiteral("Folder with the sample photos."), QStringLiteral("dir"));
    const QCommandLineOption outOpt(QStringLiteral("out"), QStringLiteral("Output folder."),
                                    QStringLiteral("dir"), QStringLiteral("build/screenshots"));
    const QCommandLineOption langOpt(QStringLiteral("lang"), QStringLiteral("UI language (en, de, ...)."),
                                     QStringLiteral("code"), QStringLiteral("en"));
    const QCommandLineOption appearanceOpt(QStringLiteral("appearance"), QStringLiteral("light or dark."),
                                           QStringLiteral("mode"), QStringLiteral("light"));
    const QCommandLineOption shotsOpt(QStringLiteral("shots"),
                                      QStringLiteral("Comma separated list or \"all\": %1.")
                                          .arg(lf::shots::knownShots().join(QLatin1Char(','))),
                                      QStringLiteral("list"), QStringLiteral("all"));
    const QCommandLineOption sizeOpt(QStringLiteral("size"), QStringLiteral("Window size in logical px."),
                                     QStringLiteral("WxH"), QStringLiteral("1280x800"));
    const QCommandLineOption scaleOpt(QStringLiteral("scale"),
                                      QStringLiteral("Device pixel ratio (default 2)."),
                                      QStringLiteral("factor"), QStringLiteral("2"));
    parser.addOptions({photosOpt, outOpt, langOpt, appearanceOpt, shotsOpt, sizeOpt, scaleOpt});
    parser.process(app);

    lf::shots::Options o;
    o.photos = parser.value(photosOpt);
    if (o.photos.isEmpty() || !QDir(o.photos).exists())
        return fail(QStringLiteral("--photos must name the folder of tools/fetch_sample_photos.py"));
    o.photos = QDir(o.photos).absolutePath();
    o.out = QDir(parser.value(outOpt)).absolutePath();
    o.language = parser.value(langOpt);
    const auto appearance = lf::shots::appearanceFromId(parser.value(appearanceOpt));
    if (!appearance)
        return fail(QStringLiteral("--appearance must be light or dark"));
    o.appearance = *appearance;
    QString error;
    const auto shots = lf::shots::parseShotList(parser.value(shotsOpt), &error);
    if (!shots)
        return fail(error);
    o.shots = *shots;
    const QStringList wh = parser.value(sizeOpt).split(QLatin1Char('x'));
    if (wh.size() != 2 || wh[0].toInt() < 640 || wh[1].toInt() < 400)
        return fail(QStringLiteral("--size must be WxH, at least 640x400"));
    o.windowSize = QSize(wh[0].toInt(), wh[1].toInt());

    // Device pixel ratio: run again with QT_SCALE_FACTOR when the screen has another one.
    const double scale = parser.value(scaleOpt).toDouble();
    if (scale < 1.0 || scale > 4.0)
        return fail(QStringLiteral("--scale must be between 1 and 4"));
    const double native =
        QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->devicePixelRatio() : 1;
    if (std::abs(native - scale) > 0.01) {
        if (qEnvironmentVariableIsSet("LF_SCREENSHOTS_CHILD"))
            return fail(QStringLiteral("device pixel ratio is %1, not %2 (QT_SCALE_FACTOR=%3)")
                            .arg(native)
                            .arg(scale)
                            .arg(qEnvironmentVariable("QT_SCALE_FACTOR")));
        QProcess child;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        const double base = env.contains(QStringLiteral("QT_SCALE_FACTOR"))
                                ? native / env.value(QStringLiteral("QT_SCALE_FACTOR")).toDouble()
                                : native;
        env.insert(QStringLiteral("QT_SCALE_FACTOR"), QString::number(scale / base));
        env.insert(QStringLiteral("LF_SCREENSHOTS_CHILD"), QStringLiteral("1"));
        child.setProcessEnvironment(env);
        child.setProcessChannelMode(QProcess::ForwardedChannels);
        QStringList args = QCoreApplication::arguments().mid(1);
        std::fprintf(stderr, "lf_screenshots: screen ratio %g, running again with QT_SCALE_FACTOR=%g\n",
                     native, scale / base);
        child.start(QCoreApplication::applicationFilePath(), args);
        if (!child.waitForFinished(-1))
            return fail(QStringLiteral("child process failed"));
        return child.exitCode();
    }

    // The app's settings in a scratch file: defaults plus the language of this run.
    QTemporaryDir settingsDir;
    lf::Settings::useTestingBackend(settingsDir.filePath(QStringLiteral("settings.ini")));
    lf::Settings::instance().setValue(QString::fromLatin1(lf::key::Language), o.language);
    lf::RecentItems::setSystemRegistrationEnabled(false);
    lf::installQtTranslations();
    if (lf::installedUiLanguage() != o.language)
        return fail(QStringLiteral("language %1 not available (installed: %2)")
                        .arg(o.language, lf::installedUiLanguage()));
    QGuiApplication::styleHints()->setColorScheme(
        o.appearance == lf::shots::Appearance::Dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);

    qRegisterMetaType<lf::InputEvent>();
    qRegisterMetaType<lf::SortSpec>();
    qRegisterMetaType<lf::DecodedImagePtr>("lf::DecodedImagePtr");
    qRegisterMetaType<lf::Quality>("lf::Quality");

    int rc = 0;
    {
        lf::shots::ShotDriver driver(o);
        rc = driver.run();
    }
    return rc;
}
