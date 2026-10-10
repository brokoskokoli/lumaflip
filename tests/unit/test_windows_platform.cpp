// SPDX-License-Identifier: GPL-3.0-or-later
// Windows platform layer (src/platform/windows): name order like Explorer, Recycle Bin,
// folder watching, command line quoting against CommandLineToArgvW, the WIC decoder, the
// decoder helper's sandbox. Built only on Windows (tests/CMakeLists.txt).

#include "QtTestApp.h"
#include "core/CommandLine.h"
#include "core/CustomActions.h"
#include "core/DecoderHelper.h"
#include "core/Platform.h"
#include "core/QtImageDecoder.h"
#include "platform/windows/WicDecoder.h"
#include "platform/windows/WinDecoderSandbox.h"
#include "platform/windows/WinFileUtil.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>

#include <windows.h>

#include <shellapi.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

using namespace lf;
using Microsoft::WRL::ComPtr;

namespace {

bool writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

QStringList commandLineToArgv(const QString& line) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(reinterpret_cast<LPCWSTR>(line.utf16()), &argc);
    QStringList out;
    for (int i = 0; argv && i < argc; ++i)
        out << QString::fromWCharArray(argv[i]);
    if (argv)
        LocalFree(argv);
    return out;
}

QImage testPattern(int w, int h) {
    QImage img(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            img.setPixelColor(x, y,
                              QColor((x * 255) / std::max(1, w - 1), (y * 255) / std::max(1, h - 1), 80));
    // A red corner marks the top left (orientation checks).
    for (int y = 0; y < h / 4; ++y)
        for (int x = 0; x < w / 4; ++x)
            img.setPixelColor(x, y, Qt::red);
    return img;
}

// A JPEG written by WIC with the EXIF orientation `orientation`.
bool writeWicJpeg(const QString& path, const QImage& source, int orientation) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))))
        return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    const QString native = QDir::toNativeSeparators(path);
    if (FAILED(f->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_WRITE)) ||
        FAILED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, &props)) || FAILED(frame->Initialize(props.Get())))
        return false;
    const QImage bgr = source.convertToFormat(QImage::Format_RGB888).rgbSwapped();
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetSize(UINT(bgr.width()), UINT(bgr.height()))) || FAILED(frame->SetPixelFormat(&pf)))
        return false;
    ComPtr<IWICMetadataQueryWriter> meta;
    if (SUCCEEDED(frame->GetMetadataQueryWriter(&meta))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        v.vt = VT_UI2;
        v.uiVal = USHORT(orientation);
        meta->SetMetadataByName(L"/app1/ifd/{ushort=274}", &v);
    }
    return SUCCEEDED(frame->WritePixels(UINT(bgr.height()), UINT(bgr.bytesPerLine()), UINT(bgr.sizeInBytes()),
                                        const_cast<BYTE*>(bgr.constBits()))) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

struct ComInit {
    ComInit() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComInit() { CoUninitialize(); }
};

} // namespace

// ---- F-SORT-01: Explorer name order ---------------------------------------------------------

