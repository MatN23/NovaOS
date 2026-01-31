/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * NovaShell - Desktop Environment Implementation
 */

#include <sys/types.h>
#include <sys/time.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "nova_shell.h"

#define MAX_NOTIFICATIONS   50
#define MAX_WORKSPACES	    16
#define MAX_WINDOWS	    256
#define PANEL_HEIGHT	    32
#define NOTIFICATION_WIDTH  350
#define NOTIFICATION_HEIGHT 80

/*
 * Panel widget types
 */
typedef enum {
	WIDGET_APP_MENU,
	WIDGET_WINDOW_LIST,
	WIDGET_WORKSPACE_SWITCHER,
	WIDGET_SYSTEM_TRAY,
	WIDGET_CLOCK,
	WIDGET_BATTERY,
	WIDGET_VOLUME,
	WIDGET_NETWORK,
	WIDGET_NOTIFICATIONS,
} widget_type_t;

/*
 * Panel widget
 */
struct panel_widget {
	widget_type_t type;
	int x, y;
	int width, height;
	bool visible;
	void *data;
};

/*
 * Panel
 */
struct nova_panel_internal {
	nova_panel_position_t position;
	int height;
	bool auto_hide;
	bool visible;
	float opacity;

	struct panel_widget *widgets;
	size_t widget_count;

	/* Clock widget data */
	char clock_text[32];
	time_t last_clock_update;
};

/*
 * Notification
 */
struct nova_notification_internal {
	uint32_t id;
	char title[128];
	char body[512];
	char icon[256];
	nova_notification_urgency_t urgency;
	int timeout_ms;
	time_t created;
	bool dismissed;

	/* Position for animation */
	int target_y;
	float current_y;
	float opacity;
};

/*
 * Window in the shell
 */
struct nova_window_internal {
	uint32_t id;
	char title[256];
	char app_id[128];
	int x, y;
	int width, height;
	nova_window_state_t state;
	uint32_t workspace;
	bool focused;
	bool decorated;

	/* Animation state */
	float target_x, target_y;
	float current_scale;
	float target_scale;
};

/*
 * Workspace
 */
struct nova_workspace_internal {
	uint32_t id;
	char name[64];
	struct nova_window_internal **windows;
	size_t window_count;
	bool active;
};

/*
 * Launcher item
 */
struct nova_launcher_item {
	char name[128];
	char icon[256];
	char exec[512];
	char categories[256];
	bool favorite;
	int launch_count;
};

/*
 * NovaShell state
 */
struct nova_shell {
	/* Configuration */
	nova_shell_config_t config;

	/* Panel */
	struct nova_panel_internal panel;

	/* Launcher */
	bool launcher_visible;
	char launcher_search[256];
	struct nova_launcher_item *launcher_items;
	size_t launcher_item_count;
	int launcher_selected;

	/* Notifications */
	struct nova_notification_internal *notifications[MAX_NOTIFICATIONS];
	size_t notification_count;
	uint32_t next_notification_id;

	/* Workspaces */
	struct nova_workspace_internal *workspaces[MAX_WORKSPACES];
	size_t workspace_count;
	uint32_t active_workspace;

	/* Windows */
	struct nova_window_internal *windows[MAX_WINDOWS];
	size_t window_count;
	uint32_t focused_window;
	uint32_t next_window_id;

	/* Theme */
	nova_shell_theme_t theme;

	/* Screen dimensions */
	int screen_width;
	int screen_height;

	/* Animation state */
	bool animating;
	uint64_t last_frame_time;

	/* Callbacks */
	nova_window_callback_t window_callback;
	void *window_callback_data;
	nova_workspace_callback_t workspace_callback;
	void *workspace_callback_data;
	nova_launcher_callback_t launcher_callback;
	void *launcher_callback_data;
};

/*
 * Default theme colors
 */
static nova_shell_theme_t default_theme = {
	.panel_background = { 0.1f, 0.1f, 0.12f, 0.95f },
	.panel_text = { 1.0f, 1.0f, 1.0f, 1.0f },
	.window_active_border = { 0.4f, 0.6f, 1.0f, 1.0f },
	.window_inactive_border = { 0.3f, 0.3f, 0.35f, 1.0f },
	.window_title_background = { 0.15f, 0.15f, 0.18f, 1.0f },
	.accent_color = { 0.4f, 0.6f, 1.0f, 1.0f },
	.notification_background = { 0.12f, 0.12f, 0.15f, 0.98f },
	.launcher_background = { 0.08f, 0.08f, 0.1f, 0.98f },
	.font_family = "Inter",
	.font_size = 13,
	.border_radius = 8,
	.animation_duration_ms = 200,
};

