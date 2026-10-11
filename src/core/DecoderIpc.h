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
// kProtocolVersion. Pixels do not go through the pipe but through POSIX shared memory that
// the APP owns (the helper's sandbox cannot create shared memory objects):
//   1. Decode: the helper decodes, keeps the image and answers with its PixelHeader.
//   2. The app checks the header (validatePixelHeader), creates the object
//      shmName(appPid, requestId) with exactly header.shmSize bytes (createShared, keeps the
//      descriptor) and sends Deliver with that name.
//   3. The helper opens the existing object, unlinks the name at once, copies the pixels
//      (fillSharedImage) and answers.
//   4. The app unlinks the name (if still there) and maps its own descriptor
//      (mapSharedImage: zero copy, the QImage releases the mapping).
// A name therefore exists only between steps 2 and 3, and the app removes it on every
// failure path; neither a helper killed mid-decode nor an app that quits or crashes during
// a decode leaves a segment behind.
//
// Linux: no named object at all (the helper's seccomp filter forbids opening anything for
// writing, LinuxSandbox.h). Steps 2-4 become:
//   2. The app sends Deliver without a name.
//   3. The helper copies the pixels into a memfd of its own, seals it (no writes, no
//      resizing: F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) and answers with
//      its descriptor number (Response::pixelFd); it closes it with the next request.
//   4. The app opens /proc/<helper pid>/fd/<n> read-only (its own child), checks the seals
//      and the size and maps it privately (mapHelperImage).
//
// All functions are thread-safe (no shared state).

#include "Archive.h"
#include "Decoder.h"

#include <QByteArray>
#include <QImage>
#include <QString>

#include <optional>

