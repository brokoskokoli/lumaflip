// SPDX-License-Identifier: GPL-3.0-or-later
// M4 macOS features, the testable logic without windows:
//  F-MAC-06 Live Text requests (fake analyzer), F-MAC-07 Services types, F-FILE-09 screen
//  choice of the desktop picture (fake workspace), F-FILE-10 custom actions (argument
//  passing safety, timeout, Shortcuts command line).

#include "QtTestApp.h"
#include "core/CustomActions.h"
#include "core/InputBindings.h"
#include "core/LiveText.h"
#include "core/ServiceTypes.h"
#include "core/Settings.h"
#include "core/VirtualPath.h"
#include "core/Wallpaper.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <csignal>

using namespace lf;

namespace {

// ---- F-MAC-06 ----

class FakeAnalyzer : public ImageAnalysisBackend {
public:
    bool supported = true;
    QList<quint64> started;
    int cancels = 0;
    QSize lastSize;
    bool isSupported() const override { return supported; }
    void analyze(quint64 token, const QImage& image) override {
        started << token;
        lastSize = image.size();
    }
    void cancel() override { ++cancels; }
};

QImage image(int w = 40, int h = 30) {
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(Qt::white);
    return img;
}

// ---- F-FILE-09 ----

class FakeWorkspace : public WallpaperService {
public:
    QList<WallpaperScreen> list;
    QHash<QString, QString> set; // screen -> image
    QSet<QString> failing;
    QList<WallpaperScreen> screens() const override { return list; }
    bool setDesktopImage(const QString& id, const QString& path, QString* error) override {
        if (failing.contains(id)) {
            *error = QStringLiteral("verweigert");
            return false;
        }
        set.insert(id, path);
        return true;
    }
};

WallpaperScreen screen(const char* id, bool main, bool window) {
    return WallpaperScreen{QString::fromLatin1(id), QStringLiteral("Bildschirm ") + QLatin1String(id), main,
                           window};
}

// ---- F-FILE-10 ----

CustomActionRun::Result runAction(const CustomAction& a, const QStringList& files, int timeoutMs = 10000,
                                  const QString& tool = QStringLiteral("/usr/bin/shortcuts")) {
    test::ensureQtApp();
    CustomActionRun run(a, files, timeoutMs, nullptr, tool);
    bool done = false;
    CustomActionRun::Result result;
    QObject::connect(&run, &CustomActionRun::finished, [&](const CustomActionRun::Result& r) {
        result = r;
        done = true;
    });
    run.start();
    EXPECT_TRUE(test::waitUntil([&] { return done; }, timeoutMs + 10000));
    return result;
}

CustomAction shell(const QString& command) {
    CustomAction a;
    a.id = QStringLiteral("t");
    a.name = QStringLiteral("Test");
    a.kind = CustomAction::Kind::Shell;
    a.command = command;
    return a;
}

} // namespace

// =========================================================================================
// F-MAC-06 Live Text
// =========================================================================================

TEST(LiveText, F_MAC_06_AnalysisOnlyOnDemand) {
    FakeAnalyzer fake;
    LiveTextSession s(&fake);
    s.setImage(QStringLiteral("/a.jpg"), image());
    EXPECT_TRUE(fake.started.isEmpty()); // not switched on: nothing is analysed
    EXPECT_EQ(s.state(), LiveTextSession::State::Off);
    EXPECT_TRUE(s.toggle());
    ASSERT_EQ(fake.started.size(), 1);
    EXPECT_EQ(s.state(), LiveTextSession::State::Analyzing);
    EXPECT_EQ(fake.lastSize, QSize(40, 30));
    s.analysisFinished(fake.started.last(), true, QString());
    EXPECT_EQ(s.state(), LiveTextSession::State::Ready);
    EXPECT_TRUE(s.overlayVisible());
    // Another stage of the same file without more detail does not start another analysis.
    s.setImage(QStringLiteral("/a.jpg"), image(44, 33));
    EXPECT_EQ(fake.started.size(), 1);
    EXPECT_TRUE(s.overlayVisible());
}

