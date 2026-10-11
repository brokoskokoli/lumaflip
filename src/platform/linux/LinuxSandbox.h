// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Sandbox of the decoder helper process on Linux (SPEC 8.5; core/DecoderHelper.h
// applyDecoderSandbox()). Unprivileged, no setuid helper, no namespaces (Ubuntu ≥ 23.10
// restricts unprivileged user namespaces through AppArmor, so they are not relied on):
//
//  0. Before anything else (prepareProcess(), src/helper/main.cpp): every descriptor except
//     stdin/stdout/stderr is closed (the app also passes only those, DecoderHelper.cpp), and
//     RLIMIT_DATA caps the helper's private memory (DecoderHelperPool::Options::memoryLimitMb).
//  1. PR_SET_NO_NEW_PRIVS.
//  2. seccomp-bpf (required; every distribution kernel has it). Denied (EPERM unless noted):
//     - network and IPC endpoints: socket();
//     - new processes and other processes: execve/execveat, fork/vfork, clone without
//       CLONE_THREAD (clone3 reports ENOSYS so that glibc uses clone), ptrace,
//       process_vm_readv/writev, process_madvise/process_mrelease, kcmp, the pidfd calls
//       (pidfd_open, pidfd_getfd, pidfd_send_signal), tkill; kill, tgkill,
//       rt_sigqueueinfo and rt_tgsigqueueinfo only towards the helper itself; prlimit64 only
//       for itself; move_pages/migrate_pages;
//     - every change to the file system, also without Landlock: open/openat with O_WRONLY,
//       O_RDWR, O_CREAT or O_TRUNC, openat2 (ENOSYS, its flags are not visible to the
//       filter), creat, rename*, unlink*, rmdir, mkdir*, mknod*, link*, symlink*, chmod
//       family, chown family, truncate, utime family, the xattr setters/removers,
//       name_to_handle_at/open_by_handle_at, acct, swapon/off, quotactl;
//     - the terminal: ioctl TIOCSTI and TIOCLINUX (no input injection into a terminal);
//     - mount/namespace/keyring/bpf/perf_event_open/io_uring/userfaultfd/kexec/module calls.
//     Pixels reach the app in a sealed memfd the helper creates (DecoderIpc.h), so the
//     helper never opens anything for writing.
//  3. Landlock (best effort, kernel ≥ 5.13): the file system becomes read-only (no rule
//     grants any write access, not even to /dev/shm); with ABI ≥ 4 TCP bind/connect are
//     denied as well. Landlock also forbids ptrace-style access to processes outside the
//     helper's domain (e.g. reading /proc/<app pid>/mem).
//
// Without Landlock, file writes are still impossible (2.); reading other processes' memory
// through /proc/<pid>/mem is then left to the kernel's ptrace rules (Yama ptrace_scope ≥ 1,
// the default of Ubuntu, Debian and Fedora, refuses it for non-descendants). docs/LINUX.md.
//
// The helper reports itself as sandboxed when 1 and 2 are in place; Landlock is listed in
// the details (`lumaflip-decoder` logs them). Without seccomp the app refuses the helper
// (error card), unless LF_DECODER_ALLOW_UNSANDBOXED=1 is set (docs/LINUX.md).

#include <QString>

namespace lf::linuxsandbox {

struct Status {
    bool noNewPrivs = false;
    bool seccomp = false;
    int landlockAbi = 0; // 0 = not available / not applied
    QString error;       // why seccomp could not be applied
};

struct Options {
    bool landlock = true; // false: seccomp only (tests of the no-Landlock path)
};

// Restricts the calling process (irreversible; all threads created later inherit it).
// Call from a single-threaded process or before threads exist: seccomp is applied with
// SECCOMP_FILTER_FLAG_TSYNC, so existing threads are covered too.
Status apply(const Options& options = {});

// The Landlock ABI version of the running kernel (0 = none).
int landlockAbiVersion();

// Closes every descriptor >= 3 (close_range, else one by one). For the very start of the
// helper, before any library opened something.
void closeInheritedDescriptors();

// RLIMIT_DATA (soft and hard) of the calling process; 0 = unchanged. False if it failed.
bool limitMemory(qint64 bytes);

} // namespace lf::linuxsandbox
