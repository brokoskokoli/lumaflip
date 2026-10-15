// SPDX-License-Identifier: GPL-3.0-or-later
// M3 culling in real windows: culling mode with overlay and locked zoom (F-CULL-06), rating
// keys -> ratings -> auto advance (F-CULL-01/03), the filter in menu and status line
// (F-CULL-04), "all rejects" (F-CULL-05), quick move/copy keys (F-FILE-08) and the
// conflict dialog (F-FILE-11). Files only in temporary folders; the trash is a temporary
// folder too.

#include "app/Application.h"
#include "app/CullingController.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/MainMenu.h"
#include "app/PreferencesDialog.h"
#include "app/QuickMoveController.h"
#include "app/RecentItems.h"
#include "app/ViewerWindow.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/InputBindings.h"
#include "core/Platform.h"
#include "core/QuickMove.h"
#include "core/RatingStore.h"
#include "core/Settings.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QUndoStack>
#include <QUuid>
#include <QtTest>

#include <memory>

using namespace lf;

namespace {

// "Trash" = a temporary folder (never the user's trash).
class FolderTrash : public TrashService {
public:
    std::optional<QString> moveToTrash(const QString& path, QString* error) override {
        QString target =
            QDir(dir.path())
                .filePath(QUuid::createUuid().toString(QUuid::Id128) + QFileInfo(path).fileName());
        if (!QFile::rename(path, target)) {
            if (error)
                *error = QStringLiteral("trash failed");
            return std::nullopt;
        }
        return target;
    }
    bool restoreFromTrash(const QString& trashed, const QString& original, QString* error) override {
        if (QFileInfo::exists(original) || !QFile::rename(trashed, original)) {
            if (error)
                *error = QStringLiteral("restore failed");
            return false;
        }
        return true;
    }
    QTemporaryDir dir;
};

class NullShell : public Shell {
public:
    void revealInFileManager(const QString&) override {}
    void openWith(const QString&, const QString&) override {}
    QStringList imageDocumentTypes() const override { return {"public.jpeg"}; }
    bool makeDefaultViewer(const QStringList&, QString*) override { return true; }
};

QString makeImage(const QString& path, QColor color) {
    QImage img(64, 48, QImage::Format_RGB32);
    img.fill(color);
    return img.save(path) ? QDir::cleanPath(path) : QString();
}

Rating stars(int n, PickFlag flag = PickFlag::None) {
    Rating r;
    r.stars = n;
    r.flag = flag;
    return r;
}

} // namespace

