// SPDX-License-Identifier: GPL-3.0-or-later
// lumaflip-decoder: the decoder helper process (SPEC 8.5, M3). Started by the app
// (core/DecoderHelper.h) with the protocol on stdin/stdout (core/DecoderIpc.h); runs the
// library decoders that are not part of the system (Qt image plugins, QtSvg, the own
// "Rest" format parsers of core/SimpleDecoders.h) so that a malformed file can only take
// down this process. Not meant to be started by hand.

#include "core/DecoderHelper.h"
#include "core/PdfDocument.h"
#include "core/PsbDecoder.h"
#include "core/QtImageDecoder.h"
#include "core/ResvgDecoder.h"
#include "core/SimpleDecoders.h"
#include "core/XcfDecoder.h"

#include <QGuiApplication>
#include <QImageReader>
#include <QLoggingCategory>

#include <csignal>
#include <cstdio>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

int main(int argc, char** argv) {
#ifdef Q_OS_UNIX
    // The app may vanish at any time: a write to its pipe must fail, not kill the helper.
    std::signal(SIGPIPE, SIG_IGN);
    // The protocol owns the original stdout; anything a library prints goes to stderr.
    const int protocolOut = dup(STDOUT_FILENO);
    dup2(STDERR_FILENO, STDOUT_FILENO);
#else
    const int protocolOut = 1;
#endif
    if (argc < 2 || qstrcmp(argv[1], "--decoder-helper") != 0) {
        std::fprintf(stderr,
                     "lumaflip-decoder is started by Lumaflip and is not meant to be run directly.\n");
        return 64;
    }
    // Fonts and image plugins without a window server connection (no Dock icon, no menu).
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    // Load the plugins before the sandbox closes in (it allows reading, but keep the
    // start-up independent of the profile).
    (void)QImageReader::supportedImageFormats();

    const bool testOps = qEnvironmentVariableIntValue("LF_DECODER_HELPER_TEST_OPS") == 1;
    QString sandboxError = QStringLiteral("disabled for a test");
    // The app refuses a helper without sandbox (DecoderHelperPool, Ping); tests check that.
    const bool sandboxed = testOps && qEnvironmentVariableIntValue("LF_DECODER_HELPER_TEST_NO_SANDBOX") == 1
                               ? false
                               : lf::applyDecoderSandbox(&sandboxError);
    if (!sandboxed)
        qWarning("lumaflip-decoder: running without sandbox: %s", qPrintable(sandboxError));

    QList<std::shared_ptr<lf::Decoder>> backends = {std::make_shared<lf::QtImageDecoder>(),
                                                    std::make_shared<lf::SvgDecoder>()};
    backends << lf::simpleDecoders(); // M3 "Rest" formats (QOI, PCX, PNM/PFM, SGI, ORA, KRA)
    backends << std::make_shared<lf::XcfDecoder>() << std::make_shared<lf::PsbDecoder>(); // M3: XCF, PSB
    backends << std::make_shared<lf::pdf::PdfPageDecoder>(); // M4: PDF pages (F-OPEN-10)
    if (auto resvg = lf::createResvgDecoder()) // M3: SVG 1.1 via resvg (fonts load on first text)
        backends << resvg;
    return lf::runDecoderHelper(STDIN_FILENO, protocolOut, backends, sandboxed, testOps);
}
