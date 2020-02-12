#include <assert.h>
#include <linux/limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include "config.h"
#include "log.h"

#define CGROUP_LIMIT_MAX	((uint64_t) -1)
#define MAX_PIDS		16
#define CONF_FILE		"/etc/sapwmp.conf"
#define TASK_COMM_LEN		18	/* +2 for parentheses */
#define UNIT_NAME_LEN		128

#define _cleanup_(x) __attribute__((cleanup(x)))

struct config config;

static inline void freep(void *p) {
	free(*(void **)p);
}

int migrate(sd_bus *bus, const char *target_unit, const char *target_slice,
            size_t n_pids, pid_t *pids) {
	_cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
	_cleanup_(sd_bus_error_free) sd_bus_error bus_error = SD_BUS_ERROR_NULL;
	int r;

	r = sd_bus_message_new_method_call(
		bus,
		&m,
		"org.freedesktop.systemd1",
		"/org/freedesktop/systemd1",
		"org.freedesktop.systemd1.Manager",
		"StartTransientUnit");

	if (r < 0)
		return r;

	r = sd_bus_message_append(m, "ss", target_unit, "fail");
	if (r < 0)
		return r;

	/* Set properties */

	r = sd_bus_message_open_container(m, 'a', "(sv)");
	if (r < 0)
		return r;

	/* These scopes are for resource control only, processes must be
	 * stopped by other means, only the scope terminates*/
	r = sd_bus_message_append(m, "(sv)", "KillMode", "s", "none");
	if (r < 0)
		return r;

	r = sd_bus_message_append(m, "(sv)", "Slice", "s", target_slice);
	if (r < 0)
		return r;

	/* Parent slice will control actual limit */
	r = sd_bus_message_append(m, "(sv)", "MemoryLow", "t", CGROUP_LIMIT_MAX);
	if (r < 0)
		return r;

	/* PIDs array
	 * container nesting: (sv(a(u)))
	 */
	r = sd_bus_message_open_container(m, 'r', "sv");
	if (r < 0)
		return r;
	r = sd_bus_message_append(m, "s", "PIDs");
	if (r < 0)
		return r;

	r = sd_bus_message_open_container(m, 'v', "au");
	if (r < 0)
		return r;

	r = sd_bus_message_open_container(m, 'a', "u");
	if (r < 0)
		return r;

	for (size_t i = 0; i < n_pids; i++) {
		r = sd_bus_message_append(m, "u", (uint32_t) pids[i]);
		if (r < 0)
			return r;
	}

	r = sd_bus_message_close_container(m); /* au */
	if (r < 0)
		return r;

	r = sd_bus_message_close_container(m); /* v(au) */
	if (r < 0)
		return r;

	r = sd_bus_message_close_container(m); /* (sv) */
	if (r < 0)
		return r;

	r = sd_bus_message_close_container(m); /* properties array */
	if (r < 0)
		return r;

	/* Aux array */
        r = sd_bus_message_append(m, "a(sa(sv))", 0);
        if (r < 0)
		return r;

        r = sd_bus_call(bus, m, 0, &bus_error, NULL);
	if (r < 0) {
		log_info("DBus call error: %s\n", strerror(sd_bus_error_get_errno(&bus_error)));
	}
	/* ignore reply, i.e. don't wait for the job to finish */
	return r;
}

int read_stat(pid_t pid, pid_t *ppid, char *rcomm) {
	char path[PATH_MAX];
	char *comm = NULL, *p;
	FILE *f;
	int r;

	r = snprintf(path, PATH_MAX, "/proc/%i/stat", pid);
	if (r < 0)
		return r;

	f = fopen(path, "r");
	if (!f)
		return -errno;

	r = fscanf(f, "%*d %ms %*c %d", &comm, ppid);
	if (r < 0) {
		r = -errno;
		goto final;
	} else if (r < 2) {
		r = -EINVAL;
		goto final;
	}

	/* Strip parentheses and silently truncate if needed */
	if ((p = strrchr(comm, ')'))) {
		*p = '\0';
		strncpy(rcomm, comm+1, TASK_COMM_LEN);
	} else {
		strncpy(rcomm, comm, TASK_COMM_LEN);
	}
	rcomm[TASK_COMM_LEN] = '\0';
	r = 0;
final:
	free(comm);
	fclose(f);
	return r;
}

