/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * NovaCompositor - Core Implementation
 */

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "backend/nova_drm.h"
#include "backend/nova_input.h"
#include "nova_compositor.h"
#include "render/nova_renderer.h"

/*
 * Internal compositor state
 */

struct nova_compositor {
	/* Configuration */
	nova_compositor_config_t config;

	/* Event loop */
	int kq; /* kqueue fd */
	volatile bool running;

	/* Backends */
	struct nova_drm *drm;
	struct nova_input *input;
	struct nova_renderer *renderer;

	/* Wayland */
	struct wl_display *wl_display;
	struct wl_event_loop *wl_event_loop;

	/* Outputs */
	struct nova_output *outputs;
	size_t output_count;

	/* Surfaces */
	struct nova_surface *surfaces;
	size_t surface_count;

	/* Workspaces */
	struct nova_workspace *workspaces;
	size_t workspace_count;
	uint32_t active_workspace;

	/* Callbacks */
	nova_output_callback_t output_callback;
	void *output_callback_data;
	nova_surface_callback_t surface_callback;
	void *surface_callback_data;
};

/*
 * Forward declarations for Wayland stubs
 * (Full implementation would include wayland-server.h)
 */
struct wl_display;
struct wl_event_loop;

/*
 * Create the compositor
 */
struct nova_compositor *
nova_compositor_create(const nova_compositor_config_t *config)
{
	struct nova_compositor *comp;

	comp = calloc(1, sizeof(*comp));
	if (comp == NULL) {
		syslog(LOG_ERR, "Failed to allocate compositor");
		return NULL;
	}

	/* Copy configuration */
	memcpy(&comp->config, config, sizeof(*config));

	/* Create kqueue for event loop */
	comp->kq = kqueue();
	if (comp->kq < 0) {
		syslog(LOG_ERR, "Failed to create kqueue: %s", strerror(errno));
		free(comp);
		return NULL;
	}

	/* Initialize DRM backend */
	nova_drm_config_t drm_config = {
		.device_path = config->drm_device,
		.prefer_atomic = true,
	};

	comp->drm = nova_drm_create(&drm_config);
	if (comp->drm == NULL) {
		syslog(LOG_ERR, "Failed to initialize DRM backend");
		close(comp->kq);
		free(comp);
		return NULL;
	}

	/* Initialize input backend */
	nova_input_config_t input_config = {
		.xkb_rules = "evdev",
		.xkb_model = "pc105",
		.xkb_layout = "us",
		.xkb_variant = "",
		.xkb_options = "",
	};

	comp->input = nova_input_create(&input_config);
	if (comp->input == NULL) {
		syslog(LOG_WARNING, "Failed to initialize input backend");
		/* Continue without input for now */
	}

	/* Initialize renderer */
	nova_renderer_config_t renderer_config = {
		.preferred_gpu = NULL,
		.enable_validation = config->debug_mode,
		.max_frames_in_flight = 2,
	};

	comp->renderer = nova_renderer_create(&renderer_config);
	if (comp->renderer == NULL) {
		syslog(LOG_ERR, "Failed to initialize Vulkan renderer");
		if (comp->input)
			nova_input_destroy(comp->input);
		nova_drm_destroy(comp->drm);
		close(comp->kq);
		free(comp);
		return NULL;
	}

	/* Initialize default workspace */
	comp->workspaces = calloc(1, sizeof(struct nova_workspace));
	if (comp->workspaces != NULL) {
		comp->workspace_count = 1;
		comp->active_workspace = 0;
	}

	syslog(LOG_INFO, "Compositor created successfully");
	return comp;
}

/*
 * Destroy the compositor
 */
void
nova_compositor_destroy(struct nova_compositor *comp)
{
	if (comp == NULL)
		return;

	syslog(LOG_INFO, "Destroying compositor");

	/* Clean up backends */
	if (comp->renderer)
		nova_renderer_destroy(comp->renderer);
	if (comp->input)
		nova_input_destroy(comp->input);
	if (comp->drm)
		nova_drm_destroy(comp->drm);

	/* Clean up workspaces */
	free(comp->workspaces);

	/* Close kqueue */
	if (comp->kq >= 0)
		close(comp->kq);

	free(comp);
}

/*
 * Run the compositor main loop
 */
