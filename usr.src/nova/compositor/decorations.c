/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * NovaCompositor - Window Decoration Implementation
 */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "nova_compositor.h"

#define TITLEBAR_HEIGHT 32
#define BORDER_WIDTH	1
#define BUTTON_SIZE	16
#define BUTTON_PADDING	8
#define CORNER_RADIUS	12

/*
 * Decoration button types
 */
typedef enum {
	DECO_BUTTON_CLOSE,
	DECO_BUTTON_MAXIMIZE,
	DECO_BUTTON_MINIMIZE,
} deco_button_type_t;

/*
 * Decoration state
 */
struct nova_decoration {
	/* Window */
	uint32_t window_id;

	/* Geometry */
	int x, y;
	int width, height;
	int content_x, content_y;
	int content_width, content_height;

	/* State */
	bool focused;
	bool maximized;

	/* Title */
	char title[256];

	/* Colors */
	nova_color_t bg_active;
	nova_color_t bg_inactive;
	nova_color_t text_active;
	nova_color_t text_inactive;
	nova_color_t border_active;
	nova_color_t border_inactive;

	/* Button hover states */
	deco_button_type_t hovered_button;
	bool has_hovered_button;

	/* Button colors */
	nova_color_t close_color;
	nova_color_t maximize_color;
	nova_color_t minimize_color;

	/* Drag state */
	bool dragging;
	int drag_start_x, drag_start_y;
	int window_start_x, window_start_y;

	/* Resize state */
	bool resizing;
	int resize_edge;
};

/*
 * Create decoration for a window
 */
struct nova_decoration *
nova_decoration_create(uint32_t window_id, const char *title, int content_width,
    int content_height)
{
	struct nova_decoration *deco;

	deco = calloc(1, sizeof(*deco));
	if (deco == NULL)
		return NULL;

	deco->window_id = window_id;

	if (title != NULL)
		strlcpy(deco->title, title, sizeof(deco->title));

	/* Calculate geometry */
	deco->content_x = BORDER_WIDTH;
	deco->content_y = TITLEBAR_HEIGHT;
	deco->content_width = content_width;
	deco->content_height = content_height;
	deco->width = content_width + BORDER_WIDTH * 2;
	deco->height = content_height + TITLEBAR_HEIGHT + BORDER_WIDTH;

	/* Default colors */
	deco->bg_active = (nova_color_t) { 0.12f, 0.12f, 0.15f, 1.0f };
	deco->bg_inactive = (nova_color_t) { 0.15f, 0.15f, 0.18f, 1.0f };
	deco->text_active = (nova_color_t) { 1.0f, 1.0f, 1.0f, 1.0f };
	deco->text_inactive = (nova_color_t) { 0.6f, 0.6f, 0.6f, 1.0f };
	deco->border_active = (nova_color_t) { 0.4f, 0.6f, 1.0f, 1.0f };
	deco->border_inactive = (nova_color_t) { 0.3f, 0.3f, 0.35f, 1.0f };

	/* Button colors */
	deco->close_color = (nova_color_t) { 0.9f, 0.3f, 0.3f, 1.0f };
	deco->maximize_color = (nova_color_t) { 0.4f, 0.8f, 0.4f, 1.0f };
	deco->minimize_color = (nova_color_t) { 0.9f, 0.8f, 0.3f, 1.0f };

	return deco;
}

/*
 * Destroy decoration
 */
void
nova_decoration_destroy(struct nova_decoration *deco)
{
	free(deco);
}

/*
 * Set title
 */
void
nova_decoration_set_title(struct nova_decoration *deco, const char *title)
{
	if (deco != NULL && title != NULL)
		strlcpy(deco->title, title, sizeof(deco->title));
}

/*
 * Set focus state
 */
void
nova_decoration_set_focused(struct nova_decoration *deco, bool focused)
{
	if (deco != NULL)
		deco->focused = focused;
}

/*
 * Set maximized state
 */
void
nova_decoration_set_maximized(struct nova_decoration *deco, bool maximized)
{
	if (deco != NULL)
		deco->maximized = maximized;
}

/*
 * Get button rectangles
 */
