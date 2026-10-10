// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Protocol between the app and the decoder helper process (SPEC 8.5, M3).
//
// Library decoders that are not part of the system (Qt image plugins, QtSvg, later
// KImageFormats/LibRaw ...) run in `lumaflip-decoder`, a child process started by
// DecoderHelper.h. A malformed file can crash or hang that process, never the app.
//
// Transport: the child's stdin/stdout pipes. Every message is a frame
//     [u32 payload length, little endian][payload]
// and the payload a QDataStream (Qt 6.8 format) of Request or Response, starting with
// kProtocolVersion. Pixels do not go through the pipe: the helper writes them into a POSIX
// shared memory object named shmName(helperPid, requestId); the app maps it (zero copy, the
// QImage releases the mapping) and unlinks the name. Because the app can compute the name,
// it also removes it when the helper died or timed out halfway.
//
// All functions are thread-safe (no shared state).

#include "Decoder.h"

#include <QByteArray>
#include <QImage>
#include <QString>

#include <optional>

namespace lf::ipc {

inline constexpr quint32 kProtocolVersion = 1;
// Upper bound of one frame (metadata, ICC profiles, error texts - never pixels).
inline constexpr quint32 kMaxFrameBytes = 64u * 1024 * 1024;

enum class Op : quint8 {
    Ping = 1,     // liveness + sandbox state
    Info = 2,     // Decoder::info
    Decode = 3,   // Decoder::decode (pixels via shared memory)
    Metadata = 4, // Decoder::metadata
    // Only honoured when the helper runs with LF_DECODER_HELPER_TEST_OPS=1 (tests):
    TestCrash = 100,     // abort()
    TestHang = 101,      // sleep forever
    TestWriteFile = 102, // try to create `path` (must fail in the sandbox)
};

struct Request {
    quint32 id = 0;
    Op op = Op::Ping;
    QString backend; // Decoder::name() of the helper-side backend ("Qt", "QtSvg")
    QString path;
    DecodeRequest decode;
};

// Pixel buffer description of a decoded image (the bytes are in shared memory).
struct PixelHeader {
    int width = 0;
    int height = 0;
    int format = 0; // QImage::Format
    qint64 bytesPerLine = 0;
    QByteArray iccProfile; // QColorSpace::iccProfile(), empty = untagged
    qint64 shmSize = 0;    // bytes in the shared memory object (height * bytesPerLine)
};

struct Response {
    quint32 id = 0;
    bool ok = false;
    QString error; // German, user facing (backend text), when !ok
    // Ping
    bool sandboxed = false;
    qint64 pid = 0;
    // Info
    ImageInfo info;
    // Decode (DecodedImage without the QImage)
    DecodedImage decoded;
    PixelHeader pixels;
    // Metadata
    Metadata metadata;
    // Decode, app side only (not serialized): the pixels, mapped by DecoderHelperPool::call()
    // while it still holds the helper (its next request would remove the shared memory).
    QImage image;
};

QByteArray encode(const Request& r);
QByteArray encode(const Response& r);
std::optional<Request> decodeRequest(const QByteArray& payload);
std::optional<Response> decodeResponse(const QByteArray& payload);

// Shared memory object name for a request of helper `pid` (at most 31 characters, the
// macOS limit).
QByteArray shmName(qint64 pid, quint32 requestId);

// ---- framing over file descriptors ----

enum class IoStatus { Ok, Closed, Timeout, Error, TooLarge };

// Writes one frame (blocking; retries on EINTR, SIGPIPE must be ignored by the caller).
IoStatus writeFrame(int fd, const QByteArray& payload);
// Reads one frame. `timeoutMs` < 0 waits forever. `Closed` = EOF before a full frame.
IoStatus readFrame(int fd, QByteArray* payload, int timeoutMs);

// ---- shared memory ----

// Helper side: creates the object `name` (exclusive), copies the image's bytes into it.
// Returns false (and an error text) on failure; nothing is left behind then.
bool writeSharedImage(const QByteArray& name, const QImage& image, PixelHeader* header, QString* error);
// App side: maps the object and wraps it in a QImage that unmaps when released. The name
// is unlinked in any case. Validates the header against the object's size.
QImage readSharedImage(const QByteArray& name, const PixelHeader& header, QString* error);
// Removes the name if it exists (cleanup after a failed request). True if it existed.
bool unlinkShared(const QByteArray& name);
bool sharedExists(const QByteArray& name); // tests

} // namespace lf::ipc
