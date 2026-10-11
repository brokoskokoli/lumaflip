// SPDX-License-Identifier: GPL-3.0-or-later
// Decoder helper sandbox on Linux: no_new_privs + seccomp-bpf + Landlock (LinuxSandbox.h).

#include "LinuxSandbox.h"

#include "core/DecoderHelper.h"

#include <QByteArray>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#if __has_include(<linux/landlock.h>)
#include <linux/landlock.h>
#define LF_HAVE_LANDLOCK_HEADER 1
#else
#define LF_HAVE_LANDLOCK_HEADER 0
#endif

namespace lf {

namespace linuxsandbox {

namespace {

#if defined(__x86_64__)
constexpr quint32 kArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr quint32 kArch = AUDIT_ARCH_AARCH64;
#else
constexpr quint32 kArch = 0; // unknown architecture: no seccomp filter
#endif

sock_filter stmt(quint16 code, quint32 k) {
    return sock_filter{code, 0, 0, k};
}

sock_filter jump(quint16 code, quint32 k, quint8 jt, quint8 jf) {
    return sock_filter{code, jt, jf, k};
}

constexpr quint32 kAllow = SECCOMP_RET_ALLOW;
constexpr quint32 kEperm = SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);
constexpr quint32 kEnosys = SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA);

// The low 32 bits of syscall argument `i` (x86_64 and arm64 are little endian). Every
// argument checked here is an int/pid_t/unsigned int in the kernel, which ignores the upper
// half of the register - so only the low half may decide.
quint32 argLow(int i) {
    return quint32(offsetof(seccomp_data, args) + sizeof(quint64) * size_t(i));
}

// Syscalls that fail with EPERM inside the helper (only those this architecture has).
std::vector<int> deniedSyscalls() {
    return {
        // network, new processes, other processes
        SYS_socket,
        SYS_execve,
        SYS_execveat,
#ifdef SYS_fork
        SYS_fork,
#endif
#ifdef SYS_vfork
        SYS_vfork,
#endif
        SYS_ptrace,
        SYS_process_vm_readv,
        SYS_process_vm_writev,
        SYS_kcmp,
        SYS_tkill,
        SYS_move_pages,
        SYS_migrate_pages,
#ifdef SYS_pidfd_open
        SYS_pidfd_open,
#endif
#ifdef SYS_pidfd_send_signal
        SYS_pidfd_send_signal,
#endif
#ifdef SYS_pidfd_getfd
        SYS_pidfd_getfd,
#endif
#ifdef SYS_process_madvise
        SYS_process_madvise,
#endif
#ifdef SYS_process_mrelease
        SYS_process_mrelease,
#endif
    // file system changes (open/openat are checked by their flags)
#ifdef SYS_creat
        SYS_creat,
#endif
#ifdef SYS_rename
        SYS_rename,
#endif
        SYS_renameat,
#ifdef SYS_renameat2
        SYS_renameat2,
#endif
#ifdef SYS_unlink
        SYS_unlink,
#endif
        SYS_unlinkat,
#ifdef SYS_rmdir
        SYS_rmdir,
#endif
#ifdef SYS_mkdir
        SYS_mkdir,
#endif
        SYS_mkdirat,
#ifdef SYS_mknod
        SYS_mknod,
#endif
        SYS_mknodat,
#ifdef SYS_link
        SYS_link,
#endif
        SYS_linkat,
#ifdef SYS_symlink
        SYS_symlink,
#endif
        SYS_symlinkat,
#ifdef SYS_chmod
        SYS_chmod,
#endif
        SYS_fchmod,
        SYS_fchmodat,
#ifdef SYS_fchmodat2
        SYS_fchmodat2,
#endif
#ifdef SYS_chown
        SYS_chown,
#endif
#ifdef SYS_lchown
        SYS_lchown,
#endif
        SYS_fchown,
        SYS_fchownat,
        SYS_truncate,
#ifdef SYS_utime
        SYS_utime,
#endif
#ifdef SYS_utimes
        SYS_utimes,
#endif
#ifdef SYS_futimesat
        SYS_futimesat,
#endif
        SYS_utimensat,
        SYS_setxattr,
        SYS_lsetxattr,
        SYS_fsetxattr,
        SYS_removexattr,
        SYS_lremovexattr,
        SYS_fremovexattr,
#ifdef SYS_setxattrat
        SYS_setxattrat,
        SYS_removexattrat,
#endif
        SYS_name_to_handle_at,
        SYS_open_by_handle_at,
        SYS_acct,
        SYS_swapon,
        SYS_swapoff,
        SYS_quotactl,
#ifdef SYS_quotactl_fd
        SYS_quotactl_fd,
#endif
        SYS_fanotify_init,
        // system state
        SYS_mount,
        SYS_umount2,
        SYS_pivot_root,
        SYS_chroot,
        SYS_unshare,
        SYS_setns,
#ifdef SYS_mount_setattr
        SYS_mount_setattr,
#endif
        SYS_kexec_load,
#ifdef SYS_kexec_file_load
        SYS_kexec_file_load,
#endif
        SYS_init_module,
        SYS_finit_module,
        SYS_delete_module,
        SYS_bpf,
        SYS_perf_event_open,
        SYS_keyctl,
        SYS_add_key,
        SYS_request_key,
        SYS_userfaultfd,
#ifdef SYS_io_uring_setup
        SYS_io_uring_setup,
        SYS_io_uring_enter,
        SYS_io_uring_register,
#endif
#ifdef SYS_open_tree
        SYS_open_tree,
        SYS_move_mount,
        SYS_fsopen,
        SYS_fsconfig,
        SYS_fsmount,
        SYS_fspick,
#endif
    };
}

// Builds the filter: after the architecture checks the accumulator holds the syscall
// number; every argument check is a block "if nr == X { ...; return }" that a non-matching
// syscall jumps over (the accumulator is only changed inside a block).
class FilterBuilder {
public:
    std::vector<sock_filter> f;