/*
 * Initialize panel
 */
static int
init_panel(struct nova_shell *shell)
{
	struct nova_panel_internal *panel = &shell->panel;

	panel->position = shell->config.panel_position;
	panel->height = PANEL_HEIGHT;
	panel->auto_hide = shell->config.panel_auto_hide;
	panel->visible = true;
	panel->opacity = 1.0f;

	/* Allocate widgets */
	panel->widget_count = 9;
	panel->widgets = calloc(panel->widget_count,
	    sizeof(struct panel_widget));
	if (panel->widgets == NULL)
		return -1;

	int x = 8;

	/* App menu */
	panel->widgets[0].type = WIDGET_APP_MENU;
	panel->widgets[0].x = x;
	panel->widgets[0].width = 32;
	panel->widgets[0].visible = true;
	x += 40;

	/* Window list (takes remaining space) */
	panel->widgets[1].type = WIDGET_WINDOW_LIST;
	panel->widgets[1].x = x;
	panel->widgets[1].width = shell->screen_width - 400;
	panel->widgets[1].visible = true;

	/* Right side widgets */
	x = shell->screen_width - 300;

	/* System tray */
	panel->widgets[2].type = WIDGET_SYSTEM_TRAY;
	panel->widgets[2].x = x;
	panel->widgets[2].width = 80;
	panel->widgets[2].visible = true;
	x += 88;

	/* Volume */
	panel->widgets[3].type = WIDGET_VOLUME;
	panel->widgets[3].x = x;
	panel->widgets[3].width = 24;
	panel->widgets[3].visible = true;
	x += 32;

	/* Network */
	panel->widgets[4].type = WIDGET_NETWORK;
	panel->widgets[4].x = x;
	panel->widgets[4].width = 24;
	panel->widgets[4].visible = true;
	x += 32;

	/* Battery */
	panel->widgets[5].type = WIDGET_BATTERY;
	panel->widgets[5].x = x;
	panel->widgets[5].width = 32;
	panel->widgets[5].visible = true;
	x += 40;

	/* Clock */
	panel->widgets[6].type = WIDGET_CLOCK;
	panel->widgets[6].x = x;
	panel->widgets[6].width = 80;
	panel->widgets[6].visible = true;
	x += 88;

	/* Notifications */
	panel->widgets[7].type = WIDGET_NOTIFICATIONS;
	panel->widgets[7].x = x;
	panel->widgets[7].width = 24;
	panel->widgets[7].visible = true;

	return 0;
}

/*
 * Initialize default workspaces
 */
static int
init_workspaces(struct nova_shell *shell)
{
	for (int i = 0; i < 4; i++) {
		struct nova_workspace_internal *ws = calloc(1,
		    sizeof(struct nova_workspace_internal));
		if (ws == NULL)
			return -1;

		ws->id = i;
		snprintf(ws->name, sizeof(ws->name), "Workspace %d", i + 1);
		ws->active = (i == 0);

		shell->workspaces[i] = ws;
		shell->workspace_count++;
	}

	shell->active_workspace = 0;
	return 0;
}

/*
 * Create shell
 */
struct nova_shell *
nova_shell_create(const nova_shell_config_t *config)
{
	struct nova_shell *shell;

	shell = calloc(1, sizeof(*shell));
	if (shell == NULL) {
		syslog(LOG_ERR, "Failed to allocate shell");
		return NULL;
	}

	/* Copy configuration */
	if (config != NULL) {
		memcpy(&shell->config, config, sizeof(*config));
	} else {
		shell->config.panel_position = NOVA_PANEL_TOP;
		shell->config.panel_auto_hide = false;
		shell->config.show_window_previews = true;
		shell->config.animation_speed = 1.0f;
	}

	/* Set screen size (TODO: Get from compositor) */
	shell->screen_width = 1920;
	shell->screen_height = 1080;

	/* Initialize theme */
	memcpy(&shell->theme, &default_theme, sizeof(shell->theme));

	/* Initialize panel */
	if (init_panel(shell) != 0) {
		free(shell);
		return NULL;
	}

	/* Initialize workspaces */
	if (init_workspaces(shell) != 0) {
		free(shell->panel.widgets);
		free(shell);
		return NULL;
	}

	/* Initialize IDs */
	shell->next_notification_id = 1;
	shell->next_window_id = 1;

	syslog(LOG_INFO, "NovaShell created: %dx%d", shell->screen_width,
	    shell->screen_height);

	return shell;
}

/*
 * Destroy shell
 */
