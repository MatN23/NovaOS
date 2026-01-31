/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * nova-serviced - Service Manager Implementation
 */

#include <sys/types.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "nova_serviced.h"

#define NOVA_SERVICED_SOCKET "/var/run/nova/serviced.sock"
#define MAX_SERVICES	     32

/*
 * Service states
 */
typedef enum {
	SVC_STATE_STOPPED,
	SVC_STATE_STARTING,
	SVC_STATE_RUNNING,
	SVC_STATE_STOPPING,
	SVC_STATE_FAILED,
} svc_state_t;

/*
 * Internal service structure
 */
struct nova_service_internal {
	char name[64];
	char exec[256];
	char description[128];
	nova_service_type_t type;
	svc_state_t state;
	pid_t pid;
	int restart_count;
	time_t last_start;
	time_t last_stop;

	/* Dependencies */
	char **
		requires;
	size_t requires_count;
	char **wants;
	size_t wants_count;

	/* Restart policy */
	nova_restart_policy_t restart_policy;
	int restart_delay_ms;
	int max_restarts;
};

/*
 * Service manager state
 */
struct nova_serviced {
	/* Event loop */
	int kq;
	volatile bool running;

	/* IPC socket */
	int sock_fd;
	char sock_path[256];

	/* Services */
	struct nova_service_internal *services[MAX_SERVICES];
	size_t service_count;

	/* Session info */
	uid_t session_uid;
	char session_name[64];

	/* Callbacks */
	nova_service_event_callback_t event_callback;
	void *event_callback_data;
};

/*
 * Forward declarations
 */
static int serviced_start_service(struct nova_serviced *sd,
    struct nova_service_internal *svc);
static int serviced_stop_service(struct nova_serviced *sd,
    struct nova_service_internal *svc);

/*
 * Create service manager
 */
struct nova_serviced *
nova_serviced_create(const nova_serviced_config_t *config)
{
	struct nova_serviced *sd;
	struct sockaddr_un addr;

	sd = calloc(1, sizeof(*sd));
	if (sd == NULL) {
		syslog(LOG_ERR, "Failed to allocate service manager");
		return NULL;
	}

	sd->sock_fd = -1;
	sd->session_uid = config->session_uid;
	strlcpy(sd->session_name, config->session_name,
	    sizeof(sd->session_name));

	/* Create kqueue */
	sd->kq = kqueue();
	if (sd->kq < 0) {
		syslog(LOG_ERR, "Failed to create kqueue: %s", strerror(errno));
		free(sd);
		return NULL;
	}

	/* Create IPC socket */
	sd->sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sd->sock_fd < 0) {
		syslog(LOG_ERR, "Failed to create socket: %s", strerror(errno));
		close(sd->kq);
		free(sd);
		return NULL;
	}

	/* Set socket path */
	if (config->socket_path != NULL)
		strlcpy(sd->sock_path, config->socket_path,
		    sizeof(sd->sock_path));
	else
		strlcpy(sd->sock_path, NOVA_SERVICED_SOCKET,
		    sizeof(sd->sock_path));

	/* Bind socket */
	unlink(sd->sock_path); /* Remove stale socket */

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strlcpy(addr.sun_path, sd->sock_path, sizeof(addr.sun_path));

	if (bind(sd->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		syslog(LOG_ERR, "Failed to bind socket: %s", strerror(errno));
		close(sd->sock_fd);
		close(sd->kq);
		free(sd);
		return NULL;
	}

	if (listen(sd->sock_fd, 5) < 0) {
		syslog(LOG_ERR, "Failed to listen on socket: %s",
		    strerror(errno));
		unlink(sd->sock_path);
		close(sd->sock_fd);
		close(sd->kq);
		free(sd);
		return NULL;
	}

	syslog(LOG_INFO, "Service manager created, socket: %s", sd->sock_path);
	return sd;
}

/*
 * Destroy service manager
 */
