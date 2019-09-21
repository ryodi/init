#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <dirent.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/un.h>

#define VERSION "2.0"

/*
   `struct child` contains all of the details for each of the
   child processes that we are supervising.  Partially, this
   configuration comes from the inittab, but it also stores the
   running state of the child process, in the `pid` member.

   This is inherently list-like, since we probably need to
   supervisor more than one child process (otherwise, we wouldn't
   actually _need_ init in the first place, now would we?)
 */
struct child {
	struct child *next; /* the rest of the linked list */
	                    /* (NULL at end-of-list)       */

	int quiet;          /* send std* to /dev/null      */

	char *name;         /* a unique name, for printing */
	char *full;         /* full printable command line */

	char  *command;     /* command script to run       */
	char **argv;        /* the arguments to pass       */
	char **envp;        /* the environment to use      */

	pid_t pid;          /* PID of the running process. */
	                    /* set to 0 for "not running"  */
};

void format_datetime(char *buf, size_t n) {
	int rc;
	struct tm *tm;
	struct timeval tv;

	rc = gettimeofday(&tv, NULL);
	if (rc != 0) {
		fprintf(stderr, "init | gettimeofday() call failed: %s\n", strerror(errno));
		strcpy(buf, "UNKNOWN");
		return;
	}

	tm = localtime(&tv.tv_sec);
	if (!tm) {
		fprintf(stderr, "init | localtime() call failed: %s\n", strerror(errno));
		strcpy(buf, "UNKNOWN");
		return;
	}

	rc = strftime(buf, 64, "%Y-%m-%d %H:%M:%S", tm);
	if (rc == 0) {
		strcpy(buf, "DATE-TOO-LONG");
		return;
	}

	snprintf(buf+rc, n - rc, ".%06li", tv.tv_usec);
}

static char NOW[64];
const char * datetime() {
	format_datetime(NOW, 64);
	return NOW;
}

void append_command(struct child **chain, struct child **next, char *command, int argc, char **argv, char **envp) {
	int i, n, dn;
	struct child *kid;

	/* determine our distinguishing number _before_ we
	   append a new child struct to the chain. */
	dn = 1;
	for (kid = *chain; kid; kid = kid->next) {
		if (strcasecmp(kid->argv[0], argv[0]) == 0) {
			dn++;
		}
	}

	/* allocate a new child struct in the chain. */
	if (*next == NULL) {
		*chain = *next = calloc(1, sizeof(struct child));
	} else {
		struct child *tmp = *next;
		*next = calloc(1, sizeof(struct child));
		tmp->next = *next;
	}

	/* generate a unique name */
	n = snprintf(NULL, 0, "%s/%d", argv[0], dn);
	(*next)->name = calloc(n+1, sizeof(char));
	snprintf((*next)->name, n+1, "%s/%d", argv[0], dn);

	/* by default, all execs are quiet. */
	(*next)->quiet = 1;

	/* caller has gifted us this pointer. */
	(*next)->command = command;

	/* we may be given a subset of argv, based on argc;
	   allocate a new list and copy the pointers in. */
	(*next)->argv = calloc(argc + 1, sizeof(char *));
	for (i = 0; i < argc; i++) {
		(*next)->argv[i] = argv[i];
	}
	(*next)->argv[argc] = NULL;

	/* for now, envp is not mucked with, so we can
	   share a single environ between all of the kids. */
	(*next)->envp = envp;

	/* calculate the length of the full command */
	n = strlen(command) + 1;
	for (i = 1; (*next)->argv[i]; i++) {
		char *p;
		for (p = (*next)->argv[i]; *p; p++) {
			if (isspace(*p)) {
				n += 2; /* room for quotes... */
				break;
			}
		}
		n += strlen(argv[i]) + 1;
	}
	n--; /* remove trailing space */

	/* format the full command */
	(*next)->full = calloc(n+1, sizeof(char));
	n = sprintf((*next)->full, "%s ", command);
	for (i = 1; (*next)->argv[i]; i++) {
		char *p;
		const char *q = "";
		for (p = (*next)->argv[i]; *p; p++) {
			if (isspace(*p)) {
				q = "'";
				break;
			}
		}
		n += sprintf((*next)->full+n, "%s%s%s ", q, (*next)->argv[i], q);
	}
	(*next)->full[n] = '\0';
}

