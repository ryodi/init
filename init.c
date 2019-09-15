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

#define VERSION "1.0"

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

	char  *command;     /* command script to run       */
	int    argc;        /* how many arguments to pass? */
	char **argv;        /* the arguments to pass       */

	pid_t pid;          /* PID of the running process. */
	                    /* set to 0 for "not running"  */
};

void append_command(struct child **chain, struct child **next, char *command, int argc, char **argv) {
	int i;

	if (*next == NULL) {
		*chain = *next = calloc(1, sizeof(struct child));
	} else {
		struct child *tmp = *next;
		*next = calloc(1, sizeof(struct child));
		tmp->next = *next;
	}

	(*next)->command = command;
	(*next)->argv = calloc(argc + 1, sizeof(char *));
	for (i = 0; i < argc; i++) {
		(*next)->argv[i] = argv[i];
	}
	(*next)->argv[argc] = NULL;
}

struct child *last_child(struct child *head) {
	while (head && head->next) {
		head = head->next;
	}
	return head;
}

int configure_from_argv(struct child **chain, int argc, char **argv) {
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
				append_command(chain, &next, command, i - start, argv+start);
			}
			start = i + 1;
			continue;
		}
	}

	return 0;
}

int configure_from_directory(struct child **chain, const char *root) {
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
		fprintf(stderr, "%s: %s\n", root, strerror(errno));
		return -1;
	}
	fd = dirfd(dir);
	if (fd < 0) {
		fprintf(stderr, "dirfd(%s): %s\n", root, strerror(errno));
		return -1;
	}

	while ((ent = readdir(dir)) != NULL) {
		/* check the fstatat() to get executable regular files only */
		struct stat st;
		rc = fstatat(fd, ent->d_name, &st, 0);
		if (rc < 0) {
			fprintf(stderr, "fstat(%s%s%s): %s\n", root, sep, ent->d_name, strerror(errno));
			return -1;
		}
		if (!S_ISREG(st.st_mode) || !(st.st_mode & 0111)) {
			continue;
		}

		rc = snprintf(buf, PATH_MAX, "%s%s%s", root, sep, ent->d_name);
		if (rc < 0) {
			fprintf(stderr, "snprintf failed: %s\n", strerror(errno));
			return -1;
		}
		if (rc >= PATH_MAX) {
			fprintf(stderr, "snprintf failed: '%s/%s' exceeds PATH_MAX of %d\n", root, ent->d_name, PATH_MAX);
			return -1;
		}

		argv[0] = strdup(ent->d_name);
		append_command(chain, &next, strdup(buf), 1, argv);
	}

	closedir(dir);
	return 0;
}

void spin(struct child *config)
{
	config->pid = fork();

	if (config->pid < 0) {
		fprintf(stderr, "fork failed: %s\n", strerror(errno));
		return;
	}

	if (config->pid == 0) {
		/* in child process; set up for an exec! */
		char *argv[2] = { NULL, NULL };
		char *envp[1] = { NULL };
		argv[0] = strrchr(config->command, '/') + 1;

		freopen("/dev/null", "r", stdin);
		freopen("/dev/null", "w", stdout);
		freopen("/dev/null", "w", stderr);

		execve(config->command, argv, envp);
		/* uh-oh, exec failed (bad binary? non-executable?
		   who knows!), and we can't error because we just
		   redirected standard error to /dev/null. -_- */
		exit(42);

	} else {
		int rc;
		struct tm *tm;
		struct timeval tv;
#define _DATE_BUF_SIZE 64
		char now[_DATE_BUF_SIZE];

		rc = gettimeofday(&tv, NULL);
		if (rc != 0) {
			fprintf(stderr, "gettimeofday(): %s\n", strerror(errno));
			strcpy(now, "UNKNOWN");

		} else {
			tm = localtime(&tv.tv_sec);
			if (!tm) {
				fprintf(stderr, "localtime(): %s\n", strerror(errno));
				strcpy(now, "UNKNOWN");

			} else {
				rc = strftime(now, 64, "%Y-%m-%d %H:%M:%S", tm);
				if (rc == 0) {
					strcpy(now, "DATE-TOO-LONG");
				} else {
					snprintf(now+rc, _DATE_BUF_SIZE - rc, ".%06li", tv.tv_usec);
				}
			}
		}
		fprintf(stderr, "[%s] exec pid %d `%s`\n", now, config->pid, config->command);
	}
}

static struct child *CONFIG;

void reaper(int sig, siginfo_t *info, void *_)
{
	struct child *chain = CONFIG;
	while (chain) {
		if (chain->pid == info->si_pid) {
			int rc;
			waitpid(chain->pid, &rc, 0);
			chain->pid = 0;
			break;
		}
		chain = chain->next; /* ooh! linked list! */
	}
}

int main(int argc, char **argv)
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
			rc = configure_from_directory(&CONFIG, optarg);
			if (rc != 0) {
				exit(1);
			}
			break;
		}
	}
	rc = configure_from_argv(&CONFIG, argc - optind, argv + optind);
	if (rc != 0) {
		exit(1);
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

			for (int i = 1; i < CONFIG->argc; i++) {
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
		fprintf(stderr, "No sub-processes identified.\nWhat shall I supervise?\n");
		exit(1);
	}

	sa.sa_sigaction = reaper;
	sa.sa_flags = SA_NOCLDSTOP | SA_SIGINFO;
	rc = sigaction(SIGCHLD, &sa, NULL);
	if (rc != 0) {
		fprintf(stderr, "failed to set up SIGCLD handler: %s\n", strerror(errno));
		return 1;
	}

#define ms(n) ((n) * 1000000)
	nap.tv_sec = 0;
	nap.tv_nsec = ms(100);
	for (;;) {
		struct child *tmp;
		tmp = CONFIG;
		while (tmp) {
			if (tmp->pid == 0 || kill(tmp->pid, 0) != 0)
				spin(tmp);
			tmp = tmp->next;
		}

		while (nanosleep(&nap, NULL) == -1);

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

	return 0;
}
