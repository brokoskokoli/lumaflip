// SPDX-License-Identifier: GPL-3.0-or-later
// M2 review fixes in the UI: native menu key equivalents (F-MAC-01), start screen with
// recent items (F-OPEN-12), default app checklist (F-MAC-04), info panel layout and
// formatting, executed input actions (F-IN-xx, F-VIEW-03). Real windows (cocoa).
//
// Never touches the user's system state: fake shell (no Finder, no default-app change),
// settings in a temporary backend, recent documents not written to the system list.

#include "app/Application.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/MainMenu.h"
#include "app/RecentItems.h"
#include "app/ViewerWindow.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/ImageProvider.h"
#include "core/InputBindings.h"
#include "core/Platform.h"
#include "core/Settings.h"
#include "support/NativeEvents.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QMenu>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QUndoStack>
#include <QtTest>

#include <memory>

using namespace lf;

namespace {

class NullTrash : public TrashService {
public:
    std::optional<QString> moveToTrash(const QString&, QString* error) override {
        if (error)
            *error = QStringLiteral("not in this test");
        return std::nullopt;
    }
    bool restoreFromTrash(const QString&, const QString&, QString*) override { return false; }
};

class FakeShell : public Shell {
public:
    void revealInFileManager(const QString& p) override { revealed << p; }
    void openWith(const QString&, const QString&) override {}
    QStringList imageDocumentTypes() const override { return types; }
    bool makeDefaultViewer(const QStringList& t, QString*) override {
        madeDefault << t;
        return true;
    }
    QStringList revealed;
    QStringList types{"public.jpeg", "public.png", "public.heic"};
    QList<QStringList> madeDefault;
};

QString makeImage(const QString& path, QColor color, QSize size = QSize(64, 48)) {
    QImage img(size, QImage::Format_RGB32);
    img.fill(color);
    return img.save(path) ? QDir::cleanPath(path) : QString();
}

} // namespace

class TstM2FixesUi : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<NullTrash> trash;
    std::unique_ptr<FileOps> ops;
    std::unique_ptr<FakeShell> shell;
    std::unique_ptr<Application> app;
    QList<Action> performed;
    bool swallow = false;

    QString path(const QString& name) const { return QDir::cleanPath(dir->path()) + QLatin1Char('/') + name; }
    static bool waitLoaded(ViewerWindow* w, int ms = 5000) {
        return QTest::qWaitFor([w] { return !w->collection()->isLoading(); }, ms);
    }
    static bool waitShown(ViewerWindow* w, const QString& p, int ms = 5000) {
        return QTest::qWaitFor([&] { return w->shownPath() == p && w->view()->image() != nullptr; }, ms);
    }
    ViewerWindow* openAndWait(const QString& file) {
        ViewerWindow* w = app->openPaths({file});
        if (!w || !QTest::qWaitForWindowExposed(w, 5000) || !waitLoaded(w) || !waitShown(w, file))
            return nullptr;
        return w;
    }
    void makeApp() {
        AppServices services;
        services.fileOps = ops.get();
        services.shell = shell.get();
        services.openUrl = [](const QUrl&) { return true; };
        services.quitApp = [] {};
        services.interceptAction = [this](ViewerWindow*, Action a) {
            performed << a;
            return swallow;
        };
        app = std::make_unique<Application>(services);
    }