TEST(WindowsPlatform, F_SORT_01_NameOrderLikeExplorer) {
    auto c = createNameCollator();
    // Digits by value, case-insensitive, like Explorer (StrCmpLogicalW).
    EXPECT_LT(c->compare(QStringLiteral("img2.jpg"), QStringLiteral("img10.jpg")), 0);
    EXPECT_LT(c->compare(QStringLiteral("a.jpg"), QStringLiteral("B.jpg")), 0);
    EXPECT_LT(c->compare(QStringLiteral("Bild 9.jpg"), QStringLiteral("bild 10.jpg")), 0);
    EXPECT_GT(c->compare(QStringLiteral("z.jpg"), QStringLiteral("Ä.jpg")), 0);
    // Forced ordering: only identical names are equal.
    EXPECT_NE(c->compare(QStringLiteral("a.jpg"), QStringLiteral("A.jpg")), 0);
    EXPECT_EQ(c->compare(QStringLiteral("a.jpg"), QStringLiteral("a.jpg")), 0);
    // The same order as StrCmpLogicalW for a mixed list, and prepared == compare.
    QVector<QString> names = {
        QStringLiteral("x10.png"), QStringLiteral("x9.png"),   QStringLiteral("X1.png"),
        QStringLiteral("x 2.png"), QStringLiteral("_a.png"),   QStringLiteral("10.png"),
        QStringLiteral("9.png"),   QStringLiteral("Über.png"), QStringLiteral("ueber.png")};
    auto prepared = c->prepare(names);
    for (qsizetype i = 0; i < names.size(); ++i)
        for (qsizetype j = 0; j < names.size(); ++j) {
            SCOPED_TRACE((names[i] + u" / " + names[j]).toStdString());
            const int a = c->compare(names[i], names[j]);
            const int b = prepared->compare(i, j);
            EXPECT_EQ(a < 0, b < 0);
            EXPECT_EQ(a > 0, b > 0);
            const int logical = StrCmpLogicalW(reinterpret_cast<LPCWSTR>(names[i].utf16()),
                                               reinterpret_cast<LPCWSTR>(names[j].utf16()));
            if (logical != 0)
                EXPECT_EQ(a < 0, logical < 0) << "differs from Explorer (StrCmpLogicalW)";
        }
}

// ---- F-FILE-01: Recycle Bin -----------------------------------------------------------------

TEST(WindowsPlatform, F_FILE_01_RecycleBinMoveAndRestore) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Papierkorb Test ä.jpg"));
    ASSERT_TRUE(writeFile(path, QByteArray("not really a jpeg, but a file")));
    auto trash = createTrashService();
    QString error;
    const auto inTrash = trash->moveToTrash(path, &error);
    ASSERT_TRUE(inTrash.has_value()) << error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(path));
    if (inTrash->isEmpty())
        GTEST_SKIP() << "Windows did not report the recycled item (no Recycle Bin on this volume?)";
    EXPECT_TRUE(QFileInfo::exists(*inTrash)) << inTrash->toStdString();
    EXPECT_TRUE(inTrash->contains(QStringLiteral("$Recycle.Bin"), Qt::CaseInsensitive))
        << inTrash->toStdString();
    const QString record =
        QFileInfo(*inTrash).dir().filePath(QStringLiteral("$I") + QFileInfo(*inTrash).fileName().mid(2));
    EXPECT_TRUE(QFileInfo::exists(record)) << "the Recycle Bin's record of the item";

    // Restoring never overwrites.
    ASSERT_TRUE(writeFile(path, QByteArray("new file in the way")));
    EXPECT_FALSE(trash->restoreFromTrash(*inTrash, path, &error));
    EXPECT_FALSE(error.isEmpty());
    ASSERT_TRUE(QFile::remove(path));

    ASSERT_TRUE(trash->restoreFromTrash(*inTrash, path, &error)) << error.toStdString();
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), QByteArray("not really a jpeg, but a file"));
    EXPECT_FALSE(QFileInfo::exists(*inTrash));
    EXPECT_FALSE(QFileInfo::exists(record)) << "no ghost entry left in the Recycle Bin";
}

// ---- F-OPEN-07/08: folder watching ------------------------------------------------------------