int
nova_compositor_run(struct nova_compositor *comp)
{
	struct kevent events[32];
	struct timespec timeout;
	int nevents;

	comp->running = true;

	syslog(LOG_INFO, "Entering compositor main loop");

	while (comp->running) {
		/* Set timeout for 16ms (approximately 60fps) */
		timeout.tv_sec = 0;
		timeout.tv_nsec = 16666666;

		nevents = kevent(comp->kq, NULL, 0, events, 32, &timeout);

		if (nevents < 0) {
			if (errno == EINTR)
				continue;
			syslog(LOG_ERR, "kevent error: %s", strerror(errno));
			return -1;
		}

		/* Process events */
		for (int i = 0; i < nevents; i++) {
			/* Handle events based on filter and ident */
			/* TODO: Implement event handling */
		}

		/* Process input */
		if (comp->input)
			nova_input_dispatch(comp->input);

		/* Render frame */
		/* TODO: Implement frame rendering */
	}

	syslog(LOG_INFO, "Exiting compositor main loop");
	return 0;
}

/*
 * Request compositor termination
 */
void
nova_compositor_terminate(struct nova_compositor *comp)
{
	if (comp != NULL)
		comp->running = false;
}

/*
 * Get compositor configuration
 */
int
nova_compositor_get_config(struct nova_compositor *comp,
    nova_compositor_config_t *config)
{
	if (comp == NULL || config == NULL)
		return -1;

	memcpy(config, &comp->config, sizeof(*config));
	return 0;
}

/*
 * Output management stubs
 */
int
nova_compositor_get_outputs(struct nova_compositor *comp,
    nova_output_info_t **outputs, size_t *count)
{
	if (comp == NULL || outputs == NULL || count == NULL)
		return -1;

	/* TODO: Query DRM backend for outputs */
	*outputs = NULL;
	*count = 0;
	return 0;
}

void
nova_compositor_free_outputs(nova_output_info_t *outputs)
{
	free(outputs);
}

int
nova_compositor_configure_output(struct nova_compositor *comp,
    uint32_t output_id, const nova_output_config_t *config)
{
	if (comp == NULL || config == NULL)
		return -1;

	/* TODO: Apply output configuration via DRM */
	return 0;
}

/*
 * Surface management stubs
 */
int
nova_compositor_get_surfaces(struct nova_compositor *comp,
    nova_surface_info_t **surfaces, size_t *count)
{
	if (comp == NULL || surfaces == NULL || count == NULL)
		return -1;

	*surfaces = NULL;
	*count = 0;
	return 0;
}

void
nova_compositor_free_surfaces(nova_surface_info_t *surfaces)
{
	free(surfaces);
}

/*
 * Workspace management
 */
int
nova_compositor_get_workspaces(struct nova_compositor *comp,
    nova_workspace_info_t **workspaces, size_t *count)
{
	if (comp == NULL || workspaces == NULL || count == NULL)
		return -1;

	*workspaces = NULL;
	*count = 0;
	return 0;
}

void
nova_compositor_free_workspaces(nova_workspace_info_t *workspaces)
{
	free(workspaces);
}

uint32_t
nova_compositor_get_active_workspace(struct nova_compositor *comp)
{
	return comp ? comp->active_workspace : 0;
}

int
nova_compositor_set_active_workspace(struct nova_compositor *comp,
    uint32_t workspace_id)
{
	if (comp == NULL)
		return -1;

	/* TODO: Implement workspace switching */
	comp->active_workspace = workspace_id;
	return 0;
}

/*
 * Visual effects configuration
 */
int
nova_compositor_set_effects(struct nova_compositor *comp,
    const nova_effects_config_t *config)
{
	if (comp == NULL || config == NULL)
		return -1;

	/* TODO: Apply effects to renderer */
	return 0;
}

/*
 * Callbacks
 */
void
nova_compositor_set_output_callback(struct nova_compositor *comp,
    nova_output_callback_t callback, void *data)
{
	if (comp != NULL) {
		comp->output_callback = callback;
		comp->output_callback_data = data;
	}
}

void
nova_compositor_set_surface_callback(struct nova_compositor *comp,
    nova_surface_callback_t callback, void *data)
{
	if (comp != NULL) {
		comp->surface_callback = callback;
		comp->surface_callback_data = data;
	}
}