TEST(LiveText, F_MAC_06_BetterStageIsAnalysedAgain) {
    FakeAnalyzer fake;
    LiveTextSession s(&fake);
    s.setActive(true);
    s.setImage(QStringLiteral("/a.jpg"), image()); // the small preview stage
    ASSERT_EQ(fake.started.size(), 1);
    const quint64 preview = fake.started.last();
    // The screen stage arrives while the preview is analysed: cancelled, analysed again.
    s.setImage(QStringLiteral("/a.jpg"), image(80, 60));
    ASSERT_EQ(fake.started.size(), 2);
    EXPECT_GE(fake.cancels, 1);
    EXPECT_EQ(fake.lastSize, QSize(80, 60));
    s.analysisFinished(preview, true, QString()); // late: ignored
    EXPECT_EQ(s.state(), LiveTextSession::State::Analyzing);
    s.analysisFinished(fake.started.last(), true, QString());
    EXPECT_EQ(s.state(), LiveTextSession::State::Ready);
    // After a finished analysis too.
    s.setImage(QStringLiteral("/a.jpg"), image(400, 300));
    EXPECT_EQ(fake.started.size(), 3);
    // Beyond what the analysis uses: no further run.
    FakeAnalyzer big;
    LiveTextSession t(&big);
    t.setActive(true);
    t.setImage(QStringLiteral("/b.jpg"), image(LiveTextSession::kMaxAnalysisSide, 100));
    t.setImage(QStringLiteral("/b.jpg"), image(2 * LiveTextSession::kMaxAnalysisSide, 200));
    EXPECT_EQ(big.started.size(), 1);
}

TEST(LiveText, F_MAC_06_FlippingCancelsAndIgnoresLateResults) {
    FakeAnalyzer fake;
    LiveTextSession s(&fake);
    s.setActive(true);
    s.setImage(QStringLiteral("/a.jpg"), image());
    const quint64 first = fake.started.last();
    s.setImage(QStringLiteral("/b.jpg"), image());
    EXPECT_GE(fake.cancels, 1);
    ASSERT_EQ(fake.started.size(), 2);
    // The late result of the first image changes nothing.
    s.analysisFinished(first, true, QString());
    EXPECT_EQ(s.state(), LiveTextSession::State::Analyzing);
    s.analysisFinished(fake.started.last(), false, QString());
    EXPECT_EQ(s.state(), LiveTextSession::State::NoResults);
    EXPECT_FALSE(s.overlayVisible());
    // No image yet (loading): waits without analysing.
    s.setImage(QStringLiteral("/c.jpg"), QImage());
    EXPECT_EQ(s.state(), LiveTextSession::State::Waiting);
    EXPECT_EQ(fake.started.size(), 2);
}

TEST(LiveText, F_MAC_06_SwitchingOffSettingAndBlockingCancel) {
    FakeAnalyzer fake;
    LiveTextSession s(&fake);
    s.setActive(true);
    s.setImage(QStringLiteral("/a.jpg"), image());
    const int before = fake.cancels;
    s.setActive(false);
    EXPECT_GT(fake.cancels, before);
    EXPECT_EQ(s.state(), LiveTextSession::State::Off);
    EXPECT_TRUE(s.statusText().isEmpty());
    // The setting off: unavailable, nothing runs.
    s.setAllowed(false);
    s.setActive(true);
    EXPECT_EQ(s.state(), LiveTextSession::State::Unavailable);
    EXPECT_EQ(fake.started.size(), 1);
    EXPECT_FALSE(s.statusText().isEmpty());
    s.setAllowed(true);
    EXPECT_EQ(fake.started.size(), 2); // back on: the shown image is analysed
    // Blocked (EDR surface): cancelled, the reason is the status.
    s.setBlocked(QStringLiteral("HDR"));
    EXPECT_EQ(s.state(), LiveTextSession::State::Unavailable);
    EXPECT_EQ(s.statusText(), QStringLiteral("HDR"));
    s.analysisFinished(fake.started.last(), true, QString());
    EXPECT_FALSE(s.overlayVisible());
    s.setBlocked(QString());
    EXPECT_EQ(fake.started.size(), 3);
    // Unsupported hardware.
    FakeAnalyzer none;
    none.supported = false;
    LiveTextSession t(&none);
    t.setImage(QStringLiteral("/a.jpg"), image());
    t.setActive(true);
    EXPECT_EQ(t.state(), LiveTextSession::State::Unavailable);
    EXPECT_TRUE(none.started.isEmpty());
}

