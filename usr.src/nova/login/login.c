/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * nova-login - Graphical Login Manager Implementation
 */

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <grp.h>
#include <login_cap.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "../lib/libnova-ui/nova_ui.h"
#include "nova_login.h"

#define PAM_SERVICE_NAME "login"
#define DEFAULT_SESSION	 "/usr/local/bin/nova-session"

/*
 * Login state
 */
typedef enum {
	LOGIN_STATE_USERNAME,
	LOGIN_STATE_PASSWORD,
	LOGIN_STATE_AUTHENTICATING,
	LOGIN_STATE_SESSION_STARTING,
	LOGIN_STATE_ERROR,
} login_state_t;

/*
 * Login manager
 */
struct nova_login {
	/* State */
	login_state_t state;
	char username[128];
	char password[256];
	char error_message[256];

	/* Available users */
	nova_user_info_t *users;
	size_t user_count;
	int selected_user;

	/* PAM */
	pam_handle_t *pam_handle;

	/* Session */
	pid_t session_pid;
	char session_cmd[256];

	/* UI */
	struct nova_ui *ui;
	struct nova_widget *username_input;
	struct nova_widget *password_input;
	struct nova_widget *login_button;
	struct nova_widget *error_label;
	struct nova_widget *user_list;

	/* Screen */
	int screen_width;
	int screen_height;

	/* Callbacks */
	nova_login_callback_t callback;
	void *callback_data;
};

/*
 * PAM conversation function
 */
static int
pam_conversation(int num_msg, const struct pam_message **msg,
    struct pam_response **resp, void *appdata_ptr)
{
	struct nova_login *login = appdata_ptr;
	struct pam_response *reply;

	if (num_msg <= 0 || num_msg > PAM_MAX_NUM_MSG)
		return PAM_CONV_ERR;

	reply = calloc(num_msg, sizeof(*reply));
	if (reply == NULL)
		return PAM_BUF_ERR;

