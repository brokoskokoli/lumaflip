// SPDX-License-Identifier: GPL-3.0-or-later
#include "DecoderHelper.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMutex>
#include <QWaitCondition>

#include <QFile>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef Q_OS_DARWIN
#include <crt_externs.h>
#include <sandbox.h>
#endif

Q_LOGGING_CATEGORY(lcHelper, "lf.decode.helper")

namespace lf {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::DecoderHelper)
};

char** currentEnvironment() {
#ifdef Q_OS_DARWIN
    return *_NSGetEnviron();
#elif defined(Q_OS_UNIX)
    extern char** environ;
    return environ;
#else
    return nullptr;
#endif
}

} // namespace

QString messageDecoderCrashed() {
    return Tr::tr("Das Bild hat den Decoder zum Absturz gebracht – die Datei ist vermutlich beschädigt.");
}

QString messageDecoderTimeout() {
    return Tr::tr("Das Dekodieren hat zu lange gedauert und wurde abgebrochen.");
}

// ---- one helper process ----

struct HelperProcess {
    qint64 pid = 0;
    int toChild = -1;
    int fromChild = -1;
    bool busy = false;

    bool running() const { return pid > 0; }

    void kill() {
#ifdef Q_OS_UNIX
        if (toChild >= 0)
            close(toChild);
        if (fromChild >= 0)
            close(fromChild);
        if (pid > 0) {
            ::kill(pid_t(pid), SIGKILL);
            int status = 0;
            while (waitpid(pid_t(pid), &status, 0) < 0 && errno == EINTR) {
            }
        }
#endif
        toChild = fromChild = -1;
        pid = 0;
    }

    bool start(const DecoderHelperPool::Options& o, QString* error) {
#ifdef Q_OS_UNIX
        int in[2], out[2]; // in: app -> helper (helper's stdin), out: helper -> app
        if (pipe(in) != 0)
            return false;
        if (pipe(out) != 0) {
            close(in[0]);
            close(in[1]);
            return false;
        }
        for (int fd : {in[0], in[1], out[0], out[1]})
            fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef F_SETNOSIGPIPE
        fcntl(in[1], F_SETNOSIGPIPE, 1); // a dead helper must not kill the app with SIGPIPE
#else
        static const bool ignored = [] {
            std::signal(SIGPIPE, SIG_IGN);
            return true;
        }();
        Q_UNUSED(ignored);
#endif
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, in[0], 0);
        posix_spawn_file_actions_adddup2(&actions, out[1], 1);
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
        // Only stdin/stdout/stderr reach the helper (no inherited files or sockets).
        posix_spawn_file_actions_addinherit_np(&actions, 2);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
        const QByteArray exe = QFile::encodeName(o.executable);
        QByteArray arg1("--decoder-helper");
        char* argv[] = {const_cast<char*>(exe.constData()), arg1.data(), nullptr};
        std::vector<QByteArray> envStore;
        for (char** e = currentEnvironment(); e && *e; ++e)
            envStore.emplace_back(*e);
        for (const QString& extra : o.extraEnvironment)
            envStore.push_back(extra.toLocal8Bit());
        std::vector<char*> envp;
        for (QByteArray& e : envStore)
            envp.push_back(e.data());
        envp.push_back(nullptr);
        pid_t child = 0;
        const int rc = posix_spawn(&child, exe.constData(), &actions, &attr, argv, envp.data());
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attr);
        close(in[0]);
        close(out[1]);
        if (rc != 0) {
            close(in[1]);
            close(out[0]);
            if (error)
                *error = Tr::tr("Der Decoder-Hilfsprozess konnte nicht gestartet werden (%1).")
                             .arg(QString::fromLocal8Bit(strerror(rc)));
            return false;
        }
        pid = child;
        toChild = in[1];
        fromChild = out[0];
        return true;
#else
        Q_UNUSED(o);
        if (error)
            *error = Tr::tr("Der Decoder-Hilfsprozess wird auf diesem System nicht unterstützt.");
        return false;
#endif
    }
};

struct DecoderHelperPool::Private {
    mutable QMutex mutex;
    QWaitCondition idle;
    std::vector<std::unique_ptr<HelperProcess>> processes;
    std::atomic<quint32> nextId{1};
    int started = 0;
};