TEST(WindowsPlatform, F_OPEN_07_WatcherReportsChangesAndSidecars) {
    test::ensureQtApp();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QDir().mkpath(dir.filePath(QStringLiteral("sub/deeper")));
    auto watcher = createFolderWatcher();
    QStringList changed, sidecars;
    QObject::connect(watcher.get(), &FolderWatcher::folderChanged,
                     [&](const QString& f) { changed << QDir::cleanPath(f); });
    QObject::connect(watcher.get(), &FolderWatcher::sidecarChanged,
                     [&](const QString& f) { sidecars << QDir::cleanPath(f); });
    auto waitFor = [](const std::function<bool()>& done, int ms) {
        QElapsedTimer t;
        t.start();
        while (!done() && t.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(10);
        }
        return done();
    };
    const QString root = QDir::cleanPath(dir.path());
    watcher->watch(dir.path());
    QThread::msleep(100);
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("neu.jpg")), "x"));
    EXPECT_TRUE(waitFor([&] { return changed.contains(root); }, 5000)) << changed.join(u',').toStdString();
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("neu.xmp")), "<x/>"));
    EXPECT_TRUE(waitFor([&] { return !sidecars.isEmpty(); }, 5000));
    // Hidden temporary files do not count.
    waitFor([] { return false; }, 300);
    changed.clear();
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral(".tmp-lumaflip")), "x"));
    waitFor([] { return false; }, 500);
    EXPECT_TRUE(changed.isEmpty()) << changed.join(u',').toStdString();
    watcher->unwatch(dir.path());

    // Tree (F-OPEN-08): a change two levels down names that folder.
    watcher->watchTree(dir.path());
    QThread::msleep(100);
    changed.clear();
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("sub/deeper/tief.png")), "x"));
    const QString deeper = QDir::cleanPath(dir.filePath(QStringLiteral("sub/deeper")));
    EXPECT_TRUE(waitFor([&] { return changed.contains(deeper); }, 5000)) << changed.join(u',').toStdString();
    watcher->unwatchTree(dir.path());
}

// ---- F-FILE-10: quoting against the real CommandLineToArgvW ----------------------------------

TEST(WindowsPlatform, F_FILE_10_QuotingMatchesCommandLineToArgvW) {
    const QStringList args = {QStringLiteral("plain"),
                              QStringLiteral("with space"),
                              QString(),
                              QStringLiteral("tab\there"),
                              QStringLiteral("quote\"inside"),
                              QStringLiteral("\"quoted\""),
                              QStringLiteral("trailing\\"),
                              QStringLiteral("trailing space\\"),
                              QStringLiteral("back\\\"quote"),
                              QStringLiteral("C:\\Bilder\\Ordner mit Leerzeichen\\a b.jpg"),
                              QStringLiteral("a&b|c<d>e^f%PATH%!x!"),
                              QStringLiteral("Grüße ✓ 日本")};
    const QString line = cmdline::build(QStringLiteral("C:\\Program Files\\Tool\\tool.exe"), args);
    const QStringList argv = commandLineToArgv(line);
    ASSERT_EQ(argv.size(), args.size() + 1) << line.toStdString();
    EXPECT_EQ(argv.first(), QStringLiteral("C:\\Program Files\\Tool\\tool.exe"));
    EXPECT_EQ(argv.mid(1), args);
    // Our own splitter agrees with Windows.
    EXPECT_EQ(cmdline::splitArguments(line.mid(line.indexOf(QLatin1String(".exe\"")) + 5)), args);
}

TEST(WindowsPlatform, F_FILE_10_CustomActionArgumentsStayData) {
    CustomAction a;
    a.name = QStringLiteral("Test");
    a.command = QStringLiteral("\"C:\\Program Files\\Tool\\tool.exe\" --open");
    EXPECT_TRUE(customactions::validate(a).isEmpty());
    const QStringList files = {QStringLiteral("C:/Bilder/a&calc.jpg"),
                               QStringLiteral("C:/Bilder/x \" y.jpg")};
    const auto spec = customactions::processFor(a, files);
    EXPECT_EQ(spec.program, QStringLiteral("C:\\Program Files\\Tool\\tool.exe"));
    EXPECT_EQ(spec.arguments, (QStringList{QStringLiteral("--open"), QStringLiteral("C:\\Bilder\\a&calc.jpg"),
                                           QStringLiteral("C:\\Bilder\\x \" y.jpg")}));
    const QStringList argv = commandLineToArgv(cmdline::build(spec.program, spec.arguments));
    EXPECT_EQ(argv.mid(1), spec.arguments);
    // Batch files would go through cmd.exe: refused.
    a.command = QStringLiteral("C:\\x\\run.cmd");
    EXPECT_FALSE(customactions::validate(a).isEmpty());
    a.kind = CustomAction::Kind::Shortcut;
    a.command = QStringLiteral("Kurzbefehl");
    EXPECT_FALSE(customactions::validate(a).isEmpty()) << "no Shortcuts app on Windows";
}

