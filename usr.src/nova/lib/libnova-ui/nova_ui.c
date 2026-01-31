/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * libnova-ui - Native UI Toolkit Implementation
 */

#include <sys/types.h>

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "nova_ui.h"

/*
 * Color manipulation
 */

nova_color_t
nova_color_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	nova_color_t c = {
		.r = r / 255.0f,
		.g = g / 255.0f,
		.b = b / 255.0f,
		.a = 1.0f,
	};
	return c;
}

nova_color_t
nova_color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	nova_color_t c = {
		.r = r / 255.0f,
		.g = g / 255.0f,
		.b = b / 255.0f,
		.a = a / 255.0f,
	};
	return c;
}

nova_color_t
nova_color_hex(uint32_t hex)
{
	nova_color_t c = {
		.r = ((hex >> 16) & 0xFF) / 255.0f,
		.g = ((hex >> 8) & 0xFF) / 255.0f,
		.b = (hex & 0xFF) / 255.0f,
		.a = 1.0f,
	};
	return c;
}

nova_color_t
nova_color_lerp(nova_color_t a, nova_color_t b, float t)
{
	nova_color_t c = {
		.r = a.r + (b.r - a.r) * t,
		.g = a.g + (b.g - a.g) * t,
		.b = a.b + (b.b - a.b) * t,
		.a = a.a + (b.a - a.a) * t,
	};
	return c;
}

/*
 * Rectangle utilities
 */

bool
nova_rect_contains(const nova_rect_t *rect, int x, int y)
{
	return x >= rect->x && x < rect->x + rect->width && y >= rect->y &&
	    y < rect->y + rect->height;
}

bool
nova_rect_intersects(const nova_rect_t *a, const nova_rect_t *b)
{
	return a->x < b->x + b->width && a->x + a->width > b->x &&
	    a->y < b->y + b->height && a->y + a->height > b->y;
}

nova_rect_t
nova_rect_intersection(const nova_rect_t *a, const nova_rect_t *b)
{
	nova_rect_t r;
	int x1 = a->x > b->x ? a->x : b->x;
	int y1 = a->y > b->y ? a->y : b->y;
	int x2 = (a->x + a->width) < (b->x + b->width) ? (a->x + a->width) :
							 (b->x + b->width);
	int y2 = (a->y + a->height) < (b->y + b->height) ? (a->y + a->height) :
							   (b->y + b->height);

	r.x = x1;
	r.y = y1;
	r.width = x2 > x1 ? x2 - x1 : 0;
	r.height = y2 > y1 ? y2 - y1 : 0;

	return r;
}

/*
 * Internal widget structure
 */

struct nova_widget {
	nova_widget_type_t type;
	uint32_t id;
	nova_rect_t rect;
	bool visible;
	bool enabled;
	bool focused;
	bool hovered;
	float opacity;

	/* Parent/child relationships */
	struct nova_widget *parent;
	struct nova_widget **children;
	size_t child_count;
	size_t child_capacity;

	/* Style */
	nova_color_t background;
	nova_color_t foreground;
	nova_color_t border_color;
	int border_width;
	int border_radius;
	nova_padding_t padding;
	nova_padding_t margin;

	/* Layout */
	nova_layout_t layout;
	nova_alignment_t h_align;
	nova_alignment_t v_align;
	int min_width, min_height;
	int max_width, max_height;
	float flex_grow;

	/* Event handlers */
	nova_click_handler_t click_handler;
	void *click_data;
	nova_hover_handler_t hover_handler;
	void *hover_data;
	nova_key_handler_t key_handler;
	void *key_data;

	/* Animation state */
	float anim_progress;
	float anim_target;

	/* Type-specific data */
	union {
		struct {
			char text[256];
			int font_size;
			bool bold;
		} label;

		struct {
			char text[256];
			bool pressed;
			bool primary;
		} button;

		struct {
			char text[1024];
			char placeholder[128];
			int cursor_pos;
			int selection_start;
			int selection_end;
			bool password;
		} input;

		struct {
			float value; /* 0.0 - 1.0 */
			float min, max;
			bool dragging;
		} slider;

		struct {
			bool checked;
		} checkbox;

		struct {
			float progress; /* 0.0 - 1.0 */
			bool indeterminate;
		} progress;

		struct {
			char *src;
			void *texture;
			int src_width, src_height;
		} image;

		struct {
			int scroll_x, scroll_y;
			int content_width, content_height;
			bool show_h_scrollbar;
			bool show_v_scrollbar;
		} scroll;
	} data;
};

/*
 * UI context
 */

struct nova_ui {
	/* Root widget */
	struct nova_widget *root;

	/* Focus and hover tracking */
	struct nova_widget *focused;
	struct nova_widget *hovered;
	struct nova_widget *pressed;

	/* ID generation */
	uint32_t next_id;

