// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzz target: the app side of the decoder helper protocol (SPEC 8.5). A compromised
// helper controls every byte the app reads: the frame parser, the Response decoding and
// the pixel header check must hold. Invariants: an accepted header describes a buffer
// whose size does not overflow; an accepted response survives encode/decode unchanged.

#include "FuzzTarget.h"
#include "core/DecoderIpc.h"

#include <QColorSpace>
#include <QImage>

using namespace lf;

QList<QByteArray> lf::fuzz::seeds() {
    ipc::Response r;
    r.id = 7;
    r.ok = true;
    r.sandboxed = true;
    r.pid = 1234;
    r.info.pixelSize = QSize(640, 480);
    r.info.formatName = QStringLiteral("PNG");
    r.decoded.fullSize = QSize(640, 480);
    r.pixels.width = 640;
    r.pixels.height = 480;
    r.pixels.format = int(QImage::Format_RGBA8888_Premultiplied);
    r.pixels.bytesPerLine = 640 * 4;
    r.pixels.shmSize = r.pixels.bytesPerLine * 480;
    r.pixels.iccProfile = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    addMetadata(r.metadata, QStringLiteral("camera.model"), QStringLiteral("Test"));
    r.metadata.latitude = 48.1;
    auto frame = [](const QByteArray& payload) {
        QByteArray f(4, '\0');
        const quint32 n = quint32(payload.size());
        for (int i = 0; i < 4; ++i)
            f[i] = char((n >> (8 * i)) & 255);
        return f + payload;
    };
    ipc::Response error;
    error.id = 8;
    error.error = QStringLiteral("Fehler");
    ipc::Request q;
    q.id = 9;
    q.op = ipc::Op::Deliver;
    q.shm = "/lfd.1.9";
    return {frame(ipc::encode(r)), frame(ipc::encode(error)), frame(ipc::encode(q))};
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > (8 << 20))
        return 0;
    QByteArray buffer = fuzz::bytes(data, size);
    // Frames back to back, as readFrame() would see them on the pipe.
    for (int frames = 0; frames < 8 && !buffer.isEmpty(); ++frames) {
        QByteArray payload;
        const qsizetype used = ipc::parseFrame(buffer, &payload);
        if (used <= 0)
            break;
        buffer.remove(0, used);
        if (auto r = ipc::decodeResponse(payload)) {
            QString error;
            if (ipc::validatePixelHeader(r->pixels, &error)) {
                const ipc::PixelHeader& h = r->pixels;
                const int bpp = QImage::toPixelFormat(QImage::Format(h.format)).bitsPerPixel();
                if (h.width <= 0 || h.height <= 0 || h.shmSize <= 0 || bpp < 8 ||
                    h.bytesPerLine < qint64(h.width) * (bpp / 8) || h.shmSize / h.height != h.bytesPerLine)
                    fuzz::fail("accepted an inconsistent pixel header");
            }
            const auto again = ipc::decodeResponse(ipc::encode(*r));
            if (!again || again->id != r->id || again->ok != r->ok ||
                again->pixels.shmSize != r->pixels.shmSize || again->error != r->error)
                fuzz::fail("response does not survive encode/decode");
        }
        if (auto q = ipc::decodeRequest(payload))
            (void)ipc::encode(*q);
    }
    return 0;
}