	for (int i = 0; i < num_msg; i++) {
		switch (msg[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
			/* Password prompt */
			reply[i].resp = strdup(login->password);
			break;
		case PAM_PROMPT_ECHO_ON:
			/* Username prompt */
			reply[i].resp = strdup(login->username);
			break;
		case PAM_ERROR_MSG:
		case PAM_TEXT_INFO:
			/* Display message */
			syslog(LOG_INFO, "PAM: %s", msg[i]->msg);
			break;
		default:
			free(reply);
			return PAM_CONV_ERR;
		}
	}

	*resp = reply;
	return PAM_SUCCESS;
}

/*
 * Enumerate users
 */
static int
enumerate_users(struct nova_login *login)
{
	struct passwd *pw;
	size_t capacity = 16;

	login->users = calloc(capacity, sizeof(nova_user_info_t));
	if (login->users == NULL)
		return -1;

	setpwent();
	while ((pw = getpwent()) != NULL) {
		/* Skip system users */
		if (pw->pw_uid < 1000 || pw->pw_uid > 60000)
			continue;

		/* Skip users without valid shells */
		if (pw->pw_shell == NULL || strstr(pw->pw_shell, "nologin") ||
		    strstr(pw->pw_shell, "false"))
			continue;

		/* Resize if needed */
		if (login->user_count >= capacity) {
			capacity *= 2;
			nova_user_info_t *new_users = realloc(login->users,
			    capacity * sizeof(nova_user_info_t));
			if (new_users == NULL)
				break;
			login->users = new_users;
		}

		nova_user_info_t *user = &login->users[login->user_count++];
		user->uid = pw->pw_uid;
		user->gid = pw->pw_gid;
		strlcpy(user->username, pw->pw_name, sizeof(user->username));
		strlcpy(user->home_dir, pw->pw_dir, sizeof(user->home_dir));
		strlcpy(user->shell, pw->pw_shell, sizeof(user->shell));

		/* Get real name from GECOS */
		if (pw->pw_gecos != NULL) {
			char *comma = strchr(pw->pw_gecos, ',');
			if (comma != NULL) {
				size_t len = comma - pw->pw_gecos;
				if (len >= sizeof(user->real_name))
					len = sizeof(user->real_name) - 1;
				memcpy(user->real_name, pw->pw_gecos, len);
				user->real_name[len] = '\0';
			} else {
				strlcpy(user->real_name, pw->pw_gecos,
				    sizeof(user->real_name));
			}
		}

		/* Check for user avatar */
		snprintf(user->avatar_path, sizeof(user->avatar_path),
		    "%s/.face", pw->pw_dir);
	}
	endpwent();

	syslog(LOG_INFO, "Found %zu users", login->user_count);
	return 0;
}

/*
 * Create login UI
 */
static int
create_login_ui(struct nova_login *login)
{
	login->ui = nova_ui_create(login->screen_width, login->screen_height);
	if (login->ui == NULL)
		return -1;

	/* Username input */
	login->username_input = nova_input_create(login->ui, "Username");
	if (login->username_input != NULL) {
		nova_widget_set_rect(login->username_input,
		    (nova_rect_t) {
			.x = (login->screen_width - 300) / 2,
			.y = login->screen_height / 2 - 60,
			.width = 300,
			.height = 40,
		    });
	}

	/* Password input */
	login->password_input = nova_input_create(login->ui, "Password");
	if (login->password_input != NULL) {
		nova_widget_set_rect(login->password_input,
		    (nova_rect_t) {
			.x = (login->screen_width - 300) / 2,
			.y = login->screen_height / 2,
			.width = 300,
			.height = 40,
		    });
	}

	/* Login button */
	login->login_button = nova_button_create(login->ui, "Log In");
	if (login->login_button != NULL) {
		nova_button_set_primary(login->login_button, true);
		nova_widget_set_rect(login->login_button,
		    (nova_rect_t) {
			.x = (login->screen_width - 120) / 2,
			.y = login->screen_height / 2 + 60,
			.width = 120,
			.height = 44,
		    });
	}

	/* Error label */
	login->error_label = nova_label_create(login->ui, "");
	if (login->error_label != NULL) {
		nova_widget_set_rect(login->error_label,
		    (nova_rect_t) {
			.x = (login->screen_width - 300) / 2,
			.y = login->screen_height / 2 + 120,
			.width = 300,
			.height = 24,
		    });
		nova_widget_set_visible(login->error_label, false);
	}

	return 0;
}

/*
 * Create login manager
 */
struct nova_login *
nova_login_create(int screen_width, int screen_height)
{
	struct nova_login *login;

	login = calloc(1, sizeof(*login));
	if (login == NULL) {
		syslog(LOG_ERR, "Failed to allocate login manager");
		return NULL;
	}

	login->screen_width = screen_width;
	login->screen_height = screen_height;
	login->state = LOGIN_STATE_USERNAME;
	strlcpy(login->session_cmd, DEFAULT_SESSION,
	    sizeof(login->session_cmd));

	/* Enumerate available users */
	enumerate_users(login);

	/* Create UI */
	if (create_login_ui(login) != 0) {
		free(login->users);
		free(login);
		return NULL;
	}

	syslog(LOG_INFO, "Login manager created");
	return login;
}

/*
 * Destroy login manager
 */
void
nova_login_destroy(struct nova_login *login)
{
	if (login == NULL)
		return;

	if (login->pam_handle != NULL)
		pam_end(login->pam_handle, PAM_SUCCESS);

	nova_ui_destroy(login->ui);
	free(login->users);

	/* Clear sensitive data */
	explicit_bzero(login->password, sizeof(login->password));

	free(login);
}

/*
 * Authenticate user
 */
static int
authenticate_user(struct nova_login *login)
{
	struct pam_conv conv = {
		.conv = pam_conversation,
		.appdata_ptr = login,
	};
	int ret;

	/* Start PAM */
	ret = pam_start(PAM_SERVICE_NAME, login->username, &conv,
	    &login->pam_handle);
	if (ret != PAM_SUCCESS) {
		strlcpy(login->error_message, "PAM initialization failed",
		    sizeof(login->error_message));
		return -1;
	}

	/* Authenticate */
	ret = pam_authenticate(login->pam_handle, 0);
	if (ret != PAM_SUCCESS) {
		const char *err = pam_strerror(login->pam_handle, ret);
		strlcpy(login->error_message, err,
		    sizeof(login->error_message));
		pam_end(login->pam_handle, ret);
		login->pam_handle = NULL;
		return -1;
	}

	/* Check account */
	ret = pam_acct_mgmt(login->pam_handle, 0);
	if (ret != PAM_SUCCESS) {
		const char *err = pam_strerror(login->pam_handle, ret);
		strlcpy(login->error_message, err,
		    sizeof(login->error_message));
		pam_end(login->pam_handle, ret);
		login->pam_handle = NULL;
		return -1;
	}

	return 0;
}

/*
 * Start user session
 */
static int
start_session(struct nova_login *login)
{
	struct passwd *pw;
	pid_t pid;
	int ret;

	pw = getpwnam(login->username);
	if (pw == NULL) {
		strlcpy(login->error_message, "User not found",
		    sizeof(login->error_message));
		return -1;
	}

	/* Open PAM session */
	ret = pam_open_session(login->pam_handle, 0);
	if (ret != PAM_SUCCESS) {
		const char *err = pam_strerror(login->pam_handle, ret);
		strlcpy(login->error_message, err,
		    sizeof(login->error_message));
		return -1;
	}

	/* Set credentials */
	ret = pam_setcred(login->pam_handle, PAM_ESTABLISH_CRED);
	if (ret != PAM_SUCCESS) {
		syslog(LOG_WARNING, "pam_setcred failed: %s",
		    pam_strerror(login->pam_handle, ret));
	}

	/* Fork to start session */
	pid = fork();
	if (pid < 0) {
		strlcpy(login->error_message, "fork failed",
		    sizeof(login->error_message));
		return -1;
	}

	if (pid == 0) {
		/* Child process - run user session */

		/* Apply login class */
		login_cap_t *lc = login_getclass(pw->pw_class);
		if (setusercontext(lc, pw, pw->pw_uid,
			LOGIN_SETALL & ~LOGIN_SETPATH) != 0) {
			syslog(LOG_ERR, "setusercontext failed");
			_exit(1);
		}
		login_close(lc);

		/* Set environment */
		char *env[] = {
			NULL, /* HOME */
			NULL, /* USER */
			NULL, /* SHELL */
			NULL, /* PATH */
			NULL, /* DISPLAY */
			NULL, /* XDG_SESSION_TYPE */
			NULL, /* XDG_RUNTIME_DIR */
			NULL,
		};

		char home[256], user[256], shell[256], runtime[256];
		snprintf(home, sizeof(home), "HOME=%s", pw->pw_dir);
		snprintf(user, sizeof(user), "USER=%s", pw->pw_name);
		snprintf(shell, sizeof(shell), "SHELL=%s", pw->pw_shell);
		snprintf(runtime, sizeof(runtime),
		    "XDG_RUNTIME_DIR=/var/run/user/%d", pw->pw_uid);

		env[0] = home;
		env[1] = user;
		env[2] = shell;
		env[3] = "PATH=/usr/local/bin:/usr/bin:/bin";
		env[4] = "DISPLAY=:0";
		env[5] = "XDG_SESSION_TYPE=wayland";
		env[6] = runtime;

		/* Change to home directory */
		if (chdir(pw->pw_dir) != 0)
			chdir("/");

		/* Execute session */
		execle(login->session_cmd, login->session_cmd, NULL, env);
		syslog(LOG_ERR, "execle failed: %s", strerror(errno));
		_exit(1);
	}

	/* Parent process */
	login->session_pid = pid;
	login->state = LOGIN_STATE_SESSION_STARTING;

	syslog(LOG_INFO, "Started session for %s (pid %d)", login->username,
	    pid);

	/* Notify callback */
	if (login->callback) {
		nova_session_info_t session = {
			.pid = pid,
			.uid = pw->pw_uid,
		};
		strlcpy(session.username, login->username,
		    sizeof(session.username));
		login->callback(login->callback_data, &session);
	}

	return 0;
}

/*
 * Handle login attempt
 */
int
nova_login_attempt(struct nova_login *login, const char *username,
    const char *password)
{
	if (login == NULL || username == NULL || password == NULL)
		return -1;

	strlcpy(login->username, username, sizeof(login->username));
	strlcpy(login->password, password, sizeof(login->password));

	login->state = LOGIN_STATE_AUTHENTICATING;
	login->error_message[0] = '\0';

	/* Authenticate */
	if (authenticate_user(login) != 0) {
		login->state = LOGIN_STATE_ERROR;
		explicit_bzero(login->password, sizeof(login->password));
		return -1;
	}

	/* Clear password from memory */
	explicit_bzero(login->password, sizeof(login->password));

	/* Start session */
	if (start_session(login) != 0) {
		login->state = LOGIN_STATE_ERROR;
		return -1;
	}

	return 0;
}

/*
 * Get available users
 */
int
nova_login_get_users(struct nova_login *login, nova_user_info_t **users,
    size_t *count)
{
	if (login == NULL || users == NULL || count == NULL)
		return -1;

	*users = login->users;
	*count = login->user_count;
	return 0;
}

/*
 * Set session command
 */
void
nova_login_set_session(struct nova_login *login, const char *session_cmd)
{
	if (login != NULL && session_cmd != NULL)
		strlcpy(login->session_cmd, session_cmd,
		    sizeof(login->session_cmd));
}

/*
 * Set callback
 */
void
nova_login_set_callback(struct nova_login *login,
    nova_login_callback_t callback, void *data)
{
	if (login != NULL) {
		login->callback = callback;
		login->callback_data = data;
	}
}

/*
 * Get error message
 */
const char *
nova_login_get_error(struct nova_login *login)
{
	return login ? login->error_message : "";
}

/*
 * Check session status
 */
int
nova_login_check_session(struct nova_login *login)
{
	if (login == NULL || login->session_pid <= 0)
		return -1;

	int status;
	pid_t result = waitpid(login->session_pid, &status, WNOHANG);

	if (result == 0) {
		/* Session still running */
		return 0;
	}

	if (result > 0) {
		/* Session ended */
		syslog(LOG_INFO, "Session ended with status %d", status);

		/* Close PAM session */
		if (login->pam_handle != NULL) {
			pam_close_session(login->pam_handle, 0);
			pam_end(login->pam_handle, PAM_SUCCESS);
			login->pam_handle = NULL;
		}

		login->session_pid = 0;
		login->state = LOGIN_STATE_USERNAME;
		return 1;
	}

	return -1;
}

/*
 * Render login screen
 */
void
nova_login_render(struct nova_login *login, void *render_ctx)
{
	if (login == NULL || login->ui == NULL)
		return;

	/* Update error label */
	if (login->state == LOGIN_STATE_ERROR && login->error_label != NULL) {
		nova_label_set_text(login->error_label, login->error_message);
		nova_widget_set_visible(login->error_label, true);
	}

	/* Render UI */
	nova_ui_render(login->ui, render_ctx);
}
