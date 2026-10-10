// SPDX-License-Identifier: GPL-3.0-or-later
// Decoder helper sandbox on Linux: no_new_privs + seccomp-bpf + Landlock (LinuxSandbox.h).

#include "LinuxSandbox.h"

#include "core/DecoderHelper.h"

#include <QByteArray>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/prctl.h>
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

// Syscalls that fail with EPERM inside the helper (only those this architecture has).
std::vector<int> deniedSyscalls() {
    return {
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
        SYS_mount,
        SYS_umount2,
        SYS_pivot_root,
        SYS_chroot,
        SYS_unshare,
        SYS_setns,
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

bool applySeccomp(QString* error) {
    if (kArch == 0) {
        *error = QStringLiteral("seccomp: unsupported architecture");
        return false;
    }
    constexpr quint32 kErrno = SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);
    std::vector<sock_filter> f;
    // Wrong architecture (e.g. 32-bit syscalls on x86_64): kill.
    f.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)));
    f.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, kArch, 1, 0));
    f.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    f.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)));
#if defined(__x86_64__)
    // x32 syscalls (bit 30): denied.
    f.push_back(jump(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1));
    f.push_back(stmt(BPF_RET | BPF_K, kErrno));
#endif
    // clone3: ENOSYS, so that glibc falls back to clone() (whose flags can be checked).
#ifdef SYS_clone3
    f.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, quint32(SYS_clone3), 0, 1));
    f.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA)));
#endif
    // clone: threads only (CLONE_THREAD set in the flags, argument 0 on x86_64 and arm64).
    f.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, quint32(SYS_clone), 0, 4));
    f.push_back(stmt(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[0])));
    f.push_back(jump(BPF_JMP | BPF_JSET | BPF_K, CLONE_THREAD, 0, 1));
    f.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    f.push_back(stmt(BPF_RET | BPF_K, kErrno));
    for (int nr : deniedSyscalls()) {
        f.push_back(jump(BPF_JMP | BPF_JEQ | BPF_K, quint32(nr), 0, 1));
        f.push_back(stmt(BPF_RET | BPF_K, kErrno));
    }
    f.push_back(stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
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
        return errno == ENOENT; // e.g. no /dev/shm: nothing to allow
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
    quint64 truncate = 0;
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2)
        fs |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3) {
        fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
        truncate = LANDLOCK_ACCESS_FS_TRUNCATE;
    }
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
    bool ok = addPathRule(ruleset, "/", read);
    // The app's shared memory objects (DecoderIpc.h): open read/write and unlink, no create.
    ok = ok && addPathRule(ruleset, "/dev/shm",
                           read | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_FILE | truncate);
    ok =
        ok && addPathRule(ruleset, "/dev/null", LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE);
    if (ok)
        ok = syscall(SYS_landlock_restrict_self, ruleset, 0) == 0;
    close(ruleset);
    return ok ? abi : 0;
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

Status apply() {
    Status s;
    s.noNewPrivs = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0;
    if (!s.noNewPrivs) {
        s.error = QStringLiteral("PR_SET_NO_NEW_PRIVS: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        return s;
    }
    // Landlock first: creating the ruleset needs no syscall that seccomp blocks, but keep
    // the file system rules independent of the filter order.
    s.landlockAbi = applyLandlock();
    s.seccomp = applySeccomp(&s.error);
    return s;
}

} // namespace linuxsandbox

// core/DecoderHelper.cpp calls this on Linux (applyDecoderSandbox()).
bool applyPlatformDecoderSandbox(QString* error, QString* details) {
    const linuxsandbox::Status s = linuxsandbox::apply();
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