TEST(LiveText, F_MAC_06_FailureIsReported) {
    FakeAnalyzer fake;
    LiveTextSession s(&fake);
    s.setActive(true);
    s.setImage(QStringLiteral("/a.jpg"), image());
    s.analysisFinished(fake.started.last(), false, QStringLiteral("kaputt"));
    EXPECT_EQ(s.state(), LiveTextSession::State::Failed);
    EXPECT_TRUE(s.statusText().contains(QStringLiteral("kaputt")));
}

// =========================================================================================
// F-MAC-07 Services
// =========================================================================================

TEST(Services, F_MAC_07_ProvidesFileUrlAndImageOfTheCurrentFile) {
    services::Context file{QStringLiteral("/Fotos/a.jpg"), true};
    EXPECT_TRUE(services::canProvide(QStringLiteral("public.file-url"), QString(), file));
    EXPECT_TRUE(services::canProvide(QStringLiteral("public.tiff"), QString(), file));
    EXPECT_TRUE(services::canProvide(QStringLiteral("public.png"), QString(), file));
    EXPECT_FALSE(services::canProvide(QStringLiteral("public.utf8-plain-text"), QString(), file));
    // No service may send something back (the image is not editable through services).
    EXPECT_FALSE(
        services::canProvide(QStringLiteral("public.file-url"), QStringLiteral("public.tiff"), file));
    // Nothing shown yet: only the file URL.
    services::Context loading{QStringLiteral("/Fotos/a.jpg"), false};
    EXPECT_TRUE(services::canProvide(QStringLiteral("public.file-url"), QString(), loading));
    EXPECT_FALSE(services::canProvide(QStringLiteral("public.tiff"), QString(), loading));
    // An archive member has no file of its own: pixels only.
    services::Context member{vpath::join(QStringLiteral("/a/b.cbz"), QStringLiteral("1.jpg")), true};
    EXPECT_FALSE(services::canProvide(QStringLiteral("public.file-url"), QString(), member));
    EXPECT_TRUE(services::canProvide(QStringLiteral("public.png"), QString(), member));
    services::Context empty;
    for (const QString& t : services::sendTypes())
        EXPECT_FALSE(services::canProvide(t, QString(), empty));
    EXPECT_EQ(services::typesToWrite({QStringLiteral("public.file-url"), QStringLiteral("public.rtf"),
                                      QStringLiteral("public.tiff")},
                                     member),
              QStringList{QStringLiteral("public.tiff")});
    EXPECT_EQ(services::sendTypes(), (QStringList{"public.file-url", "public.tiff", "public.png"}));
}