void
nova_serviced_destroy(struct nova_serviced *sd)
{
	size_t i;

	if (sd == NULL)
		return;

	/* Stop all services */
	for (i = 0; i < sd->service_count; i++) {
		if (sd->services[i] != NULL) {
			serviced_stop_service(sd, sd->services[i]);
			free(sd->services[i]->requires);
			free(sd->services[i]->wants);
			free(sd->services[i]);
		}
	}

	/* Clean up socket */
	if (sd->sock_fd >= 0) {
		close(sd->sock_fd);
		unlink(sd->sock_path);
	}

	if (sd->kq >= 0)
		close(sd->kq);

	free(sd);
}

/*
 * Run service manager main loop
 */
int
nova_serviced_run(struct nova_serviced *sd)
{
	struct kevent events[16];
	int nevents;

	sd->running = true;

	syslog(LOG_INFO, "Service manager entering main loop");

	while (sd->running) {
		nevents = kevent(sd->kq, NULL, 0, events, 16, NULL);

		if (nevents < 0) {
			if (errno == EINTR)
				continue;
			syslog(LOG_ERR, "kevent error: %s", strerror(errno));
			return -1;
		}

		for (int i = 0; i < nevents; i++) {
			/* Handle SIGCHLD for process exits */
			if (events[i].filter == EVFILT_SIGNAL) {
				int status;
				pid_t pid;

				while (
				    (pid = waitpid(-1, &status, WNOHANG)) > 0) {
					/* Find service by PID and update state
					 */
					for (size_t j = 0;
					    j < sd->service_count; j++) {
						if (sd->services[j] != NULL &&
						    sd->services[j]->pid ==
							pid) {
							sd->services[j]->state =
							    SVC_STATE_STOPPED;
							sd->services[j]->pid =
							    0;
							syslog(LOG_INFO,
							    "Service %s exited",
							    sd->services[j]
								->name);
						}
					}
				}
			}
		}
	}

	return 0;
}

/*
 * Request shutdown
 */
void
nova_serviced_shutdown(struct nova_serviced *sd)
{
	if (sd != NULL)
		sd->running = false;
}

/*
 * Register a service
 */
int
nova_serviced_register(struct nova_serviced *sd, const nova_service_def_t *def)
{
	struct nova_service_internal *svc;

	if (sd == NULL || def == NULL)
		return -1;

	if (sd->service_count >= MAX_SERVICES) {
		syslog(LOG_ERR, "Maximum service count reached");
		return -1;
	}

	svc = calloc(1, sizeof(*svc));
	if (svc == NULL)
		return -1;

	strlcpy(svc->name, def->name, sizeof(svc->name));
	strlcpy(svc->exec, def->exec, sizeof(svc->exec));
	strlcpy(svc->description, def->description, sizeof(svc->description));
	svc->type = def->type;
	svc->restart_policy = def->restart_policy;
	svc->restart_delay_ms = def->restart_delay_ms;
	svc->max_restarts = def->max_restarts;
	svc->state = SVC_STATE_STOPPED;

	sd->services[sd->service_count++] = svc;

	syslog(LOG_INFO, "Registered service: %s", svc->name);
	return 0;
}

/*
 * Start a service
 */
static int
serviced_start_service(struct nova_serviced *sd,
    struct nova_service_internal *svc)
{
	pid_t pid;

	if (svc->state == SVC_STATE_RUNNING)
		return 0;

	svc->state = SVC_STATE_STARTING;

	pid = fork();
	if (pid < 0) {
		syslog(LOG_ERR, "Failed to fork for service %s: %s", svc->name,
		    strerror(errno));
		svc->state = SVC_STATE_FAILED;
		return -1;
	}

	if (pid == 0) {
		/* Child process */
		execl("/bin/sh", "sh", "-c", svc->exec, NULL);
		_exit(127);
	}

	/* Parent */
	svc->pid = pid;
	svc->state = SVC_STATE_RUNNING;
	svc->last_start = time(NULL);
	svc->restart_count = 0;

	syslog(LOG_INFO, "Started service %s (PID %d)", svc->name, pid);
	return 0;
}

/*
 * Stop a service
 */