void
nova_shell_destroy(struct nova_shell *shell)
{
	if (shell == NULL)
		return;

	/* Free notifications */
	for (size_t i = 0; i < MAX_NOTIFICATIONS; i++) {
		free(shell->notifications[i]);
	}

	/* Free workspaces */
	for (size_t i = 0; i < MAX_WORKSPACES; i++) {
		if (shell->workspaces[i] != NULL) {
			free(shell->workspaces[i]->windows);
			free(shell->workspaces[i]);
		}
	}

	/* Free windows */
	for (size_t i = 0; i < MAX_WINDOWS; i++) {
		free(shell->windows[i]);
	}

	/* Free launcher items */
	free(shell->launcher_items);

	/* Free panel */
	free(shell->panel.widgets);

	free(shell);
}

/*
 * Update clock text
 */
static void
update_clock(struct nova_shell *shell)
{
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);

	if (now != shell->panel.last_clock_update) {
		strftime(shell->panel.clock_text,
		    sizeof(shell->panel.clock_text), "%H:%M", tm);
		shell->panel.last_clock_update = now;
	}
}

/*
 * Animate notifications
 */
static void
animate_notifications(struct nova_shell *shell, float dt)
{
	int target_y = PANEL_HEIGHT + 16;

	for (size_t i = 0; i < shell->notification_count; i++) {
		struct nova_notification_internal *notif =
		    shell->notifications[i];
		if (notif == NULL || notif->dismissed)
			continue;

		notif->target_y = target_y;

		/* Smooth animation */
		float diff = notif->target_y - notif->current_y;
		notif->current_y += diff * dt * 10.0f;

		/* Fade in */
		if (notif->opacity < 1.0f) {
			notif->opacity += dt * 4.0f;
			if (notif->opacity > 1.0f)
				notif->opacity = 1.0f;
		}

		target_y += NOTIFICATION_HEIGHT + 8;
	}
}

/*
 * Process one frame
 */
int
nova_shell_update(struct nova_shell *shell, float dt)
{
	if (shell == NULL)
		return -1;

	/* Update clock */
	update_clock(shell);

	/* Animate notifications */
	animate_notifications(shell, dt);

	/* Check notification timeouts */
	time_t now = time(NULL);
	for (size_t i = 0; i < shell->notification_count; i++) {
		struct nova_notification_internal *notif =
		    shell->notifications[i];
		if (notif == NULL || notif->dismissed)
			continue;

		if (notif->timeout_ms > 0) {
			int elapsed_ms = (now - notif->created) * 1000;
			if (elapsed_ms > notif->timeout_ms) {
				notif->dismissed = true;
			}
		}
	}

	return 0;
}

/*
 * Render the shell
 */
int
nova_shell_render(struct nova_shell *shell, void *render_context)
{
	/* This would use the Vulkan renderer to draw:
	 * 1. Panel background
	 * 2. Panel widgets (clock, icons, window list)
	 * 3. Notifications
	 * 4. Launcher if visible
	 * 5. Window decorations
	 *
	 * For now, this is a placeholder that would integrate
	 * with nova_renderer.
	 */

	return 0;
}

/*
 * Panel API
 */
int
nova_shell_panel_get_info(struct nova_shell *shell, nova_panel_info_t *info)
{
	if (shell == NULL || info == NULL)
		return -1;

	info->position = shell->panel.position;
	info->height = shell->panel.height;
	info->visible = shell->panel.visible;
	info->auto_hide = shell->panel.auto_hide;

	return 0;
}

int
nova_shell_panel_set_position(struct nova_shell *shell,
    nova_panel_position_t position)
{
	if (shell == NULL)
		return -1;

	shell->panel.position = position;
	return 0;
}

int
nova_shell_panel_set_visible(struct nova_shell *shell, bool visible)
{
	if (shell == NULL)
		return -1;

	shell->panel.visible = visible;
	return 0;
}

/*
 * Launcher API
 */
int
nova_shell_show_launcher(struct nova_shell *shell)
{
	if (shell == NULL)
		return -1;

	shell->launcher_visible = true;
	shell->launcher_search[0] = '\0';
	shell->launcher_selected = 0;

	syslog(LOG_DEBUG, "Launcher shown");
	return 0;
}

int
nova_shell_hide_launcher(struct nova_shell *shell)
{
	if (shell == NULL)
		return -1;

	shell->launcher_visible = false;
	return 0;
}

bool
nova_shell_launcher_is_visible(struct nova_shell *shell)
{
	return shell ? shell->launcher_visible : false;
}

