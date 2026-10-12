// SPDX-License-Identifier: GPL-3.0-or-later
// M2 shell features: rename (F-FILE-04), open with / editor (F-FILE-05), share (F-FILE-06),
// drag-out (F-FILE-07), recent items (F-OPEN-11), arrange windows (F-WIN-04), macOS tabs
// (F-WIN-05), trackpad gestures (F-NAV-04), output profile per screen (F-COL-02) and the
// live Finder query opt-in (F-SORT-04). Real windows like tst_app (cocoa on macOS).
//
// Never touches the user's system state: the shell is a fake (no app is launched), the
// sharing picker is not shown, drags do not run a real drag session, the system recent
// documents list is not written and the Finder is never asked (fake order provider).

#include "app/Application.h"
#include "app/DragOut.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/MainMenu.h"
#include "app/MenuExtras.h"
#include "app/OpenWith.h"
#include "app/PreferencesDialog.h"
#include "app/PreferencesExtras.h"
#include "app/RecentItems.h"
#include "app/RenameController.h"
#include "app/ScreenProfileWatcher.h"
#include "app/Share.h"
#include "app/ViewerWindow.h"
#include "app/WindowArranger.h"
#include "app/WindowTabbing.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/ImageProvider.h"
#include "core/InputBindings.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QDrag>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPushButton>
#include <QScreen>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QUndoStack>
#include <QWheelEvent>
#include <QWindow>
#include <QtConcurrent/QtConcurrentRun>
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
    void revealInFileManager(const QString&) override {}
    void openWith(const QString& path, const QString& appPath) override {
        opened << qMakePair(path, appPath);
    }
    QStringList imageDocumentTypes() const override { return {"public.jpeg"}; }
    bool makeDefaultViewer(const QStringList&, QString*) override { return true; }
    QList<QPair<QString, QString>> opened;
};

// F-SORT-04: never asks the Finder; tells which tier the window asked for.
class FakeOrderProvider : public FolderOrderProvider {
public:
    SortSpec finderOrder(const QString&, bool allowLiveQuery) override {
        calls.fetch_add(1);
        if (allowLiveQuery) {
            liveCalls.fetch_add(1);
            return {SortKey::Name, false, SortSource::FinderLive};
        }
        return {SortKey::Name, true, SortSource::FinderDsStore};
    }
    QHash<QString, QPointF> iconPositions(const QString&) override { return {}; }
    std::atomic<int> calls{0};
    std::atomic<int> liveCalls{0};
};

QString makeImage(const QString& path, QColor color, QSize size = QSize(64, 48)) {
    QImage img(size, QImage::Format_RGB32);
    img.fill(color);
    return img.save(path) ? QDir::cleanPath(path) : QString();
}

QStringList names(const Collection* c) {
    QStringList out;
    for (const auto& e : c->entries())
        out << e.name;
    return out;
}

AppEntry appEntry(const QString& name, const QString& id, bool isDefault = false) {
    AppEntry a;
    a.name = name;
    a.path = QStringLiteral("/Applications/%1.app").arg(name);
    a.bundleId = id;
    a.isDefault = isDefault;
    return a;
}

} // namespace

class TstShellM2 : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<NullTrash> trash;
    std::unique_ptr<FileOps> ops;
    std::unique_ptr<FakeShell> shell;
    std::unique_ptr<FakeOrderProvider> order;
    std::unique_ptr<Application> app;

    QString path(const QString& name) const { return QDir::cleanPath(dir->path()) + QLatin1Char('/') + name; }

    void makeFolder() {
        QVERIFY(!makeImage(path("IMG_10.jpg"), Qt::blue).isEmpty());
        QVERIFY(!makeImage(path("IMG_2.jpg"), Qt::green).isEmpty());
        QVERIFY(!makeImage(path("IMG_1.png"), Qt::red).isEmpty());
    }
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
    QAction* command(const char* id) const { return app->menu()->command(QString::fromLatin1(id)); }
    static ImageCanvas* canvas(ViewerWindow* w) { return w->view()->findChild<ImageCanvas*>(); }
    // Trackpad scroll event; `natural`: macOS "natural scrolling" (default), the deltas
    // follow the fingers.
    static void sendWheel(QWidget* target, QPoint pixelDelta, Qt::ScrollPhase phase, bool natural = true) {
        const QPointF pos(target->width() / 2.0, target->height() / 2.0);
        QWheelEvent ev(pos, target->mapToGlobal(pos), pixelDelta, pixelDelta * 2, Qt::NoButton,
                       Qt::NoModifier, phase, natural);
        QCoreApplication::sendEvent(target, &ev);
    }
    static void sendMouse(QWidget* target, QEvent::Type type, QPointF pos, Qt::KeyboardModifiers mods) {
        const Qt::MouseButtons buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
        const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent ev(type, pos, target->mapToGlobal(pos), button, buttons, mods);
        QCoreApplication::sendEvent(target, &ev);
    }
    RenameController* startRename(ViewerWindow* w) {
        QTest::keyClick(w->view(), Qt::Key_Return); // F-FILE-04: Enter
        auto* r = w->findChild<RenameController*>(QStringLiteral("lfRenameController"));
        return r && r->isEditing() ? r : nullptr;
    }