struct child *last_child(struct child *head) {
	while (head && head->next) {
		head = head->next;
	}
	return head;
}

int configure_from_argv(struct child **chain, int argc, char **argv, char **envp) {
	int start, i, rc;
	struct child *next;
	char *p, *command;

	next = last_child(*chain);
	for (start = i = 0; i <= argc; i++) {
		if (i == argc || strcmp(argv[i], "--") == 0) {
			if (start != i) {
				command = strdup(argv[start]);
				p = strrchr(argv[start], '/');
				if (p) memmove(argv[start], p+1, strlen(p));
				append_command(chain, &next, command, i - start, argv+start, envp);
			}
			start = i + 1;
			continue;
		}
	}

	return 0;
}

int configure_from_directory(struct child **chain, const char *root, char **envp) {
	DIR *dir;
	int rc, fd;
	struct dirent *ent;
	struct child *next;
	char buf[PATH_MAX], *argv[2];
	const char *sep;

	rc = 0; fd = -1;
	next = last_child(*chain);
	sep = (*root && root[strlen(root)-1] == '/') ? "" : "/";

	dir = opendir(root);
	if (!dir) {
		fprintf(stderr, "init | failed to list contents of %s: %s\n", root, strerror(errno));
		return -1;
	}
	fd = dirfd(dir);
	if (fd < 0) {
		fprintf(stderr, "init | dirfd(%s) call failed: %s\n", root, strerror(errno));
		return -1;
	}

	while ((ent = readdir(dir)) != NULL) {
		/* check the fstatat() to get executable regular files only */
		struct stat st;
		rc = fstatat(fd, ent->d_name, &st, 0);
		if (rc < 0) {
			fprintf(stderr, "init | fstat(%s%s%s) call failed: %s\n", root, sep, ent->d_name, strerror(errno));
			return -1;
		}
		if (!S_ISREG(st.st_mode) || !(st.st_mode & 0111)) {
			continue;
		}

		rc = snprintf(buf, PATH_MAX, "%s%s%s", root, sep, ent->d_name);
		if (rc < 0) {
			fprintf(stderr, "init | snprintf failed: %s\n", strerror(errno));
			return -1;
		}
		if (rc >= PATH_MAX) {
			fprintf(stderr, "init | snprintf failed: '%s/%s' exceeds PATH_MAX of %d\n", root, ent->d_name, PATH_MAX);
			return -1;
		}

		argv[0] = strdup(ent->d_name);
		append_command(chain, &next, strdup(buf), 1, argv, envp);
	}

	closedir(dir);
	return 0;
}