static void
get_button_rect(struct nova_decoration *deco, deco_button_type_t button, int *x,
    int *y, int *w, int *h)
{
	int button_y = (TITLEBAR_HEIGHT - BUTTON_SIZE) / 2;
	int base_x = deco->width - BUTTON_PADDING;

	switch (button) {
	case DECO_BUTTON_CLOSE:
		*x = base_x - BUTTON_SIZE;
		break;
	case DECO_BUTTON_MAXIMIZE:
		*x = base_x - BUTTON_SIZE * 2 - BUTTON_PADDING;
		break;
	case DECO_BUTTON_MINIMIZE:
		*x = base_x - BUTTON_SIZE * 3 - BUTTON_PADDING * 2;
		break;
	}

	*y = button_y;
	*w = BUTTON_SIZE;
	*h = BUTTON_SIZE;
}

/*
 * Hit test - determine what part of decoration is under pointer
 */
int
nova_decoration_hit_test(struct nova_decoration *deco, int x, int y)
{
	if (deco == NULL)
		return 0;

	/* Translate to local coordinates */
	x -= deco->x;
	y -= deco->y;

	/* Check if inside decoration */
	if (x < 0 || y < 0 || x >= deco->width || y >= deco->height)
		return 0;

	/* Check buttons */
	for (int i = 0; i < 3; i++) {
		int bx, by, bw, bh;
		get_button_rect(deco, i, &bx, &by, &bw, &bh);

		if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
			switch (i) {
			case DECO_BUTTON_CLOSE:
				return 4; /* Close button */
			case DECO_BUTTON_MAXIMIZE:
				return 5; /* Maximize button */
			case DECO_BUTTON_MINIMIZE:
				return 6; /* Minimize button */
			}
		}
	}

	/* Title bar (for dragging) */
	if (y < TITLEBAR_HEIGHT)
		return 1; /* Title bar */

	/* Content area */
	if (x >= deco->content_x && x < deco->content_x + deco->content_width &&
	    y >= deco->content_y && y < deco->content_y + deco->content_height)
		return 2; /* Content */

	/* Border (for resizing) */
	int edge = 0;
	if (x < BORDER_WIDTH * 4)
		edge |= 1; /* Left */
	if (x >= deco->width - BORDER_WIDTH * 4)
		edge |= 2; /* Right */
	if (y < BORDER_WIDTH * 4)
		edge |= 4; /* Top */
	if (y >= deco->height - BORDER_WIDTH * 4)
		edge |= 8; /* Bottom */

	if (edge != 0)
		return 3; /* Resize border */

	return 0;
}

/*
 * Begin drag
 */
void
nova_decoration_begin_drag(struct nova_decoration *deco, int x, int y)
{
	if (deco != NULL && !deco->maximized) {
		deco->dragging = true;
		deco->drag_start_x = x;
		deco->drag_start_y = y;
		deco->window_start_x = deco->x;
		deco->window_start_y = deco->y;
	}
}

/*
 * Update drag
 */
void
nova_decoration_update_drag(struct nova_decoration *deco, int x, int y)
{
	if (deco != NULL && deco->dragging) {
		deco->x = deco->window_start_x + (x - deco->drag_start_x);
		deco->y = deco->window_start_y + (y - deco->drag_start_y);
	}
}

/*
 * End drag
 */
void
nova_decoration_end_drag(struct nova_decoration *deco)
{
	if (deco != NULL)
		deco->dragging = false;
}

/*
 * Begin resize
 */
void
nova_decoration_begin_resize(struct nova_decoration *deco, int x, int y,
    int edge)
{
	if (deco != NULL && !deco->maximized) {
		deco->resizing = true;
		deco->resize_edge = edge;
		deco->drag_start_x = x;
		deco->drag_start_y = y;
		deco->window_start_x = deco->x;
		deco->window_start_y = deco->y;
	}
}

/*
 * Update resize
 */