namespace {

CustomActionRun::Result runAction(const CustomAction& a, const QStringList& files, int timeoutMs) {
    test::ensureQtApp();
    CustomActionRun run(a, files, timeoutMs);
    bool done = false;
    CustomActionRun::Result result;
    QObject::connect(&run, &CustomActionRun::finished, [&](const CustomActionRun::Result& r) {
        result = r;
        done = true;
    });
    run.start();
    EXPECT_TRUE(test::waitUntil([&] { return done; }, timeoutMs + 20000));
    return result;
}

} // namespace

// File names that cmd.exe or PowerShell would parse as commands reach the program as plain
// arguments (end to end through CreateProcess and PowerShell's own argument parsing).
TEST(WindowsPlatform, F_FILE_10_CustomActionRunsWithoutShellParsing) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString base = QDir::cleanPath(dir.path());
    const QStringList names = {QStringLiteral("a b & calc.jpg"), QStringLiteral("x;y %PATH% ^z.jpg"),
                               QStringLiteral("$(New-Item PWNED1).jpg"), QStringLiteral("'q' `w.jpg"),
                               QStringLiteral("Grüße ✓.jpg")};
    QStringList files;
    for (const QString& n : names) {
        files << base + u'/' + n;
        ASSERT_TRUE(writeFile(files.last(), "x")) << n.toStdString();
    }
    const QString script = base + QStringLiteral("/list.ps1");
    ASSERT_TRUE(writeFile(script, "$out = Join-Path (Split-Path -Parent $args[0]) 'out.txt'\r\n"
                                  "$args | Set-Content -LiteralPath $out -Encoding utf8\r\n"));
    CustomAction a;
    a.id = QStringLiteral("w");
    a.name = QStringLiteral("Liste");
    a.command =
        QStringLiteral("powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"%1\"")
            .arg(QDir::toNativeSeparators(script));
    ASSERT_TRUE(customactions::validate(a).isEmpty());
    const CustomActionRun::Result r = runAction(a, files, 60000);
    ASSERT_TRUE(r.ok()) << r.error.toStdString() << r.output.toStdString();
    QFile out(base + QStringLiteral("/out.txt"));
    ASSERT_TRUE(out.open(QIODevice::ReadOnly));
    QString text = QString::fromUtf8(out.readAll());
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    QStringList expected;
    for (const QString& f : files)
        expected << QDir::toNativeSeparators(f);
    EXPECT_EQ(text.split(QRegularExpression(QStringLiteral("\\r?\\n")), Qt::SkipEmptyParts), expected);
    EXPECT_FALSE(QFile::exists(base + QStringLiteral("/PWNED1")));
}

TEST(WindowsPlatform, F_FILE_10_CustomActionTimeoutEndsTheProgram) {
    CustomAction a;
    a.id = QStringLiteral("s");
    a.name = QStringLiteral("Warten");
    a.command = QStringLiteral("powershell.exe -NoProfile -NonInteractive -Command Start-Sleep -Seconds 60");
    QElapsedTimer t;
    t.start();
    const CustomActionRun::Result r = runAction(a, {}, 3000);
    EXPECT_TRUE(r.timedOut);
    EXPECT_FALSE(r.ok());
    EXPECT_LT(t.elapsed(), 20000);
}

// ---- WIC decoder ------------------------------------------------------------------------------