	/* Theme */
	nova_color_t theme_bg;
	nova_color_t theme_fg;
	nova_color_t theme_accent;
	nova_color_t theme_border;
	int theme_radius;

	/* Animation */
	float anim_speed;

	/* Screen size */
	int width, height;

	/* Pointer state */
	int mouse_x, mouse_y;
	uint32_t mouse_buttons;
};

/*
 * Default theme colors
 */
static void
set_default_theme(struct nova_ui *ui)
{
	ui->theme_bg = nova_color_hex(0x1a1a1f);
	ui->theme_fg = nova_color_hex(0xffffff);
	ui->theme_accent = nova_color_hex(0x6699ff);
	ui->theme_border = nova_color_hex(0x3a3a45);
	ui->theme_radius = 8;
	ui->anim_speed = 1.0f;
}

/*
 * Create UI context
 */
struct nova_ui *
nova_ui_create(int width, int height)
{
	struct nova_ui *ui;

	ui = calloc(1, sizeof(*ui));
	if (ui == NULL) {
		syslog(LOG_ERR, "Failed to allocate UI context");
		return NULL;
	}

	ui->width = width;
	ui->height = height;
	ui->next_id = 1;

	set_default_theme(ui);

	syslog(LOG_INFO, "Nova UI created: %dx%d", width, height);
	return ui;
}

/*
 * Destroy UI context
 */
static void
destroy_widget_recursive(struct nova_widget *widget)
{
	if (widget == NULL)
		return;

	for (size_t i = 0; i < widget->child_count; i++) {
		destroy_widget_recursive(widget->children[i]);
	}

	free(widget->children);

	if (widget->type == NOVA_WIDGET_IMAGE && widget->data.image.src)
		free(widget->data.image.src);

	free(widget);
}

void
nova_ui_destroy(struct nova_ui *ui)
{
	if (ui == NULL)
		return;

	destroy_widget_recursive(ui->root);
	free(ui);
}

/*
 * Create widget
 */
static struct nova_widget *
create_base_widget(struct nova_ui *ui, nova_widget_type_t type)
{
	struct nova_widget *w;

	w = calloc(1, sizeof(*w));
	if (w == NULL)
		return NULL;

	w->type = type;
	w->id = ui->next_id++;
	w->visible = true;
	w->enabled = true;
	w->opacity = 1.0f;

	w->background = ui->theme_bg;
	w->foreground = ui->theme_fg;
	w->border_color = ui->theme_border;
	w->border_radius = ui->theme_radius;

	return w;
}

/*
 * Container widget
 */
struct nova_widget *
nova_container_create(struct nova_ui *ui)
{
	return create_base_widget(ui, NOVA_WIDGET_CONTAINER);
}

/*
 * Label widget
 */
struct nova_widget *
nova_label_create(struct nova_ui *ui, const char *text)
{
	struct nova_widget *w = create_base_widget(ui, NOVA_WIDGET_LABEL);
	if (w != NULL && text != NULL) {
		strlcpy(w->data.label.text, text, sizeof(w->data.label.text));
		w->data.label.font_size = 14;
	}
	return w;
}

void
nova_label_set_text(struct nova_widget *label, const char *text)
{
	if (label != NULL && label->type == NOVA_WIDGET_LABEL && text != NULL)
		strlcpy(label->data.label.text, text,
		    sizeof(label->data.label.text));
}

/*
 * Button widget
 */
struct nova_widget *
nova_button_create(struct nova_ui *ui, const char *text)
{
	struct nova_widget *w = create_base_widget(ui, NOVA_WIDGET_BUTTON);
	if (w != NULL) {
		if (text != NULL)
			strlcpy(w->data.button.text, text,
			    sizeof(w->data.button.text));
		w->padding = (nova_padding_t) { 12, 16, 12, 16 };
		w->border_radius = 6;
	}
	return w;
}

void
nova_button_set_text(struct nova_widget *button, const char *text)
{
	if (button != NULL && button->type == NOVA_WIDGET_BUTTON &&
	    text != NULL)
		strlcpy(button->data.button.text, text,
		    sizeof(button->data.button.text));
}

void
nova_button_set_primary(struct nova_widget *button, bool primary)
{
	if (button != NULL && button->type == NOVA_WIDGET_BUTTON)
		button->data.button.primary = primary;
}

/*
 * Input widget
 */
struct nova_widget *
nova_input_create(struct nova_ui *ui, const char *placeholder)
{
	struct nova_widget *w = create_base_widget(ui, NOVA_WIDGET_INPUT);
	if (w != NULL) {
		if (placeholder != NULL)
			strlcpy(w->data.input.placeholder, placeholder,
			    sizeof(w->data.input.placeholder));
		w->padding = (nova_padding_t) { 8, 12, 8, 12 };
		w->border_width = 1;
	}
	return w;
}