DecoderHelperPool::DecoderHelperPool(Options options)
    : d(std::make_unique<Private>()), m_options(std::move(options)) {
    m_options.maxProcesses = std::max(1, m_options.maxProcesses);
}

DecoderHelperPool::~DecoderHelperPool() {
    shutdown();
}

void DecoderHelperPool::shutdown() {
    QMutexLocker lock(&d->mutex);
    // Busy helpers belong to a running call; it notices the closed pipes (and the process
    // object stays alive until it is released).
    for (auto& p : d->processes)
        if (!p->busy)
            p->kill();
}

int DecoderHelperPool::startedProcesses() const {
    QMutexLocker lock(&d->mutex);
    return d->started;
}

int DecoderHelperPool::runningProcesses() const {
    QMutexLocker lock(&d->mutex);
    int n = 0;
    for (const auto& p : d->processes)
        n += p->running() ? 1 : 0;
    return n;
}

std::optional<ipc::Response> DecoderHelperPool::call(ipc::Request request, int timeoutMs, Failure* failure,
                                                     QString* error, qint64* helperPid) {
    auto fail = [&](Failure f, const QString& text) -> std::optional<ipc::Response> {
        if (failure)
            *failure = f;
        if (error)
            *error = text;
        return std::nullopt;
    };
    if (failure)
        *failure = Failure::None;
    request.id = d->nextId.fetch_add(1);

    // Take an idle helper (or a slot for a new one).
    HelperProcess* proc = nullptr;
    {
        QMutexLocker lock(&d->mutex);
        for (;;) {
            for (auto& p : d->processes) {
                if (!p->busy) {
                    proc = p.get();
                    break;
                }
            }
            if (proc)
                break;
            if (int(d->processes.size()) < m_options.maxProcesses) {
                d->processes.push_back(std::make_unique<HelperProcess>());
                proc = d->processes.back().get();
                break;
            }
            d->idle.wait(&d->mutex);
        }
        proc->busy = true;
    }
    auto release = [&] {
        QMutexLocker lock(&d->mutex);
        proc->busy = false;
        d->idle.wakeOne();
    };

    if (!proc->running()) {
        QString startError;
        if (!proc->start(m_options, &startError)) {
            release();
            qCWarning(lcHelper) << "cannot start" << m_options.executable << startError;
            return fail(Failure::StartFailed,
                        startError.isEmpty()
                            ? Tr::tr("Der Decoder-Hilfsprozess konnte nicht gestartet werden.")
                            : startError);
        }
        QMutexLocker lock(&d->mutex);
        ++d->started;
    }
    const qint64 pid = proc->pid;
    if (helperPid)
        *helperPid = pid;
    const QByteArray shm = ipc::shmName(pid, request.id);

    ipc::IoStatus st = ipc::writeFrame(proc->toChild, ipc::encode(request));
    QByteArray payload;
    if (st == ipc::IoStatus::Ok)
        st = ipc::readFrame(proc->fromChild, &payload, timeoutMs);
    std::optional<ipc::Response> response;
    if (st == ipc::IoStatus::Ok) {
        response = ipc::decodeResponse(payload);
        if (response && response->id != request.id)
            response.reset();
    }
    if (!response) {
        // Crashed, hung or talking nonsense: the helper is replaced on the next call.
        proc->kill();
        release();
        ipc::unlinkShared(shm); // pixels it may have written before dying
        if (st == ipc::IoStatus::Timeout) {
            qCWarning(lcHelper) << "timeout, helper" << pid << "killed:" << request.path;
            return fail(Failure::Timeout, messageDecoderTimeout());
        }
        if (st == ipc::IoStatus::Ok) {
            qCWarning(lcHelper) << "protocol error, helper" << pid << "killed:" << request.path;
            return fail(Failure::Protocol, messageDecoderCrashed());
        }
        qCWarning(lcHelper) << "helper" << pid << "died:" << request.path;
        return fail(Failure::Crashed, messageDecoderCrashed());
    }
    if (request.op == ipc::Op::Decode && response->ok) {
        // Map before the helper is released: its next request removes the object.
        QString mapError;
        response->image = ipc::readSharedImage(shm, response->pixels, &mapError);
        if (response->image.isNull()) {
            response->ok = false;
            response->error = mapError;
        }
    }
    release();
    return response;
}