TEST(Services, F_MAC_07_OpenServiceTakesLocalFilesAndMatchesInfoPlist) {
    const QStringList files = services::filesFromUrls({QUrl::fromLocalFile(QStringLiteral("/a/b.jpg")),
                                                       QUrl(QStringLiteral("https://example.com/x.jpg")),
                                                       QUrl::fromLocalFile(QStringLiteral("/a/b.jpg")),
                                                       QUrl::fromLocalFile(QStringLiteral("/a/../c/d.cbz"))});
    EXPECT_EQ(files, (QStringList{"/a/b.jpg", "/c/d.cbz"}));
    // Info.plist: the service "In Lumaflip öffnen" with exactly these file types, and the
    // archive/PDF document types as Alternate (the default apps stay).
    QFile plist(QStringLiteral(LF_SRC_DIR "/packaging/macos/Info.plist.in"));
    ASSERT_TRUE(plist.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(plist.readAll());
    EXPECT_TRUE(text.contains(QStringLiteral("<key>NSMessage</key><string>openInLumaflip</string>")));
    const qsizetype send = text.indexOf(QStringLiteral("<key>NSSendFileTypes</key>"));
    ASSERT_GE(send, 0);
    const QString block = text.mid(send, text.indexOf(QStringLiteral("</array>"), send) - send);
    for (const QString& t : services::openServiceFileTypes())
        EXPECT_TRUE(block.contains(QStringLiteral("<string>%1</string>").arg(t))) << t.toStdString();
    for (const char* type : {"com.adobe.pdf", "public.zip-archive", "com.rarlab.rar-archive",
                             "io.github.brokoskokoli.lumaflip.cbz"}) {
        const qsizetype at = text.indexOf(QStringLiteral("<string>%1</string>").arg(QLatin1String(type)));
        ASSERT_GE(at, 0) << type;
        const qsizetype dictStart = text.lastIndexOf(QStringLiteral("<dict>"), at);
        const QString dict = text.mid(dictStart, at - dictStart);
        if (dict.contains(QStringLiteral("LSHandlerRank")))
            EXPECT_TRUE(dict.contains(QStringLiteral("<string>Alternate</string>"))) << type;
    }
}

// =========================================================================================
// F-FILE-09 Desktop picture
// =========================================================================================

TEST(Wallpaper, F_FILE_09_ScreenSelection) {
    using wallpaper::Target;
    const QList<WallpaperScreen> three = {screen("1", true, false), screen("2", false, true),
                                          screen("3", false, false)};
    EXPECT_EQ(wallpaper::selectScreens(three, Target::WindowScreen), QStringList{QStringLiteral("2")});
    EXPECT_EQ(wallpaper::selectScreens(three, Target::AllScreens), (QStringList{"1", "2", "3"}));
    // Window on no screen (minimized): the main screen; nothing marked: the first.
    const QList<WallpaperScreen> noWindow = {screen("1", false, false), screen("2", true, false)};
    EXPECT_EQ(wallpaper::selectScreens(noWindow, Target::WindowScreen), QStringList{QStringLiteral("2")});
    const QList<WallpaperScreen> plain = {screen("7", false, false), screen("8", false, false)};
    EXPECT_EQ(wallpaper::selectScreens(plain, Target::WindowScreen), QStringList{QStringLiteral("7")});
    EXPECT_TRUE(wallpaper::selectScreens({}, Target::AllScreens).isEmpty());
}

TEST(Wallpaper, F_FILE_09_AppliesThroughTheWorkspaceAndReportsErrors) {
    FakeWorkspace ws;
    ws.list = {screen("1", true, false), screen("2", false, true)};
    wallpaper::Result r =
        wallpaper::apply(ws, QStringLiteral("/Fotos/a.jpg"), wallpaper::Target::WindowScreen);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(ws.set.value(QStringLiteral("2")), QStringLiteral("/Fotos/a.jpg"));
    EXPECT_FALSE(ws.set.contains(QStringLiteral("1")));
    ws.failing.insert(QStringLiteral("1"));
    r = wallpaper::apply(ws, QStringLiteral("/Fotos/b.jpg"), wallpaper::Target::AllScreens);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.changed, 1);
    ASSERT_EQ(r.errors.size(), 1);
    EXPECT_TRUE(r.errors.first().contains(QStringLiteral("Bildschirm 1")));
    // Archive members and PDF pages are no files; nothing is changed for them.
    ws.set.clear();
    r = wallpaper::apply(ws, vpath::join(QStringLiteral("/a/b.cbz"), QStringLiteral("1.jpg")),
                         wallpaper::Target::AllScreens);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(ws.set.isEmpty());
    EXPECT_FALSE(wallpaper::checkImage(QString()).isEmpty());
}

// =========================================================================================
// F-FILE-10 Custom actions
// =========================================================================================

#ifndef Q_OS_WIN // POSIX shell and Shortcuts (Windows: test_windows_platform.cpp)
TEST(CustomActions, F_FILE_10_ShellArgumentsAreNeverInterpreted) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString base = QDir::cleanPath(dir.path());
    // File names that would run commands if they reached a shell's parser.
    const QStringList evil = {
        base + QStringLiteral("/a b \"c\" 'd'.jpg"), base + QStringLiteral("/$(touch PWNED1).jpg"),
        base + QStringLiteral("/`touch PWNED2`.jpg"), base + QStringLiteral("/x; touch PWNED3 #.jpg"),
        base + QStringLiteral("/-n.jpg")};
    for (const QString& f : evil) {
        QFile file(f);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly)) << f.toStdString();
    }
    const QString out = base + QStringLiteral("/out.txt");
    const CustomAction a = shell(
        QStringLiteral("for f in \"$@\"; do printf '%s\\n' \"$f\"; done > \"$(dirname \"$1\")/out.txt\""));
    const customactions::ProcessSpec spec = customactions::processFor(a, evil);
    EXPECT_EQ(spec.program, QStringLiteral("/bin/sh"));
    EXPECT_EQ(spec.arguments.mid(0, 3),
              (QStringList{"-c", a.command, QString::fromLatin1(customactions::kShellArgv0)}));
    EXPECT_EQ(spec.arguments.mid(3), evil); // the paths are separate arguments, untouched
    for (const QString& arg : spec.arguments.mid(0, 2))
        for (const QString& f : evil)
            EXPECT_FALSE(arg.contains(f)); // never part of the script
    const CustomActionRun::Result r = runAction(a, evil);
    EXPECT_TRUE(r.ok()) << r.error.toStdString() << r.output.toStdString();
    QFile result(out);
    ASSERT_TRUE(result.open(QIODevice::ReadOnly));
    EXPECT_EQ(QString::fromUtf8(result.readAll()).split(u'\n', Qt::SkipEmptyParts), evil);
    for (const char* marker : {"PWNED1", "PWNED2", "PWNED3"}) {
        EXPECT_FALSE(QFile::exists(base + u'/' + QLatin1String(marker))) << marker;
        EXPECT_FALSE(QFile::exists(QDir::currentPath() + u'/' + QLatin1String(marker))) << marker;
    }
}