const char *
nova_input_get_text(struct nova_widget *input)
{
	if (input != NULL && input->type == NOVA_WIDGET_INPUT)
		return input->data.input.text;
	return "";
}

void
nova_input_set_text(struct nova_widget *input, const char *text)
{
	if (input != NULL && input->type == NOVA_WIDGET_INPUT && text != NULL)
		strlcpy(input->data.input.text, text,
		    sizeof(input->data.input.text));
}

/*
 * Slider widget
 */
struct nova_widget *
nova_slider_create(struct nova_ui *ui, float min, float max, float value)
{
	struct nova_widget *w = create_base_widget(ui, NOVA_WIDGET_SLIDER);
	if (w != NULL) {
		w->data.slider.min = min;
		w->data.slider.max = max;
		w->data.slider.value = (value - min) / (max - min);
		w->rect.height = 24;
	}
	return w;
}

float
nova_slider_get_value(struct nova_widget *slider)
{
	if (slider != NULL && slider->type == NOVA_WIDGET_SLIDER) {
		float v = slider->data.slider.value;
		float min = slider->data.slider.min;
		float max = slider->data.slider.max;
		return min + v * (max - min);
	}
	return 0;
}

void
nova_slider_set_value(struct nova_widget *slider, float value)
{
	if (slider != NULL && slider->type == NOVA_WIDGET_SLIDER) {
		float min = slider->data.slider.min;
		float max = slider->data.slider.max;
		slider->data.slider.value = (value - min) / (max - min);
		if (slider->data.slider.value < 0)
			slider->data.slider.value = 0;
		if (slider->data.slider.value > 1)
			slider->data.slider.value = 1;
	}
}

/*
 * Checkbox widget
 */
struct nova_widget *
nova_checkbox_create(struct nova_ui *ui, const char *label, bool checked)
{
	struct nova_widget *w = create_base_widget(ui, NOVA_WIDGET_CHECKBOX);
	if (w != NULL) {
		w->data.checkbox.checked = checked;
		/* Store label in label.text field */
		if (label != NULL)
			strlcpy(w->data.label.text, label,
			    sizeof(w->data.label.text));
	}
	return w;
}

bool
nova_checkbox_is_checked(struct nova_widget *checkbox)
{
	if (checkbox != NULL && checkbox->type == NOVA_WIDGET_CHECKBOX)
		return checkbox->data.checkbox.checked;
	return false;
}

void
nova_checkbox_set_checked(struct nova_widget *checkbox, bool checked)
{
	if (checkbox != NULL && checkbox->type == NOVA_WIDGET_CHECKBOX)
		checkbox->data.checkbox.checked = checked;
}

/*
 * Progress bar widget
 */
struct nova_widget *
nova_progress_create(struct nova_ui *ui)
{
	struct nova_widget *w = create_base_widget(ui, NOVA_WIDGET_PROGRESS);
	if (w != NULL)
		w->rect.height = 8;
	return w;
}

void
nova_progress_set_value(struct nova_widget *progress, float value)
{
	if (progress != NULL && progress->type == NOVA_WIDGET_PROGRESS) {
		progress->data.progress.progress = value;
		if (progress->data.progress.progress < 0)
			progress->data.progress.progress = 0;
		if (progress->data.progress.progress > 1)
			progress->data.progress.progress = 1;
	}
}

/*
 * Image widget
 */
struct nova_widget *
nova_image_create(struct nova_ui *ui, const char *src)
{
	struct nova_widget *w = create_base_widget(ui, NOVA_WIDGET_IMAGE);
	if (w != NULL && src != NULL)
		w->data.image.src = strdup(src);
	return w;
}

/*
 * Scroll container
 */
struct nova_widget *
nova_scroll_create(struct nova_ui *ui)
{
	struct nova_widget *w = create_base_widget(ui, NOVA_WIDGET_SCROLL);
	if (w != NULL)
		w->data.scroll.show_v_scrollbar = true;
	return w;
}

/*
 * Widget hierarchy
 */
void
nova_widget_add_child(struct nova_widget *parent, struct nova_widget *child)
{
	if (parent == NULL || child == NULL)
		return;

	/* Resize array if needed */
	if (parent->child_count >= parent->child_capacity) {
		size_t new_cap = parent->child_capacity == 0 ?
		    8 :
		    parent->child_capacity * 2;
		struct nova_widget **new_children = realloc(parent->children,
		    new_cap * sizeof(struct nova_widget *));
		if (new_children == NULL)
			return;
		parent->children = new_children;
		parent->child_capacity = new_cap;
	}

	parent->children[parent->child_count++] = child;
	child->parent = parent;
}

