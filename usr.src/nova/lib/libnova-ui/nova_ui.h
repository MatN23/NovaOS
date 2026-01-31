/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * libnova-ui - UI Toolkit Header
 */

#ifndef _NOVA_UI_H_
#define _NOVA_UI_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * libnova-ui - Native UI Toolkit for Nova Desktop
 *
 * GPU-accelerated widget library for building Nova applications:
 * - Consistent visual style with NovaShell
 * - Hardware-accelerated rendering
 * - Accessibility support
 * - Animations and transitions
 */

/*
 * Widget types
 */

struct nova_widget;
struct nova_window;
struct nova_button;
struct nova_label;
struct nova_entry;
struct nova_list;
struct nova_slider;
struct nova_toggle;
struct nova_popover;
struct nova_dialog;

/*
 * Application
 */

struct nova_app;

typedef struct nova_app_info {
	const char *app_id; /* Reverse domain name */
	const char *name;
	const char *version;
	const char *icon;
} nova_app_info_t;

struct nova_app *nova_app_new(const nova_app_info_t *info);
void nova_app_free(struct nova_app *app);

int nova_app_run(struct nova_app *app);
void nova_app_quit(struct nova_app *app);

/*
 * Windows
 */

typedef enum nova_window_type {
	NOVA_WINDOW_NORMAL,
	NOVA_WINDOW_DIALOG,
	NOVA_WINDOW_POPOVER,
	NOVA_WINDOW_TOOLTIP,
} nova_window_type_t;

struct nova_window *nova_window_new(struct nova_app *app, const char *title,
    int width, int height);
void nova_window_free(struct nova_window *window);

void nova_window_show(struct nova_window *window);
void nova_window_hide(struct nova_window *window);
void nova_window_close(struct nova_window *window);

void nova_window_set_title(struct nova_window *window, const char *title);
void nova_window_set_size(struct nova_window *window, int width, int height);
void nova_window_set_resizable(struct nova_window *window, bool resizable);

void nova_window_set_content(struct nova_window *window,
    struct nova_widget *content);

/*
 * Layout containers
 */

struct nova_box *nova_box_new(bool vertical, int spacing);
void nova_box_append(struct nova_box *box, struct nova_widget *child);
void nova_box_prepend(struct nova_box *box, struct nova_widget *child);

struct nova_grid *nova_grid_new(int columns, int row_spacing, int col_spacing);
void nova_grid_attach(struct nova_grid *grid, struct nova_widget *child,
    int col, int row, int col_span, int row_span);

struct nova_stack *nova_stack_new(void);
void nova_stack_add(struct nova_stack *stack, struct nova_widget *child,
    const char *name);
void nova_stack_set_visible(struct nova_stack *stack, const char *name);

/*
 * Basic widgets
 */

struct nova_label *nova_label_new(const char *text);
void nova_label_set_text(struct nova_label *label, const char *text);

struct nova_button *nova_button_new(const char *label);
void nova_button_set_label(struct nova_button *button, const char *label);

struct nova_entry *nova_entry_new(void);
const char *nova_entry_get_text(struct nova_entry *entry);
void nova_entry_set_text(struct nova_entry *entry, const char *text);
void nova_entry_set_placeholder(struct nova_entry *entry, const char *text);
void nova_entry_set_password(struct nova_entry *entry, bool password);

struct nova_toggle *nova_toggle_new(void);
bool nova_toggle_get_active(struct nova_toggle *toggle);
void nova_toggle_set_active(struct nova_toggle *toggle, bool active);

struct nova_slider *nova_slider_new(double min, double max, double step);
double nova_slider_get_value(struct nova_slider *slider);
void nova_slider_set_value(struct nova_slider *slider, double value);

/*
 * Signals
 */

typedef void (*nova_callback_t)(void *widget, void *data);

void nova_signal_connect(void *widget, const char *signal,
    nova_callback_t callback, void *data);

/*
 * Styling
 */

void nova_widget_add_class(struct nova_widget *widget, const char *class_name);
void nova_widget_remove_class(struct nova_widget *widget,
    const char *class_name);

void nova_widget_set_margin(struct nova_widget *widget, int top, int right,
    int bottom, int left);

void nova_widget_set_padding(struct nova_widget *widget, int top, int right,
    int bottom, int left);

/*
 * Animations
 */

typedef void (*nova_anim_callback_t)(void *data, double progress);

int nova_animate(struct nova_widget *widget, int duration_ms,
    nova_anim_callback_t callback, void *data);

void nova_animate_opacity(struct nova_widget *widget, double target,
    int duration_ms);

void nova_animate_position(struct nova_widget *widget, int x, int y,
    int duration_ms);

#ifdef __cplusplus
}
#endif

#endif /* _NOVA_UI_H_ */