void spin(struct child *config) {
	config->pid = fork();

	if (config->pid < 0) {
		fprintf(stderr, "init | fork failed: %s\n", strerror(errno));
		return;
	}

	if (config->pid == 0) {
		int saw_eaccess, rc, fd;
		FILE *err;
		char *p, *q, *path, abs[PATH_MAX], *f;

		/* in child process; set up for an exec! */
		freopen("/dev/null", "r", stdin); /* we always do this */

		if (config->quiet) {
			/* first duplicate stderr as 'err' so we can still
			   emit error messages to the terminal. */
			fd = dup(fileno(stderr));
			if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
			err = fd < 0 ? NULL : fdopen(fd, "w");
			if (!err) {
				fprintf(stderr, "init | unable to duplicate stderr.  don't expect much in the way of error reporting from here on out...\n");
				err = stderr; /* it's going to get silenced anyway... */
			}

			/* then silence the "standard" descriptors. */
			freopen("/dev/null", "w", stdout);
			freopen("/dev/null", "w", stderr);
		}

		/* commands with slashes are relative-or-absolute
		   paths, and should be execed straight away without
		   consulting $PATH or a default $PATH.
		 */
		if (strchr(config->command, '/')) {
			execve(config->command, config->argv, config->envp);
			fprintf(err, "init | [%s] exec %s pid %d; execve(%s) failed: %s\n", datetime(), config->name, config->pid, config->command, strerror(errno));
			exit(-errno);
		}

		/* otherwise, we just keep ripping through $PATH
		   and attempting an execve() until one works... */
		path = getenv("PATH");
		if (!path) path = "/usr/local/bin:/bin:/usr/bin";

		saw_eaccess = 0;
		for (p = path; *p; p = q) {
			for (q = p; *q && *q != ':'; q++);

			rc = snprintf(abs, PATH_MAX, "%.*s/%s", (int)(q - p), p, config->command);
			if (rc >= PATH_MAX) {
				fprintf(err, "init | [%s] exec %s pid %d; unable to exec: '%.*s/%s' is too long of a file path name!\n", datetime(), config->name, config->pid, (int)(q - p), p, config->command);
				exit(-ENAMETOOLONG);
			}
			q++;

			execve(abs, config->argv, config->envp);
			switch (errno) {
			case EACCES: saw_eaccess = 1;
			case ENOENT:
			case ENOTDIR:
				break;
			default:
				fprintf(err, "init | [%s] exec %s pid %d; execve(%s) failed: %s\n", datetime(), config->name, config->pid, config->command, strerror(errno));
				exit(-errno);
			}
		}
		fprintf(err, "init | [%s] exec %s pid %d; execve(%s) failed: executable not found in $PATH\n", datetime(), config->name, config->pid, config->command);
		if (saw_eaccess) {
			fprintf(err, "  additionally, the following error was encountered: %s\n", strerror(EACCES));
			exit(-EACCES);
		}
		exit(1);

	} else {
		fprintf(stderr, "init | [%s] exec %s pid %d `%s`\n", datetime(), config->name, config->pid, config->command);
	}
}

static struct child *CONFIG;

void reaper(int sig, siginfo_t *info, void *_)
{
	struct child *kid = CONFIG;
	while (kid) {
		if (kid->pid == info->si_pid) {
			int rc;
			fprintf(stderr, "init | [%s] received SIGCHLD for %s pid %d\n", datetime(), kid->name, kid->pid);
			waitpid(kid->pid, &rc, 0);
			kid->pid = 0;
			break;
		}
		kid = kid->next;
	}
}

static int RUNNING = 1;

void terminator(int sig, siginfo_t *info, void *_) {
	RUNNING = 0;
	fprintf(stderr, "init | received signal %d; shutting down\n", sig);
}

const char * facility(int f) {
	switch (f) {
	case 0:  return "kern";
	case 1:  return "user";
	case 2:  return "mail";
	case 3:  return "system";
	case 4:  return "daemon";
	case 5:  return "syslog";
	case 6:  return "lpd";
	case 7:  return "nntp";
	case 8:  return "uucp";
	case 9:  return "clock";
	case 10: return "auth";
	case 11: return "ftp";
	case 12: return "ntp";
	case 13: return "audit";
	case 14: return "alert";
	case 15: return "clock";
	case 16: return "local0";
	case 17: return "local1";
	case 18: return "local2";
	case 19: return "local3";
	case 20: return "local4";
	case 21: return "local5";
	case 22: return "local6";
	case 23: return "local7";
	default: return "unknown";
	}
}

const char * severity(int s) {
	switch (s) {
	case 0:  return "emerg";
	case 1:  return "alert";
	case 2:  return "crit";
	case 3:  return "error";
	case 4:  return "warn";
	case 5:  return "notice";
	case 6:  return "info";
	case 7:  return "debug";
	default: return "unknown";
	}
}

