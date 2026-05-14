/*
 * sandbox.c -- per-OS privilege reduction.  See sandbox.h.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "sandbox.h"
#include "log.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#if defined(__linux__)

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/socket.h>

#if __has_include(<seccomp.h>)
#  include <seccomp.h>
#  define ACCD_HAVE_SECCOMP 1
#endif

#if __has_include(<linux/landlock.h>)
#  include <linux/landlock.h>
#  define ACCD_HAVE_LANDLOCK 1
#endif

#ifdef ACCD_HAVE_LANDLOCK
static int
landlock_path_beneath_add(int rfd, int path_fd, uint64_t mask)
{
	struct landlock_path_beneath_attr a;

	memset(&a, 0, sizeof(a));
	a.allowed_access = mask;
	a.parent_fd = path_fd;
	return syscall(__NR_landlock_add_rule, rfd,
	    LANDLOCK_RULE_PATH_BENEATH, &a, 0);
}

/* 0 on success (ruleset loaded), -1 if Landlock is unavailable or
 * setup failed.  Failure paths still log_warn with the specific
 * reason; the return value is for the sandbox_apply summary line. */
static int
apply_landlock(const char *cfg_dir, const char *results_dir)
{
	struct landlock_ruleset_attr ra;
	uint64_t mask;
	int abi, rfd, fd;

	abi = (int)syscall(__NR_landlock_create_ruleset, NULL, 0,
	    LANDLOCK_CREATE_RULESET_VERSION);
	if (abi < 0) {
		if (errno == ENOSYS || errno == EOPNOTSUPP)
			log_warn("landlock: kernel < 5.13, "
			    "filesystem sandbox disabled");
		else
			log_warn("landlock probe: %s", strerror(errno));
		return -1;
	}

	mask = LANDLOCK_ACCESS_FS_READ_FILE
	     | LANDLOCK_ACCESS_FS_READ_DIR
	     | LANDLOCK_ACCESS_FS_WRITE_FILE
	     | LANDLOCK_ACCESS_FS_MAKE_DIR
	     | LANDLOCK_ACCESS_FS_MAKE_REG
	     | LANDLOCK_ACCESS_FS_REMOVE_FILE
	     | LANDLOCK_ACCESS_FS_REMOVE_DIR;
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
	if (abi >= 3)
		mask |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif

	memset(&ra, 0, sizeof(ra));
	ra.handled_access_fs = mask;
	rfd = (int)syscall(__NR_landlock_create_ruleset, &ra, sizeof(ra), 0);
	if (rfd < 0) {
		log_warn("landlock_create_ruleset: %s", strerror(errno));
		return -1;
	}

	fd = open(cfg_dir, O_PATH | O_CLOEXEC);
	if (fd < 0) {
		log_warn("landlock open %s: %s", cfg_dir, strerror(errno));
	} else {
		if (landlock_path_beneath_add(rfd, fd, mask) < 0)
			log_warn("landlock add %s: %s",
			    cfg_dir, strerror(errno));
		close(fd);
	}

	fd = open(results_dir, O_PATH | O_CLOEXEC);
	if (fd < 0) {
		log_warn("landlock open %s: %s",
		    results_dir, strerror(errno));
	} else {
		if (landlock_path_beneath_add(rfd, fd, mask) < 0)
			log_warn("landlock add %s: %s",
			    results_dir, strerror(errno));
		close(fd);
	}

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
		log_warn("PR_SET_NO_NEW_PRIVS: %s", strerror(errno));
		close(rfd);
		return -1;
	}
	if (syscall(__NR_landlock_restrict_self, rfd, 0) < 0) {
		log_warn("landlock_restrict_self: %s", strerror(errno));
		close(rfd);
		return -1;
	}
	close(rfd);
	return 0;
}
#endif /* ACCD_HAVE_LANDLOCK */

#ifdef ACCD_HAVE_SECCOMP