QString DecoderHelperPool::defaultExecutable() {
    if (qEnvironmentVariableIntValue("LF_DECODER_IN_PROCESS") == 1)
        return {};
    const QString env = qEnvironmentVariable("LF_DECODER_HELPER");
    if (!env.isEmpty())
        return QFileInfo(env).isExecutable() ? env : QString();
    if (!QCoreApplication::instance())
        return {};
    const QString exe =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("lumaflip-decoder"));
    return QFileInfo(exe).isExecutable() ? exe : QString();
}

std::shared_ptr<DecoderHelperPool> DecoderHelperPool::shared() {
    static std::shared_ptr<DecoderHelperPool> pool = []() -> std::shared_ptr<DecoderHelperPool> {
        Options o;
        o.executable = defaultExecutable();
        if (o.executable.isEmpty())
            return nullptr;
        return std::make_shared<DecoderHelperPool>(o);
    }();
    return pool;
}

// ---- RemoteDecoder ----

RemoteDecoder::RemoteDecoder(std::shared_ptr<Decoder> local, std::shared_ptr<DecoderHelperPool> pool)
    : m_local(std::move(local)), m_pool(std::move(pool)) {}

std::optional<ImageInfo> RemoteDecoder::info(const QString& path, QString* error) {
    ipc::Request r;
    r.op = ipc::Op::Info;
    r.backend = m_local->name();
    r.path = path;
    auto resp = m_pool->call(r, m_pool->options().infoTimeoutMs, nullptr, error);
    if (!resp)
        return std::nullopt;
    if (!resp->ok) {
        if (error)
            *error = resp->error;
        return std::nullopt;
    }
    return resp->info;
}

std::optional<DecodedImage> RemoteDecoder::decode(const QString& path, const DecodeRequest& req,
                                                  QString* error) {
    ipc::Request r;
    r.op = ipc::Op::Decode;
    r.backend = m_local->name();
    r.path = path;
    r.decode = req;
    auto resp = m_pool->call(r, m_pool->options().decodeTimeoutMs, nullptr, error);
    if (!resp)
        return std::nullopt;
    if (!resp->ok) {
        if (error)
            *error = resp->error;
        return std::nullopt;
    }
    DecodedImage out = resp->decoded;
    out.image = std::move(resp->image);
    return out;
}

Metadata RemoteDecoder::metadata(const QString& path) {
    ipc::Request r;
    r.op = ipc::Op::Metadata;
    r.backend = m_local->name();
    r.path = path;
    auto resp = m_pool->call(r, m_pool->options().infoTimeoutMs);
    return resp && resp->ok ? resp->metadata : Metadata();
}

std::shared_ptr<Decoder> isolatedDecoder(std::shared_ptr<Decoder> decoder) {
    if (auto pool = DecoderHelperPool::shared())
        return std::make_shared<RemoteDecoder>(std::move(decoder), std::move(pool));
    return decoder;
}

// ---- helper side ----

namespace {
// Test operations: die like a crash, without a crash report (ReportCrash) per test run.
[[noreturn]] void simulateCrash() {
#ifdef Q_OS_UNIX
    ::kill(getpid(), SIGKILL);
#endif
    std::_Exit(134);
}
} // namespace