static int
serviced_stop_service(struct nova_serviced *sd,
    struct nova_service_internal *svc)
{
	if (svc->pid <= 0 || svc->state != SVC_STATE_RUNNING)
		return 0;

	svc->state = SVC_STATE_STOPPING;

	/* Send SIGTERM first */
	if (kill(svc->pid, SIGTERM) < 0) {
		syslog(LOG_WARNING, "Failed to send SIGTERM to %s: %s",
		    svc->name, strerror(errno));
	}

	/* TODO: Wait for graceful shutdown, then SIGKILL if needed */

	svc->state = SVC_STATE_STOPPED;
	svc->last_stop = time(NULL);

	syslog(LOG_INFO, "Stopped service %s", svc->name);
	return 0;
}

/*
 * Control a service
 */
int
nova_serviced_control(struct nova_serviced *sd, const char *name,
    nova_service_action_t action)
{
	size_t i;
	struct nova_service_internal *svc = NULL;

	if (sd == NULL || name == NULL)
		return -1;

	/* Find service */
	for (i = 0; i < sd->service_count; i++) {
		if (sd->services[i] != NULL &&
		    strcmp(sd->services[i]->name, name) == 0) {
			svc = sd->services[i];
			break;
		}
	}

	if (svc == NULL) {
		syslog(LOG_WARNING, "Service not found: %s", name);
		return -1;
	}

	switch (action) {
	case NOVA_SERVICE_START:
		return serviced_start_service(sd, svc);
	case NOVA_SERVICE_STOP:
		return serviced_stop_service(sd, svc);
	case NOVA_SERVICE_RESTART:
		serviced_stop_service(sd, svc);
		return serviced_start_service(sd, svc);
	default:
		return -1;
	}
}

/*
 * Get service status
 */
int
nova_serviced_get_status(struct nova_serviced *sd, const char *name,
    nova_service_status_t *status)
{
	size_t i;
	struct nova_service_internal *svc = NULL;

	if (sd == NULL || name == NULL || status == NULL)
		return -1;

	for (i = 0; i < sd->service_count; i++) {
		if (sd->services[i] != NULL &&
		    strcmp(sd->services[i]->name, name) == 0) {
			svc = sd->services[i];
			break;
		}
	}

	if (svc == NULL)
		return -1;

	memset(status, 0, sizeof(*status));
	strlcpy(status->name, svc->name, sizeof(status->name));

	switch (svc->state) {
	case SVC_STATE_RUNNING:
		status->state = NOVA_SERVICE_RUNNING;
		break;
	case SVC_STATE_STARTING:
		status->state = NOVA_SERVICE_STARTING;
		break;
	case SVC_STATE_STOPPING:
		status->state = NOVA_SERVICE_STOPPING;
		break;
	case SVC_STATE_FAILED:
		status->state = NOVA_SERVICE_FAILED;
		break;
	default:
		status->state = NOVA_SERVICE_STOPPED;
		break;
	}

	status->pid = svc->pid;
	status->restart_count = svc->restart_count;

	return 0;
}

/*
 * List all services
 */
int
nova_serviced_list(struct nova_serviced *sd, nova_service_status_t **services,
    size_t *count)
{
	nova_service_status_t *list;
	size_t i, n = 0;

	if (sd == NULL || services == NULL || count == NULL)
		return -1;

	list = calloc(sd->service_count, sizeof(*list));
	if (list == NULL)
		return -1;

	for (i = 0; i < sd->service_count; i++) {
		if (sd->services[i] != NULL) {
			nova_serviced_get_status(sd, sd->services[i]->name,
			    &list[n]);
			n++;
		}
	}

	*services = list;
	*count = n;
	return 0;
}

void
nova_serviced_free_list(nova_service_status_t *services)
{
	free(services);
}

/*
 * Set event callback
 */
void
nova_serviced_set_callback(struct nova_serviced *sd,
    nova_service_event_callback_t callback, void *data)
{
	if (sd != NULL) {
		sd->event_callback = callback;
		sd->event_callback_data = data;
	}
}