namespace lf::ipc {

// 3: archive operations, 4: ArchiveReadRun (F-OPEN-10), 5: loop count, 6: pixelFd, sandboxDetails
inline constexpr quint32 kProtocolVersion = 6;
// Upper bound of one frame (metadata, ICC profiles, error texts - never pixels).
inline constexpr quint32 kMaxFrameBytes = 64u * 1024 * 1024;

enum class Op : quint8 {
    Ping = 1,     // liveness + sandbox state
    Info = 2,     // Decoder::info
    Decode = 3,   // Decoder::decode (pixels via shared memory)
    Metadata = 4, // Decoder::metadata
    Deliver = 5,  // copy the image of the preceding Decode (same id) into Request::shm
    // F-OPEN-10 (M4): archives are parsed in the helper too (archive::listLocal/readLocal).
    ArchiveList = 6, // members of the archive/PDF `path` -> Response::members
    ArchiveRead = 7, // bytes of member `member` of `path` -> Response::data (<= kMaxMemberBytes)
    // archive::readRunLocal(): the extracted members in order -> Response::members (name,
    // size = their byte count) and Response::data (their bytes one after the other)
    ArchiveReadRun = 8,
    // Only honoured when the helper runs with LF_DECODER_HELPER_TEST_OPS=1 (tests). Each
    // answers ok = the operation succeeded (all of them must fail in the sandbox):
    TestCrash = 100,      // abort()
    TestHang = 101,       // sleep forever
    TestWriteFile = 102,  // create the file `path`
    TestSocket = 103,     // open a TCP socket and connect to 127.0.0.1:`path`
    TestMachLookup = 104, // look up the Mach service named `path`
    TestCreateShm = 105,  // create the shared memory object named `path`
    TestExec = 106,       // start the program `path`
    TestSignal = 107,     // send signal 0 to the process `path` (a pid): kill() and tgkill()
    TestPidfd = 108,      // pidfd_open(`path`), pidfd_getfd, pidfd_send_signal (Linux)
    TestModifyFile = 109, // rename, unlink, chmod, truncate, link, symlink, mkdir at `path`
    TestTerminal = 110,   // ioctl(TIOCSTI) on stdin (Linux; ok = the filter let it through)
    TestAllocate = 111,   // allocate and touch `path` MB of memory
    // Not denied, a report (ok = true): Response::error lists the open descriptors
    // ("<fd> -> <target>" per line; Linux) or the helper's desktop (Windows).
    TestListDescriptors = 120,
    TestDesktop = 121,
};

struct Request {
    quint32 id = 0;
    Op op = Op::Ping;
    QString backend; // Decoder::name() of the helper-side backend ("Qt", "QtSvg")
    QString path;
    DecodeRequest decode;
    QByteArray shm; // Deliver: name of the shared memory object created by the app
    QString member; // ArchiveRead, ArchiveReadRun
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
    QString sandboxDetails; // e.g. "seccomp on, Landlock ABI 4" (logs, tests)
    qint64 pid = 0;
    // Info
    ImageInfo info;
    // Decode (DecodedImage without the QImage)
    DecodedImage decoded;
    PixelHeader pixels;
    // Deliver (Linux): the helper's descriptor number of the sealed memfd with the pixels
    qint32 pixelFd = -1;
    // Metadata
    Metadata metadata;
    // ArchiveList / ArchiveRead / ArchiveReadRun (F-OPEN-10); at most archive::kMaxMembers members and
    // archive::kMaxMemberBytes bytes (decodeResponse() rejects more)
    QVector<archive::Member> members;
    QByteArray data;
    // Decode, app side only (not serialized): the pixels, mapped by DecoderHelperPool::call()
    // while it still holds the helper (its next request would remove the shared memory).
    QImage image;
};

QByteArray encode(const Request& r);
QByteArray encode(const Response& r);
std::optional<Request> decodeRequest(const QByteArray& payload);
std::optional<Response> decodeResponse(const QByteArray& payload);

// Shared memory object name for request `requestId` of the app process `pid` (at most 31
// characters, the macOS limit). The helper's sandbox only grants access to this prefix.
inline constexpr char kShmPrefix[] = "/lfd.";
QByteArray shmName(qint64 pid, quint32 requestId);

// ---- framing over file descriptors ----

enum class IoStatus { Ok, Closed, Timeout, Error, TooLarge };

// Writes one frame (blocking; retries on EINTR, SIGPIPE must be ignored by the caller).
IoStatus writeFrame(int fd, const QByteArray& payload);
// Reads one frame. `timeoutMs` < 0 waits forever, else it bounds the whole frame (an
// absolute deadline: a peer trickling single bytes cannot extend it). `Closed` = EOF before
// a full frame.
IoStatus readFrame(int fd, QByteArray* payload, int timeoutMs);
// Parses a frame from a byte buffer (the length prefix and the payload): the app-side frame
// parser of readFrame() without a file descriptor (fuzz target). Returns the bytes used
// (0 = incomplete, -1 = invalid length).
qsizetype parseFrame(const QByteArray& buffer, QByteArray* payload);

// ---- shared memory ----

// Helper side: the header describing `image` (shmSize = bytes the app must provide).
PixelHeader describeImage(const QImage& image);
// Helper side: opens the EXISTING object `name` (created by the app), removes the name and
// copies the image's bytes into it. Fails if the object is smaller than the image.
bool fillSharedImage(const QByteArray& name, const QImage& image, QString* error);
// App side: checks a header from the helper: positive size, a supported 8/16/32-bit
// per channel format (no indexed or mono formats), bytesPerLine large enough, and
// shmSize == bytesPerLine * height without overflow.
bool validatePixelHeader(const PixelHeader& header, QString* error);
// App side: creates the object `name` (exclusive) with `size` bytes. Returns the open
// descriptor or -1 (nothing left behind then).
int createShared(const QByteArray& name, qint64 size, QString* error);
// App side: maps the object behind `fd` (from createShared, closed here in any case) and
// wraps it in a QImage that unmaps when released. Validates the header and the size.
QImage mapSharedImage(int fd, const PixelHeader& header, QString* error);
#ifdef Q_OS_LINUX
// Helper side (Linux): a sealed memfd with the image's bytes (F_SEAL_WRITE, _SHRINK, _GROW,
// _SEAL). Returns the descriptor (the caller closes it) or -1.
int createSealedImage(const QImage& image, QString* error);
// App side (Linux): opens descriptor `fd` of the process `pid` (/proc/<pid>/fd/<fd>, read
// only), requires a sealed memfd at least header.shmSize bytes large and maps it privately
// (copy on write) into a QImage. Validates the header like mapSharedImage().
QImage mapHelperImage(qint64 pid, int fd, const PixelHeader& header, QString* error);
#endif
// Removes the name if it exists (cleanup after a failed request). True if it existed.
bool unlinkShared(const QByteArray& name);
bool sharedExists(const QByteArray& name); // tests

#ifdef Q_OS_WIN
// Windows: descriptors are HANDLE values (see DecoderIpc.cpp). Closes a pipe end or a shared
// memory object from createShared().
void closeHandle(int fd);
// A one-way pipe (a named pipe pair, never reachable remotely). Overlapped ends support the
// timeouts of readFrame(); inheritable ends can be handed to a child process.
bool createPipe(int* readEnd, int* writeEnd, bool overlappedRead, bool overlappedWrite, bool inheritRead,
                bool inheritWrite);
#endif

} // namespace lf::ipc