static int parse_month(const char *s, size_t n) {
	if (n < 3) return -1;

	switch (*s++) {
	default: goto fail;
	case 'J': case 'j':
		switch (*s++) {
		default: goto fail;
		case 'A': case 'a':
			switch (*s++) {
			default: goto fail;
			case 'N': case 'n': return 0;
			}
		case 'U': case 'u':
			switch (*s++) {
			default: goto fail;
			case 'N': case 'n': return 5;
			case 'L': case 'l': return 6;
			}
		}

	case 'F': case 'f':
		switch (*s++) {
		default: goto fail;
		case 'E': case 'e':
			switch (*s++) {
			default: goto fail;
			case 'B': case 'b': return 1;
			}
		}

	case 'M': case 'm':
		switch (*s++) {
		default: goto fail;
		case 'A': case 'a':
			switch (*s++) {
			default: goto fail;
			case 'R': case 'r': return 2;
			case 'Y': case 'y': return 4;
			}
		}

	case 'A': case 'a':
		switch (*s++) {
		default: goto fail;
		case 'P': case 'p':
			switch (*s++) {
			default: goto fail;
			case 'R': case 'r': return 3;
			}
		case 'U': case 'u':
			switch (*s++) {
			default: goto fail;
			case 'G': case 'g': return 7;
			}
		}

	case 'S': case 's':
		switch (*s++) {
		default: goto fail;
		case 'E': case 'e':
			switch (*s++) {
			default: goto fail;
			case 'P': case 'p': return 8;
			}
		}

	case 'O': case 'o':
		switch (*s++) {
		default: goto fail;
		case 'C': case 'c':
			switch (*s++) {
			default: goto fail;
			case 'T': case 't': return 9;
			}
		}

	case 'N': case 'n':
		switch (*s++) {
		default: goto fail;
		case 'O': case 'o':
			switch (*s++) {
			default: goto fail;
			case 'V': case 'v': return 10;
			}
		}

	case 'D': case 'd':
		switch (*s++) {
		default: goto fail;
		case 'E': case 'e':
			switch (*s++) {
			default: goto fail;
			case 'C': case 'c': return 11;
			}
		}
	}

fail:
	return -1;
}