class TstCulling : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<FolderTrash> trash;
    std::unique_ptr<FileOps> ops;
    std::unique_ptr<NullShell> shell;
    std::unique_ptr<Application> app;

    QString path(const QString& name) const { return QDir::cleanPath(dir->path()) + QLatin1Char('/') + name; }
    void makeFolder() {
        QVERIFY(!makeImage(path("a.jpg"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path("b.jpg"), Qt::green).isEmpty());
        QVERIFY(!makeImage(path("c.jpg"), Qt::blue).isEmpty());
    }
    static bool waitLoaded(ViewerWindow* w, int ms = 5000) {
        return QTest::qWaitFor([w] { return !w->collection()->isLoading(); }, ms);
    }
    ViewerWindow* openAndWait(const QString& file) {
        ViewerWindow* w = app->openPaths({file});
        if (!w || !QTest::qWaitForWindowExposed(w, 5000) || !waitLoaded(w))
            return nullptr;
        if (!QTest::qWaitFor([&] { return w->shownPath() == file; }, 5000))
            return nullptr;
        return w;
    }
    QAction* command(const QString& id) const { return app->menu()->command(id); }
    static Rating sidecar(const QString& image) { return ratingio::readSidecar(image).value_or(Rating()); }
    template <typename T> static T* waitForTopLevel(const QString& name, int ms = 3000) {
        T* found = nullptr;
        (void)QTest::qWaitFor(
            [&] {
                for (QWidget* w : QApplication::topLevelWidgets())
                    if (auto* t = qobject_cast<T*>(w); t && t->objectName() == name && t->isVisible())
                        found = t;
                return found != nullptr;
            },
            ms);
        return found;
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
        trash = std::make_unique<FolderTrash>();
        ops = std::make_unique<FileOps>(*trash);
        shell = std::make_unique<NullShell>();
        AppServices services;
        services.fileOps = ops.get();
        services.shell = shell.get();
        services.openUrl = [](const QUrl&) { return true; };
        app = std::make_unique<Application>(services);
        RatingStore::instance().setOptions(RatingOptions()); // sidecar only
    }

    void cleanup() {
        if (auto* dlg = PreferencesDialog::existingInstance())
            dlg->close();
        app.reset();
        QTest::qWait(10);
        QThreadPool::globalInstance()->waitForDone(5000);
        ops.reset();
        trash.reset();
        shell.reset();
        dir.reset();
    }

    // Covers: F-CULL-06 (mode, overlay, locked zoom, Esc), F-CULL-01 (digits = stars only here)
    void F_CULL_06_cullingModeOverlayAndLockedZoom() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("a.jpg"));
        QVERIFY(w);
        CullingController* c = w->culling();
        QVERIFY(c);
        QVERIFY(!c->isActive());
        // Outside the culling mode "3" is 100 %.
        QTest::keyClick(w->view(), Qt::Key_3);
        QCOMPARE(w->view()->zoomMode(), ZoomMode::Actual);
        QTest::keyClick(w->view(), Qt::Key_1);
        QCOMPARE(w->view()->zoomMode(), ZoomMode::Fit);
        QAction* toggle = app->menu()->action(Action::ToggleCullingMode);
        QVERIFY(toggle);
        QVERIFY(toggle->shortcuts().contains(QKeySequence(Qt::ControlModifier | Qt::Key_K)));
        toggle->trigger();
        QVERIFY(c->isActive());
        QVERIFY(c->overlay() && c->overlay()->isVisible());
        QVERIFY(w->view()->zoomLocked()); // key::CullLockZoom default
        // In the culling mode "3" gives three stars (and does not zoom).
        QTest::keyClick(w->view(), Qt::Key_3);
        QTRY_COMPARE(sidecar(path("a.jpg")), stars(3));
        QCOMPARE(w->view()->zoomMode(), ZoomMode::Fit);
        QTRY_COMPARE(c->currentRating().value_or(Rating()), stars(3));
        // Esc leaves the mode, the zoom lock is restored.
        QTest::keyClick(w->view(), Qt::Key_Escape);
        QVERIFY(!c->isActive());
        QVERIFY(!c->overlay()->isVisible());
        QVERIFY(!w->view()->zoomLocked());
        // K toggles it too.
        QTest::keyClick(w->view(), Qt::Key_K);
        QVERIFY(c->isActive());
    }

    // Covers: F-CULL-01, F-CULL-02 (undo), F-CULL-03 (auto advance)
    void F_CULL_03_ratingKeysAutoAdvanceAndUndo() {
        makeFolder();
        Settings::instance().setValue(key::CullAutoAdvance, true);
        ViewerWindow* w = openAndWait(path("a.jpg"));
        QVERIFY(w);
        w->culling()->setActive(true);
        QTest::keyClick(w->view(), Qt::Key_P);
        QTRY_COMPARE(w->currentPath(), path("b.jpg")); // advanced at once
        QTRY_COMPARE(sidecar(path("a.jpg")), stars(0, PickFlag::Pick));
        QTest::keyClick(w->view(), Qt::Key_4);
        QTRY_COMPARE(w->currentPath(), path("c.jpg"));
        QTRY_COMPARE(sidecar(path("b.jpg")), stars(4));
        QTest::keyClick(w->view(), Qt::Key_X);
        QTRY_COMPARE(sidecar(path("c.jpg")), stars(0, PickFlag::Reject));
        // ⌘Z undoes the last rating.
        QTRY_VERIFY(!ops->isBusy());
        w->perform(Action::Undo);
        QTRY_COMPARE(sidecar(path("c.jpg")), Rating());
        QCOMPARE(sidecar(path("b.jpg")), stars(4));
        // U removes the flag but keeps the stars.
        Settings::instance().setValue(key::CullAutoAdvance, false);
        QVERIFY(w->collection()->setCurrentPath(path("a.jpg")));
        QTest::keyClick(w->view(), Qt::Key_5);
        QTRY_COMPARE(sidecar(path("a.jpg")), stars(5, PickFlag::Pick));
        QTest::keyClick(w->view(), Qt::Key_U);
        QTRY_COMPARE(sidecar(path("a.jpg")), stars(5));
        QCOMPARE(w->currentPath(), path("a.jpg")); // no auto advance now
    }

    // Covers: F-CULL-04 (menu filter, status "Filter: ≥ 3 ★ — 1 / 3")
    void F_CULL_04_filterMenuAndStatus() {
        makeFolder();
        QVERIFY(ratingio::writeSidecar(path("a.jpg"), stars(4)).isEmpty());
        QVERIFY(ratingio::writeSidecar(path("b.jpg"), stars(1)).isEmpty());
        ViewerWindow* w = openAndWait(path("a.jpg"));
        QVERIFY(w);
        QAction* min3 = command(QStringLiteral("filter:min3"));
        QVERIFY(min3);
        min3->trigger();
        QTRY_COMPARE(w->collection()->count(), 1);
        QCOMPARE(w->collection()->totalCount(), 3);
        QVERIFY(min3->isChecked());
        QTRY_VERIFY(w->statusText().contains(QStringLiteral("Filter: ≥ 3 ★ — 1 / 3")));
        command(QStringLiteral("filter:all"))->trigger();
        QTRY_COMPARE(w->collection()->count(), 3);
        QVERIFY(!w->statusText().contains(QStringLiteral("Filter:")));
    }

    // Covers: F-CULL-05 (bulk move to _Rejected/ and trash, one undo step each)
    void F_CULL_05_bulkRejectsUi() {
        makeFolder();
        QVERIFY(ratingio::writeSidecar(path("a.jpg"), stars(0, PickFlag::Reject)).isEmpty());
        QVERIFY(ratingio::writeSidecar(path("c.jpg"), stars(2, PickFlag::Reject)).isEmpty());
        ViewerWindow* w = openAndWait(path("b.jpg"));
        QVERIFY(w);
        command(QStringLiteral("culling:moveRejects"))->trigger();
        auto* box = waitForTopLevel<QMessageBox>(QStringLiteral("lfBulkRejectsDialog"));
        if (!box) // window-modal sheets are children of the window
            QTRY_VERIFY((box = w->findChild<QMessageBox*>(QStringLiteral("lfBulkRejectsDialog"))));
        auto* go = box->findChild<QPushButton*>(QStringLiteral("lfBulkRejectsConfirm"));
        QVERIFY(go);
        go->click();
        QTRY_VERIFY(QFileInfo::exists(path("_Rejected/a.jpg")));
        QTRY_VERIFY(QFileInfo::exists(path("_Rejected/c.jpg")));
        QVERIFY(QFileInfo::exists(path("_Rejected/a.xmp"))); // sidecars travel along
        QTRY_COMPARE(w->collection()->count(), 1);
        QCOMPARE(ops->undoStack()->count(), 1);
        w->perform(Action::Undo);
        QTRY_VERIFY(QFileInfo::exists(path("a.jpg")));
        QTRY_VERIFY(!QFileInfo::exists(path("_Rejected")));
        QTRY_COMPARE(w->collection()->count(), 3);
        // ... and to the (temporary) trash.
        command(QStringLiteral("culling:trashRejects"))->trigger();
        QTRY_VERIFY((box = w->findChild<QMessageBox*>(QStringLiteral("lfBulkRejectsDialog"))) ||
                    (box = waitForTopLevel<QMessageBox>(QStringLiteral("lfBulkRejectsDialog"), 100)));
        box->findChild<QPushButton*>(QStringLiteral("lfBulkRejectsConfirm"))->click();
        QTRY_VERIFY(!QFileInfo::exists(path("a.jpg")) && !QFileInfo::exists(path("c.jpg")));
        QVERIFY(QFileInfo::exists(path("b.jpg")));
    }

    // Covers: F-FILE-08 (M + digit moves, C + digit copies, M M repeats, undo)
    void F_FILE_08_quickMoveKeys() {
        makeFolder();
        QVERIFY(QDir().mkpath(path("Eins")) && QDir().mkpath(path("Zwei")));
        quickmove::setTarget(1, path("Eins"));
        quickmove::setTarget(2, path("Zwei"));
        QuickChord::setLastSlot(0);
        ViewerWindow* w = openAndWait(path("a.jpg"));
        QVERIFY(w);
        QTest::keyClick(w->view(), Qt::Key_M);
        QVERIFY(w->culling()->quickMove()->isPending());
        QTest::keyClick(w->view(), Qt::Key_1);
        QCOMPARE(QuickChord::lastSlot(), 0); // not before the move ran
        QTRY_VERIFY(QFileInfo::exists(path("Eins/a.jpg")));
        QVERIFY(!QFileInfo::exists(path("a.jpg")));
        QTRY_COMPARE(QuickChord::lastSlot(), 1); // remembered once the move succeeded
        QTRY_COMPARE(w->currentPath(), path("b.jpg"));
        QCOMPARE(w->view()->zoomMode(), ZoomMode::Fit); // "1" was the target, not the zoom key
        // M M: the same target again.
        QTest::keyClick(w->view(), Qt::Key_M);
        QTest::keyClick(w->view(), Qt::Key_M);
        QTRY_VERIFY(QFileInfo::exists(path("Eins/b.jpg")));
        // C 2 copies.
        QTRY_COMPARE(w->currentPath(), path("c.jpg"));
        QTest::keyClick(w->view(), Qt::Key_C);
        QTest::keyClick(w->view(), Qt::Key_2);
        QTRY_VERIFY(QFileInfo::exists(path("Zwei/c.jpg")));
        QVERIFY(QFileInfo::exists(path("c.jpg")));
        // Undo all three.
        QTRY_VERIFY(!ops->isBusy());
        for (int i = 0; i < 3; ++i)
            w->perform(Action::Undo);
        QTRY_VERIFY(QFileInfo::exists(path("a.jpg")) && QFileInfo::exists(path("b.jpg")));
        QTRY_VERIFY(!QFileInfo::exists(path("Zwei/c.jpg")));
        // A target that is not set explains itself and is not remembered.
        QTest::keyClick(w->view(), Qt::Key_M);
        QTest::keyClick(w->view(), Qt::Key_7);
        QVERIFY(w->toastText().contains(QStringLiteral("nicht festgelegt")));
        QCOMPARE(QuickChord::lastSlot(), 2);
    }

    // Covers: F-FILE-11 (conflict dialog: keep both, replace, skip)
    void F_FILE_11_conflictDialogUi() {
        makeFolder();
        QVERIFY(QDir().mkpath(path("Ziel")));
        QVERIFY(!makeImage(path("Ziel/a.jpg"), Qt::white).isEmpty());
        quickmove::setTarget(1, path("Ziel"));
        ViewerWindow* w = openAndWait(path("a.jpg"));
        QVERIFY(w);
        QTest::keyClick(w->view(), Qt::Key_M);
        QTest::keyClick(w->view(), Qt::Key_1);
        QMessageBox* box = nullptr;
        QTRY_VERIFY((box = w->findChild<QMessageBox*>(QStringLiteral("lfConflictDialog"))) ||
                    (box = waitForTopLevel<QMessageBox>(QStringLiteral("lfConflictDialog"), 100)));
        QVERIFY(box->findChild<QPushButton*>(QStringLiteral("lfConflictReplace")));
        QVERIFY(box->findChild<QPushButton*>(QStringLiteral("lfConflictSkip")));
        box->findChild<QPushButton*>(QStringLiteral("lfConflictKeepBoth"))->click();
        QTRY_VERIFY(QFileInfo::exists(path("Ziel/a 2.jpg")));
        QVERIFY(QFileInfo::exists(path("Ziel/a.jpg")));
        QVERIFY(!QFileInfo::exists(path("a.jpg")));
    }

    // Covers: F-CULL-02 (preferences section enabled), F-FILE-08 (target list)
    void F_CULL_02_preferencesRatingSection() {
        app->showPreferences();
        PreferencesDialog* prefs = PreferencesDialog::existingInstance();
        QVERIFY(prefs);
        prefs->showSection(PreferencesDialog::Rating);
        for (const char* k : {key::RatingStoreSidecar, key::RatingStoreEmbedded, key::RatingStoreFinderTags,
                              key::CullAutoAdvance, key::CullLockZoom}) {
            auto* cb = prefs->findChild<QCheckBox*>(QStringLiteral("setting:") + QLatin1String(k));
            QVERIFY2(cb && cb->isEnabled(), k);
        }
        auto* embedded = prefs->findChild<QCheckBox*>(QStringLiteral("setting:") + key::RatingStoreEmbedded);
        embedded->setChecked(true);
        QTRY_VERIFY(RatingStore::instance().options().embedded);
        quickmove::setTarget(3, path(""));
        auto* row = prefs->findChild<QLineEdit*>(QStringLiteral("quickTarget:3"));
        QVERIFY(row);
        QCOMPARE(row->text(), QDir::toNativeSeparators(path("")));
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
    TstCulling tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_culling.moc"