    // `body` must end in a return on every path and be shorter than 256 instructions.
    void onSyscall(int nr, const std::vector<sock_filter>& body) {
        f.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, quint32(nr), 0, quint8(body.size())));
        f.insert(f.end(), body.begin(), body.end());
    }
    void deny(int nr, quint32 ret = kEperm) { onSyscall(nr, {stmt(BPF_RET | BPF_K, ret)}); }

    // Allowed only when argument `arg` is this process (signals, prlimit64); with
    // `orZero` also for 0 (prlimit64: "the calling process").
    void onlySelf(int nr, int arg, quint32 self, bool orZero = false) {
        std::vector<sock_filter> b;
        b.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, argLow(arg)));
        if (orZero)
            b.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, 0, 2, 0));
        b.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, self, 1, 0));
        b.push_back(stmt(BPF_RET | BPF_K, kEperm));
        b.push_back(stmt(BPF_RET | BPF_K, kAllow));
        onSyscall(nr, b);
    }

    // open/openat: no flag that writes, creates or truncates.
    void readOnlyOpen(int nr, int flagsArg) {
        constexpr quint32 kWriting = O_WRONLY | O_RDWR | O_CREAT | O_TRUNC;
        onSyscall(nr, {stmt(BPF_LD | BPF_W | BPF_ABS, argLow(flagsArg)),
                       jump(BPF_JMP | BPF_JSET | BPF_K, kWriting, 0, 1), stmt(BPF_RET | BPF_K, kEperm),
                       stmt(BPF_RET | BPF_K, kAllow)});
    }
};

bool applySeccomp(QString* error) {
    if (kArch == 0) {
        *error = QStringLiteral("seccomp: unsupported architecture");
        return false;
    }
    const auto self = quint32(getpid());
    FilterBuilder b;
    auto& f = b.f;
    // Wrong architecture (e.g. 32-bit syscalls on x86_64): kill.
    f.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)));
    f.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, kArch, 1, 0));
    f.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    f.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)));
#if defined(__x86_64__)
    // x32 syscalls (bit 30): denied.
    f.push_back(jump(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1));
    f.push_back(stmt(BPF_RET | BPF_K, kEperm));
#endif
    // clone3 and openat2 take their flags in a struct the filter cannot read: ENOSYS, so
    // that glibc falls back to clone()/openat(), whose flags are checked.
#ifdef SYS_clone3
    b.deny(SYS_clone3, kEnosys);
#endif
#ifdef SYS_openat2
    b.deny(SYS_openat2, kEnosys);
#endif
    // clone: threads only (CLONE_THREAD set in the flags, argument 0 on x86_64 and arm64).
    b.onSyscall(SYS_clone, {stmt(BPF_LD | BPF_W | BPF_ABS, argLow(0)),
                            jump(BPF_JMP | BPF_JSET | BPF_K, CLONE_THREAD, 0, 1),
                            stmt(BPF_RET | BPF_K, kAllow), stmt(BPF_RET | BPF_K, kEperm)});
#ifdef SYS_open
    b.readOnlyOpen(SYS_open, 1);