int drain_syslog(const char *log, int raw) {
	int fd, rc;
	struct sockaddr_un addr;

	fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0) {
		fprintf(stderr, "syslog | failed to create unix socket: %s\n", strerror(errno));
		return 1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, log, sizeof(addr.sun_path));
	unlink(log);

	umask(0);
	rc = bind(fd, (struct sockaddr *)(&addr), sizeof(addr));
	if (rc < 0) {
		fprintf(stderr, "syslog | failed to bind unix socket %s: %s\n", log, strerror(errno));
		return 1;
	}

	while (RUNNING) {
		int n, prio, year = 0, fac, sev;
		char *p, buf[8192];
		struct tm tm;

		n = recvfrom(fd, buf, 8192, 0, NULL, NULL);
		if (n < 0) {
			if (errno == EINTR) break;
			fprintf(stderr, "syslog | failed to read from unix socket %s: %s\n", log, strerror(errno));
			return 1;
		}
		buf[n] = '\0';

		for (; n && buf[n - 1] == '\0'; --n);
		if (!n) goto unknown;

		if (raw) {
			fprintf(stderr, "syslog | RAW:%s\n", buf);
		}

		/* HEADER = PRI VERSION SP TIMESTAMP SP HOSTNAME
		            SP APP-NAME SP PROCID SP MSGID             */
		p = buf;
#define dshift(d,c) ((d) = (d) * 10 + ((c) - '0'))
		if (*p++ != '<') goto unknown;
		for (prio = 0; isdigit(*p); dshift(prio, *p), p++);
		if (*p++ != '>') goto unknown;

		sev = (prio & 0x07);
		fac = (prio & 0xff) >> 3;

		memset(&tm, 0, sizeof(tm));
		tm.tm_mon = parse_month(p, buf+8192-p);
		if (tm.tm_mon >= 0) {
			/* old format: <13>Sep 18 16:37:09 root: the test message */
			p += 3;
			if (*p++ != ' ') goto unknown;
			for (tm.tm_mday = 0; isdigit(*p); dshift(tm.tm_mday, *p), p++);
			if (*p++ != ' ') goto unknown;
			for (tm.tm_hour = 0; isdigit(*p); dshift(tm.tm_hour, *p), p++);
			if (*p++ != ':') goto unknown;
			for (tm.tm_min = 0; isdigit(*p); dshift(tm.tm_min, *p), p++);
			if (*p++ != ':') goto unknown;
			for (tm.tm_sec = 0; isdigit(*p); dshift(tm.tm_sec, *p), p++);
			if (*p++ != ' ') goto unknown;

			time_t now;
			now = time(NULL);
			if (!localtime_r(&now, &tm)) {
				fprintf(stderr, "syslog | failed to get localtime(): %s\n", strerror(errno));
			} else {
				year = tm.tm_year + 1900;
			}

			char ftime[256];
			if (strftime(ftime, 256, "%Y-%m-%dT%H:%M:%S.------+--:--", &tm) == 0) {
				strcpy(ftime, "YYYY-MM-DDTHH:MM:SS.------+--:--");
			}
			fprintf(stderr, "[%s] %s.%s: %s\n", ftime, facility(fac), severity(sev), p);

		} else if (*p == '1') {
			/* new format: <13>1 2019-09-18T16:37:39.625645+00:00 a08374e6c309 root - - \
			                     [... structured ...] the test message */
			p++;
			if (*p++ != ' ') goto unknown;

			int i;
			char ftime[256];
			memset(ftime, 0, 256);
			for (i = 0; i < 255; i++) {
				ftime[i] = *p++;
				if (ftime[i] == ' ') {
					ftime[i] = '\0';
					break;
				}
			}
			p++;
			fprintf(stderr, "[%s] %s.%s: %s\n", ftime, facility(fac), severity(sev), p);

		} else {
unknown:
			fprintf(stderr, "syslog | UNRECOGNIZED FORMAT (at offset %li: '%s')\n", p-buf, p);
			fprintf(stderr, "%s\n", buf);
		}
	}

	return 0;
}

