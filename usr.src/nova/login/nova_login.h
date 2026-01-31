/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * nova-login - Graphical Login Manager
 */

#ifndef _NOVA_LOGIN_H_
#define _NOVA_LOGIN_H_

#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Nova Login Manager
 *
 * Graphical login manager for Nova Desktop:
 * - User authentication
 * - Session selection
 * - Power controls
 * - Accessibility options
 */

/*
 * Session types
 */

typedef struct nova_session_entry {
	char id[64];
	char name[128];
	char exec[256];
	char icon[256];
	bool is_default;
} nova_session_entry_t;

/*
 * User information
 */

typedef struct nova_user_entry {
	uid_t uid;
	char username[64];
	char display_name[128];
	char home[256];
	char avatar[256];
	bool logged_in;
} nova_user_entry_t;

/*
 * Login manager handle
 */

struct nova_login;

/*
 * Initialization
 */

typedef struct nova_login_config {
	int vt;			     /* Virtual terminal number */
	const char *theme;	     /* Login screen theme */
	bool auto_login;	     /* Enable auto-login */
	const char *auto_login_user; /* User for auto-login */
	int auto_login_delay;	     /* Delay before auto-login (seconds) */
} nova_login_config_t;

struct nova_login *nova_login_create(const nova_login_config_t *config);
void nova_login_destroy(struct nova_login *login);

/* Run the login manager */
int nova_login_run(struct nova_login *login);

/*
 * User management
 */

int nova_login_list_users(struct nova_login *login, nova_user_entry_t **users,
    size_t *count);

void nova_login_free_users(nova_user_entry_t *users);

/* Select user to log in */
int nova_login_select_user(struct nova_login *login, const char *username);

/*
 * Session management
 */

int nova_login_list_sessions(struct nova_login *login,
    nova_session_entry_t **sessions, size_t *count);

void nova_login_free_sessions(nova_session_entry_t *sessions);

/* Select session type */
int nova_login_select_session(struct nova_login *login, const char *session_id);

/*
 * Authentication
 */

typedef enum nova_auth_result {
	NOVA_AUTH_SUCCESS,
	NOVA_AUTH_FAILURE,
	NOVA_AUTH_LOCKED,  /* Account locked */
	NOVA_AUTH_EXPIRED, /* Password expired */
} nova_auth_result_t;

nova_auth_result_t nova_login_authenticate(struct nova_login *login,
    const char *username, const char *password);

/* Start the user session after successful authentication */
int nova_login_start_session(struct nova_login *login);

/*
 * Power controls
 */

int nova_login_power_off(struct nova_login *login);
int nova_login_reboot(struct nova_login *login);
int nova_login_suspend(struct nova_login *login);

/*
 * UI callbacks
 */

typedef void (*nova_login_ui_callback_t)(void *data, const char *event,
    const void *event_data);

void nova_login_set_ui_callback(struct nova_login *login,
    nova_login_ui_callback_t callback, void *data);

#ifdef __cplusplus
}
#endif

#endif /* _NOVA_LOGIN_H_ */