int
nova_shell_launcher_set_search(struct nova_shell *shell, const char *query)
{
	if (shell == NULL)
		return -1;

	if (query != NULL)
		strlcpy(shell->launcher_search, query,
		    sizeof(shell->launcher_search));
	else
		shell->launcher_search[0] = '\0';

	return 0;
}

/*
 * Notifications API
 */
uint32_t
nova_shell_notify(struct nova_shell *shell, const nova_notification_t *notif)
{
	struct nova_notification_internal *internal;

	if (shell == NULL || notif == NULL)
		return 0;

	/* Find free slot */
	size_t slot = shell->notification_count;
	if (slot >= MAX_NOTIFICATIONS) {
		/* Remove oldest */
		free(shell->notifications[0]);
		memmove(&shell->notifications[0], &shell->notifications[1],
		    (MAX_NOTIFICATIONS - 1) * sizeof(void *));
		slot = MAX_NOTIFICATIONS - 1;
		shell->notification_count = slot;
	}

	internal = calloc(1, sizeof(*internal));
	if (internal == NULL)
		return 0;

	internal->id = shell->next_notification_id++;
	strlcpy(internal->title, notif->title, sizeof(internal->title));
	strlcpy(internal->body, notif->body, sizeof(internal->body));
	strlcpy(internal->icon, notif->icon, sizeof(internal->icon));
	internal->urgency = notif->urgency;
	internal->timeout_ms = notif->timeout_ms > 0 ? notif->timeout_ms : 5000;
	internal->created = time(NULL);
	internal->current_y = -NOTIFICATION_HEIGHT; /* Start off-screen */
	internal->opacity = 0.0f;

	shell->notifications[slot] = internal;
	shell->notification_count++;

	syslog(LOG_DEBUG, "Notification %u: %s", internal->id, internal->title);

	return internal->id;
}

int
nova_shell_notification_close(struct nova_shell *shell, uint32_t id)
{
	for (size_t i = 0; i < shell->notification_count; i++) {
		if (shell->notifications[i] != NULL &&
		    shell->notifications[i]->id == id) {
			shell->notifications[i]->dismissed = true;
			return 0;
		}
	}
	return -1;
}

int
nova_shell_get_notifications(struct nova_shell *shell,
    nova_notification_t **notifications, size_t *count)
{
	if (shell == NULL || notifications == NULL || count == NULL)
		return -1;

	*notifications = NULL;
	*count = 0;
	return 0;
}

/*
 * Workspace API
 */
uint32_t
nova_shell_workspace_get_active(struct nova_shell *shell)
{
	return shell ? shell->active_workspace : 0;
}

int
nova_shell_workspace_set_active(struct nova_shell *shell, uint32_t id)
{
	if (shell == NULL || id >= shell->workspace_count)
		return -1;

	/* Deactivate current */
	if (shell->workspaces[shell->active_workspace])
		shell->workspaces[shell->active_workspace]->active = false;

	/* Activate new */
	shell->active_workspace = id;
	if (shell->workspaces[id])
		shell->workspaces[id]->active = true;

	/* Notify callback */
	if (shell->workspace_callback) {
		nova_workspace_info_t info = {
			.id = id,
			.active = true,
		};
		strlcpy(info.name, shell->workspaces[id]->name,
		    sizeof(info.name));
		shell->workspace_callback(shell->workspace_callback_data, &info,
		    "activated");
	}

	syslog(LOG_DEBUG, "Switched to workspace %u", id);
	return 0;
}

int
nova_shell_workspace_get_count(struct nova_shell *shell)
{
	return shell ? (int)shell->workspace_count : 0;
}

int
nova_shell_workspace_add(struct nova_shell *shell, const char *name)
{
	if (shell == NULL || shell->workspace_count >= MAX_WORKSPACES)
		return -1;

	struct nova_workspace_internal *ws = calloc(1, sizeof(*ws));
	if (ws == NULL)
		return -1;

	ws->id = shell->workspace_count;
	if (name != NULL)
		strlcpy(ws->name, name, sizeof(ws->name));
	else
		snprintf(ws->name, sizeof(ws->name), "Workspace %zu",
		    shell->workspace_count + 1);

	shell->workspaces[shell->workspace_count++] = ws;

	return ws->id;
}

/*
 * Window management
 */
int
nova_shell_window_focus(struct nova_shell *shell, uint32_t window_id)
{
	if (shell == NULL)
		return -1;

	/* Unfocus current */
	for (size_t i = 0; i < shell->window_count; i++) {
		if (shell->windows[i] != NULL)
			shell->windows[i]->focused = false;
	}

	/* Focus new */
	for (size_t i = 0; i < shell->window_count; i++) {
		if (shell->windows[i] != NULL &&
		    shell->windows[i]->id == window_id) {
			shell->windows[i]->focused = true;
			shell->focused_window = window_id;
			break;
		}
	}

	return 0;
}

