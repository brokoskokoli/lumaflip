// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Sandbox of the decoder helper process on Linux (SPEC 8.5; core/DecoderHelper.h
// applyDecoderSandbox()). Unprivileged, no setuid helper, no namespaces (Ubuntu ≥ 23.10
// restricts unprivileged user namespaces through AppArmor, so they are not relied on):
//
//  1. PR_SET_NO_NEW_PRIVS.
//  2. seccomp-bpf (required; every distribution kernel has it): socket() fails (no network,
//     no D-Bus, no X11/Wayland connection), new processes cannot be started (execve,
//     execveat, fork/vfork, clone without CLONE_THREAD; clone3 reports ENOSYS so glibc uses
//     clone for threads), and ptrace, process_vm_*, mount/namespace/keyring/bpf/
//     perf_event_open/io_uring/userfaultfd/kexec/module calls fail with EPERM.
//  3. Landlock (best effort, kernel ≥ 5.13): the file system becomes read-only - files can
//     be read and libraries mapped everywhere; writing is only allowed to the app's shared
//     memory objects in /dev/shm (open, write, unlink - not create) and /dev/null. With
//     Landlock ABI ≥ 4 TCP bind/connect are denied as well (seccomp already blocks sockets).
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

// Restricts the calling process (irreversible; all threads created later inherit it).
// Call from a single-threaded process or before threads exist: seccomp is applied with
// SECCOMP_FILTER_FLAG_TSYNC, so existing threads are covered too.
Status apply();

// The Landlock ABI version of the running kernel (0 = none).
int landlockAbiVersion();

} // namespace lf::linuxsandbox
