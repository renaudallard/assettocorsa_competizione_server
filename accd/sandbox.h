/*
 * sandbox.h -- per-OS privilege reduction
 *
 * Applies the platform's strongest available syscall + filesystem
 * confinement just before the main poll loop.  All paths log via
 * log_warn() on failure and never abort the server; the caller does
 * not need to check the return value.
 *
 *   OpenBSD: pledge("stdio rpath wpath cpath inet") + unveil() on
 *            the two writable directories.
 *   Linux:   seccomp-BPF allowlist (SCMP_ACT_KILL_PROCESS default)
 *            plus a Landlock ruleset scoped to the same directories.
 *            Either feature is skipped at runtime if the kernel or
 *            libseccomp is too old; both are skipped under ASAN /
 *            UBSAN / TSAN builds.
 *   Others:  no-op + one log_warn line.
 */
#ifndef ACCD_SANDBOX_H
#define ACCD_SANDBOX_H

void sandbox_apply(const char *cfg_dir, const char *results_dir);

#endif