private slots:
    void initTestCase() {
        qRegisterMetaType<lf::Quality>("lf::Quality");
        qRegisterMetaType<lf::DecodedImagePtr>("lf::DecodedImagePtr");
        RecentItems::setSystemRegistrationEnabled(false);
        Settings::useTestingBackend(m_settingsDir.filePath(QStringLiteral("init.ini")));
        ImageView probe;
        probe.resize(64, 64);
        probe.show();
        QVERIFY(QTest::qWaitForWindowExposed(&probe));
        auto* c = probe.findChild<ImageCanvas*>();
        QVERIFY(c);
        if (c->grabFramebuffer().isNull() || !c->isRhiAvailable())
            QSKIP("No QRhi (GPU) available on this platform");
    }

    void init() {
        Settings::useTestingBackend(
            m_settingsDir.filePath(QStringLiteral("s%1.ini").arg(++m_settingsCounter)));
        dir = std::make_unique<QTemporaryDir>();
        QVERIFY(dir->isValid());
        trash = std::make_unique<NullTrash>();
        ops = std::make_unique<FileOps>(*trash);
        shell = std::make_unique<FakeShell>();
        performed.clear();
        swallow = false;
        makeApp();
    }

    void cleanup() {
        app.reset();
        QTest::qWait(10);
        QThreadPool::globalInstance()->waitForDone(5000);
        ops.reset();
        trash.reset();
        shell.reset();
        dir.reset();
    }

    // F-MAC-01: every default menu shortcut of SPEC 5.2, sent as a real NSEvent through
    // [NSApp sendEvent:] (window key equivalents, main menu, Qt key handling), triggers
    // exactly its action, once. Regression: ⌘R revealed the file in the Finder (⇧⌘R).
    void F_MAC_01_nativeKeyEquivalentsRouteCorrectly() {
        if (!lf::test::nativeEventsAvailable())
            QSKIP("needs the cocoa platform");
        QVERIFY(!makeImage(path("a.png"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path("b.png"), Qt::green).isEmpty());
        ViewerWindow* w = openAndWait(path("a.png"));
        QVERIFY(w);
        w->raise();
        w->activateWindow();
        // Undo and redo enabled (no-op commands; the actions themselves are swallowed).
        QUndoStack* stack = app->fileOps().undoStack();
        stack->push(new QUndoCommand(QStringLiteral("eins")));
        stack->push(new QUndoCommand(QStringLiteral("zwei")));
        stack->undo();
        // Commands without an Action: recorded instead of run (no dialogs).
        QStringList commands;
        for (const char* id : {"open", "openFolder", "minimize", "zoomWindow"}) {
            QAction* act = app->menu()->command(QString::fromLatin1(id));
            QVERIFY(act);
            QObject::disconnect(act, &QAction::triggered, nullptr, nullptr);
            connect(act, &QAction::triggered, this, [&commands, id] { commands << QString::fromLatin1(id); });
        }
        swallow = true;
        const Qt::KeyboardModifiers cmd = Qt::ControlModifier, shift = Qt::ShiftModifier,
                                    alt = Qt::AltModifier, ctrl = Qt::MetaModifier;
        struct Case {
            Qt::Key key;
            Qt::KeyboardModifiers mods;
            Action expected; // None: only `command` (or nothing at all)
            const char* command = nullptr;
        };
        const Case cases[] = {
            {Qt::Key_N, cmd, Action::NewWindow},
            {Qt::Key_N, alt | cmd, Action::DuplicateWindow},
            {Qt::Key_O, cmd, Action::None, "open"},
            {Qt::Key_O, shift | cmd, Action::None, "openFolder"},
            {Qt::Key_Up, cmd, Action::FirstImage},
            {Qt::Key_Down, cmd, Action::LastImage},
            {Qt::Key_Equal, cmd, Action::ZoomIn},
            {Qt::Key_Minus, cmd, Action::ZoomOut},
            {Qt::Key_0, cmd, Action::ZoomActual},
            {Qt::Key_9, cmd, Action::ZoomFit},
            {Qt::Key_F, ctrl | cmd, Action::ToggleFullScreen},
            {Qt::Key_F, shift | cmd, Action::ToggleSlideshow},
            {Qt::Key_F, cmd, Action::None}, // nothing (was: slideshow)
            {Qt::Key_L, cmd, Action::RotateLeft},
            {Qt::Key_R, cmd, Action::RotateRight},
            {Qt::Key_R, shift | cmd, Action::RevealInFinder},
            {Qt::Key_I, cmd, Action::ToggleInfoPanel},
            {Qt::Key_Backspace, cmd, Action::Trash},
            {Qt::Key_Z, cmd, Action::Undo},
            {Qt::Key_Z, shift | cmd, Action::Redo},
            {Qt::Key_C, cmd, Action::CopyImage},
            {Qt::Key_C, alt | cmd, Action::CopyPath},
            {Qt::Key_S, cmd, Action::SaveRotation},
            {Qt::Key_W, cmd, Action::CloseWindow},
            {Qt::Key_W, alt | cmd, Action::CloseAllWindows},
            {Qt::Key_Down, alt, Action::NextPage},
            {Qt::Key_Up, alt, Action::PreviousPage},
            {Qt::Key_M, cmd, Action::None, "minimize"},
            {Qt::Key_Comma, cmd, Action::OpenPreferences},
            // Single keys (SPEC 5.2) next to menu items with the same letter.
            {Qt::Key_Z, {}, Action::ToggleFitActualAtCursor},
            {Qt::Key_Z, shift, Action::ToggleZoomLock},
            {Qt::Key_F, {}, Action::ToggleFullScreen},
            {Qt::Key_S, {}, Action::ToggleSlideshow},
            {Qt::Key_H, {}, Action::FlipHorizontal},
            {Qt::Key_H, shift, Action::FlipVertical},
            {Qt::Key_I, {}, Action::ToggleInfoPanel},
            {Qt::Key_T, {}, Action::ToggleFilmstrip},
            {Qt::Key_E, {}, Action::OpenInEditor},
            {Qt::Key_1, {}, Action::ZoomFit},
            {Qt::Key_2, {}, Action::ZoomFillWidth},
            {Qt::Key_3, {}, Action::ZoomActual},
            {Qt::Key_Return, {}, Action::Rename},
            {Qt::Key_F2, {}, Action::Rename},
            {Qt::Key_Delete, {}, Action::Trash},
            {Qt::Key_Right, {}, Action::NextImage},
            {Qt::Key_Left, {}, Action::PreviousImage},
            {Qt::Key_Escape, {}, Action::ExitModeOrNothing},
        };
        QStringList failures;
        for (const Case& c : cases) {
            performed.clear();
            commands.clear();
            const QString name =
                QKeySequence(QKeyCombination(c.mods, c.key)).toString(QKeySequence::NativeText);
            if (!lf::test::sendNativeKey(w, c.key, c.mods)) {
                failures << name + QStringLiteral(": not sent");
                continue;
            }
            const QList<Action> want =
                c.expected == Action::None ? QList<Action>() : QList<Action>{c.expected};
            const QStringList wantCommands =
                c.command ? QStringList{QString::fromLatin1(c.command)} : QStringList();
            // Menu actions are queued: wait for the first one, then for a possible second.
            if (!want.isEmpty() || !wantCommands.isEmpty())
                QTest::qWaitFor([&] { return !performed.isEmpty() || !commands.isEmpty(); }, 2000);
            QTest::qWait(want.isEmpty() && wantCommands.isEmpty() ? 200 : 60);
            if (performed != want || commands != wantCommands) {
                QStringList got;
                for (Action a : performed)
                    got << actionId(a);
                failures << QStringLiteral("%1: expected %2%3, got [%4] [%5]")
                                .arg(name, c.expected == Action::None ? QString() : actionId(c.expected),
                                     wantCommands.join(QLatin1Char(',')), got.join(QLatin1Char(',')),
                                     commands.join(QLatin1Char(',')));
            }
        }
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));
        QVERIFY(shell->revealed.isEmpty()); // never the real Finder, never even the fake
        // The native items carry AppKit's convention for shifted letters.
        QCOMPARE(lf::test::nativeKeyEquivalent(app->menu()->action(Action::RevealInFinder)),
                 QStringLiteral("⇧⌘R"));
        QCOMPARE(lf::test::nativeKeyEquivalent(app->menu()->action(Action::RotateRight)),
                 QStringLiteral("⌘r"));
    }
};

int main(int argc, char* argv[]) {
#ifdef Q_OS_MACOS
    if (qgetenv("QT_QPA_PLATFORM") == "offscreen")
        qputenv("QT_QPA_PLATFORM", "cocoa");
#endif
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("LumaflipTest"));
    TstM2FixesUi tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_m2_fixes_ui.moc"