TEST(CustomActions, F_FILE_10_ExitCodeAndOutputAreReported) {
    const CustomActionRun::Result r =
        runAction(shell(QStringLiteral("echo hallo; echo fehler >&2; exit 3")), {});
    EXPECT_TRUE(r.started);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.exitCode, 3);
    EXPECT_TRUE(r.output.contains(QStringLiteral("hallo")));
    EXPECT_TRUE(r.output.contains(QStringLiteral("fehler")));
    EXPECT_TRUE(r.error.contains(QStringLiteral("3")));
    // Output is bounded.
    const CustomActionRun::Result big =
        runAction(shell(QStringLiteral("head -c 300000 /dev/zero | tr '\\0' x")), {});
    EXPECT_TRUE(big.ok());
    EXPECT_LE(big.output.size(), customactions::kMaxOutputBytes);
}

TEST(CustomActions, F_FILE_10_TimeoutKillsTheCommandAndItsChildren) {
    QTemporaryDir dir;
    const QString pidFile = QDir::cleanPath(dir.path()) + QStringLiteral("/pid");
    QElapsedTimer t;
    t.start();
    // A child that would outlive its shell: the whole process group goes.
    const CustomActionRun::Result r =
        // 2 s: long enough for the shell to start the child and write its pid even under
        // heavy load; still far below the child's 30 s.
        runAction(shell(QStringLiteral("sleep 30 & echo $! > \"$1\"; wait")), {pidFile}, 2000);
    EXPECT_LT(t.elapsed(), 12000);
    EXPECT_TRUE(r.timedOut);
    EXPECT_FALSE(r.ok());
    EXPECT_FALSE(r.error.isEmpty());
    QFile f(pidFile);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const qint64 pid = f.readAll().trimmed().toLongLong();
    ASSERT_GT(pid, 0);
    EXPECT_TRUE(test::waitUntil([&] { return ::kill(pid_t(pid), 0) != 0; }, 3000)) << "sleep still running";
}

TEST(CustomActions, F_FILE_10_ShortcutsCommandConstruction) {
    CustomAction a;
    a.id = QStringLiteral("s");
    a.name = QStringLiteral("Hochladen");
    a.kind = CustomAction::Kind::Shortcut;
    a.command = QStringLiteral("Foto hochladen; rm -rf ~");
    const QStringList files{QStringLiteral("/Fotos/a b.jpg"), QStringLiteral("/Fotos/$(x).jpg")};
    const customactions::ProcessSpec spec = customactions::processFor(a, files);
    EXPECT_EQ(spec.program, QStringLiteral("/usr/bin/shortcuts"));
    EXPECT_EQ(spec.arguments, (QStringList{"run", "Foto hochladen; rm -rf ~", "--input-path",
                                           "/Fotos/a b.jpg", "/Fotos/$(x).jpg"}));
    // A name that would be read as an option is refused.
    a.command = QStringLiteral("--output-path");
    EXPECT_FALSE(customactions::validate(a).isEmpty());
    a.command = QStringLiteral("Foto hochladen");
    EXPECT_TRUE(customactions::validate(a).isEmpty());
    a.name.clear();
    EXPECT_FALSE(customactions::validate(a).isEmpty());
    // Run with a stand-in for the Shortcuts tool: it receives exactly these arguments.
    a.name = QStringLiteral("Hochladen");
    const CustomActionRun::Result r = runAction(a, files, 10000, QStringLiteral("/bin/echo"));
    EXPECT_TRUE(r.ok()) << r.error.toStdString();
    EXPECT_EQ(r.output.trimmed(),
              QStringLiteral("run Foto hochladen --input-path /Fotos/a b.jpg /Fotos/$(x).jpg"));
    // A missing tool fails cleanly.
    const CustomActionRun::Result missing =
        runAction(a, files, 10000, QStringLiteral("/nonexistent/shortcuts"));
    EXPECT_FALSE(missing.started);
    EXPECT_FALSE(missing.error.isEmpty());
}