#ifdef SCMP_ACT_KILL_PROCESS
#  define ACCD_SECCOMP_KILL SCMP_ACT_KILL_PROCESS
#else
#  define ACCD_SECCOMP_KILL SCMP_ACT_KILL
#endif

/*
 * Allowlist mirrors pledge("stdio rpath wpath cpath inet").  Any
 * syscall not on this list terminates the process with SIGSYS.  The
 * socket() narrowing to AF_INET / AF_INET6 is added separately below.
 *
 * libseccomp emits per-arch syscall numbers; SCMP_SYS() of a syscall
 * that doesn't exist on the build target returns __NR_SCMP_UNDEF, in
 * which case seccomp_rule_add fails with EOPNOTSUPP and we log it.
 */
static const int seccomp_allow[] = {
	/* stdio */
	SCMP_SYS(read), SCMP_SYS(write), SCMP_SYS(writev), SCMP_SYS(readv),
	SCMP_SYS(close), SCMP_SYS(fstat), SCMP_SYS(newfstatat),
	SCMP_SYS(lseek), SCMP_SYS(fcntl), SCMP_SYS(ioctl),
	SCMP_SYS(rt_sigaction), SCMP_SYS(rt_sigprocmask),
	SCMP_SYS(rt_sigreturn), SCMP_SYS(sigaltstack),
	SCMP_SYS(ppoll), SCMP_SYS(poll),
	SCMP_SYS(clock_gettime), SCMP_SYS(gettimeofday),
	SCMP_SYS(clock_nanosleep), SCMP_SYS(nanosleep),
	SCMP_SYS(getpid), SCMP_SYS(getppid),
	SCMP_SYS(getuid), SCMP_SYS(geteuid),
	SCMP_SYS(getgid), SCMP_SYS(getegid),
	SCMP_SYS(brk), SCMP_SYS(mmap), SCMP_SYS(munmap),
	SCMP_SYS(mremap), SCMP_SYS(mprotect), SCMP_SYS(madvise),
	SCMP_SYS(prctl), SCMP_SYS(arch_prctl),
	SCMP_SYS(set_robust_list), SCMP_SYS(set_tid_address),
	SCMP_SYS(futex), SCMP_SYS(getrandom),
	SCMP_SYS(sched_yield), SCMP_SYS(sched_getaffinity),
	SCMP_SYS(exit), SCMP_SYS(exit_group), SCMP_SYS(restart_syscall),

	/* rpath */
	SCMP_SYS(openat), SCMP_SYS(open),
	SCMP_SYS(pread64), SCMP_SYS(readlinkat),
	SCMP_SYS(statx), SCMP_SYS(getdents64),
	SCMP_SYS(faccessat), SCMP_SYS(faccessat2),

	/* wpath */
	SCMP_SYS(pwrite64), SCMP_SYS(ftruncate),
	SCMP_SYS(fsync), SCMP_SYS(fdatasync),

	/* cpath */
	SCMP_SYS(mkdirat), SCMP_SYS(mkdir),
	SCMP_SYS(unlinkat), SCMP_SYS(unlink),
	SCMP_SYS(renameat), SCMP_SYS(renameat2), SCMP_SYS(rename),

	/* inet (socket() arg-0 narrowed to AF_INET/INET6 below) */
	SCMP_SYS(bind), SCMP_SYS(listen),
	SCMP_SYS(accept), SCMP_SYS(accept4),
	SCMP_SYS(connect),
	SCMP_SYS(sendto), SCMP_SYS(recvfrom),
	SCMP_SYS(sendmsg), SCMP_SYS(recvmsg),
	SCMP_SYS(sendmmsg), SCMP_SYS(recvmmsg),
	SCMP_SYS(setsockopt), SCMP_SYS(getsockopt),
	SCMP_SYS(getsockname), SCMP_SYS(getpeername),
	SCMP_SYS(shutdown),
};