TEST(WindowsPlatform, F_FMT_01_WicDecodesLikeQt) {
    ComInit com;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QImage pattern = testPattern(160, 100);
    WicDecoder wic;
    QtImageDecoder qt;
    int compared = 0;
    for (const char* format : {"png", "bmp", "tiff", "jpg"}) {
        SCOPED_TRACE(format);
        const QString path = dir.filePath(QStringLiteral("muster.") + QString::fromLatin1(format));
        {
            QImageWriter w(path, format); // closes the file when it goes
            if (!w.canWrite())
                continue;
            ASSERT_TRUE(w.write(pattern)) << w.errorString().toStdString();
        }
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        const QByteArray header = f.read(512);
        ASSERT_TRUE(wic.canDecode(path, header));
        QString e1, e2;
        const auto a = wic.decode(path, DecodeRequest{}, &e1);
        const auto b = qt.decode(path, DecodeRequest{}, &e2);
        ASSERT_TRUE(a.has_value()) << e1.toStdString();
        ASSERT_TRUE(b.has_value()) << e2.toStdString();
        EXPECT_EQ(a->image.size(), QSize(160, 100));
        EXPECT_EQ(a->image.format(), QImage::Format_RGBA8888_Premultiplied);
        const QImage x = a->image.convertToFormat(QImage::Format_RGBA8888);
        const QImage y = b->image.convertToFormat(QImage::Format_RGBA8888);
        int worst = 0;
        for (int row = 0; row < x.height(); ++row)
            for (int i = 0; i < x.width() * 4; ++i)
                worst =
                    std::max(worst, std::abs(int(x.constScanLine(row)[i]) - int(y.constScanLine(row)[i])));
        EXPECT_LE(worst, qstrcmp(format, "jpg") == 0 ? 8 : 0);
        const auto info = wic.info(path, &e1);
        ASSERT_TRUE(info.has_value()) << e1.toStdString();
        EXPECT_EQ(info->pixelSize, QSize(160, 100));
        // Decode-time downscaling.
        DecodeRequest reduced; // ("small" is a macro of the Windows headers)
        reduced.maxPixelSize = 40;
        const auto s = wic.decode(path, reduced, &e1);
        ASSERT_TRUE(s.has_value()) << e1.toStdString();
        EXPECT_EQ(s->image.size(), QSize(40, 25));
        EXPECT_EQ(s->fullSize, QSize(160, 100));
        EXPECT_TRUE(s->isPreview);
        ++compared;
    }
    EXPECT_GE(compared, 3);
    // Not an image: no claim, no crash.
    const QString garbage = dir.filePath(QStringLiteral("kaputt.jpg"));
    ASSERT_TRUE(writeFile(garbage, QByteArray(600, 'x')));
    QString error;
    EXPECT_FALSE(wic.decode(garbage, DecodeRequest{}, &error).has_value());
    EXPECT_FALSE(error.isEmpty());
    std::printf("WIC codecs: %s\n", qPrintable(WicDecoder::installedCodecs().join(QStringLiteral("; "))));
}

TEST(WindowsPlatform, F_VIEW_02_WicAppliesExifOrientation) {
    ComInit com;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("gedreht.jpg"));
    ASSERT_TRUE(writeWicJpeg(path, testPattern(120, 80), 6));
    WicDecoder wic;
    QString error;
    const auto info = wic.info(path, &error);
    ASSERT_TRUE(info.has_value()) << error.toStdString();
    EXPECT_EQ(info->exifOrientation, 6);
    EXPECT_EQ(info->pixelSize, QSize(120, 80));
    EXPECT_EQ(info->displaySize, QSize(80, 120));
    const auto img = wic.decode(path, DecodeRequest{}, &error);
    ASSERT_TRUE(img.has_value()) << error.toStdString();
    EXPECT_EQ(img->image.size(), QSize(80, 120));
    // Orientation 6 (rotate 90° clockwise): the red top-left corner is now top right.
    const QColor topRight = img->image.pixelColor(img->image.width() - 3, 2);
    EXPECT_GT(topRight.red(), 200);
    EXPECT_LT(topRight.green(), 60);
    const Metadata md = wic.metadata(path);
    EXPECT_FALSE(md.entries.isEmpty());
}