int main(int argc, char **argv, char **envp)
{
	struct sigaction sa;
	struct timespec nap;
	int rc;
	int quiet = 0;
	int rawlog = 0;
	int dry_run = 0;
	const char *log = "/dev/log";

	CONFIG = NULL;

	for (;;) {
		int c, idx = 0;
		const char *shorts = "hvnqd:L:";
		static struct option longs[] = {
			{"help",       no_argument,       0,  'h' },
			{"version",    no_argument,       0,  'v' },
			{"dry-run",    no_argument,       0,  'n' },
			{"quiet",      no_argument,       0,  'q' },
			{"directory",  required_argument, 0,  'd' },
			{"log",        required_argument, 0,  'L' },
			{"raw-log",    required_argument, 0,   1  },
			{0, 0, 0,  0 }
		};

		c = getopt_long(argc, argv, shorts, longs, &idx);
		if (c == -1)
			break;

		switch (c) {
		case 'h':
			printf("Usage: init [options] [-- command --to -run [-- or --more]]\n"
			       "Supervisor some processes, for Docker containers.\n"
			       "\n"
			       "  -h, --help       Print out a help screen.\n"
			       "  -v, --version    Print out the version of `init`\n"
			       "\n"
			       "  -n, --dry-run    Parse and print commands to be run,\n"
			       "                   but do not actually execute them.\n"
			       "\n"
			       "  -q, --quiet      Suppress output from a --dry-run.\n"
			       "\n"
			       "  -d, --directory  Process all regular executable files\n"
			       "                   (and symbolic links to the same) in a\n"
			       "                   given directory.  Can be used more\n"
			       "                   than once.\n"
			       "\n"
			       "  -L, --log        Path to the /dev/log socket, or \"\"\n"
			       "                   to skip container syslog drain.\n"
			       "\n"
			       "      --raw-log    Always dump raw syslog messages.\n"
			       "\n");
			exit(0);

		case 'v':
			printf("init v" VERSION ", Copyright (c) 2016-2019 James Hunt\n");
			exit(0);

		case 'n':
			dry_run = 1;
			break;

		case 'q':
			quiet = 1;
			break;

		case 'd':
			rc = configure_from_directory(&CONFIG, optarg, envp);
			if (rc != 0) {
				exit(1);
			}
			break;

		case 'L':
			log = optarg;
			break;

		case 1:
			rawlog = 1;
			break;
		}
	}
	rc = configure_from_argv(&CONFIG, argc - optind, argv + optind, envp);
	if (rc != 0) {
		exit(1);
	}

	{
		struct child *kid;
		for (kid = CONFIG; kid; kid = kid->next) {
				kid->quiet = quiet;
		}
	}

	if (dry_run) {
		if (quiet) {
			if (!CONFIG) exit(1);
			exit(0);
		}

		if (!CONFIG) {
			fprintf(stderr, "no processes to supervise.\n");
			exit(1);
		}

		while (CONFIG) {
			printf("%-20s | %s\n", CONFIG->name, CONFIG->full);
			CONFIG = CONFIG->next;
		}
		exit(0);
	}

	if (!CONFIG) {
		fprintf(stderr, "init | no processes identified -- what shall I supervise?\n");
		exit(1);
	}

	sa.sa_sigaction = terminator;
	sa.sa_flags = SA_SIGINFO;
	rc = sigaction(SIGINT, &sa, NULL);
	if (rc != 0) {
		fprintf(stderr, "init | failed to set up SIGINT handler: %s\n", strerror(errno));
		return 1;
	}
	rc = sigaction(SIGTERM, &sa, NULL);
	if (rc != 0) {
		fprintf(stderr, "init | failed to set up SIGTERM handler: %s\n", strerror(errno));
		return 1;
	}

	if (log && *log) {
		pid_t pid;

		pid = fork();
		if (pid < 0) {
			fprintf(stderr, "init | unable to fork syslog drain: %s\n", strerror(errno));
			return 1;
		}

		if (pid == 0) {
			fprintf(stderr, "init | draining syslog from %s...\n", log);
			drain_syslog(log, rawlog);
			fprintf(stderr, "init | syslog drain terminated.\n");
			return 0;
		}
	}

	sa.sa_sigaction = reaper;
	sa.sa_flags = SA_NOCLDSTOP | SA_SIGINFO;
	rc = sigaction(SIGCHLD, &sa, NULL);
	if (rc != 0) {
		fprintf(stderr, "init | failed to set up SIGCLD handler: %s\n", strerror(errno));
		return 1;
	}

#define ms(n) ((n) * 1000000)
	nap.tv_sec = 0;
	nap.tv_nsec = ms(100);
	while (RUNNING) {
		struct child *tmp;
		tmp = CONFIG;
		while (tmp) {
			if (tmp->pid == 0 || kill(tmp->pid, 0) != 0)
				spin(tmp);
			tmp = tmp->next;
		}

		/* if we get an EINTR, and it was from a SIGTERM / SIGINT,
		   then RUNNING should be falsish, and we wake up early. */
		while (RUNNING && nanosleep(&nap, NULL) == -1);

		if (nap.tv_sec == 0) {
			nap.tv_nsec += ms(100);
			if (nap.tv_nsec >= ms(1000)) {
				nap.tv_sec = 1;
				nap.tv_nsec %= ms(1000);
			}

		} else if (nap.tv_sec < 10) {
			nap.tv_nsec = 0;
			nap.tv_sec++;
		}
	}
#undef ms

	while (CONFIG) {
		if (CONFIG->pid > 0) {
			fprintf(stderr, "init | [%s] terminating pid %d...\n", datetime(), CONFIG->pid);
			kill(SIGTERM, CONFIG->pid);
		}
		CONFIG = CONFIG->next;
	}

	fprintf(stderr, "init | [%s] shutting down.\n", datetime());
	return 0;
}