private slots:
    void initTestCase() {
        qRegisterMetaType<lf::Quality>("lf::Quality");
        qRegisterMetaType<lf::DecodedImagePtr>("lf::DecodedImagePtr");
        RecentItems::setSystemRegistrationEnabled(false); // never the user's recent documents
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
        order = std::make_unique<FakeOrderProvider>();
        AppServices services;
        services.fileOps = ops.get();
        services.shell = shell.get();
        services.openUrl = [](const QUrl&) { return true; };
        app = std::make_unique<Application>(services);
        OpenWithService::instance().setQueryForTesting({});
        setDragExecHookForTesting({});
        ImageView::setColorSpaceResolverForTesting({});
    }

    void cleanup() {
        if (auto* dlg = PreferencesDialog::existingInstance())
            dlg->close();
        app.reset();
        QTest::qWait(10);
        QThreadPool::globalInstance()->waitForDone(5000); // jobs may still use the fake provider
        OpenWithService::instance().setQueryForTesting({});
        setDragExecHookForTesting({});
        ImageView::setColorSpaceResolverForTesting({});
        ops.reset();
        trash.reset();
        shell.reset();
        order.reset();
        dir.reset();
    }

    // ---- F-FILE-04 rename ----

    void F_FILE_04_inlineEditorProtectsExtensionAndRenames() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("IMG_2.jpg"));
        QVERIFY(w);
        QVERIFY(InputBindings::isActionAvailable(Action::Rename));
        QVERIFY(app->menu()->action(Action::Rename));
        QVERIFY(app->menu()->action(Action::Rename)->shortcuts().contains(QKeySequence(Qt::Key_F2)));
        RenameController* r = startRename(w);
        QVERIFY(r);
        QLineEdit* edit = r->editor();
        QVERIFY(edit && edit->isVisible());
        QCOMPARE(edit->text(), QStringLiteral("IMG_2.jpg"));
        QCOMPARE(edit->selectedText(), QStringLiteral("IMG_2")); // the extension is protected
        QVERIFY(edit->hasFocus() || !w->isActiveWindow());
        QTest::keyClicks(edit, QStringLiteral("Zebra")); // replaces the selection only
        QCOMPARE(edit->text(), QStringLiteral("Zebra.jpg"));
        QTest::keyClick(edit, Qt::Key_Return);
        QTRY_VERIFY(QFileInfo::exists(path("Zebra.jpg")));
        QVERIFY(!QFileInfo::exists(path("IMG_2.jpg")));
        QTRY_VERIFY(!r->isEditing());
        QCOMPARE(w->currentPath(), path("Zebra.jpg"));
        // Sort position follows the new name (Finder name order).
        QCOMPARE(names(w->collection()).last(), QStringLiteral("Zebra.jpg"));
        QTRY_VERIFY(w->windowTitle().startsWith(QStringLiteral("Zebra.jpg")));
        // Undoable (⌘Z).
        QVERIFY(app->fileOps().undoStack()->canUndo());
    }

    void F_FILE_04_extensionChangeAsks() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("IMG_2.jpg"));
        QVERIFY(w);
        RenameController* r = startRename(w);
        QVERIFY(r);
        r->editor()->setText(QStringLiteral("Foo.png"));
        r->commit();
        QMessageBox* box = r->extensionQuestion();
        QVERIFY(box);
        QVERIFY(box->text().contains(QStringLiteral(".jpg")) && box->text().contains(QStringLiteral(".png")));
        box->findChild<QPushButton*>(QStringLiteral("lfKeepExtension"))->click();
        QTRY_VERIFY(QFileInfo::exists(path("Foo.jpg"))); // name changed, extension kept
        QVERIFY(!QFileInfo::exists(path("Foo.png")));
        QTRY_VERIFY(!r->isEditing());

        QVERIFY(startRename(w));
        r->editor()->setText(QStringLiteral("Bar.jpeg"));
        r->commit();
        QVERIFY(r->extensionQuestion());
        r->extensionQuestion()->findChild<QPushButton*>(QStringLiteral("lfUseExtension"))->click();
        QTRY_VERIFY(QFileInfo::exists(path("Bar.jpeg")));

        // Case-only change of the extension is not a change.
        QVERIFY(startRename(w));
        r->editor()->setText(QStringLiteral("Bar.JPEG"));
        r->commit();
        QVERIFY(!r->extensionQuestion());
        QTRY_VERIFY(!r->isEditing());
    }

    void F_FILE_04_invalidAndTakenNames() {
        makeFolder();
        QVERIFY(QDir().mkpath(path("Ordner.jpg"))); // not an image: only FileOps knows it
        ViewerWindow* w = openAndWait(path("IMG_2.jpg"));
        QVERIFY(w);
        QCOMPARE(RenameController::validationError(QStringLiteral("a/b.jpg"))
                     .contains(QStringLiteral("Schrägstrich")),
                 true);
        QVERIFY(RenameController::validationError(QStringLiteral("a:b.jpg"))
                    .contains(QStringLiteral("Doppelpunkt")));
        QVERIFY(
            RenameController::validationError(QStringLiteral(".x.jpg")).contains(QStringLiteral("Punkt")));
        QVERIFY(!RenameController::validationError(QStringLiteral("   ")).isEmpty());
        QVERIFY(!RenameController::validationError(QString(300, QLatin1Char('a'))).isEmpty());
        QVERIFY(RenameController::validationError(QStringLiteral("Urlaub 2026 (1).jpg")).isEmpty());
        QCOMPARE(RenameController::baseNameLength(QStringLiteral("a.b.tiff")), 3);
        QCOMPARE(RenameController::extensionOf(QStringLiteral(".hidden")), QString());

        RenameController* r = startRename(w);
        QVERIFY(r);
        r->editor()->setText(QStringLiteral("img_10.JPG")); // taken (case-insensitive volume)
        r->commit();
        QVERIFY(w->toastText().contains(QStringLiteral("existiert bereits")));
        QVERIFY(r->isEditing());
        r->editor()->setText(QStringLiteral("a/b.jpg"));
        r->commit();
        QVERIFY(w->toastText().contains(QStringLiteral("Schrägstrich")));
        QVERIFY(r->isEditing());
        // Taken by something that is not in the collection: FileOps reports it, the editor stays.
        r->editor()->setText(QStringLiteral("Ordner.jpg"));
        r->commit();
        QTRY_VERIFY(!r->isPending());
        QVERIFY(r->isEditing());
        QVERIFY(w->toastText().contains(QStringLiteral("existiert bereits")));
        QVERIFY(QFileInfo::exists(path("IMG_2.jpg")));
        // Esc cancels without renaming.
        QTest::keyClick(r->editor(), Qt::Key_Escape);
        QVERIFY(!r->isEditing());
        QVERIFY(QFileInfo::exists(path("IMG_2.jpg")));
        QCOMPARE(w->currentPath(), path("IMG_2.jpg"));
    }

    void F_FILE_04_otherWindowsAndUndo() {
        makeFolder();
        ViewerWindow* a = openAndWait(path("IMG_2.jpg"));
        QVERIFY(a);
        ViewerWindow* b = app->duplicateWindow(a);
        QVERIFY(QTest::qWaitForWindowExposed(b));
        QVERIFY(waitLoaded(b));
        RecentItems::add({path("IMG_2.jpg")});
        RenameController* r = RenameController::of(a, *app);
        QVERIFY(r->start());
        r->editor()->setText(QStringLiteral("A_first.jpg"));
        r->commit();
        QTRY_VERIFY(QFileInfo::exists(path("A_first.jpg")));
        // Every window shows the new name at its sorted position.
        QTRY_COMPARE(names(b->collection()).first(), QStringLiteral("A_first.jpg"));
        QCOMPARE(names(a->collection()).first(), QStringLiteral("A_first.jpg"));
        QCOMPARE(b->currentPath(), path("A_first.jpg"));
        QCOMPARE(RecentItems::paths().first(), path("A_first.jpg")); // F-OPEN-11 entry follows
        // ⌘Z restores the old name everywhere.
        app->menu()->action(Action::Undo)->trigger();
        QTRY_VERIFY(QFileInfo::exists(path("IMG_2.jpg")));
        QTRY_VERIFY(names(a->collection()).contains(QStringLiteral("IMG_2.jpg")));
        QTRY_VERIFY(names(b->collection()).contains(QStringLiteral("IMG_2.jpg")));
        QVERIFY(!names(b->collection()).contains(QStringLiteral("A_first.jpg")));
        QCOMPARE(a->currentPath(), path("IMG_2.jpg"));
        // Flipping cancels an open edit.
        QVERIFY(r->start());
        QTest::keyClick(a->view(), Qt::Key_Escape);
        r->cancel();
        QVERIFY(r->start());
        a->perform(Action::NextImage);
        QVERIFY(!r->isEditing());
    }

    // ---- F-FILE-05 open with / editor ----

    void F_FILE_05_realQueryListsAppsForJpeg() {
        makeFolder();
        // LaunchServices on a worker thread (read-only query).
        const QList<AppEntry> apps =
            QtConcurrent::run([p = path("IMG_2.jpg")] { return queryApplicationsForFile(p); }).result();
        QVERIFY(!apps.isEmpty());
        int defaults = 0;
        QStringList paths;
        for (const AppEntry& a : apps) {
            QVERIFY(!a.name.isEmpty());
            QVERIFY(!paths.contains(a.path));
            paths << a.path;
            defaults += a.isDefault ? 1 : 0;
        }
        QVERIFY(defaults <= 1);
        if (defaults == 1)
            QVERIFY(apps.first().isDefault);
#ifndef LF_GENERIC_PLATFORM
        QVERIFY(std::any_of(apps.cbegin(), apps.cend(), [](const AppEntry& a) {
            return a.bundleId == QLatin1String("com.apple.Preview");
        }));
        QVERIFY(std::any_of(apps.cbegin(), apps.cend(), [](const AppEntry& a) { return !a.icon.isNull(); }));
#elif defined(Q_OS_WIN)
        // SHAssocEnumHandlers: programs (Paint is part of every Windows) with their icons.
        QVERIFY(std::any_of(apps.cbegin(), apps.cend(), [](const AppEntry& a) { return !a.icon.isNull(); }));
#else
        // Desktop entries (XdgDesktop.h): each app is its .desktop file.
        for (const AppEntry& a : apps)
            QVERIFY2(a.path.endsWith(QLatin1String(".desktop")), qPrintable(a.path));
#endif
    }

    void F_FILE_05_openWithMenuFromWorkerQuery() {
        makeFolder();
        std::atomic<int> queries{0};
        OpenWithService::instance().setQueryForTesting([&queries](const QString&) {
            ++queries;
            QThread::msleep(50); // slow system: the menu shows a placeholder meanwhile
            return QList<AppEntry>{appEntry("Lumaflip", kLumaflipBundleId, true),
                                   appEntry("Vorschau", "com.apple.Preview"),
                                   appEntry("Zeichner", "org.example.draw")};
        });
        ViewerWindow* w = openAndWait(path("IMG_2.jpg"));
        QVERIFY(w);
        QMenu* menu = app->menu()->extras()->openWithMenu();
        QVERIFY(menu);
        QTRY_VERIFY(command("openWith")->isEnabled()); // the menu state follows within 250 ms
        emit menu->aboutToShow();
        QVERIFY(menu->findChild<QAction*>(QStringLiteral("openWith:pending")));
        QTRY_VERIFY(menu->findChild<QAction*>(QStringLiteral("openWith:/Applications/Vorschau.app")));
        QVERIFY(!menu->findChild<QAction*>(
            QStringLiteral("openWith:/Applications/Lumaflip.app"))); // never itself
        QVERIFY(menu->findChild<QAction*>(QStringLiteral("openWith:other")));
        menu->findChild<QAction*>(QStringLiteral("openWith:/Applications/Zeichner.app"))->trigger();
        QCOMPARE(shell->opened.size(), 1);
        QCOMPARE(shell->opened.first(),
                 qMakePair(path("IMG_2.jpg"), QStringLiteral("/Applications/Zeichner.app")));
        // Cached per type: the next file of the type needs no query.
        emit menu->aboutToShow();
        QVERIFY(!menu->findChild<QAction*>(QStringLiteral("openWith:pending")));
        QCOMPARE(queries.load(), 1);
    }

    void F_FILE_05_editorNeverLumaflip() {
        const QList<AppEntry> apps = {appEntry("Lumaflip", kLumaflipBundleId, true),
                                      appEntry("Albert", "org.example.albert"),
                                      appEntry("Vorschau", "com.apple.Preview")};
        // Lumaflip is the default viewer: Preview is chosen, not Lumaflip.
        QCOMPARE(chooseEditor(apps, {}), QStringLiteral("/Applications/Vorschau.app"));
        QCOMPARE(chooseEditor(apps, QStringLiteral("/Applications/Lumaflip.app")),
                 QStringLiteral("/Applications/Vorschau.app"));
        QCOMPARE(chooseEditor(apps, QStringLiteral("/Applications/Pixel.app")),
                 QStringLiteral("/Applications/Pixel.app"));
        QCOMPARE(chooseEditor({appEntry("Albert", "a", true), appEntry("Vorschau", "com.apple.Preview")}, {}),
                 QStringLiteral("/Applications/Albert.app")); // the system default
        QVERIFY(chooseEditor({appEntry("Lumaflip", kLumaflipBundleId, true)}, {}).isEmpty());
        QString error;
        QVERIFY(!prefs::setEditorApp(QStringLiteral("/Applications/Lumaflip.app"), &error));
        QVERIFY(error.contains(QStringLiteral("Lumaflip")));
        QVERIFY(Settings::instance().value(key::EditorApp).toString().isEmpty());

        // "E" opens the file with the chosen editor (via the shell, never really).
        makeFolder();
        OpenWithService::instance().setQueryForTesting(
            [apps](const QString&) { return QList<AppEntry>(apps); });
        ViewerWindow* w = openAndWait(path("IMG_1.png"));
        QVERIFY(w);
        QVERIFY(InputBindings::isActionAvailable(Action::OpenInEditor));
        QTest::keyClick(w->view(), Qt::Key_E);
        QTRY_COMPARE(shell->opened.size(), 1);
        QCOMPARE(shell->opened.first().second, QStringLiteral("/Applications/Vorschau.app"));
        QVERIFY(prefs::setEditorApp(QStringLiteral("/Applications/Pixel.app")));
        QTest::keyClick(w->view(), Qt::Key_E);
        QTRY_COMPARE(shell->opened.size(), 2);
        QCOMPARE(shell->opened.last().second, QStringLiteral("/Applications/Pixel.app"));
        // Preferences show the choice.
        PreferencesDialog dlg;
        auto* row = dlg.findChild<QWidget*>(QStringLiteral("setting:") + key::EditorApp);
        QVERIFY(row);
        QCOMPARE(row->findChild<QLabel*>(QStringLiteral("editorName"))->text(), QStringLiteral("Pixel"));
        row->findChild<QPushButton*>(QStringLiteral("defaultEditor"))->click();
        QVERIFY(Settings::instance().value(key::EditorApp).toString().isEmpty());
    }

    // ---- F-FILE-06 share ----

    void F_FILE_06_shareItemsAndMenu() {
#ifdef LF_GENERIC_PLATFORM
        QSKIP("no share sheet outside macOS (docs/LINUX.md)");
#endif
        makeFolder();
        QCOMPARE(shareItems({path("IMG_2.jpg")}), QList<QUrl>{QUrl::fromLocalFile(path("IMG_2.jpg"))});
        QVERIFY(shareItems({QString()}).isEmpty());
        const QList<ShareService> services = shareServices({path("IMG_2.jpg")});
        QVERIFY(!services.isEmpty()); // AirDrop, Mail, Nachrichten … (not performed)
        for (const ShareService& s : services)
            QVERIFY(!s.title.isEmpty());
        QVERIFY(shareServices({}).isEmpty());

        ViewerWindow* w = openAndWait(path("IMG_2.jpg"));
        QVERIFY(w);
        QMenu* menu = app->menu()->extras()->shareMenu();
        QVERIFY(menu);
        QVERIFY(command("share")->isEnabled());
        emit menu->aboutToShow();
        // The services arrive from a worker (placeholder until then, never blocking).
        QTRY_VERIFY(menu->findChild<QAction*>(QStringLiteral("share:") + services.first().title));
        QAction* picker = menu->findChild<QAction*>(QStringLiteral("share:picker"));
        QVERIFY(picker && picker->isEnabled());
        // Without an image there is nothing to share.
        ViewerWindow* empty = app->newWindow();
        QVERIFY(QTest::qWaitForWindowExposed(empty));
        app->menu()->refreshState();
        QVERIFY(!command("share")->isEnabled());
        emit menu->aboutToShow();
        QVERIFY(!menu->findChild<QAction*>(QStringLiteral("share:picker"))->isEnabled());
    }

    // ---- F-FILE-07 drag-out ----

    void F_FILE_07_dragMimeDataContents() {
        makeFolder();
        QImage big(4000, 3000, QImage::Format_RGB32);
        big.fill(Qt::darkCyan);
        std::unique_ptr<QMimeData> mime(createDragMimeData(path("IMG_2.jpg"), big));
        QCOMPARE(mime->urls(), QList<QUrl>{QUrl::fromLocalFile(path("IMG_2.jpg"))});
        QVERIFY(mime->hasImage());
        const QImage carried = qvariant_cast<QImage>(mime->imageData());
        QCOMPARE(std::max(carried.width(), carried.height()), kDragImageMaxPx);
        QImage hdr(100, 50, QImage::Format_RGBA16FPx4);
        hdr.fill(Qt::red);
        std::unique_ptr<QMimeData> hdrMime(createDragMimeData(path("IMG_2.jpg"), hdr));
        QCOMPARE(qvariant_cast<QImage>(hdrMime->imageData()).format(), QImage::Format_ARGB32);
        std::unique_ptr<QMimeData> noImage(createDragMimeData(path("IMG_2.jpg"), QImage()));
        QVERIFY(!noImage->hasImage());
        QVERIFY(!noImage->urls().isEmpty());
        const QPixmap pm = dragPixmap(big, 2.0);
        QCOMPARE(std::max(pm.width(), pm.height()), kDragThumbnailMaxPt * 2);
        QCOMPARE(pm.devicePixelRatio(), 2.0);
    }

    void F_FILE_07_altDragStartsDragOut() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("IMG_2.jpg"));
        QVERIFY(w);
        QList<QUrl> dragged;
        int drags = 0;
        setDragExecHookForTesting([&](QDrag* drag) {
            ++drags;
            dragged = drag->mimeData()->urls();
            return Qt::CopyAction;
        });
        QWidget* c = canvas(w);
        const QPointF p(c->width() / 2.0, c->height() / 2.0);
        const QPointF far = p + QPointF(QApplication::startDragDistance() * 3, 0);
        // Default (F-IN-14): ⌥-drag = drag-out, plain drag pans.
        sendMouse(c, QEvent::MouseButtonPress, p, Qt::AltModifier);
        sendMouse(c, QEvent::MouseMove, far, Qt::AltModifier);
        sendMouse(c, QEvent::MouseButtonRelease, far, Qt::AltModifier);
        QCOMPARE(drags, 1);
        QCOMPARE(dragged, QList<QUrl>{QUrl::fromLocalFile(path("IMG_2.jpg"))});
        sendMouse(c, QEvent::MouseButtonPress, p, Qt::NoModifier);
        sendMouse(c, QEvent::MouseMove, far, Qt::NoModifier);
        sendMouse(c, QEvent::MouseButtonRelease, far, Qt::NoModifier);
        QCOMPARE(drags, 1);
        // Setting "Datei herausziehen": plain drag drags out, ⌥ pans.
        QVERIFY(InputBindings::isChoiceAvailable(QStringLiteral("drag"), QStringLiteral("dragOut")));
        app->bindings().setChoice(QStringLiteral("drag"), QStringLiteral("dragOut"));
        sendMouse(c, QEvent::MouseButtonPress, p, Qt::NoModifier);
        sendMouse(c, QEvent::MouseMove, far, Qt::NoModifier);
        sendMouse(c, QEvent::MouseButtonRelease, far, Qt::NoModifier);
        QCOMPARE(drags, 2);
        sendMouse(c, QEvent::MouseButtonPress, p, Qt::AltModifier);
        sendMouse(c, QEvent::MouseMove, far, Qt::AltModifier);
        sendMouse(c, QEvent::MouseButtonRelease, far, Qt::AltModifier);
        QCOMPARE(drags, 2);
    }

    // ---- F-OPEN-11 recent items ----

    void F_OPEN_11_recentOrderMaxClearPersisted() {
        QVERIFY(!RecentItems::systemRegistrationEnabled());
        const QString ini = m_settingsDir.filePath(QStringLiteral("recent.ini"));
        Settings::useTestingBackend(ini);
        RecentItems::clear();
        for (int i = 0; i < 25; ++i)
            RecentItems::add({path(QStringLiteral("f%1.jpg").arg(i))});
        QList<RecentItem> items = RecentItems::items();
        QCOMPARE(items.size(), RecentItems::kMaxItems);
        QCOMPARE(items.first().path, path("f24.jpg")); // newest first
        QCOMPARE(items.last().path, path("f5.jpg"));
        RecentItems::add({path("f10.jpg")}); // again: moves to the top, no duplicate
        items = RecentItems::items();
        QCOMPARE(items.first().path, path("f10.jpg"));
        QCOMPARE(items.size(), RecentItems::kMaxItems);
        QCOMPARE(RecentItems::paths().count(path("f10.jpg")), 1);
        RecentItems::add({path("a.jpg"), path("b.jpg")}, {path("Ordner")});
        items = RecentItems::items();
        QCOMPARE(items.at(0), (RecentItem{path("a.jpg"), false}));
        QCOMPARE(items.at(1), (RecentItem{path("b.jpg"), false}));
        QCOMPARE(items.at(2), (RecentItem{path("Ordner"), true}));
        // Persisted: a fresh settings object reads the same list.
        Settings::useTestingBackend(m_settingsDir.filePath(QStringLiteral("other.ini")));
        QVERIFY(RecentItems::items().isEmpty());
        Settings::useTestingBackend(ini);
        QCOMPARE(RecentItems::items(), items);
        // Menu: entries + "Einträge löschen"; equal names show their folder.
        RecentItems::add({path("x/IMG.jpg"), path("y/IMG.jpg")});
        QMenu menu;
        QString opened;
        bool cleared = false;
        RecentItems::populateMenu(&menu, [&](const QString& p) { opened = p; }, [&] { cleared = true; });
        QStringList texts;
        for (QAction* a : menu.actions())
            if (!a->isSeparator())
                texts << a->text();
        QCOMPARE(texts.size(), RecentItems::kMaxItems + 1);
        QCOMPARE(texts.first(), QStringLiteral("IMG.jpg — x"));
        QCOMPARE(texts.at(1), QStringLiteral("IMG.jpg — y"));
        QCOMPARE(texts.last(), QStringLiteral("Einträge löschen"));
        menu.actions().first()->trigger();
        QCOMPARE(opened, path("x/IMG.jpg"));
        menu.findChild<QAction*>(QStringLiteral("recent:clear"))->trigger();
        QVERIFY(cleared);
        RecentItems::clear();
        QVERIFY(RecentItems::items().isEmpty());
        RecentItems::populateMenu(&menu, {}, {});
        QVERIFY(!menu.findChild<QAction*>(QStringLiteral("recent:clear"))->isEnabled());
    }

    void F_OPEN_11_openingAddsFilesAndFolders() {
        makeFolder();
        QVERIFY(QDir().mkpath(path("Sub")));
        QVERIFY(!makeImage(path("Sub/s.png"), Qt::gray).isEmpty());
        ViewerWindow* w = openAndWait(path("IMG_2.jpg"));
        QVERIFY(w);
        QTRY_COMPARE(app->recentPaths().value(0), path("IMG_2.jpg"));
        app->openPaths({path("Sub")});
        QTRY_COMPARE(app->recentPaths().value(0), path("Sub"));
        QCOMPARE(RecentItems::items().first().folder, true);
        auto* recentMenu = app->menu()->menuBar()->findChild<QMenu*>(QStringLiteral("menu:recent"));
        QVERIFY(recentMenu);
        emit recentMenu->aboutToShow();
        QCOMPARE(recentMenu->actions().first()->text(), QStringLiteral("Sub"));
        app->clearRecentPaths();
        QVERIFY(app->recentPaths().isEmpty());
    }

    // ---- F-WIN-04 arrange ----

    void F_WIN_04_arrangedFramesGeometry() {
        const QRect area(0, 25, 1440, 850);
        QCOMPARE(gridShape(1), QSize(1, 1));
        QCOMPARE(gridShape(2), QSize(2, 1));
        QCOMPARE(gridShape(4), QSize(2, 2));
        QCOMPARE(gridShape(5), QSize(3, 2));
        QCOMPARE(gridShape(6), QSize(3, 2));
        QCOMPARE(gridShape(7), QSize(3, 3));

        const QList<QRect> side = arrangedFrames(Arrangement::SideBySide, 3, area);
        QCOMPARE(side,
                 (QList<QRect>{QRect(0, 25, 480, 850), QRect(480, 25, 480, 850), QRect(960, 25, 480, 850)}));
        const QList<QRect> stacked = arrangedFrames(Arrangement::Stacked, 2, area);
        QCOMPARE(stacked, (QList<QRect>{QRect(0, 25, 1440, 425), QRect(0, 450, 1440, 425)}));
        const QList<QRect> grid = arrangedFrames(Arrangement::Grid, 4, area);
        QCOMPARE(grid, (QList<QRect>{QRect(0, 25, 720, 425), QRect(720, 25, 720, 425),
                                     QRect(0, 450, 720, 425), QRect(720, 450, 720, 425)}));
        const QList<QRect> grid6 = arrangedFrames(Arrangement::Grid, 6, area); // 3×2
        QCOMPARE(grid6.at(2), QRect(960, 25, 480, 425));
        QCOMPARE(grid6.at(5), QRect(960, 450, 480, 425));
        // Odd sizes: the cells cover the area exactly, without gaps or overlaps.
        const QRect odd(10, 10, 1001, 703);
        const QList<QRect> g5 = arrangedFrames(Arrangement::Grid, 5, odd);
        int covered = 0;
        for (const QRect& r : g5) {
            QVERIFY(odd.contains(r));
            covered += r.width() * r.height();
        }
        QVERIFY(covered < odd.width() * odd.height()); // one empty cell
        QCOMPARE(arrangedFrames(Arrangement::SideBySide, 3, odd).last().right(), odd.right());

        const QList<QRect> cascade = arrangedFrames(Arrangement::Cascade, 4, area, QSize(800, 600));
        for (int i = 0; i < 4; ++i) {
            QCOMPARE(cascade.at(i).size(), QSize(800, 600));
            QCOMPARE(cascade.at(i).topLeft(), area.topLeft() + QPoint(i, i) * kArrangeCascadeStepPx);
            QVERIFY(area.contains(cascade.at(i)));
        }
        // Many windows wrap around instead of leaving the screen.
        for (const QRect& r : arrangedFrames(Arrangement::Cascade, 40, area, QSize(1300, 800)))
            QVERIFY(area.contains(r));
        QVERIFY(arrangedFrames(Arrangement::Grid, 0, area).isEmpty());
    }

    void F_WIN_04_arrangeWindowsOnScreen() {
        ViewerWindow* a = app->newWindow();
        ViewerWindow* b = app->newWindow();
        QVERIFY(QTest::qWaitForWindowExposed(a) && QTest::qWaitForWindowExposed(b));
#ifndef LF_GENERIC_PLATFORM
        QVERIFY(supportsSystemTiling(a)); // macOS 15 tiling (system commands)
#endif
        app->windowActivated(b);
        app->menu()->refreshState();
        QAction* side = command("arrange:sideBySide");
        QVERIFY(side && side->isEnabled());
        side->trigger();
        const QRect area = b->screen()->availableGeometry();
        const int tolerance = 40; // the window server may adjust frames slightly
        QTRY_VERIFY(std::abs(b->frameGeometry().left() - area.left()) <= tolerance);
        QTRY_VERIFY(std::abs(a->frameGeometry().right() - area.right()) <= tolerance);
        QVERIFY(std::abs(a->frameGeometry().width() - area.width() / 2) <= tolerance);
        QVERIFY(b->frameGeometry().right() <= a->frameGeometry().left() + tolerance); // front window first
        command("arrange:stacked")->trigger();
        QTRY_VERIFY(std::abs(b->frameGeometry().top() - area.top()) <= tolerance);
        QTRY_VERIFY(a->frameGeometry().top() >= area.top() + area.height() / 2 - tolerance);
        command("arrange:cascade")->trigger();
        QTRY_VERIFY(a->frameGeometry().topLeft() != b->frameGeometry().topLeft());
        QVERIFY(area.adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(a->frameGeometry()));
        QCOMPARE(arrangeViewerWindows(*app, Arrangement::Grid), 2);
    }

    // ---- F-WIN-05 tabs ----

    void F_WIN_05_tabbingModeFollowsSetting() {
#ifdef LF_GENERIC_PLATFORM
        QSKIP("native window tabs are macOS only (docs/LINUX.md)");
#endif
        ViewerWindow* a = app->newWindow();
        QVERIFY(QTest::qWaitForWindowExposed(a));
        QCOMPARE(nativeTabbingMode(a), NativeTabbingMode::Disallowed); // default: no tabs
        app->menu()->refreshState();
        QVERIFY(!command("tabs:merge")->isEnabled());
        QVERIFY(!performTabCommand(a, TabCommand::MergeAllWindows));
        Settings::instance().setValue(key::AllowTabs, true);
        QCOMPARE(nativeTabbingMode(a), NativeTabbingMode::Preferred);
        QVERIFY(command("tabs:merge")->isEnabled());
        ViewerWindow* b = app->newWindow(); // opens as a tab of the front window
        QVERIFY(QTest::qWaitForWindowExposed(b));
        QCOMPARE(nativeTabbingMode(b), NativeTabbingMode::Preferred);
        QVERIFY(performTabCommand(b, TabCommand::MergeAllWindows));
        QTRY_COMPARE(tabGroupSize(a), 2);
        QVERIFY(performTabCommand(b, TabCommand::MoveTabToNewWindow));
        QTRY_COMPARE(tabGroupSize(a), 1);
        Settings::instance().setValue(key::AllowTabs, false);
        QCOMPARE(nativeTabbingMode(a), NativeTabbingMode::Disallowed);
        QCOMPARE(nativeTabbingMode(b), NativeTabbingMode::Disallowed);
        // Preferences: Fenster → tabs.
        PreferencesDialog dlg;
        auto* check = dlg.findChild<QCheckBox*>(QStringLiteral("setting:") + key::AllowTabs);
        QVERIFY(check && !check->isChecked());
    }

    // ---- F-NAV-04 trackpad ----

    void F_NAV_04_twoFingerSwipeFlipsOncePerGesture() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("IMG_10.jpg")); // name order: IMG_1, IMG_2, IMG_10
        QVERIFY(w);
        QCOMPARE(w->collection()->indexOf(path("IMG_10.jpg")), 2);
        QWidget* c = canvas(w);
        // Fingers to the right (natural scrolling): previous image.
        sendWheel(c, {}, Qt::ScrollBegin);
        for (int i = 0; i < 4; ++i)
            sendWheel(c, QPoint(30, 2), Qt::ScrollUpdate);
        QCOMPARE(w->currentPath(), path("IMG_2.jpg"));
        sendWheel(c, QPoint(40, 0), Qt::ScrollUpdate); // same gesture: no second flip
        sendWheel(c, {}, Qt::ScrollEnd);
        for (int i = 0; i < 5; ++i)
            sendWheel(c, QPoint(40, 0), Qt::ScrollMomentum); // inertia is no new swipe
        QCOMPARE(w->currentPath(), path("IMG_2.jpg"));
        // Fingers to the left: next image.
        sendWheel(c, {}, Qt::ScrollBegin);
        for (int i = 0; i < 4; ++i)
            sendWheel(c, QPoint(-30, 0), Qt::ScrollUpdate);
        sendWheel(c, {}, Qt::ScrollEnd);
        QCOMPARE(w->currentPath(), path("IMG_10.jpg"));
        // Classic scrolling direction: the deltas are reversed, the fingers still decide.
        sendWheel(c, {}, Qt::ScrollBegin, false);
        for (int i = 0; i < 4; ++i)
            sendWheel(c, QPoint(-30, 0), Qt::ScrollUpdate, false); // fingers to the right
        sendWheel(c, {}, Qt::ScrollEnd, false);
        QCOMPARE(w->currentPath(), path("IMG_2.jpg"));
        w->perform(Action::NextImage);
        // Small or mostly vertical movement is not a swipe.
        sendWheel(c, {}, Qt::ScrollBegin);
        sendWheel(c, QPoint(20, 20), Qt::ScrollUpdate);
        sendWheel(c, {}, Qt::ScrollEnd);
        QCOMPARE(w->currentPath(), path("IMG_10.jpg"));
        // The native swipe gesture (three fingers, if enabled in System Settings).
        QNativeGestureEvent swipe(Qt::SwipeNativeGesture, QPointingDevice::primaryPointingDevice(), 3,
                                  QPointF(10, 10), QPointF(10, 10), c->mapToGlobal(QPointF(10, 10)), 0.0,
                                  QPointF());
        QCoreApplication::sendEvent(c, &swipe);
        QCOMPARE(w->currentPath(), path("IMG_2.jpg"));
        // Setting "aus" (F-IN-13): ignored.
        app->bindings().setChoice(QStringLiteral("swipe"), QStringLiteral("off"));
        sendWheel(c, {}, Qt::ScrollBegin);
        for (int i = 0; i < 4; ++i)
            sendWheel(c, QPoint(-30, 0), Qt::ScrollUpdate);
        sendWheel(c, {}, Qt::ScrollEnd);
        QCOMPARE(w->currentPath(), path("IMG_2.jpg"));
    }

    void F_NAV_04_pinchZoomsAndSwipePansWhenZoomed() {
        QVERIFY(!makeImage(path("a_big.jpg"), Qt::darkGreen, QSize(3000, 2000)).isEmpty());
        QVERIFY(!makeImage(path("b_next.jpg"), Qt::darkRed).isEmpty());
        ViewerWindow* w = openAndWait(path("a_big.jpg"));
        QVERIFY(w);
        w->resize(800, 600);
        QWidget* c = canvas(w);
        QTRY_VERIFY(!w->view()->isZoomed());
        const double before = w->view()->zoomFactor();
        const QPointF mid(c->width() / 2.0, c->height() / 2.0);
        QNativeGestureEvent begin(Qt::BeginNativeGesture, QPointingDevice::primaryPointingDevice(), 2, mid,
                                  mid, c->mapToGlobal(mid), 0, QPointF());
        QCoreApplication::sendEvent(c, &begin);
        for (int i = 0; i < 6; ++i) {
            QNativeGestureEvent pinch(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2, mid,
                                      mid, c->mapToGlobal(mid), 0.25, QPointF());
            QCoreApplication::sendEvent(c, &pinch);
        }
        QNativeGestureEvent end(Qt::EndNativeGesture, QPointingDevice::primaryPointingDevice(), 2, mid, mid,
                                c->mapToGlobal(mid), 0, QPointF());
        QCoreApplication::sendEvent(c, &end);
        QVERIFY(w->view()->zoomFactor() > before * 3);
        QVERIFY(w->view()->isZoomed());
        // Zoomed: the horizontal two-finger movement pans instead of flipping.
        const QPointF center = w->view()->viewCenter();
        sendWheel(c, {}, Qt::ScrollBegin);
        for (int i = 0; i < 6; ++i)
            sendWheel(c, QPoint(-40, 0), Qt::ScrollUpdate);
        sendWheel(c, {}, Qt::ScrollEnd);
        QCOMPARE(w->currentPath(), path("a_big.jpg"));
        QVERIFY(w->view()->viewCenter().x() > center.x());
        // Pinching back below "fit" and swiping flips again.
        w->setZoomMode(ZoomMode::Fit);
        sendWheel(c, {}, Qt::ScrollBegin);
        for (int i = 0; i < 4; ++i)
            sendWheel(c, QPoint(-30, 0), Qt::ScrollUpdate);
        sendWheel(c, {}, Qt::ScrollEnd);
        QCOMPARE(w->currentPath(), path("b_next.jpg"));
    }

    // ---- F-COL-02 output profile per screen ----

    void F_COL_02_screenChangeUpdatesOutputProfile() {
#ifdef LF_GENERIC_PLATFORM
        QSKIP("no per-window screen profiles outside macOS (docs/LINUX.md)");
#endif
        QColorSpace current = QColorSpace(QColorSpace::DisplayP3);
        ImageView::setColorSpaceResolverForTesting([&current](const QWidget*) { return current; });
        QWidget top;
        top.resize(200, 150);
        auto* view = new ImageView(&top);
        view->resize(200, 150);
        top.show();
        QVERIFY(QTest::qWaitForWindowExposed(&top));
        QCOMPARE(view->outputColorSpace(), QColorSpace(QColorSpace::DisplayP3));
        // The window moved to another screen (Qt reports QWindow::screenChanged).
        current = QColorSpace(QColorSpace::SRgb);
        emit top.windowHandle()->screenChanged(top.windowHandle()->screen());
        QCOMPARE(view->outputColorSpace(), QColorSpace(QColorSpace::SRgb));
        // macOS: the display's profile changed (or the window's screen, before Qt notices).
        current = QColorSpace(QColorSpace::AdobeRgb);
        QVERIFY(ScreenProfileWatcher::postProfileChangeForTesting(&top));
        QTRY_COMPARE(view->outputColorSpace(), QColorSpace(QColorSpace::AdobeRgb));
        // An explicit override wins over the screen.
        view->setOutputColorSpace(QColorSpace(QColorSpace::ProPhotoRgb));
        current = QColorSpace(QColorSpace::SRgb);
        emit top.windowHandle()->screenChanged(top.windowHandle()->screen());
        QCOMPARE(view->outputColorSpace(), QColorSpace(QColorSpace::ProPhotoRgb));
        view->setOutputColorSpace(QColorSpace());
        QCOMPARE(view->outputColorSpace(), QColorSpace(QColorSpace::SRgb));
    }

    // ---- F-SORT-04 live Finder query (opt-in) ----

    void F_SORT_04_settingEnablesLiveQueryWithFakeProvider() {
        makeFolder();
        // opt-in on macOS; Windows asks Explorer without a permission: on by default
        QCOMPARE(Settings::instance().value(key::FinderLiveQuery).toBool(),
                 platformFileManager() == FileManager::Explorer);
        Settings::instance().setValue(key::FinderLiveQuery, false);
        std::unique_ptr<NameCollator> collator = createNameCollator();
        std::unique_ptr<FileAttributeProvider> attributes = createFileAttributeProvider();
        ViewerWindow* w = app->newWindow();
        QVERIFY(QTest::qWaitForWindowExposed(w));
        w->collection()->setProviders(collator.get(), order.get(), attributes.get());
        w->openFolder(QDir::cleanPath(dir->path()));
        QVERIFY(waitLoaded(w));
        QVERIFY(order->calls.load() > 0);
        QCOMPARE(order->liveCalls.load(), 0); // never without the opt-in
        QTRY_COMPARE(w->sortText(), QStringLiteral("Wie im Finder: Name ↑"));
        Settings::instance().setValue(key::FinderLiveQuery, true);
        QTRY_COMPARE(w->collection()->sortSpec().source, SortSource::FinderLive);
        QVERIFY(order->liveCalls.load() > 0);
        QTRY_COMPARE(w->sortText(), QStringLiteral("Wie im Finder (live): Name ↓"));
        QVERIFY(w->statusText().contains(QStringLiteral("Wie im Finder (live): Name ↓")));
        QCOMPARE(names(w->collection()).first(), QStringLiteral("IMG_10.jpg")); // descending
        Settings::instance().setValue(key::FinderLiveQuery, false);
        QTRY_COMPARE(w->collection()->sortSpec().source, SortSource::FinderDsStore);
        w->close();
        QThreadPool::globalInstance()->waitForDone(5000);
    }

    void F_SORT_04_explainedBeforeEnabling() {
        PreferencesDialog dlg;
        auto* check = dlg.findChild<QCheckBox*>(QStringLiteral("setting:") + key::FinderLiveQuery);
        if (platformFileManager() == FileManager::Other) { // Linux: nothing to query
            QVERIFY(!check);
            return;
        }
        QVERIFY(check);
        if (platformFileManager() == FileManager::Explorer) { // no permission: no explanation
            QCOMPARE(check->text(), QStringLiteral("Sortierung live vom Explorer-Fenster übernehmen"));
            QVERIFY(check->isChecked()); // on by default
            check->setChecked(false);
            QVERIFY(!Settings::instance().value(key::FinderLiveQuery).toBool());
            check->setChecked(true);
            QVERIFY(!dlg.findChild<QMessageBox*>(QStringLiteral("lfLiveQueryExplanation")));
            QVERIFY(Settings::instance().value(key::FinderLiveQuery).toBool());
            return;
        }
        QCOMPARE(check->text(), QStringLiteral("Sortierung live vom Finder-Fenster übernehmen"));
        check->setChecked(true);
        auto* box = dlg.findChild<QMessageBox*>(QStringLiteral("lfLiveQueryExplanation"));
        QVERIFY(box);
        QVERIFY(box->informativeText().contains(QStringLiteral("Automation")));
        QVERIFY(!check->isChecked()); // not before the user confirms
        QVERIFY(!Settings::instance().value(key::FinderLiveQuery).toBool());
        box->reject(); // "Abbrechen"
        QTest::qWait(10);
        QVERIFY(!Settings::instance().value(key::FinderLiveQuery).toBool());
        check->setChecked(true);
        box = dlg.findChild<QMessageBox*>(QStringLiteral("lfLiveQueryExplanation"));
        QVERIFY(box);
        box->findChild<QPushButton*>(QStringLiteral("lfLiveQueryEnable"))->click();
        QVERIFY(Settings::instance().value(key::FinderLiveQuery).toBool());
        QVERIFY(check->isChecked());
        check->setChecked(false); // turning it off needs no question
        QVERIFY(!Settings::instance().value(key::FinderLiveQuery).toBool());
    }
};

int main(int argc, char* argv[]) {
#ifdef Q_OS_MACOS
    // The canvas needs Metal; the offscreen platform has no QRhi.
    if (qgetenv("QT_QPA_PLATFORM") == "offscreen")
        qputenv("QT_QPA_PLATFORM", "cocoa");
#endif
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("LumaflipTest"));
    TstShellM2 tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_shell_m2.moc"