// ---- SPEC 8.5: the decoder helper's sandbox on Windows ------------------------------------------

#ifdef LF_DECODER_HELPER_BINARY
// Which part of the sandbox a helper start depends on (printed, for diagnosing runners).
TEST(WindowsPlatform, HelperSandboxPartsDiagnostics) {
    for (const char* off :
         {"", "mitigations", "childpolicy", "ui", "token", "job", "token,job,ui,mitigations,childpolicy"}) {
        qputenv("LF_DECODER_HELPER_SANDBOX_OFF", off);
        DecoderHelperPool::Options o;
        o.executable = QStringLiteral(LF_DECODER_HELPER_BINARY);
        o.maxProcesses = 1;
        o.requireSandbox = false;
        o.extraEnvironment = {QStringLiteral("QT_QPA_PLATFORM=offscreen")};
        DecoderHelperPool pool(o);
        ipc::Request ping;
        ping.op = ipc::Op::Ping;
        QString error;
        const auto pong = pool.call(ping, 15000, nullptr, &error);
        std::printf("sandbox off [%s]: %s\n", off,
                    pong ? (pong->sandboxed ? "ping ok, sandboxed" : "ping ok, not sandboxed")
                         : qPrintable(QStringLiteral("failed: ") + error));
    }
    qunsetenv("LF_DECODER_HELPER_SANDBOX_OFF");
}

TEST(WindowsPlatform, F_FMT_02_HelperSandboxOnWindows) {
    DecoderHelperPool::Options o;
    o.executable = QStringLiteral(LF_DECODER_HELPER_BINARY);
    o.maxProcesses = 1;
    o.extraEnvironment = {QStringLiteral("LF_DECODER_HELPER_TEST_OPS=1"),
                          QStringLiteral("QT_QPA_PLATFORM=offscreen")};
    DecoderHelperPool pool(o);
    auto probe = [&](ipc::Op op, const QString& arg) {
        ipc::Request r;
        r.op = op;
        r.path = arg;
        const auto resp = pool.call(r, 10000);
        EXPECT_TRUE(resp.has_value());
        return resp && resp->ok;
    };
    ipc::Request ping;
    ping.op = ipc::Op::Ping;
    QString error;
    const auto pong = pool.call(ping, 10000, nullptr, &error);
    ASSERT_TRUE(pong.has_value()) << error.toStdString();
    EXPECT_TRUE(pong->sandboxed) << "low integrity + job";
    // Low integrity: no writes into the user's (medium integrity) folders.
    QTemporaryDir dir;
    const QString target = dir.filePath(QStringLiteral("written-by-helper.txt"));
    EXPECT_FALSE(probe(ipc::Op::TestWriteFile, target));
    EXPECT_FALSE(QFile::exists(target));
    // Job object: no new processes.
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    EXPECT_FALSE(probe(ipc::Op::TestExec, QString::fromWCharArray(system) + QStringLiteral("\\whoami.exe")));
    // The app's shared memory still works (decode round trip through the low label).
    QImage img(32, 16, QImage::Format_RGBA8888);
    img.fill(QColor(10, 200, 30));
    const QString png = dir.filePath(QStringLiteral("x.png"));
    ASSERT_TRUE(img.save(png));
    RemoteDecoder remote(std::make_shared<QtImageDecoder>(),
                         std::shared_ptr<DecoderHelperPool>(&pool, [](auto*) {}));
    const auto decoded = remote.decode(png, DecodeRequest{}, &error);
    ASSERT_TRUE(decoded.has_value()) << error.toStdString();
    EXPECT_EQ(decoded->image.size(), QSize(32, 16));
    EXPECT_EQ(decoded->image.pixelColor(3, 3).green(), 200);
}
#endif
