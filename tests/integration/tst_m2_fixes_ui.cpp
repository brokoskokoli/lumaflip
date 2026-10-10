// SPDX-License-Identifier: GPL-3.0-or-later
// M2 review fixes in the UI: native menu key equivalents (F-MAC-01), start screen with
// recent items (F-OPEN-12), default app checklist (F-MAC-04), info panel layout and
// formatting, executed input actions (F-IN-xx, F-VIEW-03). Real windows (cocoa).
//
// Never touches the user's system state: fake shell (no Finder, no default-app change),
// settings in a temporary backend, recent documents not written to the system list.

#include "app/Application.h"
#include "app/DefaultAppDialog.h"
#include "app/DisplayText.h"
#include "app/Filmstrip.h"
#include "app/HistogramWidget.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/InfoPanel.h"
#include "app/MainMenu.h"
#include "app/PreferencesDialog.h"
#include "app/RecentItems.h"
#include "app/StartScreenRecent.h"
#include "app/SystemSymbols.h"
#include "app/ViewerWindow.h"
#include "app/WindowPanels.h"
#include "app/WindowTabbing.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/ImageProvider.h"
#include "core/InputBindings.h"
#include "core/Platform.h"
#include "core/Settings.h"
#include "core/ThumbnailCache.h"
#include "support/NativeEvents.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMutex>
#include <QPushButton>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include <QtTest>

#include <atomic>
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

// Finder tags in memory. A read can be held back (blockNextRead): it returns the tags as
// they were when the read started, i.e. stale ones if a write happens meanwhile.
class SlowTags : public TagService {
public:
    bool isSupported() const override { return true; }
    std::optional<QStringList> tags(const QString& path, QString*) override {
        QStringList snapshot;
        {
            QMutexLocker lock(&mutex);
            snapshot = stored.value(path);
        }
        if (blockNextRead.exchange(false)) {
            readBlocked.release();
            release.acquire();
        }
        return snapshot;
    }
    bool setTags(const QString& path, const QStringList& t, QString*) override {
        QMutexLocker lock(&mutex);
        stored.insert(path, t);
        return true;
    }
    QMutex mutex;
    QHash<QString, QStringList> stored;
    std::atomic<bool> blockNextRead{false};
    QSemaphore readBlocked;
    QSemaphore release;
};

QString corpus(const QString& name) {
    return QStringLiteral(LF_CORPUS_DIR "/generated/") + name;
}