ipc::Response serveDecoderRequest(const ipc::Request& request,
                                  const QList<std::shared_ptr<Decoder>>& backends, qint64 pid, bool sandboxed,
                                  bool testOps) {
    ipc::Response r;
    r.id = request.id;
    r.pid = pid;
    r.sandboxed = sandboxed;
    if (request.op == ipc::Op::Ping) {
        r.ok = true;
        return r;
    }
    if (testOps) {
        switch (request.op) {
        case ipc::Op::TestCrash:
            simulateCrash();
        case ipc::Op::TestHang:
            for (;;)
                std::this_thread::sleep_for(std::chrono::seconds(60));
        case ipc::Op::TestWriteFile: {
            QFile f(request.path);
            r.ok = f.open(QIODevice::WriteOnly) && f.write("x", 1) == 1;
            if (!r.ok)
                r.error = f.errorString();
            return r;
        }
        default:
            break;
        }
        // A "file that crashes the decoder" for tests of the regular decode path.
        if (request.path.endsWith(QLatin1String(".lfcrash")))
            simulateCrash();
    }
    std::shared_ptr<Decoder> backend;
    for (const auto& b : backends)
        if (b->name() == request.backend)
            backend = b;
    if (!backend) {
        r.error = Tr::tr("Unbekannter Decoder „%1“.").arg(request.backend);
        return r;
    }
    QString error;
    switch (request.op) {
    case ipc::Op::Info:
        if (auto info = backend->info(request.path, &error)) {
            r.ok = true;
            r.info = *info;
        }
        break;
    case ipc::Op::Metadata:
        r.metadata = backend->metadata(request.path);
        r.ok = true;
        break;
    case ipc::Op::Decode:
        if (auto img = backend->decode(request.path, request.decode, &error)) {
            if (img->image.isNull()) {
                error = Tr::tr("Das Bild ist leer.");
                break;
            }
            r.ok = ipc::writeSharedImage(ipc::shmName(pid, request.id), img->image, &r.pixels, &error);
            r.decoded = *img;
            r.decoded.image = QImage();
        }
        break;
    default:
        error = Tr::tr("Unbekannte Anfrage.");
        break;
    }
    if (!r.ok)
        r.error = error;
    return r;
}

int runDecoderHelper(int inFd, int outFd, const QList<std::shared_ptr<Decoder>>& backends, bool sandboxed,
                     bool testOps) {
#ifdef Q_OS_UNIX
    const qint64 pid = qint64(getpid());
    // Exit when the app is gone, also while a decode is still running (the pipe's EOF is
    // only seen between requests).
    const pid_t parent = getppid();
    std::thread([parent] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (getppid() != parent)
                _exit(0);
        }
    }).detach();
    quint32 lastId = 0;
    bool haveLast = false;
    for (;;) {
        QByteArray payload;
        const ipc::IoStatus st = ipc::readFrame(inFd, &payload, -1);
        // The app maps and unlinks the previous image before it sends the next request;
        // anything left (the app gave up on it) goes now.
        if (haveLast)
            ipc::unlinkShared(ipc::shmName(pid, lastId));
        haveLast = false;
        if (st != ipc::IoStatus::Ok)
            return st == ipc::IoStatus::Closed ? 0 : 1;
        const auto request = ipc::decodeRequest(payload);
        if (!request)
            return 2;
        const ipc::Response response = serveDecoderRequest(*request, backends, pid, sandboxed, testOps);
        lastId = request->id;
        haveLast = request->op == ipc::Op::Decode && response.ok;
        if (ipc::writeFrame(outFd, ipc::encode(response)) != ipc::IoStatus::Ok) {
            if (haveLast)
                ipc::unlinkShared(ipc::shmName(pid, lastId));
            return 0;
        }
    }
#else
    Q_UNUSED(inFd);
    Q_UNUSED(outFd);
    Q_UNUSED(backends);
    Q_UNUSED(sandboxed);
    Q_UNUSED(testOps);
    return 1;
#endif
}

bool applyDecoderSandbox(QString* error) {
#ifdef Q_OS_DARWIN
    // Seatbelt profile: everything the decoders need to read (files, fonts, system
    // services) stays allowed; no network, no file writes, no new processes. POSIX shared
    // memory (ipc-posix-shm) is not a file write.
    static const char kProfile[] = "(version 1)\n"
                                   "(allow default)\n"
                                   "(deny network*)\n"
                                   "(deny file-write*)\n"
                                   "(allow file-write-data (literal \"/dev/null\"))\n"
                                   "(deny process-exec*)\n"
                                   "(deny process-fork)\n";
    char* err = nullptr;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const int rc = sandbox_init(kProfile, 0, &err);
    if (rc != 0) {
        if (error)
            *error = QString::fromUtf8(err ? err : "sandbox_init failed");
        if (err)
            sandbox_free_error(err);
        return false;
    }
#pragma clang diagnostic pop
    return true;
#else
    if (error)
        *error = QStringLiteral("not supported on this platform");
    return false;
#endif
}

} // namespace lf
