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
	int i;

	if (*next == NULL) {
		*chain = *next = calloc(1, sizeof(struct child));
	} else {
		struct child *tmp = *next;
		*next = calloc(1, sizeof(struct child));
		tmp->next = *next;
	}

	(*next)->quiet = 1;
	(*next)->command = command;
	(*next)->argv = calloc(argc + 1, sizeof(char *));
	for (i = 0; i < argc; i++) {
		(*next)->argv[i] = argv[i];
	}
	(*next)->argv[argc] = NULL;
	(*next)->envp = envp;
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
			fprintf(err, "execve(%s) failed: %s\n", config->command, strerror(errno));
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
				fprintf(err, "'%.*s/%s' is too long of a file path name!\n", (int)(q - p), p, config->command);
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
				fprintf(err, "execve(%s) failed: %s\n", config->command, strerror(errno));
				exit(-errno);
			}
		}
		fprintf(err, "execve(%s) failed: executable not found in $PATH\n", config->command);
		if (saw_eaccess) {
			fprintf(err, "  additionally, the following error was encountered: %s\n", strerror(EACCES));
			exit(-EACCES);
		}
		exit(1);

	} else {
		fprintf(stderr, "init | [%s] exec pid %d `%s`\n", datetime(), config->pid, config->command);
	}
}

static struct child *CONFIG;

void reaper(int sig, siginfo_t *info, void *_)
{
	struct child *kid = CONFIG;
	while (kid) {
		if (kid->pid == info->si_pid) {
			int rc;
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

int main(int argc, char **argv, char **envp)
{
	struct sigaction sa;
	struct timespec nap;
	int rc;
	int quiet = 0;
	int dry_run = 0;

	CONFIG = NULL;

	for (;;) {
		int c, idx = 0;
		const char *shorts = "hvnqd:";
		static struct option longs[] = {
			{"help",       no_argument,       0,  'h' },
			{"version",    no_argument,       0,  'v' },
			{"dry-run",    no_argument,       0,  'n' },
			{"quiet",      no_argument,       0,  'q' },
			{"directory",  required_argument, 0,  'd' },
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
			printf("%s: %s ", CONFIG->argv[0], CONFIG->command);

			for (int i = 1; CONFIG->argv[i]; i++) {
				char *p;
				const char *q = "";
				for (p = CONFIG->argv[i]; *p; p++) {
					if (isspace(*p)) {
						q = "'";
						break;
					}
				}
				printf("%s%s%s ", q, CONFIG->argv[i], q);
			}
			printf("\n");
			CONFIG = CONFIG->next;
		}
		exit(0);
	}

	if (!CONFIG) {
		fprintf(stderr, "init | no processes identified -- what shall I supervise?\n");
		exit(1);
	}

	sa.sa_sigaction = reaper;
	sa.sa_flags = SA_NOCLDSTOP | SA_SIGINFO;
	rc = sigaction(SIGCHLD, &sa, NULL);
	if (rc != 0) {
		fprintf(stderr, "init | failed to set up SIGCLD handler: %s\n", strerror(errno));
		return 1;
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