int collect_pids(pid_t **rpids) {
	int n_pids = 0;
	pid_t pid, ppid;
	char comm[TASK_COMM_LEN + 1]; 
	pid_t *pids;

	assert(rpids);

	pids = malloc(sizeof(pid_t) * MAX_PIDS);
	if (!pids)
		return -ENOMEM;

	pid = getppid();
	while (pid > 1 && n_pids < MAX_PIDS) {
		if (read_stat(pid, &ppid, comm))
			goto err;

		for (char **p = config.parent_commands.list; *p; p++) {
			if(!strcmp(comm, *p)) {
				pids[n_pids++] = pid;
				break;
			}
		}
		pid = ppid;
	}
	if (n_pids == MAX_PIDS && pid > 1)
		log_info("Incomplete forking hierarchy search after %i PIDs found\n", MAX_PIDS);

	*rpids = pids;
	return n_pids;

err:
	free(pids);
	return -ESRCH;
}

static int make_scope_name(char *buf) {
	sd_id128_t rnd;
	int r;
        r = sd_id128_randomize(&rnd);
	if (r < 0)
		return r;

	/* -r stands for random
	 * 128 bit should be enough for anyone to avoid collisions */
	r = snprintf(buf, UNIT_NAME_LEN,
		     "wmp-r" SD_ID128_FORMAT_STR ".scope", SD_ID128_FORMAT_VAL(rnd));
	if (r < 0)
		return r;

	return 0;
}

static void print_help(const char *name) {
	fprintf(stderr, "Usage: %s [-h] -a\n", name);
	fprintf(stderr, "       -h	Show this help\n");
	fprintf(stderr, "       -a	Put chosen ancestor processes under WMP scope\n");
	fprintf(stderr, "       	(similar to systemd-run --scope)\n");
}

int main(int argc, char *argv[]) {
	_cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
	_cleanup_(freep) pid_t *pids = NULL;
	char unit_name[UNIT_NAME_LEN];
	int opt, ancestors = 0;
	int n_pids;
	int r;

	while ((opt = getopt(argc, argv, "ah")) != -1) {
		switch (opt) {
		case 'a':
			ancestors = 1;
			break;
		case 'h':
			print_help(argv[0]);
			return EXIT_SUCCESS;
		default:
			log_error("Unknown option(s), use -h for help\n");
			return EXIT_FAILURE;
		}
	}
	if (optind < argc) {
		log_error("Unexpected argument(s), use -h for help\n");
		return EXIT_FAILURE;
	}
	if (!ancestors) {
		log_error("No action specified.\n");
		return EXIT_FAILURE;
	}

	r = config_init(&config);
	if (r < 0)
		exit_log(EXIT_FAILURE, r, "Failed config init");

	r = config_load(&config, CONF_FILE);
	if (r < 0)
		exit_log(EXIT_FAILURE, r, "Failed loading config");

	n_pids = collect_pids(&pids);
	if (n_pids < 0)
		exit_log(EXIT_FAILURE, n_pids, "Failed collecting PIDs");
	else if (n_pids == 0)
		return 0;

	r = make_scope_name(unit_name);
	if (r < 0)
		exit_log(EXIT_FAILURE, r, "Failed creating scope name");

	log_info("Found PIDs: ");
	for (int i = 0; i < n_pids; i++) {
		log_info("%i, ", pids[i]);
	}
	log_info("\n");

	r = sd_bus_open_system(&bus);
	if (r < 0) 
		exit_log(EXIT_FAILURE, r, "Failed opening DBus");

	r = migrate(bus, unit_name, config.slice, n_pids, pids);
	if (r < 0)
		exit_log(EXIT_FAILURE, r, "Failed capture into %s/%s", config.slice, unit_name);

	log_info("Successful capture into %s/%s", config.slice, unit_name);

	/* skip config_deinit */
	return 0;
}