void
nova_widget_remove_child(struct nova_widget *parent, struct nova_widget *child)
{
	if (parent == NULL || child == NULL)
		return;

	for (size_t i = 0; i < parent->child_count; i++) {
		if (parent->children[i] == child) {
			memmove(&parent->children[i], &parent->children[i + 1],
			    (parent->child_count - i - 1) *
				sizeof(struct nova_widget *));
			parent->child_count--;
			child->parent = NULL;
			break;
		}
	}
}

/*
 * Widget properties
 */
void
nova_widget_set_rect(struct nova_widget *widget, nova_rect_t rect)
{
	if (widget != NULL)
		widget->rect = rect;
}

nova_rect_t
nova_widget_get_rect(struct nova_widget *widget)
{
	if (widget != NULL)
		return widget->rect;
	return (nova_rect_t) { 0, 0, 0, 0 };
}

void
nova_widget_set_visible(struct nova_widget *widget, bool visible)
{
	if (widget != NULL)
		widget->visible = visible;
}

void
nova_widget_set_enabled(struct nova_widget *widget, bool enabled)
{
	if (widget != NULL)
		widget->enabled = enabled;
}

void
nova_widget_set_opacity(struct nova_widget *widget, float opacity)
{
	if (widget != NULL) {
		widget->opacity = opacity;
		if (widget->opacity < 0)
			widget->opacity = 0;
		if (widget->opacity > 1)
			widget->opacity = 1;
	}
}

/*
 * Event handlers
 */
void
nova_widget_on_click(struct nova_widget *widget, nova_click_handler_t handler,
    void *data)
{
	if (widget != NULL) {
		widget->click_handler = handler;
		widget->click_data = data;
	}
}

void
nova_widget_on_hover(struct nova_widget *widget, nova_hover_handler_t handler,
    void *data)
{
	if (widget != NULL) {
		widget->hover_handler = handler;
		widget->hover_data = data;
	}
}

void
nova_widget_on_key(struct nova_widget *widget, nova_key_handler_t handler,
    void *data)
{
	if (widget != NULL) {
		widget->key_handler = handler;
		widget->key_data = data;
	}
}

/*
 * Focus management
 */
void
nova_ui_set_focus(struct nova_ui *ui, struct nova_widget *widget)
{
	if (ui == NULL)
		return;

	if (ui->focused != NULL)
		ui->focused->focused = false;

	ui->focused = widget;

	if (widget != NULL)
		widget->focused = true;
}

struct nova_widget *
nova_ui_get_focus(struct nova_ui *ui)
{
	return ui ? ui->focused : NULL;
}

/*
 * Input handling
 */
void
nova_ui_pointer_move(struct nova_ui *ui, int x, int y)
{
	if (ui == NULL)
		return;

	ui->mouse_x = x;
	ui->mouse_y = y;

	/* TODO: Update hover state */
}

void
nova_ui_pointer_button(struct nova_ui *ui, int button, bool pressed)
{
	if (ui == NULL)
		return;

	if (pressed)
		ui->mouse_buttons |= (1 << button);
	else
		ui->mouse_buttons &= ~(1 << button);

	/* TODO: Handle clicks */
}

void
nova_ui_key_event(struct nova_ui *ui, uint32_t keycode, bool pressed)
{
	if (ui == NULL || ui->focused == NULL)
		return;

	/* TODO: Handle key events */
}

/*
 * Rendering
 */
static void
render_widget_recursive(struct nova_widget *widget, void *render_ctx,
    int offset_x, int offset_y)
{
	if (widget == NULL || !widget->visible)
		return;

	/* Calculate absolute position */
	int x = widget->rect.x + offset_x;
	int y = widget->rect.y + offset_y;

	/* TODO: Use Vulkan renderer to draw widget based on type */

	/* Render children */
	for (size_t i = 0; i < widget->child_count; i++) {
		render_widget_recursive(widget->children[i], render_ctx, x, y);
	}
}

void
nova_ui_render(struct nova_ui *ui, void *render_context)
{
	if (ui == NULL)
		return;

	render_widget_recursive(ui->root, render_context, 0, 0);
}

/*
 * Update/animation
 */
void
nova_ui_update(struct nova_ui *ui, float dt)
{
	if (ui == NULL)
		return;

	/* TODO: Update animations */
}

/*
 * Layout
 */
void
nova_widget_set_layout(struct nova_widget *widget, nova_layout_t layout)
{
	if (widget != NULL)
		widget->layout = layout;
}

void
nova_widget_set_padding(struct nova_widget *widget, nova_padding_t padding)
{
	if (widget != NULL)
		widget->padding = padding;
}

void
nova_widget_set_margin(struct nova_widget *widget, nova_padding_t margin)
{
	if (widget != NULL)
		widget->margin = margin;
}

/*
 * Theme
 */
void
nova_ui_set_accent_color(struct nova_ui *ui, nova_color_t color)
{
	if (ui != NULL)
		ui->theme_accent = color;
}