#endif

TEST(CustomActions, F_FILE_10_StoredAsJsonWithKeysAndConfirmation) {
    CustomAction a;
    a.id = QStringLiteral("abc");
    a.name = QStringLiteral("In Vorschau");
    a.command = QStringLiteral("open -a Preview \"$1\"");
    a.keys = QKeySequence(QStringLiteral("Ctrl+Alt+1"));
    a.confirmed = true;
    CustomAction b;
    b.id = QStringLiteral("def");
    b.name = QStringLiteral("Kurzbefehl");
    b.kind = CustomAction::Kind::Shortcut;
    b.command = QStringLiteral("Foto teilen");
    const QList<CustomAction> back = customactions::fromJson(customactions::toJson({a, b}));
    ASSERT_EQ(back.size(), 2);
    EXPECT_EQ(back[0], a);
    EXPECT_EQ(back[1], b);
    EXPECT_FALSE(back[1].confirmed);
    EXPECT_TRUE(customactions::fromJson(QStringLiteral("not json")).isEmpty());
    EXPECT_FALSE(customactions::fromJson(QStringLiteral("[{\"name\":\"x\"}]")).first().id.isEmpty());
    // An empty or invalid action never starts a process.
    const CustomActionRun::Result r = runAction(shell(QString()), {});
    EXPECT_FALSE(r.started);
    EXPECT_FALSE(r.error.isEmpty());
}

// F-FILE-10: keys without ⌘/⌃ (F5, K, ⇧K, ⌥1) are matched in the viewer window itself (the
// menu never sees them); the editor names the app actions the key already triggers.
TEST(CustomActions, F_FILE_10_PlainKeysMatchAndConflictsAreFound) {
    CustomAction a;
    a.keys = QKeySequence(Qt::Key_F5);
    EXPECT_TRUE(customactions::matchesKey(a, Qt::Key_F5, Qt::NoModifier));
    EXPECT_TRUE(customactions::matchesKey(a, Qt::Key_F5, Qt::KeypadModifier));
    EXPECT_FALSE(customactions::matchesKey(a, Qt::Key_F5, Qt::ShiftModifier));
    EXPECT_FALSE(customactions::matchesKey(a, Qt::Key_F6, Qt::NoModifier));
    a.keys = QKeySequence(Qt::ShiftModifier | Qt::Key_K);
    EXPECT_TRUE(customactions::matchesKey(a, Qt::Key_K, Qt::ShiftModifier));
    EXPECT_FALSE(customactions::matchesKey(a, Qt::Key_K, Qt::NoModifier));
    a.keys = QKeySequence(Qt::AltModifier | Qt::Key_1);
    EXPECT_TRUE(customactions::matchesKey(a, Qt::Key_1, Qt::AltModifier));
    EXPECT_FALSE(customactions::matchesKey(a, Qt::Key_1, Qt::NoModifier));
    a.keys = QKeySequence(Qt::Key_Question); // recorded without Shift, typed with it
    EXPECT_TRUE(customactions::matchesKey(a, Qt::Key_Question, Qt::ShiftModifier));
    a.keys = QKeySequence();
    EXPECT_FALSE(customactions::matchesKey(a, Qt::Key_F5, Qt::NoModifier));

    test::ensureQtApp();
    QTemporaryDir dir;
    Settings::useTestingBackend(dir.filePath(QStringLiteral("keys.ini")));
    InputBindings bindings;
    EXPECT_FALSE(customactions::bindingConflicts(QKeySequence(Qt::Key_K), bindings).isEmpty());
    EXPECT_FALSE(customactions::bindingConflicts(QKeySequence(Qt::Key_Right), bindings).isEmpty());
    EXPECT_TRUE(customactions::bindingConflicts(QKeySequence(Qt::Key_F19), bindings).isEmpty());
    EXPECT_TRUE(customactions::bindingConflicts(QKeySequence(), bindings).isEmpty());
}