int
nova_shell_window_close(struct nova_shell *shell, uint32_t window_id)
{
	for (size_t i = 0; i < shell->window_count; i++) {
		if (shell->windows[i] != NULL &&
		    shell->windows[i]->id == window_id) {
			/* Notify callback */
			if (shell->window_callback) {
				nova_window_info_t info = {
					.id = window_id,
				};
				shell->window_callback(
				    shell->window_callback_data, &info,
				    "closed");
			}

			free(shell->windows[i]);
			shell->windows[i] = NULL;
			return 0;
		}
	}
	return -1;
}

int
nova_shell_window_minimize(struct nova_shell *shell, uint32_t window_id)
{
	for (size_t i = 0; i < shell->window_count; i++) {
		if (shell->windows[i] != NULL &&
		    shell->windows[i]->id == window_id) {
			shell->windows[i]->state = NOVA_WINDOW_MINIMIZED;
			return 0;
		}
	}
	return -1;
}

int
nova_shell_window_maximize(struct nova_shell *shell, uint32_t window_id)
{
	for (size_t i = 0; i < shell->window_count; i++) {
		if (shell->windows[i] != NULL &&
		    shell->windows[i]->id == window_id) {
			struct nova_window_internal *win = shell->windows[i];

			if (win->state == NOVA_WINDOW_MAXIMIZED) {
				/* Restore */
				win->state = NOVA_WINDOW_NORMAL;
			} else {
				/* Maximize */
				win->state = NOVA_WINDOW_MAXIMIZED;
				win->x = 0;
				win->y = PANEL_HEIGHT;
				win->width = shell->screen_width;
				win->height = shell->screen_height -
				    PANEL_HEIGHT;
			}
			return 0;
		}
	}
	return -1;
}

int
nova_shell_window_move(struct nova_shell *shell, uint32_t window_id, int x,
    int y)
{
	for (size_t i = 0; i < shell->window_count; i++) {
		if (shell->windows[i] != NULL &&
		    shell->windows[i]->id == window_id) {
			shell->windows[i]->x = x;
			shell->windows[i]->y = y;
			return 0;
		}
	}
	return -1;
}

int
nova_shell_window_resize(struct nova_shell *shell, uint32_t window_id,
    int width, int height)
{
	for (size_t i = 0; i < shell->window_count; i++) {
		if (shell->windows[i] != NULL &&
		    shell->windows[i]->id == window_id) {
			shell->windows[i]->width = width;
			shell->windows[i]->height = height;
			return 0;
		}
	}
	return -1;
}

int
nova_shell_window_to_workspace(struct nova_shell *shell, uint32_t window_id,
    uint32_t workspace_id)
{
	if (shell == NULL || workspace_id >= shell->workspace_count)
		return -1;

	for (size_t i = 0; i < shell->window_count; i++) {
		if (shell->windows[i] != NULL &&
		    shell->windows[i]->id == window_id) {
			shell->windows[i]->workspace = workspace_id;
			return 0;
		}
	}
	return -1;
}

/*
 * Theme
 */
int
nova_shell_set_theme(struct nova_shell *shell, const nova_shell_theme_t *theme)
{
	if (shell == NULL || theme == NULL)
		return -1;

	memcpy(&shell->theme, theme, sizeof(*theme));
	return 0;
}

int
nova_shell_get_theme(struct nova_shell *shell, nova_shell_theme_t *theme)
{
	if (shell == NULL || theme == NULL)
		return -1;

	memcpy(theme, &shell->theme, sizeof(*theme));
	return 0;
}

/*
 * Callbacks
 */
void
nova_shell_set_window_callback(struct nova_shell *shell,
    nova_window_callback_t callback, void *data)
{
	if (shell != NULL) {
		shell->window_callback = callback;
		shell->window_callback_data = data;
	}
}

void
nova_shell_set_workspace_callback(struct nova_shell *shell,
    nova_workspace_callback_t callback, void *data)
{
	if (shell != NULL) {
		shell->workspace_callback = callback;
		shell->workspace_callback_data = data;
	}
}

void
nova_shell_set_launcher_callback(struct nova_shell *shell,
    nova_launcher_callback_t callback, void *data)
{
	if (shell != NULL) {
		shell->launcher_callback = callback;
		shell->launcher_callback_data = data;
	}
}
