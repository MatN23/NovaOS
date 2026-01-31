/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * This software was developed as part of the Nova Desktop project.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _NOVA_COMPOSITOR_H_
#define _NOVA_COMPOSITOR_H_

#include <sys/types.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * NovaCompositor - GPU-accelerated Wayland compositor for FreeBSD
 *
 * This is the core display server component of Nova Desktop, providing:
 * - Wayland protocol implementation
 * - Vulkan-based GPU compositing
 * - DRM/KMS modesetting
 * - Input handling via evdev
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct nova_compositor;
struct nova_output;
struct nova_surface;
struct nova_seat;

/*
 * Compositor initialization and lifecycle
 */

typedef struct nova_compositor_config {
	const char *drm_device;	 /* e.g., "/dev/dri/card0" */
	const char *socket_name; /* Wayland socket name */
	bool enable_xwayland;	 /* Start XWayland for X11 apps */
	bool debug_mode;	 /* Enable debug logging */
} nova_compositor_config_t;

/*
 * Create and initialize the compositor.
 * Returns NULL on failure.
 */
struct nova_compositor *nova_compositor_create(
    const nova_compositor_config_t *config);

/*
 * Destroy the compositor and release all resources.
 */
void nova_compositor_destroy(struct nova_compositor *compositor);

/*
 * Run the compositor event loop.
 * This function blocks until the compositor is terminated.
 * Returns 0 on clean exit, -1 on error.
 */
int nova_compositor_run(struct nova_compositor *compositor);

/*
 * Request compositor shutdown.
 * Safe to call from signal handlers.
 */
void nova_compositor_terminate(struct nova_compositor *compositor);

/*
 * Output (display) management
 */

typedef struct nova_output_info {
	char name[32];	 /* e.g., "HDMI-A-1" */
	char make[64];	 /* Manufacturer */
	char model[64];	 /* Model name */
	int width;	 /* Physical width in mm */
	int height;	 /* Physical height in mm */
	int mode_width;	 /* Current resolution width */
	int mode_height; /* Current resolution height */
	int refresh_hz;	 /* Refresh rate */
	float scale;	 /* Scale factor */
	int pos_x;	 /* Position X */
	int pos_y;	 /* Position Y */
	bool primary;	 /* Primary output */
} nova_output_info_t;

/*
 * Get list of connected outputs.
 * Caller must free the returned array with free().
 */
int nova_compositor_get_outputs(struct nova_compositor *compositor,
    nova_output_info_t **outputs, size_t *count);

/*
 * Configure an output's mode and position.
 */
int nova_compositor_configure_output(struct nova_compositor *compositor,
    const char *name, int width, int height, int refresh_hz, float scale,
    int pos_x, int pos_y);

/*
 * Surface (window) management
 */

typedef struct nova_surface_info {
	uint32_t id;	   /* Surface ID */
	pid_t pid;	   /* Owning process PID */
	char app_id[128];  /* Application ID */
	char title[256];   /* Window title */
	int x, y;	   /* Position */
	int width, height; /* Size */
	bool fullscreen;
	bool maximized;
	bool minimized;
	bool focused;
} nova_surface_info_t;

/*
 * Get list of toplevel surfaces.
 */
int nova_compositor_get_surfaces(struct nova_compositor *compositor,
    nova_surface_info_t **surfaces, size_t *count);

/*
 * Focus a specific surface.
 */
int nova_compositor_focus_surface(struct nova_compositor *compositor,
    uint32_t surface_id);

/*
 * Close a surface (request close from client).
 */
int nova_compositor_close_surface(struct nova_compositor *compositor,
    uint32_t surface_id);

/*
 * Workspace management
 */

typedef struct nova_workspace_info {
	int index;	   /* Workspace index (0-based) */
	char name[64];	   /* Workspace name */
	bool active;	   /* Currently visible */
	int surface_count; /* Number of surfaces */
} nova_workspace_info_t;

int nova_compositor_get_workspaces(struct nova_compositor *compositor,
    nova_workspace_info_t **workspaces, size_t *count);

int nova_compositor_switch_workspace(struct nova_compositor *compositor,
    int index);

int
nova_compositor_move_surface_to_workspace(struct nova_compositor *compositor,
    uint32_t surface_id, int workspace_index);

/*
 * Visual effects
 */

typedef struct nova_effect_params {
	bool blur_enabled;
	float blur_radius; /* 0-32 pixels */
	bool shadow_enabled;
	float shadow_radius;
	float shadow_opacity;
	int shadow_offset_x;
	int shadow_offset_y;
	float corner_radius; /* 0-24 pixels */
	float opacity;	     /* 0.0-1.0 */
} nova_effect_params_t;

int nova_compositor_set_surface_effects(struct nova_compositor *compositor,
    uint32_t surface_id, const nova_effect_params_t *params);

/*
 * Callbacks for compositor events
 */

typedef void (*nova_output_callback_t)(void *data,
    const nova_output_info_t *output, bool added);

typedef void (*nova_surface_callback_t)(void *data,
    const nova_surface_info_t *surface, const char *event);

typedef void (
    *nova_workspace_callback_t)(void *data, int workspace_index, bool active);

void nova_compositor_set_output_callback(struct nova_compositor *compositor,
    nova_output_callback_t callback, void *data);

void nova_compositor_set_surface_callback(struct nova_compositor *compositor,
    nova_surface_callback_t callback, void *data);

void nova_compositor_set_workspace_callback(struct nova_compositor *compositor,
    nova_workspace_callback_t callback, void *data);

#ifdef __cplusplus
}
#endif

#endif /* _NOVA_COMPOSITOR_H_ */