/* 0 on success (filter installed), -1 otherwise.  See apply_landlock
 * for the failure-path log_warn semantics. */
static int
apply_seccomp(void)
{
	scmp_filter_ctx ctx;
	size_t i;
	int rc;

	ctx = seccomp_init(ACCD_SECCOMP_KILL);
	if (!ctx) {
		log_warn("seccomp_init: failed, syscall sandbox disabled");
		return -1;
	}

	for (i = 0; i < sizeof(seccomp_allow) / sizeof(seccomp_allow[0]); i++) {
		rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW,
		    seccomp_allow[i], 0);
		if (rc < 0)
			log_warn("seccomp_rule_add #%zu: %s",
			    i, strerror(-rc));
	}

	rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socket), 1,
	    SCMP_A0(SCMP_CMP_EQ, AF_INET));
	if (rc < 0)
		log_warn("seccomp_rule_add socket AF_INET: %s",
		    strerror(-rc));
	rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socket), 1,
	    SCMP_A0(SCMP_CMP_EQ, AF_INET6));
	if (rc < 0)
		log_warn("seccomp_rule_add socket AF_INET6: %s",
		    strerror(-rc));

	rc = seccomp_load(ctx);
	if (rc < 0) {
		log_warn("seccomp_load: %s", strerror(-rc));
		seccomp_release(ctx);
		return -1;
	}
	seccomp_release(ctx);
	return 0;
}
#endif /* ACCD_HAVE_SECCOMP */

void
sandbox_apply(const char *cfg_dir, const char *results_dir)
{
	int landlock_ok = 0;
	int seccomp_ok = 0;

	(void)mkdir(results_dir, 0755);

#ifdef ACCD_HAVE_LANDLOCK
	landlock_ok = (apply_landlock(cfg_dir, results_dir) == 0);
#else
	(void)cfg_dir;
	log_warn("sandbox: landlock unavailable at build time");
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
	log_warn("sandbox: seccomp disabled under sanitizer build");
#elif defined(ACCD_HAVE_SECCOMP)
	seccomp_ok = (apply_seccomp() == 0);
#else
	log_warn("sandbox: libseccomp unavailable at build time");
#endif

	log_info("sandbox: linux landlock=%s seccomp=%s",
	    landlock_ok ? "on" : "off",
	    seccomp_ok ? "on" : "off");
}

#elif defined(__OpenBSD__)

#include <unistd.h>

/*
 * pledge(2) and unveil(2) are OpenBSD extensions declared in
 * <unistd.h> only when _POSIX_C_SOURCE is not defined.  Forward-
 * declare to keep strict-POSIX compiles clean.
 */
extern int pledge(const char *promises, const char *execpromises);
extern int unveil(const char *path, const char *permissions);

void
sandbox_apply(const char *cfg_dir, const char *results_dir)
{
	/*
	 * unveil(2) requires each path to exist when called, so
	 * precreate results/.  unveil(NULL, NULL) seals the list and
	 * pledge drops the implicit "unveil" promise OpenBSD grants
	 * while the list is open.
	 */
	(void)mkdir(results_dir, 0755);

	if (unveil(cfg_dir, "rwc") < 0)
		log_warn("unveil %s: %s", cfg_dir, strerror(errno));
	if (unveil(results_dir, "rwc") < 0)
		log_warn("unveil %s: %s", results_dir, strerror(errno));
	if (unveil(NULL, NULL) < 0)
		log_warn("unveil lock: %s", strerror(errno));

	if (pledge("stdio rpath wpath cpath inet", NULL) < 0)
		log_warn("pledge: %s", strerror(errno));
	else
		log_info("sandbox: openbsd pledge+unveil applied");
}

#else /* macOS, FreeBSD without Capsicum wiring, etc. */

void
sandbox_apply(const char *cfg_dir, const char *results_dir)
{
	(void)cfg_dir;
	(void)results_dir;
	log_warn("sandbox: no platform support, running unconfined");
}

#endif