// Every pixel differs: red follows x, green follows y (F-VIEW-03 anchor check).
QString makeGradient(const QString& path, QSize size) {
    QImage img(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < size.width(); ++x)
            line[x] = qRgb(x * 255 / (size.width() - 1), y * 255 / (size.height() - 1), 128);
    }
    return img.save(path) ? QDir::cleanPath(path) : QString();
}

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
    static ImageCanvas* canvas(ViewerWindow* w) { return w->view()->findChild<ImageCanvas*>(); }
    static void key(ViewerWindow* w, Qt::Key k, Qt::KeyboardModifiers m = Qt::NoModifier) {
        QTest::keyClick(w->view(), k, m); // ImageView -> ViewerWindow::handleInput
    }
    static void setChoice(const char* slot, const char* choice) {
        Settings::instance().setValue(QStringLiteral("input/") + QString::fromLatin1(slot),
                                      QString::fromLatin1(choice));
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
        ThumbnailCache::setDefaultDirectory(m_settingsDir.filePath(QStringLiteral("thumbs")));
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
        if (auto* dlg = PreferencesDialog::existingInstance())
            dlg->close();
        for (QWidget* top : QApplication::topLevelWidgets())
            if (qobject_cast<QMessageBox*>(top) || qobject_cast<DefaultAppDialog*>(top))
                top->close();
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
            {Qt::Key_F, cmd, Action::FilterByName}, // F-NAV-08 name filter (M3; was: slideshow)
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
                (void)QTest::qWaitFor([&] { return !performed.isEmpty() || !commands.isEmpty(); }, 2000);
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

    // F-OPEN-12: the start screen lists up to 8 recent items, files and folders mixed in
    // the order of last use, clickable; a missing one gives a note and leaves the list.
    void F_OPEN_12_startScreenShowsRecent() {
        QDir(dir->path()).mkpath(QStringLiteral("Ordner"));
        QVERIFY(!makeImage(path("Ordner/x.png"), Qt::blue).isEmpty());
        QStringList expected; // newest first
        for (int i = 0; i < 10; ++i) {
            if (i % 3 == 1) {
                const QString d = path(QStringLiteral("Ordner%1").arg(i));
                QDir().mkpath(d);
                RecentItems::add({}, {d});
                expected.prepend(d);
            } else {
                const QString f = makeImage(path(QStringLiteral("bild%1.png").arg(i)), Qt::red);
                QVERIFY(!f.isEmpty());
                RecentItems::add({f});
                expected.prepend(f);
            }
        }
        const QString gone = path(QStringLiteral("weg.png"));
        RecentItems::add({gone}); // never existed: newest entry
        expected.prepend(gone);
        ViewerWindow* w = app->newWindow();
        QVERIFY(w);
        QVERIFY(QTest::qWaitForWindowExposed(w));
        auto* recent = w->findChild<StartScreenRecent*>(QStringLiteral("lfStartRecent"));
        QVERIFY(recent);
        QTRY_VERIFY(recent->isVisible());
        QCOMPARE(recent->shownPaths(), expected.mid(0, 8));
        const auto buttons = recent->findChildren<QToolButton*>(QStringLiteral("lfStartRecentItem"));
        QCOMPARE(buttons.size(), 8);
        QCOMPARE(buttons.first()->text(), QStringLiteral("weg.png"));
        QCOMPARE(buttons.first()->toolTip(), gone);
        // Missing: a note, no error card, the entry is removed, the window stays empty.
        buttons.first()->click();
        QTRY_VERIFY(w->toastText().contains(QStringLiteral("nicht mehr vorhanden")));
        QVERIFY(w->isEmpty());
        QTRY_COMPARE(recent->shownPaths(), expected.mid(1, 8));
        // A folder entry opens the folder in this (start) window.
        int folderIndex = -1;
        for (int i = 0; i < recent->shownPaths().size(); ++i)
            if (!recent->shownPaths().at(i).endsWith(QStringLiteral(".png")))
                folderIndex = folderIndex < 0 ? i : folderIndex;
        QVERIFY(folderIndex >= 0);
        const QString folder = recent->shownPaths().at(folderIndex);
        QVERIFY(!makeImage(folder + QStringLiteral("/in.png"), Qt::yellow).isEmpty());
        recent->findChildren<QToolButton*>(QStringLiteral("lfStartRecentItem")).at(folderIndex)->click();
        QTRY_COMPARE(w->collection()->folderPath(), folder);
        QTRY_VERIFY(!w->isEmpty());
        QCOMPARE(app->windows().size(), 1);
    }

    // F-MAC-04: the types come as a checklist (name + extensions, all preselected); only the
    // selected ones are registered, the choice is remembered.
    void F_MAC_04_defaultAppChecklistPassesOnlySelectedTypes() {
        app->makeDefaultViewer();
        DefaultAppDialog* dlg = nullptr;
        auto find = [] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (auto* d = qobject_cast<DefaultAppDialog*>(w); d && d->isVisible())
                    return d;
            return static_cast<DefaultAppDialog*>(nullptr);
        };
        QTRY_VERIFY((dlg = find()) != nullptr);
        QListWidget* list = dlg->list();
        QCOMPARE(list->count(), 3);
        for (int i = 0; i < list->count(); ++i)
            QCOMPARE(list->item(i)->checkState(), Qt::Checked); // all preselected
        QVERIFY2(list->item(0)->text().startsWith(QStringLiteral("JPEG-Bild (")),
                 qPrintable(list->item(0)->text()));
        QVERIFY(list->item(0)->text().contains(QStringLiteral("jpg")));
        QVERIFY(list->item(1)->text().startsWith(QStringLiteral("PNG-Bild")));
        QCOMPARE(list->item(1)->data(Qt::UserRole).toString(), QStringLiteral("public.png"));
        list->item(1)->setCheckState(Qt::Unchecked);
        QSignalSpy finished(app.get(), &Application::defaultViewerFinished);
        dlg->confirmButton()->click();
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(shell->madeDefault.size(), 1);
        QCOMPARE(shell->madeDefault.first(), (QStringList{"public.jpeg", "public.heic"})); // PNG untouched
        QCOMPARE(
            Settings::instance().value(QString::fromLatin1(DefaultAppDialog::kExcludedKey)).toStringList(),
            QStringList{"public.png"});
        // Remembered: PNG stays deselected next time; nothing selected = no button.
        DefaultAppDialog again(shell->types);
        QCOMPARE(again.list()->item(1)->checkState(), Qt::Unchecked);
        QCOMPARE(again.selectedTypes(), (QStringList{"public.jpeg", "public.heic"}));
        for (int i = 0; i < again.list()->count(); ++i)
            again.list()->item(i)->setCheckState(Qt::Unchecked);
        QVERIFY(!again.confirmButton()->isEnabled());
        // Without the dialog (e.g. scripted): the remembered selection.
        app->setDefaultViewer();
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(shell->madeDefault.value(1), (QStringList{"public.jpeg", "public.heic"}));
    }

    // F-INFO-02 (SPEC 5.1): in a small window the metadata keep priority: no clipped rows or
    // labels, tags and histogram shrink or collapse; values elide at the end (tooltip).
    void F_INFO_02_infoPanelFitsSmallWindow() {
        QVERIFY(!makeImage(path("a.png"), Qt::red, QSize(640, 480)).isEmpty());
        ViewerWindow* w = openAndWait(path("a.png"));
        QVERIFY(w);
        w->showNormal();
        w->resize(756, 439);
        w->panels()->setInfoPanelEnabled(true);
        QTRY_VERIFY(w->panels()->infoPanelShown());
        InfoPanel* info = w->panels()->infoPanel();
        QTRY_VERIFY(!info->isLoading());
        QTest::qWait(50);
        QTreeWidget* tree = info->tree();
        QCOMPARE(tree->textElideMode(), Qt::ElideRight); // no "12.0…:13" dates
        const int row = tree->fontMetrics().height();
        QVERIFY2(tree->height() >= row * 6, qPrintable(QString::number(tree->height())));
        // Nothing in the panel is squeezed below what its text needs or cut off at the bottom.
        for (QWidget* child : info->findChildren<QWidget*>()) {
            if (!child->isVisible() || (!qobject_cast<QLabel*>(child) && !qobject_cast<QLineEdit*>(child) &&
                                        !qobject_cast<QToolButton*>(child)))
                continue;
            QVERIFY2(
                child->height() >= child->minimumSizeHint().height(),
                qPrintable(
                    child->objectName() +
                    QStringLiteral(" %1 < %2").arg(child->height()).arg(child->minimumSizeHint().height())));
            const QPoint bottom = child->mapTo(info, QPoint(0, child->height()));
            QVERIFY2(bottom.y() <= info->height(), qPrintable(child->objectName()));
        }
        HistogramWidget* hist = info->histogramWidget();
        QVERIFY(!hist->isVisible() || hist->height() >= hist->minimumSizeHint().height());
        QVERIFY(hist->mapTo(info, QPoint(0, hist->height())).y() <= info->height());
        // The histogram section collapses (remembered) and gives its room to the metadata.
        auto* header = info->findChild<QToolButton*>(QStringLiteral("lfHistogramSection"));
        QVERIFY(header);
        const int before = tree->height();
        header->click();
        QTRY_VERIFY(!hist->isVisible());
        QTRY_VERIFY(tree->height() > before);
        QVERIFY(Settings::instance().value(QStringLiteral("info/histogramCollapsed")).toBool());
        header->click();
        QTRY_VERIFY(hist->isVisible());
        // Long values: the full text is the tooltip.
        const QTreeWidgetItem* nameRow = tree->topLevelItem(0)->child(0);
        QCOMPARE(nameRow->toolTip(1), nameRow->text(1));
    }

    // German texts only: sizes with German units, type names in German, translatable row
    // labels (no "680 bytes (680 Byte)" or "JPEG image").
    void F_INFO_02_germanSizesAndKinds() {
        QCOMPARE(formatByteSize(680), QStringLiteral("680 Byte"));
        QCOMPARE(formatByteSize(301), QStringLiteral("301 Byte"));
        QCOMPARE(formatByteSize(2812345), QStringLiteral("2,8 MB"));
        QCOMPARE(formatByteSize(1500), QStringLiteral("1,5 KB"));
        QCOMPARE(formatByteSizeExact(680), QStringLiteral("680 Byte"));
        QCOMPARE(formatByteSizeExact(2812345), QStringLiteral("2,8 MB (2.812.345 Byte)"));
        QCOMPARE(displayKind(QStringLiteral("a.JPG"), QStringLiteral("JPEG image")),
                 QStringLiteral("JPEG-Bild"));
        QCOMPARE(displayKind(QStringLiteral("a.cr3"), QStringLiteral("Canon CR3 raw image")),
                 QStringLiteral("Kamera-RAW (Canon CR3)"));
        QCOMPARE(displayKind(QStringLiteral("a.heic"), {}), QStringLiteral("HEIC-Bild"));
        QCOMPARE(displayKind(QStringLiteral("a.xyz"), QStringLiteral("Systemtext")),
                 QStringLiteral("Systemtext"));
        QVERIFY(!makeImage(path("klein.png"), Qt::red, QSize(4, 4)).isEmpty());
        ImageProvider provider;
        InfoPanel panel(provider, nullptr);
        panel.resize(300, 500);
        panel.show();
        panel.setFile(path("klein.png"));
        QTRY_VERIFY(!panel.isLoading());
        const QString size = panel.value(QStringLiteral("Datei"), QStringLiteral("Größe"));
        QVERIFY2(size.endsWith(QStringLiteral(" Byte")) && !size.contains(QStringLiteral("bytes")) &&
                     !size.contains(QLatin1Char('(')),
                 qPrintable(size));
        QCOMPARE(panel.value(QStringLiteral("Datei"), QStringLiteral("Art")), QStringLiteral("PNG-Bild"));
        QCOMPARE(panel.value(QStringLiteral("Bild"), QStringLiteral("Pixelmaße")), QStringLiteral("4 × 4"));
        // Status bar of the window: German units too.
        ViewerWindow* w = openAndWait(path("klein.png"));
        QVERIFY(w);
        QTRY_VERIFY2(w->statusText().contains(formatByteSize(QFileInfo(path("klein.png")).size())),
                     qPrintable(w->statusText()));
        QVERIFY(!w->statusText().contains(QStringLiteral("bytes")));
    }

    // SVG: the status bar shows the size of the drawing (like the info panel), not the size
    // it is rendered at.
    void F_FMT_SVG_statusShowsIntrinsicSize() {
        QFile f(path("v.svg"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100'>"
                "<rect width='200' height='100' fill='teal'/></svg>");
        f.close();
        ViewerWindow* w = openAndWait(path("v.svg"));
        QVERIFY(w);
        QVERIFY(w->view()->image()->fullSize.width() > 200); // rendered larger
        QTRY_VERIFY2(w->statusText().contains(QStringLiteral("200 × 100 (Vektor)")),
                     qPrintable(w->statusText()));
        QVERIFY(!w->statusText().contains(QStringLiteral("2048")));
    }

    // F-FMT-02: unreadable files - "Kein Histogramm" instead of an empty box, an error mark
    // in the filmstrip.
    void F_FMT_02_brokenFileHistogramAndFilmstripMark() {
        QVERIFY(QFile::copy(corpus(QStringLiteral("garbage.jpg")), path("b_kaputt.jpg")));
        QVERIFY(!makeImage(path("a.png"), Qt::red).isEmpty());
        ViewerWindow* w = openAndWait(path("a.png"));
        QVERIFY(w);
        w->panels()->setInfoPanelEnabled(true);
        w->panels()->setFilmstripEnabled(true);
        QTRY_VERIFY(w->panels()->infoPanelShown() && w->panels()->filmstripShown());
        w->collection()->setCurrentPath(path("b_kaputt.jpg"));
        QTRY_COMPARE(w->shownPath(), path("b_kaputt.jpg"));
        QTRY_COMPARE(w->panels()->infoPanel()->histogramWidget()->note(), QStringLiteral("Kein Histogramm"));
        FilmstripModel* model = w->panels()->filmstrip()->filmModel();
        const int row = w->collection()->indexOf(path("b_kaputt.jpg"));
        QVERIFY(row >= 0);
        (void)model->data(model->index(row), FilmstripModel::ThumbnailRole); // as the delegate does
        QTRY_VERIFY(model->data(model->index(row), FilmstripModel::FailedRole).toBool());
        QVERIFY(model->data(model->index(row), Qt::ToolTipRole)
                    .toString()
                    .contains(QStringLiteral("kann nicht")));
        const int ok = w->collection()->indexOf(path("a.png"));
        QVERIFY(!model->data(model->index(ok), FilmstripModel::FailedRole).toBool());
    }

    // Code review #13: a metadata read that started before a tag write must not bring the
    // old tags back (cache and display keep what the write stored).
    void F_INFO_06_staleTagReadDoesNotOverwriteWrite() {
        QVERIFY(!makeImage(path("t.png"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path("u.png"), Qt::blue).isEmpty());
        auto tags = std::make_shared<SlowTags>();
        tags->stored.insert(path("t.png"), {QStringLiteral("Rot")});
        ImageProvider provider;
        InfoPanel panel(provider, tags);
        panel.resize(300, 500);
        panel.show();
        panel.setFile(path("t.png"));
        QTRY_VERIFY(!panel.isLoading());
        QCOMPARE(panel.tags(), QStringList{"Rot"});
        // A refresh read starts and is held back with the old tags ...
        tags->blockNextRead = true;
        provider.invalidate(path("t.png"));
        QVERIFY(tags->readBlocked.tryAcquire(1, 5000));
        // ... while the user adds a tag (written and read back on the write thread).
        QSignalSpy written(&panel, &InfoPanel::tagsWritten);
        panel.addTag(QStringLiteral("Blau"));
        QTRY_COMPARE(written.size(), 1);
        QCOMPARE(panel.tags(), (QStringList{"Rot", "Blau"}));
        // The stale read finishes now.
        QSignalSpy loaded(&panel, &InfoPanel::loaded);
        tags->release.release();
        QTRY_COMPARE(loaded.size(), 1);
        QCOMPARE(panel.tags(), (QStringList{"Rot", "Blau"}));
        // Also the cache: flipping away and back shows the written tags at once.
        panel.setFile(path("u.png"));
        QTRY_VERIFY(!panel.isLoading());
        panel.setFile(path("t.png"));
        QCOMPARE(panel.tags(), (QStringList{"Rot", "Blau"}));
    }

    // F-SET-01 / F-MAC-02: section icons are SF Symbols (template images in the text color).
    void F_SET_01_preferencesUseSystemSymbols() {
        if (systemSymbolMask(QStringLiteral("gearshape"), 32).isNull())
            QSKIP("SF Symbols not available");
        PreferencesDialog dlg;
        auto* toolbar = dlg.findChild<QToolBar*>(QStringLiteral("sectionToolbar"));
        QVERIFY(toolbar);
        const QStringList expected = {
            "gearshape",    "macwindow",          "photo", "computermouse", "arrow.left.arrow.right", "star",
            "doc.richtext", "slider.horizontal.3"};
        QStringList got;
        for (QAction* a : toolbar->actions()) {
            QVERIFY(!a->icon().isNull());
            got << a->property("symbol").toString();
        }
        QCOMPARE(got.size(), expected.size());
        QCOMPARE(got, expected);
        // Template: drawn in the palette's text color.
        const QImage img = toolbar->actions().first()->icon().pixmap(QSize(24, 24), 1.0).toImage();
        const QColor text = dlg.palette().color(QPalette::WindowText);
        QColor strongest(0, 0, 0, 0);
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                if (img.pixelColor(x, y).alpha() > strongest.alpha())
                    strongest = img.pixelColor(x, y);
        QVERIFY(strongest.alpha() > 128); // the shape is there
        QVERIFY2(std::abs(strongest.red() - text.red()) < 8 &&
                     std::abs(strongest.green() - text.green()) < 8 &&
                     std::abs(strongest.blue() - text.blue()) < 8,
                 qPrintable(strongest.name(QColor::HexArgb) + QStringLiteral(" vs ") + text.name()));
    }

    // F-WIN-05: switching tabs off separates windows that were merged into tabs.
    void F_WIN_05_disablingTabsSeparatesWindows() {
        Settings::instance().setValue(key::AllowTabs, true);
        ViewerWindow* a = app->newWindow();
        QVERIFY(QTest::qWaitForWindowExposed(a));
        ViewerWindow* b = app->newWindow();
        QVERIFY(QTest::qWaitForWindowExposed(b));
        if (!performTabCommand(b, TabCommand::MergeAllWindows))
            QSKIP("no native tabs on this platform");
        QTRY_COMPARE(tabGroupSize(a), 2);
        Settings::instance().setValue(key::AllowTabs, false);
        QTRY_COMPARE(tabGroupSize(a), 1);
        QTRY_COMPARE(tabGroupSize(b), 1);
        QCOMPARE(nativeTabbingMode(a), NativeTabbingMode::Disallowed);
        QVERIFY(a->isVisible() && b->isVisible());
    }

    // ---- executed input actions (spec review #12) ----

    // F-IN-04: Bild↑/Bild↓ as folder / page / scroll, executed by the window.
    void F_IN_04_pageKeysFolderPageScroll() {
        QDir(dir->path()).mkpath(QStringLiteral("A"));
        QDir(dir->path()).mkpath(QStringLiteral("B"));
        QVERIFY(!makeImage(path("A/a1.png"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path("A/a2.png"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path("B/b1.png"), Qt::green).isEmpty());
        QVERIFY(!makeImage(path("B/b2.png"), Qt::green).isEmpty());
        ViewerWindow* w = openAndWait(path("A/a1.png"));
        QVERIFY(w);
        setChoice("pageUpDown", "folder");
        key(w, Qt::Key_PageDown);
        QTRY_COMPARE(w->collection()->folderPath(), path("B"));
        QTRY_COMPARE(w->currentPath(), path("B/b1.png"));
        key(w, Qt::Key_PageUp); // backwards: the last image of the previous folder
        QTRY_COMPARE(w->currentPath(), path("A/a2.png"));
        QVERIFY(performed.contains(Action::NextFolder) && performed.contains(Action::PreviousFolder));
        QTRY_VERIFY(waitShown(w, path("A/a2.png")));

        // "page": pages of a multi-page file, otherwise images.
        setChoice("pageUpDown", "page");
        QVERIFY(QFile::copy(corpus(QStringLiteral("multipage.tiff")), path("A/m.tiff")));
        w->openFile(path("A/m.tiff"));
        QVERIFY(waitShown(w, path("A/m.tiff")));
        QTRY_VERIFY(w->statusText().contains(QStringLiteral("Seite 1/3")));
        key(w, Qt::Key_PageDown);
        QTRY_VERIFY(w->statusText().contains(QStringLiteral("Seite 2/3")));
        QCOMPARE(w->currentPath(), path("A/m.tiff"));
        key(w, Qt::Key_PageUp);
        QTRY_VERIFY(w->statusText().contains(QStringLiteral("Seite 1/3")));
        QVERIFY(waitLoaded(w));
        QTRY_VERIFY(w->collection()->setCurrentPath(path("A/a1.png")));
        QVERIFY(waitShown(w, path("A/a1.png")));
        key(w, Qt::Key_PageDown); // single page: next image
        QTRY_COMPARE(w->currentPath(), path("A/a2.png"));

        // "scroll": one screen height when zoomed, else the next image.
        setChoice("pageUpDown", "scroll");
        QVERIFY(!makeImage(path("A/zz_big.png"), Qt::gray, QSize(1200, 4000)).isEmpty());
        w->openFile(path("A/zz_big.png"));
        QVERIFY(waitShown(w, path("A/zz_big.png")));
        w->setZoomMode(ZoomMode::Actual);
        QTRY_VERIFY(w->view()->isZoomed());
        w->view()->scrollToTop();
        const QPointF top = w->view()->viewCenter();
        performed.clear();
        key(w, Qt::Key_PageDown);
        QTRY_VERIFY(w->view()->viewCenter().y() > top.y());
        QCOMPARE(w->currentPath(), path("A/zz_big.png"));
        QCOMPARE(performed, QList<Action>{Action::ScrollPageDown});
    }

    // F-IN-07 "scrollThenImage": → scrolls to the edge first, then flips.
    void F_IN_07_arrowsScrollThenImage() {
        QVERIFY(!makeImage(path("a.png"), Qt::gray, QSize(3000, 600)).isEmpty());
        QVERIFY(!makeImage(path("b.png"), Qt::red).isEmpty());
        ViewerWindow* w = openAndWait(path("a.png"));
        QVERIFY(w);
        setChoice("arrowsZoomed", "scrollThenImage");
        w->setZoomMode(ZoomMode::Actual);
        QTRY_VERIFY(w->view()->isZoomed());
        performed.clear();
        double lastX = w->view()->viewCenter().x();
        int scrolls = 0;
        for (int i = 0; i < 300 && w->currentPath() == path("a.png"); ++i) {
            key(w, Qt::Key_Right);
            if (w->currentPath() != path("a.png"))
                break;
            QVERIFY(w->view()->viewCenter().x() >= lastX);
            lastX = w->view()->viewCenter().x();
            ++scrolls;
        }
        QVERIFY2(scrolls >= 2, qPrintable(QString::number(scrolls))); // scrolled first ...
        QTRY_COMPARE(w->currentPath(), path("b.png"));                // ... then flipped
        QVERIFY(performed.contains(Action::ScrollRight));
        QCOMPARE(performed.last(), Action::NextImage);
        QCOMPARE(performed.count(Action::NextImage), 1);
    }

    // F-IN-09 "image": a click in the left half goes back, in the right half forward.
    void F_IN_09_clickHalvesFlip() {
        for (const char* n : {"a.png", "b.png", "c.png"})
            QVERIFY(!makeImage(path(QString::fromLatin1(n)), Qt::red).isEmpty());
        ViewerWindow* w = openAndWait(path("b.png"));
        QVERIFY(w);
        setChoice("click", "image");
        ImageCanvas* c = canvas(w);
        QVERIFY(c);
        QTest::mouseClick(c, Qt::LeftButton, {}, QPoint(c->width() * 3 / 4, c->height() / 2));
        QTRY_COMPARE(w->currentPath(), path("c.png"));
        QTest::qWait(QApplication::doubleClickInterval() + 50); // not a double click
        QTest::mouseClick(c, Qt::LeftButton, {}, QPoint(c->width() / 4, c->height() / 2));
        QTRY_COMPARE(w->currentPath(), path("b.png"));
        // Default "nichts": a click does not flip.
        QTest::qWait(QApplication::doubleClickInterval() + 50);
        setChoice("click", "none");
        QTest::mouseClick(c, Qt::LeftButton, {}, QPoint(c->width() * 3 / 4, c->height() / 2));
        QTest::qWait(QApplication::doubleClickInterval() + 100);
        QCOMPARE(w->currentPath(), path("b.png"));
    }

    // F-IN-12: mouse buttons 4/5 flip back/forward; "nichts" ignores them.
    void F_IN_12_backForwardButtons() {
        for (const char* n : {"a.png", "b.png", "c.png"})
            QVERIFY(!makeImage(path(QString::fromLatin1(n)), Qt::red).isEmpty());
        ViewerWindow* w = openAndWait(path("b.png"));
        QVERIFY(w);
        ImageCanvas* c = canvas(w);
        QTest::mouseClick(c, Qt::ForwardButton, {}, c->rect().center());
        QTRY_COMPARE(w->currentPath(), path("c.png"));
        QTest::mouseClick(c, Qt::BackButton, {}, c->rect().center());
        QTRY_COMPARE(w->currentPath(), path("b.png"));
        setChoice("backForward", "none");
        QTest::mouseClick(c, Qt::BackButton, {}, c->rect().center());
        QTest::qWait(100);
        QCOMPARE(w->currentPath(), path("b.png"));
    }

    // F-IN-15: Esc does nothing outside full screen by default; with "…, sonst Fenster
    // schließen" it closes the window.
    void F_IN_15_escClosesWindowWhenChosen() {
        QVERIFY(!makeImage(path("a.png"), Qt::red).isEmpty());
        ViewerWindow* w = openAndWait(path("a.png"));
        QVERIFY(w);
        key(w, Qt::Key_Escape);
        QTest::qWait(50);
        QVERIFY(w->isVisible());
        QCOMPARE(app->windows().size(), 1);
        setChoice("esc", "exitModeOrClose");
        key(w, Qt::Key_Escape);
        QTRY_COMPARE(app->windows().size(), 0);
        QCOMPARE(performed.last(), Action::ExitModeOrCloseWindow);
    }

    // F-VIEW-03: Z over a point switches fit <-> 100 % and keeps that point of the image
    // under the cursor.
    void F_VIEW_03_zKeyAnchorsAtCursor() {
        QVERIFY(!makeGradient(path("g.png"), QSize(2400, 1800)).isEmpty());
        ViewerWindow* w = openAndWait(path("g.png"));
        QVERIFY(w);
        w->showNormal();
        w->resize(800, 600);
        QTRY_COMPARE(w->view()->zoomMode(), ZoomMode::Fit);
        ImageCanvas* c = canvas(w);
        const QPointF anchor(w->view()->width() * 0.3, w->view()->height() * 0.4);
        auto sample = [&] {
            const QImage fb = c->grabFramebuffer();
            const double dpr = fb.width() / double(c->width());
            const QPoint inCanvas = c->mapFrom(w->view(), anchor.toPoint());
            return fb.pixelColor((QPointF(inCanvas) * dpr).toPoint());
        };
        QTest::qWait(150);
        const QColor before = sample();
        // Z as the window gets it from the view, with the cursor position (ImageView takes
        // QCursor::pos(), which a test must not move).
        InputEvent ev;
        ev.kind = InputKind::Key;
        ev.key = Qt::Key_Z;
        ev.zoomed = w->view()->isZoomed();
        performed.clear();
        emit w->view()->userInput(ev, anchor);
        QCOMPARE(performed, QList<Action>{Action::ToggleFitActualAtCursor});
        QTRY_VERIFY(qAbs(w->view()->zoomFactor() - 1.0) < 0.01);
        QTest::qWait(150);
        const QColor after = sample();
        QVERIFY2(std::abs(before.red() - after.red()) <= 6 && std::abs(before.green() - after.green()) <= 6,
                 qPrintable(QStringLiteral("%1 -> %2").arg(before.name(), after.name())));
        // Control: the view center shows another part of the image (the check is meaningful).
        const QImage fb = c->grabFramebuffer();
        const QColor center = fb.pixelColor(fb.rect().center());
        QVERIFY(std::abs(center.red() - after.red()) > 10 || std::abs(center.green() - after.green()) > 10);
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
