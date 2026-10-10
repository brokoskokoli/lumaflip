// SPDX-License-Identifier: GPL-3.0-or-later
// lumaflip-decoder: the decoder helper process (SPEC 8.5, M3). Started by the app
// (core/DecoderHelper.h) with the protocol on stdin/stdout (core/DecoderIpc.h); runs the
// library decoders that are not part of the system (Qt image plugins, QtSvg) so that a
// malformed file can only take down this process. Not meant to be started by hand.

#include "core/DecoderHelper.h"
#include "core/QtImageDecoder.h"

#include <QGuiApplication>
#include <QImageReader>
#include <QLoggingCategory>

#include <cstdio>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

int main(int argc, char** argv) {
#ifdef Q_OS_UNIX
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

    QString sandboxError;
    const bool sandboxed = lf::applyDecoderSandbox(&sandboxError);
    if (!sandboxed)
        qWarning("lumaflip-decoder: running without sandbox: %s", qPrintable(sandboxError));

    const QList<std::shared_ptr<lf::Decoder>> backends = {std::make_shared<lf::QtImageDecoder>(),
                                                          std::make_shared<lf::SvgDecoder>()};
    const bool testOps = qEnvironmentVariableIntValue("LF_DECODER_HELPER_TEST_OPS") == 1;
    return lf::runDecoderHelper(STDIN_FILENO, protocolOut, backends, sandboxed, testOps);
}
