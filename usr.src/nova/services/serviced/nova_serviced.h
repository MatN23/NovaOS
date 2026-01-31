/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * nova-serviced - Desktop Service Manager
 */

#ifndef _NOVA_SERVICED_H_
#define _NOVA_SERVICED_H_

#include <sys/types.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Nova Service Manager
 *
 * Lightweight service orchestration for desktop session services:
 * - Dependency-ordered startup
 * - Health monitoring and automatic restart
 * - Clean shutdown coordination
 * - Centralized logging
 *
 * This manages user session services only; system services remain
 * under FreeBSD's rc(8) system.
 */

/*
 * Service states
 */

typedef enum nova_service_state {
	NOVA_SERVICE_STOPPED,  /* Not running */
	NOVA_SERVICE_STARTING, /* Starting up */
	NOVA_SERVICE_RUNNING,  /* Running and healthy */
	NOVA_SERVICE_STOPPING, /* Shutting down */
	NOVA_SERVICE_FAILED,   /* Failed to start or crashed */
} nova_service_state_t;

/*
 * Service information
 */

typedef struct nova_service_info {
	char name[64];
	char description[256];
	nova_service_state_t state;
	pid_t pid;
	uint64_t start_time; /* Unix timestamp */
	uint64_t uptime_ms;
	int restart_count;
	int exit_code; /* Last exit code if failed */
} nova_service_info_t;

/*
 * Service control operations
 */

/* Start a service by name */
int nova_service_start(const char *name);

/* Stop a service by name */
int nova_service_stop(const char *name);

/* Restart a service */
int nova_service_restart(const char *name);

/* Reload service configuration (if supported) */
int nova_service_reload(const char *name);

/*
 * Service queries
 */

/* Get information about a specific service */
int nova_service_status(const char *name, nova_service_info_t *info);

/* List all registered services */
int nova_service_list(nova_service_info_t **services, size_t *count);

/* Free service list */
void nova_service_list_free(nova_service_info_t *services);

/*
 * Service enable/disable (persistent)
 */

/* Enable service to start at session login */
int nova_service_enable(const char *name);

/* Disable service autostart */
int nova_service_disable(const char *name);

/* Check if service is enabled */
bool nova_service_is_enabled(const char *name);

/*
 * Logging
 */

typedef struct nova_log_entry {
	uint64_t timestamp; /* Unix timestamp (microseconds) */
	char service[64];
	int priority; /* syslog priority */
	char *message;
} nova_log_entry_t;

/* Get log entries for a service */
int nova_service_logs(const char *name, nova_log_entry_t **entries,
    size_t *count, size_t limit);

/* Free log entries */
void nova_service_logs_free(nova_log_entry_t *entries, size_t count);

/* Follow logs in real-time (returns fd for reading) */
int nova_service_logs_follow(const char *name);

/*
 * Session management
 */

/* Start the entire desktop session (called by login manager) */
int nova_session_start(void);

/* Stop the entire desktop session */
int nova_session_stop(void);

/* Lock the session */
int nova_session_lock(void);

/* Unlock the session */
int nova_session_unlock(void);

/*
 * Event callbacks
 */

typedef void (*nova_service_state_callback_t)(void *data, const char *service,
    nova_service_state_t old_state, nova_service_state_t new_state);

void nova_serviced_set_callback(nova_service_state_callback_t callback,
    void *data);

/*
 * Connection to service manager daemon
 */

/* Connect to nova-serviced */
int nova_serviced_connect(void);

/* Disconnect from nova-serviced */
void nova_serviced_disconnect(void);

/* Process pending events from service manager */
int nova_serviced_dispatch(void);

/* Get file descriptor for event polling */
int nova_serviced_get_fd(void);

#ifdef __cplusplus
}
#endif

#endif /* _NOVA_SERVICED_H_ */
