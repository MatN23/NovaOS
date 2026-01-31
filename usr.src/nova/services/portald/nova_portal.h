/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * nova-portald - Application Sandboxing Portals
 */

#ifndef _NOVA_PORTAL_H_
#define _NOVA_PORTAL_H_

#include <sys/types.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Nova Portal System
 *
 * Provides controlled access to system resources for sandboxed applications:
 * - File picker/save dialog portals
 * - Notification portal
 * - Screenshot/screencast portal
 * - Device access portal
 * - Network portal
 *
 * Sandboxing uses FreeBSD's Capsicum and optionally jails.
 */

/*
 * Portal result codes
 */

typedef enum nova_portal_result {
	NOVA_PORTAL_OK = 0,
	NOVA_PORTAL_CANCELLED, /* User cancelled */
	NOVA_PORTAL_DENIED,    /* Permission denied */
	NOVA_PORTAL_ERROR,     /* Internal error */
} nova_portal_result_t;

/*
 * File portal
 */

typedef struct nova_file_filter {
	char name[64];	 /* e.g., "Images" */
	char **patterns; /* e.g., ["*.png", "*.jpg"] */
	size_t pattern_count;
	char **mimetypes; /* e.g., ["image/png"] */
	size_t mimetype_count;
} nova_file_filter_t;

typedef struct nova_file_open_options {
	char title[128];
	bool multiple;	/* Allow multiple selection */
	bool directory; /* Select directories */
	nova_file_filter_t *filters;
	size_t filter_count;
	char current_folder[1024];
} nova_file_open_options_t;

typedef struct nova_file_save_options {
	char title[128];
	char suggested_name[256];
	nova_file_filter_t *filters;
	size_t filter_count;
	char current_folder[1024];
} nova_file_save_options_t;

/* Open file picker dialog */
nova_portal_result_t
nova_portal_open_file(const nova_file_open_options_t *options,
    char ***selected_paths, size_t *count);

/* Open save dialog */
nova_portal_result_t
nova_portal_save_file(const nova_file_save_options_t *options,
    char **selected_path);

/* Free path list */
void nova_portal_free_paths(char **paths, size_t count);

/*
 * Notification portal
 */

typedef enum nova_notification_priority {
	NOVA_NOTIFICATION_LOW,
	NOVA_NOTIFICATION_NORMAL,
	NOVA_NOTIFICATION_HIGH,
	NOVA_NOTIFICATION_URGENT,
} nova_notification_priority_t;

typedef struct nova_notification_action {
	char id[64];
	char label[128];
} nova_notification_action_t;

typedef struct nova_notification {
	char title[256];
	char body[1024];
	char icon[256]; /* Icon name or path */
	nova_notification_priority_t priority;
	nova_notification_action_t *actions;
	size_t action_count;
	int timeout_ms; /* -1 for default */
} nova_notification_t;

typedef void (*nova_notification_callback_t)(void *data,
    uint32_t notification_id, const char *action_id);

nova_portal_result_t nova_portal_notify(const nova_notification_t *notif,
    uint32_t *notification_id);

nova_portal_result_t nova_portal_close_notification(uint32_t notification_id);

void
nova_portal_set_notification_callback(nova_notification_callback_t callback,
    void *data);

/*
 * Screenshot portal
 */

typedef struct nova_screenshot_options {
	bool interactive; /* Let user select area */
	bool include_cursor;
} nova_screenshot_options_t;

nova_portal_result_t
nova_portal_screenshot(const nova_screenshot_options_t *options,
    char **image_path);

/*
 * Screencast portal
 */

typedef struct nova_screencast_options {
	bool show_cursor;
	int framerate; /* 0 for default */
} nova_screencast_options_t;

typedef struct nova_screencast_stream {
	uint32_t node_id; /* PipeWire node ID */
	int width;
	int height;
} nova_screencast_stream_t;

nova_portal_result_t
nova_portal_screencast_create(const nova_screencast_options_t *options,
    nova_screencast_stream_t *stream);

nova_portal_result_t nova_portal_screencast_destroy(uint32_t node_id);

/*
 * Camera portal
 */

nova_portal_result_t nova_portal_request_camera(bool *granted);

/*
 * Device access portal
 */

typedef enum nova_device_permission {
	NOVA_DEVICE_CAMERA,
	NOVA_DEVICE_MICROPHONE,
	NOVA_DEVICE_GPS,
	NOVA_DEVICE_GAMEPAD,
} nova_device_permission_t;

nova_portal_result_t nova_portal_request_device(nova_device_permission_t device,
    bool *granted);

/*
 * Permission queries
 */

typedef enum nova_permission_state {
	NOVA_PERMISSION_UNKNOWN,
	NOVA_PERMISSION_GRANTED,
	NOVA_PERMISSION_DENIED,
	NOVA_PERMISSION_PROMPT, /* Will prompt on use */
} nova_permission_state_t;

nova_permission_state_t nova_portal_get_permission(const char *permission_name);

/*
 * Print portal
 */

typedef struct nova_print_options {
	char title[128];
	bool preview;
} nova_print_options_t;

nova_portal_result_t nova_portal_print(const char *file_path,
    const nova_print_options_t *options);

/*
 * Open URI portal
 */

nova_portal_result_t nova_portal_open_uri(const char *uri);

/*
 * Connection to portal daemon
 */

int nova_portal_connect(void);
void nova_portal_disconnect(void);

#ifdef __cplusplus
}
#endif

#endif /* _NOVA_PORTAL_H_ */
