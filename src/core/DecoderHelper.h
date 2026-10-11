// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Decoder helper process, app side (SPEC 8.5, M3). See DecoderIpc.h for the protocol.
//
// DecoderHelperPool starts up to `maxProcesses` instances of the helper executable
// (`lumaflip-decoder`, next to the app binary) on demand and hands each request to an idle
// one. A helper that crashes (EOF on its pipe) or exceeds the timeout is killed and
// replaced by a fresh one on the next request; the request fails with
// messageDecoderCrashed() / messageDecoderTimeout(), which the window shows on the error
// card of that file (F-FMT-02). The helpers sandbox themselves (no network, no file
// writes) and exit when the app's end of the pipe closes, also if the app crashed.
//
// RemoteDecoder wraps a portable backend: canDecode() (signature check of the first bytes)
// runs in the app, info()/decode()/metadata() in a helper. ImageIO stays in the app process
// (system decoder, hardened by the OS; it is the fast path for P-01..P-06).
//
// Thread-safe: worker threads call into one pool concurrently.

#include "Decoder.h"
#include "DecoderIpc.h"

#include <QString>
#include <QStringList>

#include <memory>
#include <optional>

namespace lf {

// Default upper bound of a helper's memory in MB: half the physical memory, between 2 and
// 8 GiB (Linux and Windows; 0 = no limit elsewhere).
qint64 defaultHelperMemoryLimitMb();

class DecoderHelperPool {
public:
    struct Options {
        QString executable;
        int maxProcesses = 2;
        int decodeTimeoutMs = 30000;  // a single decode (huge SVG, damaged file loops)
        int infoTimeoutMs = 10000;    // info/metadata/ping
        QStringList extraEnvironment; // "NAME=value" added to the app's environment
        // Memory of one helper (Linux: RLIMIT_DATA, Windows: job memory limit), 0 = none. A
        // decode beyond it fails like a damaged file instead of exhausting the system.
        qint64 memoryLimitMb = defaultHelperMemoryLimitMb();
#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX) || defined(Q_OS_WIN)
        bool requireSandbox = true; // refuse a helper whose Ping says sandboxed = false
#else
        bool requireSandbox = false;
#endif
    };
    enum class Failure { None, StartFailed, Crashed, Timeout, Protocol, NotSandboxed };

    explicit DecoderHelperPool(Options options);
    ~DecoderHelperPool(); // kills the helpers
    DecoderHelperPool(const DecoderHelperPool&) = delete;
    DecoderHelperPool& operator=(const DecoderHelperPool&) = delete;

    // Sends `request` (its id is assigned here) to an idle helper and waits for the answer.
    // `timeoutMs` is one deadline for the whole call (waiting for an idle helper included).
    // A newly started helper first has to answer a Ping with sandboxed = true (else
    // Failure::NotSandboxed). On failure `*failure`/`*error` say why (error texts are user
    // facing). Decode: the pixels arrive in app-owned shared memory (DecoderIpc.h) and are
    // mapped into Response::image; no shared memory name survives the call.
    std::optional<ipc::Response> call(ipc::Request request, int timeoutMs, Failure* failure = nullptr,
                                      QString* error = nullptr, qint64* helperPid = nullptr);

    const Options& options() const { return m_options; }
    int startedProcesses() const; // helpers started so far (restarts included)
    int runningProcesses() const;
    void shutdown(); // kills all helpers (they are started again on demand)

    // LF_DECODER_HELPER, else `lumaflip-decoder` next to the application binary; empty if
    // there is none (unbundled test binaries) or LF_DECODER_IN_PROCESS=1.
    static QString defaultExecutable();
    // Process-wide pool for defaultExecutable(); nullptr if there is no helper.
    static std::shared_ptr<DecoderHelperPool> shared();

private:
    struct Private;
    std::unique_ptr<Private> d;
    Options m_options;
};

QString messageDecoderCrashed(); // "Das Bild hat den Decoder zum Absturz gebracht …"
QString messageDecoderTimeout(); // "Das Dekodieren hat zu lange gedauert …"
QString messageDecoderNotSandboxed();

class RemoteDecoder final : public Decoder {
public:
    RemoteDecoder(std::shared_ptr<Decoder> local, std::shared_ptr<DecoderHelperPool> pool);
    QString name() const override { return m_local->name(); }
    int priority() const override { return m_local->priority(); }
    bool canDecode(const QString& path, const QByteArray& header) const override {
        return m_local->canDecode(path, header);
    }
    bool handlesVirtualPaths() const override { return m_local->handlesVirtualPaths(); }
    std::optional<ImageInfo> info(const QString& path, QString* error) override;
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req,
                                       QString* error) override;
    Metadata metadata(const QString& path) override;

private:
    std::shared_ptr<Decoder> m_local;
    std::shared_ptr<DecoderHelperPool> m_pool;
};

// Checks what a helper reported besides the pixels (RemoteDecoder::decode): headroom must be
// finite and within [1, kMaxHelperHeadroom] (else 1); fullSize must be positive, at least the
// image size, at most kMaxHelperFullSide per edge and of the image's aspect ratio up to the
// rounding of a downsampled decode (else the image size).
inline constexpr float kMaxHelperHeadroom = 64.0f;
inline constexpr int kMaxHelperFullSide = 1 << 20;
void sanitizeHelperResult(DecodedImage& decoded);

// Wraps every decoder in RemoteDecoder when a helper pool exists, else returns it as is.
std::shared_ptr<Decoder> isolatedDecoder(std::shared_ptr<Decoder> decoder);

// ---- helper side (lumaflip-decoder, src/helper/main.cpp) ----

// The decoded image of the last Decode until the app sends its Deliver (same id).
struct DecoderHelperState {
    QImage pending;
    quint32 pendingId = 0;
    int deliveredFd = -1; // Linux: the memfd of the last Deliver, closed with the next request
};
// Answers one request with the backend named in it. Decode keeps the image in `state` and
// answers with its PixelHeader; the following Deliver copies it into the app's shared
// memory object. Any other request drops a pending image. Test operations only with
// `testOps`.
ipc::Response serveDecoderRequest(const ipc::Request& request,
                                  const QList<std::shared_ptr<Decoder>>& backends, qint64 pid, bool sandboxed,
                                  bool testOps, DecoderHelperState* state);
// Helper side, first thing in main(): closes inherited descriptors except stdin/stdout/
// stderr (Linux) and applies the memory limit the app passed (LF_DECODER_HELPER_MEMORY_MB).
void prepareDecoderHelperProcess();
// Request loop until the app closes `inFd`. Returns the process exit code.
int runDecoderHelper(int inFd, int outFd, const QList<std::shared_ptr<Decoder>>& backends, bool sandboxed,
                     bool testOps);
// macOS: restricts the calling process with a deny-by-default Seatbelt profile
// (decoderSandboxProfile()): reading files and a few system services only; no network, no
// file writes, no new processes, no shared memory except opening the app's "/lfd." objects.
// Linux: seccomp-bpf (no sockets, no new processes, no signals to other processes, no file
// system changes) + Landlock where the kernel has it (read-only file system),
// src/platform/linux/LinuxSandbox.h.
// Returns false (and why) if the sandbox could not be applied.
bool applyDecoderSandbox(QString* error);
const char* decoderSandboxProfile();

} // namespace lf