void
nova_decoration_update_resize(struct nova_decoration *deco, int x, int y)
{
	if (deco == NULL || !deco->resizing)
		return;

	int dx = x - deco->drag_start_x;
	int dy = y - deco->drag_start_y;

	/* Apply resize based on edge */
	if (deco->resize_edge & 2) { /* Right */
		deco->content_width += dx;
		if (deco->content_width < 200)
			deco->content_width = 200;
	}
	if (deco->resize_edge & 8) { /* Bottom */
		deco->content_height += dy;
		if (deco->content_height < 100)
			deco->content_height = 100;
	}

	/* Recalculate total size */
	deco->width = deco->content_width + BORDER_WIDTH * 2;
	deco->height = deco->content_height + TITLEBAR_HEIGHT + BORDER_WIDTH;

	deco->drag_start_x = x;
	deco->drag_start_y = y;
}

/*
 * End resize
 */
void
nova_decoration_end_resize(struct nova_decoration *deco)
{
	if (deco != NULL)
		deco->resizing = false;
}

/*
 * Set hover
 */
void
nova_decoration_set_hover(struct nova_decoration *deco, int x, int y)
{
	if (deco == NULL)
		return;

	deco->has_hovered_button = false;

	/* Check buttons */
	for (int i = 0; i < 3; i++) {
		int bx, by, bw, bh;
		get_button_rect(deco, i, &bx, &by, &bw, &bh);

		int lx = x - deco->x;
		int ly = y - deco->y;

		if (lx >= bx && lx < bx + bw && ly >= by && ly < by + bh) {
			deco->hovered_button = i;
			deco->has_hovered_button = true;
			break;
		}
	}
}

/*
 * Render decoration
 */
void
nova_decoration_render(struct nova_decoration *deco, void *render_ctx)
{
	if (deco == NULL || render_ctx == NULL)
		return;

	/* This would use the Vulkan renderer to draw:
	 * 1. Window shadow
	 * 2. Window background with rounded corners
	 * 3. Title bar gradient
	 * 4. Window title text
	 * 5. Window buttons (close, maximize, minimize)
	 * 6. Border
	 *
	 * For now, this is a placeholder that would integrate
	 * with nova_renderer.
	 */

	nova_color_t bg = deco->focused ? deco->bg_active : deco->bg_inactive;
	nova_color_t text = deco->focused ? deco->text_active :
					    deco->text_inactive;
	nova_color_t border = deco->focused ? deco->border_active :
					      deco->border_inactive;

	/* TODO: Draw shadow (gaussian blur) */

	/* TODO: Draw background rectangle with rounded corners */

	/* TODO: Draw title bar background */

	/* TODO: Draw title text */

	/* TODO: Draw window buttons */
	for (int i = 0; i < 3; i++) {
		int bx, by, bw, bh;
		get_button_rect(deco, i, &bx, &by, &bw, &bh);

		nova_color_t button_color;
		switch (i) {
		case DECO_BUTTON_CLOSE:
			button_color = deco->close_color;
			break;
		case DECO_BUTTON_MAXIMIZE:
			button_color = deco->maximize_color;
			break;
		case DECO_BUTTON_MINIMIZE:
			button_color = deco->minimize_color;
			break;
		}

		/* Adjust brightness on hover */
		if (deco->has_hovered_button && deco->hovered_button == i) {
			button_color.r *= 1.2f;
			button_color.g *= 1.2f;
			button_color.b *= 1.2f;
		}

		/* TODO: Draw button circle */
	}

	/* TODO: Draw window border */
	(void)bg;
	(void)text;
	(void)border;
}

/*
 * Get content offset
 */
void
nova_decoration_get_content_offset(struct nova_decoration *deco, int *x, int *y)
{
	if (deco != NULL) {
		if (x)
			*x = deco->content_x;
		if (y)
			*y = deco->content_y;
	}
}

/*
 * Get total size
 */
void
nova_decoration_get_size(struct nova_decoration *deco, int *width, int *height)
{
	if (deco != NULL) {
		if (width)
			*width = deco->width;
		if (height)
			*height = deco->height;
	}
}

/*
 * Update content size
 */
void
nova_decoration_set_content_size(struct nova_decoration *deco, int width,
    int height)
{
	if (deco != NULL) {
		deco->content_width = width;
		deco->content_height = height;
		deco->width = width + BORDER_WIDTH * 2;
		deco->height = height + TITLEBAR_HEIGHT + BORDER_WIDTH;
	}
}
