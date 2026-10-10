// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone mutation driver for the fuzz targets when the compiler has no libFuzzer
// (Apple clang). Accepts a subset of libFuzzer's command line, so both builds are used the
// same way:
//
//   fuzz_xmp [-max_total_time=SECONDS] [-runs=N] [-seed=N] [-max_len=BYTES]
//            [-timeout=SECONDS] [-write_seeds=DIR] [seed files or folders ...]
//
// Inputs: the target's built-in seeds plus the given files. Each round picks an input,
// applies a few byte-level mutations (bit flips, interesting values, inserts, deletes,
// truncation, splicing with another input) and calls LLVMFuzzerTestOneInput. Inputs that
// reach new "shapes" are not tracked (no coverage feedback): this is a robustness smoke
// test, not a replacement for libFuzzer. A crash, a sanitizer report or an input running
// longer than -timeout writes the input to ./crash-<target>-<pid> (or $LF_FUZZ_ARTIFACTS).

#include "FuzzTarget.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LF_FUZZ_ASAN 1
#endif
#endif
#ifdef LF_FUZZ_ASAN
#include <sanitizer/common_interface_defs.h>
// Sanitizer reports abort (the input is written by the death callback).
extern "C" const char* __asan_default_options() {
    return "abort_on_error=1:detect_leaks=0:allocator_may_return_null=1";
}
extern "C" const char* __ubsan_default_options() {
    return "halt_on_error=1:print_stacktrace=1";
}
#endif

namespace {

// The input under test, for the crash handlers (async-signal-safe write).
QByteArray g_current;
char g_artifact[1024];
std::atomic<qint64> g_startedMs{-1};

void writeArtifact() {
    const int fd = ::open(g_artifact, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    const char* p = g_current.constData();
    qsizetype left = g_current.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, size_t(left));
        if (n <= 0)
            break;
        p += n;
        left -= n;
    }
    ::close(fd);
    static const char msg[] = "\nlf fuzz driver: input written to ";
    (void)!::write(2, msg, sizeof msg - 1);
    (void)!::write(2, g_artifact, std::strlen(g_artifact));
    (void)!::write(2, "\n", 1);
}

