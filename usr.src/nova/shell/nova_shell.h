/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * NovaShell - Desktop Environment Main Header
 */

#ifndef _NOVA_SHELL_H_
#define _NOVA_SHELL_H_

#include <sys/types.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NovaShell Desktop Environment
 *
 * A modern, elegant desktop environment built on NovaCompositor:
 * - Clean, distraction-free interface
 * - Smooth, purposeful animations
 * - Power-user features with approachable design
 */

/*
 * Shell components
 */

struct nova_shell;
struct nova_panel;
struct nova_launcher;
struct nova_notification_center;
struct nova_workspace_view;
struct nova_window_switcher;
struct nova_settings;

/*
 * Shell initialization
 */

typedef struct nova_shell_config {
	const char *theme;	  /* Theme name */
	const char *accent_color; /* Hex color */
	bool animations_enabled;
	float animation_speed; /* 1.0 = normal */
} nova_shell_config_t;

struct nova_shell *nova_shell_create(const nova_shell_config_t *config);
void nova_shell_destroy(struct nova_shell *shell);

/* Run the shell event loop */
int nova_shell_run(struct nova_shell *shell);

/* Request shell shutdown */
void nova_shell_quit(struct nova_shell *shell);

/*
 * Panel (top bar)
 */

typedef enum nova_panel_position {
	NOVA_PANEL_TOP,
	NOVA_PANEL_BOTTOM,
} nova_panel_position_t;

typedef struct nova_panel_config {
	nova_panel_position_t position;
	int height; /* 0 for auto */
	bool auto_hide;
	float opacity; /* 0.0 - 1.0 */
} nova_panel_config_t;

int nova_shell_configure_panel(struct nova_shell *shell,
    const nova_panel_config_t *config);

/* Panel widget types */
typedef enum nova_widget_type {
	NOVA_WIDGET_CLOCK,
	NOVA_WIDGET_BATTERY,
	NOVA_WIDGET_NETWORK,
	NOVA_WIDGET_AUDIO,
	NOVA_WIDGET_NOTIFICATIONS,
	NOVA_WIDGET_SYSTRAY,
	NOVA_WIDGET_WORKSPACES,
	NOVA_WIDGET_APP_MENU,
	NOVA_WIDGET_WINDOW_TITLE,
	NOVA_WIDGET_CUSTOM,
} nova_widget_type_t;

/* Add/remove panel widgets */
int nova_panel_add_widget(struct nova_shell *shell, nova_widget_type_t type,
    const char *position); /* "left", "center", "right" */

int nova_panel_remove_widget(struct nova_shell *shell, nova_widget_type_t type);

/*
 * Application launcher
 */

/* Show the application launcher */
int nova_shell_show_launcher(struct nova_shell *shell);

/* Hide the application launcher */
int nova_shell_hide_launcher(struct nova_shell *shell);

/* Toggle the application launcher */
int nova_shell_toggle_launcher(struct nova_shell *shell);

/* Launch an application by desktop ID */
int nova_shell_launch_app(struct nova_shell *shell, const char *app_id);

/*
 * Window switcher (Alt+Tab)
 */

int nova_shell_show_switcher(struct nova_shell *shell);
int nova_shell_hide_switcher(struct nova_shell *shell);
int nova_shell_switcher_next(struct nova_shell *shell);
int nova_shell_switcher_prev(struct nova_shell *shell);
int nova_shell_switcher_select(struct nova_shell *shell);

/*
 * Workspace overview
 */

int nova_shell_show_overview(struct nova_shell *shell);
int nova_shell_hide_overview(struct nova_shell *shell);
int nova_shell_toggle_overview(struct nova_shell *shell);

/*
 * Notifications
 */

typedef struct nova_shell_notification {
	uint32_t id;
	char app_name[128];
	char title[256];
	char body[1024];
	char icon[256];
	uint64_t timestamp;
	bool read;
} nova_shell_notification_t;

int nova_shell_show_notification_center(struct nova_shell *shell);
int nova_shell_hide_notification_center(struct nova_shell *shell);

int nova_shell_get_notifications(struct nova_shell *shell,
    nova_shell_notification_t **notifications, size_t *count);

void nova_shell_free_notifications(nova_shell_notification_t *notifications);

int nova_shell_clear_notifications(struct nova_shell *shell);

/*
 * System settings
 */

int nova_shell_show_settings(struct nova_shell *shell, const char *panel);

/*
 * Quick settings panel
 */

int nova_shell_show_quick_settings(struct nova_shell *shell);
int nova_shell_hide_quick_settings(struct nova_shell *shell);

/*
 * Screen lock
 */

int nova_shell_lock_screen(struct nova_shell *shell);

/*
 * Power menu
 */

int nova_shell_show_power_menu(struct nova_shell *shell);

/*
 * Theming
 */

typedef struct nova_theme {
	char name[64];

	/* Colors */
	char bg_primary[16];
	char bg_secondary[16];
	char bg_tertiary[16];
	char text_primary[16];
	char text_secondary[16];
	char text_muted[16];
	char accent[16];
	char success[16];
	char warning[16];
	char error[16];

	/* Effects */
	float blur_radius;
	float shadow_radius;
	float shadow_opacity;
	float corner_radius;
} nova_theme_t;

int nova_shell_get_theme(struct nova_shell *shell, nova_theme_t *theme);
int nova_shell_set_theme(struct nova_shell *shell, const nova_theme_t *theme);
int nova_shell_load_theme(struct nova_shell *shell, const char *theme_name);

/*
 * Keyboard shortcuts
 */

typedef void (*nova_shortcut_callback_t)(void *data, const char *shortcut);

int nova_shell_bind_shortcut(struct nova_shell *shell, const char *shortcut,
    nova_shortcut_callback_t callback, void *data);

int nova_shell_unbind_shortcut(struct nova_shell *shell, const char *shortcut);

/*
 * Desktop actions
 */

typedef enum nova_desktop_action {
	NOVA_ACTION_SHOW_LAUNCHER,
	NOVA_ACTION_SHOW_OVERVIEW,
	NOVA_ACTION_SHOW_SWITCHER,
	NOVA_ACTION_SHOW_QUICK_SETTINGS,
	NOVA_ACTION_SHOW_NOTIFICATION_CENTER,
	NOVA_ACTION_LOCK_SCREEN,
	NOVA_ACTION_LOG_OUT,
	NOVA_ACTION_SCREENSHOT,
	NOVA_ACTION_SCREENSHOT_AREA,
	NOVA_ACTION_WORKSPACE_NEXT,
	NOVA_ACTION_WORKSPACE_PREV,
} nova_desktop_action_t;

int nova_shell_do_action(struct nova_shell *shell,
    nova_desktop_action_t action);

#ifdef __cplusplus
}
#endif

#endif /* _NOVA_SHELL_H_ */