#endif
    b.readOnlyOpen(SYS_openat, 2);
    // Signals only to itself (abort() = tgkill(self, tid, SIGABRT)); never to the app.
    b.onlySelf(SYS_kill, 0, self);
    b.onlySelf(SYS_tgkill, 0, self);
    b.onlySelf(SYS_rt_sigqueueinfo, 0, self);
    b.onlySelf(SYS_rt_tgsigqueueinfo, 0, self);
    b.onlySelf(SYS_prlimit64, 0, self, true);
    // No input injection into a terminal the helper might still reach.
    b.onSyscall(SYS_ioctl, {stmt(BPF_LD | BPF_W | BPF_ABS, argLow(1)),
                            jump(BPF_JMP | BPF_JEQ | BPF_K, quint32(TIOCSTI), 2, 0),
                            jump(BPF_JMP | BPF_JEQ | BPF_K, quint32(TIOCLINUX), 1, 0),
                            stmt(BPF_RET | BPF_K, kAllow), stmt(BPF_RET | BPF_K, kEperm)});
    for (int nr : deniedSyscalls())
        b.deny(nr);
    f.push_back(stmt(BPF_RET | BPF_K, kAllow));
    sock_fprog prog{static_cast<unsigned short>(f.size()), f.data()};
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &prog) != 0) {
        // Older kernels without TSYNC: the calling thread only (the helper is single-threaded
        // at this point except for threads Qt may have started).
        if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) {
            *error = QStringLiteral("seccomp: %1").arg(QString::fromLocal8Bit(strerror(errno)));
            return false;
        }
    }
    return true;
}

#if LF_HAVE_LANDLOCK_HEADER

int landlockAbi() {
    const long abi = syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    return abi < 0 ? 0 : int(abi);
}

bool addPathRule(int ruleset, const char* path, quint64 access) {
    const int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT; // nothing there: nothing to allow
    landlock_path_beneath_attr attr{};
    attr.allowed_access = access;
    attr.parent_fd = fd;
    const long rc = syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &attr, 0);
    close(fd);
    return rc == 0;
}

int applyLandlock() {
    const int abi = landlockAbi();
    if (abi < 1)
        return 0;
    quint64 fs = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
                 LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR |
                 LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
                 LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO |
                 LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2)
        fs |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3)
        fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
    landlock_ruleset_attr attr{};
    attr.handled_access_fs = fs;
#ifdef LANDLOCK_ACCESS_NET_BIND_TCP
    if (abi >= 4)
        attr.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
#endif
    const long rs = syscall(SYS_landlock_create_ruleset, &attr, sizeof attr, 0);
    if (rs < 0)
        return 0;
    const int ruleset = int(rs);
    const quint64 read =
        LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE;
    // Read-only everywhere: no rule grants write access (the pixels travel in a memfd the
    // helper creates, DecoderIpc.h, not in a named shared memory object).
    const bool ok = addPathRule(ruleset, "/", read);
    const bool restricted = ok && syscall(SYS_landlock_restrict_self, ruleset, 0) == 0;
    close(ruleset);
    return restricted ? abi : 0;
}

#else

int landlockAbi() {
    return 0;
}

int applyLandlock() {
    return 0;
}

#endif

} // namespace

int landlockAbiVersion() {
    return landlockAbi();
}

void closeInheritedDescriptors() {
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3u, ~0u, 0u) == 0)
        return;
#endif
    // Kernels before 5.9: every descriptor up to the limit.
    const long max = sysconf(_SC_OPEN_MAX);
    const long end = max > 0 ? std::min<long>(max, 1L << 20) : 1024;
    for (long fd = 3; fd < end; ++fd)
        close(int(fd));
}

bool limitMemory(qint64 bytes) {
    if (bytes <= 0)
        return true;
    const rlimit limit{rlim_t(bytes), rlim_t(bytes)};
    return setrlimit(RLIMIT_DATA, &limit) == 0;
}

Status apply(const Options& options) {
    Status s;
    s.noNewPrivs = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0;
    if (!s.noNewPrivs) {
        s.error = QStringLiteral("PR_SET_NO_NEW_PRIVS: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        return s;
    }
    // Landlock first: creating the ruleset needs no syscall that seccomp blocks, but keep
    // the file system rules independent of the filter order.
    s.landlockAbi = options.landlock ? applyLandlock() : 0;
    s.seccomp = applySeccomp(&s.error);
    return s;
}

} // namespace linuxsandbox

// core/DecoderHelper.cpp calls this on Linux (applyDecoderSandbox()).
bool applyPlatformDecoderSandbox(bool landlock, QString* error, QString* details) {
    linuxsandbox::Options options;
    options.landlock = landlock;
    const linuxsandbox::Status s = linuxsandbox::apply(options);
    if (details)
        *details =
            QStringLiteral("seccomp %1, Landlock %2")
                .arg(s.seccomp ? QStringLiteral("on") : QStringLiteral("off"),
                     s.landlockAbi > 0 ? QStringLiteral("ABI %1").arg(s.landlockAbi) : QStringLiteral("off"));
    if (!s.seccomp && error)
        *error = s.error;
    return s.noNewPrivs && s.seccomp;
}

} // namespace lf