void onSignal(int sig) {
    writeArtifact();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

qint64 nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

QByteArray mutate(QByteArray d, std::mt19937_64& rng, const QList<QByteArray>& pool, qsizetype maxLen) {
    static const uchar interesting[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0xFE, 0xFF, 0x10, 0x20, 0x40};
    const int n = 1 + int(rng() % 8);
    for (int i = 0; i < n; ++i) {
        if (d.isEmpty()) {
            d.append(char(rng()));
            continue;
        }
        const qsizetype pos = qsizetype(rng() % quint64(d.size()));
        switch (rng() % 10) {
        case 0:
            d[pos] = char(d[pos] ^ (1 << (rng() % 8)));
            break;
        case 1:
            d[pos] = char(rng());
            break;
        case 2:
            d[pos] = char(interesting[rng() % sizeof interesting]);
            break;
        case 3:
            d.truncate(pos);
            break;
        case 4:
            d.insert(pos, QByteArray(int(1 + rng() % 16), char(rng())));
            break;
        case 5:
            if (pos + 4 <= d.size()) {
                const quint32 v = rng() % 3 == 0 ? 0xFFFFFFFFu : quint32(rng() % 3 == 1 ? 0x7FFFFFFF : rng());
                std::memcpy(d.data() + pos, &v, 4);
            }
            break;
        case 6:
            d.remove(pos, qsizetype(1 + rng() % 32));
            break;
        case 7: { // duplicate a chunk
            const qsizetype len = qsizetype(1 + rng() % 64);
            d.insert(pos, d.mid(pos, len));
            break;
        }
        case 8: { // splice with another input
            const QByteArray& other = pool.at(qsizetype(rng() % quint64(pool.size())));
            if (!other.isEmpty()) {
                const qsizetype from = qsizetype(rng() % quint64(other.size()));
                d = d.left(pos) + other.mid(from, qsizetype(1 + rng() % 256));
            }
            break;
        }
        default: { // ASCII digits (text formats: PNM headers, XMP values)
            const QByteArray num = QByteArray::number(qlonglong(rng() % 100000) - 50);
            d.replace(pos, std::min<qsizetype>(num.size(), d.size() - pos), num);
            break;
        }
        }
    }
    if (d.size() > maxLen)
        d.truncate(maxLen);
    return d;
}

void addPath(const QString& path, QList<QByteArray>& pool) {
    const QFileInfo fi(path);
    if (fi.isDir()) {
        QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
            addPath(it.next(), pool);
        return;
    }
    QFile f(path);
    if (f.size() <= (4 << 20) && f.open(QIODevice::ReadOnly))
        pool.append(f.readAll());
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    qint64 maxSeconds = 60, runs = -1, seed = 12345, timeoutSeconds = 10;
    qsizetype maxLen = 1 << 16;
    QString writeSeeds;
    QList<QByteArray> pool = lf::fuzz::seeds();
    const QString target = QFileInfo(QString::fromLocal8Bit(argv[0])).fileName();
    for (int i = 1; i < argc; ++i) {
        const QByteArray a(argv[i]);
        auto value = [&](const char* name) -> std::optional<QByteArray> {
            const QByteArray prefix = QByteArray("-") + name + '=';
            if (a.startsWith(prefix))
                return a.mid(prefix.size());
            return std::nullopt;
        };
        if (auto v = value("max_total_time"))
            maxSeconds = v->toLongLong();
        else if (auto r = value("runs"))
            runs = r->toLongLong();
        else if (auto s = value("seed"))
            seed = s->toLongLong();
        else if (auto m = value("max_len"))
            maxLen = qsizetype(m->toLongLong());
        else if (auto t = value("timeout"))
            timeoutSeconds = t->toLongLong();
        else if (auto w = value("write_seeds"))
            writeSeeds = QString::fromLocal8Bit(*w);
        else if (a.startsWith('-'))
            std::fprintf(stderr, "ignored option %s\n", argv[i]);
        else
            addPath(QString::fromLocal8Bit(a), pool);
    }
    if (!writeSeeds.isEmpty()) {
        QDir().mkpath(writeSeeds);
        const QList<QByteArray> builtIn = lf::fuzz::seeds();
        for (int i = 0; i < builtIn.size(); ++i) {
            QFile f(QDir(writeSeeds).filePath(QStringLiteral("seed-%1").arg(i)));
            if (f.open(QIODevice::WriteOnly))
                f.write(builtIn[i]);
        }
        std::printf("%lld seeds written to %s\n", qlonglong(builtIn.size()), qPrintable(writeSeeds));
        return 0;
    }
    if (pool.isEmpty())
        pool.append(QByteArray());
    const QByteArray dir =
        qEnvironmentVariableIsEmpty("LF_FUZZ_ARTIFACTS") ? QByteArray(".") : qgetenv("LF_FUZZ_ARTIFACTS");
    std::snprintf(g_artifact, sizeof g_artifact, "%s/crash-%s-%d", dir.constData(), qPrintable(target),
                  int(getpid()));
    for (int sig : {SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL})
        std::signal(sig, onSignal);
#ifdef LF_FUZZ_ASAN
    __sanitizer_set_death_callback(writeArtifact);
#endif
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        while (!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const qint64 started = g_startedMs.load();
            if (started >= 0 && nowMs() - started > timeoutSeconds * 1000) {
                std::fprintf(stderr, "lf fuzz driver: input ran longer than %llds\n",
                             qlonglong(timeoutSeconds));
                writeArtifact();
                std::_Exit(70);
            }
        }
    });

    std::mt19937_64 rng{quint64(seed)};
    QElapsedTimer t;
    t.start();
    qint64 iterations = 0;
    // The seeds unchanged first, then mutations.
    for (const QByteArray& s : std::as_const(pool)) {
        g_current = s;
        g_startedMs = nowMs();
        LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(g_current.constData()),
                               size_t(g_current.size()));
        ++iterations;
    }
    while ((runs < 0 || iterations < runs) && t.elapsed() < maxSeconds * 1000) {
        g_current = mutate(pool.at(qsizetype(rng() % quint64(pool.size()))), rng, pool, maxLen);
        g_startedMs = nowMs();
        LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(g_current.constData()),
                               size_t(g_current.size()));
        ++iterations;
        // Keep a few mutants as new starting points (cheap diversity without coverage).
        if (iterations % 4096 == 0 && pool.size() < 512)
            pool.append(g_current);
    }
    g_startedMs = -1;
    done = true;
    watchdog.join();
    std::printf("%s: %lld inputs in %lld ms, no crash\n", qPrintable(target), qlonglong(iterations),
                qlonglong(t.elapsed()));
    return 0;
}
