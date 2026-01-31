/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * NovaCompositor - DRM/KMS Backend Implementation
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

/* FreeBSD DRM headers */
#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>

#include "nova_drm.h"

/*
 * Internal DRM state
 */

struct nova_drm_connector {
	uint32_t id;
	uint32_t encoder_id;
	drmModeConnector *conn;
	bool connected;
	char name[32];
};

struct nova_drm_crtc {
	uint32_t id;
	uint32_t fb_id;
	drmModeCrtc *crtc;
	bool active;
};

struct nova_drm_output {
	struct nova_drm_connector connector;
	struct nova_drm_crtc crtc;
	nova_drm_mode_t current_mode;
	bool enabled;

	/* Framebuffers for double-buffering */
	struct {
		uint32_t fb_id;
		uint32_t handle;
		uint32_t pitch;
		void *map;
		size_t size;
	} buffers[2];
	int front_buffer;
};

struct nova_drm {
	/* Configuration */
	nova_drm_config_t config;

	/* Device */
	int fd;
	char device_path[256];

	/* Resources */
	drmModeRes *resources;

	/* Outputs (one per connected display) */
	struct nova_drm_output *outputs;
	size_t output_count;

	/* Capabilities */
	bool atomic_supported;
	bool vrr_supported;

	/* Callbacks */
	nova_drm_page_flip_callback_t flip_callback;
	void *flip_callback_data;
	nova_drm_hotplug_callback_t hotplug_callback;
	void *hotplug_callback_data;
};

/*
 * Helper: Convert DRM connector type to string
 */
static const char *
connector_type_name(uint32_t type)
{
	switch (type) {
	case DRM_MODE_CONNECTOR_VGA:
		return "VGA";
	case DRM_MODE_CONNECTOR_DVII:
		return "DVI-I";
	case DRM_MODE_CONNECTOR_DVID:
		return "DVI-D";
	case DRM_MODE_CONNECTOR_DVIA:
		return "DVI-A";
	case DRM_MODE_CONNECTOR_HDMIA:
		return "HDMI-A";
	case DRM_MODE_CONNECTOR_HDMIB:
		return "HDMI-B";
	case DRM_MODE_CONNECTOR_DisplayPort:
		return "DP";
	case DRM_MODE_CONNECTOR_eDP:
		return "eDP";
	case DRM_MODE_CONNECTOR_LVDS:
		return "LVDS";
	default:
		return "Unknown";
	}
}

/*
 * Open DRM device and get resources
 */
static int
drm_open_device(struct nova_drm *drm)
{
	uint64_t cap;

	drm->fd = open(drm->config.device_path, O_RDWR | O_CLOEXEC);
	if (drm->fd < 0) {
		syslog(LOG_ERR, "Failed to open DRM device %s: %s",
		    drm->config.device_path, strerror(errno));
		return -1;
	}

	/* Check for atomic modesetting support */
	if (drmGetCap(drm->fd, DRM_CAP_ATOMIC, &cap) == 0 && cap) {
		drm->atomic_supported = true;
		syslog(LOG_INFO, "DRM atomic modesetting supported");

		if (drm->config.prefer_atomic) {
			if (drmSetClientCap(drm->fd, DRM_CLIENT_CAP_ATOMIC,
				1) != 0) {
				syslog(LOG_WARNING,
				    "Failed to enable atomic modesetting");
				drm->atomic_supported = false;
			}
		}
	}

	/* Enable universal planes */
	drmSetClientCap(drm->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

	/* Get resources */
	drm->resources = drmModeGetResources(drm->fd);
	if (drm->resources == NULL) {
		syslog(LOG_ERR, "Failed to get DRM resources");
		close(drm->fd);
		drm->fd = -1;
		return -1;
	}

	syslog(LOG_INFO, "DRM device opened: %d CRTCs, %d connectors",
	    drm->resources->count_crtcs, drm->resources->count_connectors);

	return 0;
}

/*
 * Enumerate connected outputs
 */
static int
drm_enumerate_outputs(struct nova_drm *drm)
{
	int i, count = 0;

	/* First pass: count connected connectors */
	for (i = 0; i < drm->resources->count_connectors; i++) {
		drmModeConnector *conn = drmModeGetConnector(drm->fd,
		    drm->resources->connectors[i]);
		if (conn != NULL) {
			if (conn->connection == DRM_MODE_CONNECTED)
				count++;
			drmModeFreeConnector(conn);
		}
	}

	if (count == 0) {
		syslog(LOG_WARNING, "No connected displays found");
		return 0;
	}

	/* Allocate outputs */
	drm->outputs = calloc(count, sizeof(struct nova_drm_output));
	if (drm->outputs == NULL) {
		syslog(LOG_ERR, "Failed to allocate outputs");
		return -1;
	}

	/* Second pass: populate output information */
	drm->output_count = 0;
	for (i = 0; i < drm->resources->count_connectors; i++) {
		drmModeConnector *conn = drmModeGetConnector(drm->fd,
		    drm->resources->connectors[i]);
		if (conn == NULL)
			continue;

		if (conn->connection == DRM_MODE_CONNECTED &&
		    conn->count_modes > 0) {
			struct nova_drm_output *output =
			    &drm->outputs[drm->output_count];

			output->connector.id = conn->connector_id;
			output->connector.encoder_id = conn->encoder_id;
			output->connector.conn = conn;
			output->connector.connected = true;

			snprintf(output->connector.name,
			    sizeof(output->connector.name), "%s-%d",
			    connector_type_name(conn->connector_type),
			    conn->connector_type_id);

			/* Find CRTC for this connector */
			if (conn->encoder_id) {
				drmModeEncoder *enc = drmModeGetEncoder(drm->fd,
				    conn->encoder_id);
				if (enc != NULL) {
					output->crtc.id = enc->crtc_id;
					output->crtc.crtc = drmModeGetCrtc(
					    drm->fd, enc->crtc_id);
					drmModeFreeEncoder(enc);
				}
			}

			/* Use preferred mode (first in list) */
			drmModeModeInfo *mode = &conn->modes[0];
			output->current_mode.width = mode->hdisplay;
			output->current_mode.height = mode->vdisplay;
			output->current_mode.refresh_hz = mode->vrefresh;

			syslog(LOG_INFO, "Output %s: %dx%d@%dHz",
			    output->connector.name, output->current_mode.width,
			    output->current_mode.height,
			    output->current_mode.refresh_hz);

			drm->output_count++;
		} else {
			drmModeFreeConnector(conn);
		}
	}

	return 0;
}

/*
 * Create DRM backend
 */
struct nova_drm *
nova_drm_create(const nova_drm_config_t *config)
{
	struct nova_drm *drm;

	drm = calloc(1, sizeof(*drm));
	if (drm == NULL) {
		syslog(LOG_ERR, "Failed to allocate DRM backend");
		return NULL;
	}

	drm->fd = -1;
	memcpy(&drm->config, config, sizeof(*config));
	strlcpy(drm->device_path, config->device_path,
	    sizeof(drm->device_path));

	if (drm_open_device(drm) != 0) {
		free(drm);
		return NULL;
	}

	if (drm_enumerate_outputs(drm) != 0) {
		drmModeFreeResources(drm->resources);
		close(drm->fd);
		free(drm);
		return NULL;
	}

	syslog(LOG_INFO, "DRM backend initialized with %zu outputs",
	    drm->output_count);
	return drm;
}

/*
 * Destroy DRM backend
 */
void
nova_drm_destroy(struct nova_drm *drm)
{
	size_t i;

	if (drm == NULL)
		return;

	/* Free outputs */
	for (i = 0; i < drm->output_count; i++) {
		struct nova_drm_output *output = &drm->outputs[i];

		if (output->connector.conn)
			drmModeFreeConnector(output->connector.conn);
		if (output->crtc.crtc)
			drmModeFreeCrtc(output->crtc.crtc);
	}
	free(drm->outputs);

	/* Free resources */
	if (drm->resources)
		drmModeFreeResources(drm->resources);

	/* Close device */
	if (drm->fd >= 0)
		close(drm->fd);

	free(drm);
}

/*
 * Get file descriptor for event polling
 */
int
nova_drm_get_fd(struct nova_drm *drm)
{
	return drm ? drm->fd : -1;
}

/*
 * Process pending DRM events
 */
int
nova_drm_dispatch(struct nova_drm *drm)
{
	drmEventContext ctx = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.page_flip_handler = NULL, /* TODO: Implement */
		.vblank_handler = NULL,
	};

	if (drm == NULL || drm->fd < 0)
		return -1;

	return drmHandleEvent(drm->fd, &ctx);
}

/*
 * Get output information
 */
int
nova_drm_get_outputs(struct nova_drm *drm, nova_drm_output_info_t **outputs,
    size_t *count)
{
	nova_drm_output_info_t *info;
	size_t i;

	if (drm == NULL || outputs == NULL || count == NULL)
		return -1;

	if (drm->output_count == 0) {
		*outputs = NULL;
		*count = 0;
		return 0;
	}

	info = calloc(drm->output_count, sizeof(*info));
	if (info == NULL)
		return -1;

	for (i = 0; i < drm->output_count; i++) {
		struct nova_drm_output *output = &drm->outputs[i];

		info[i].id = output->connector.id;
		strlcpy(info[i].name, output->connector.name,
		    sizeof(info[i].name));
		info[i].connected = output->connector.connected;
		info[i].width_mm = output->connector.conn->mmWidth;
		info[i].height_mm = output->connector.conn->mmHeight;
		memcpy(&info[i].current_mode, &output->current_mode,
		    sizeof(nova_drm_mode_t));
	}

	*outputs = info;
	*count = drm->output_count;
	return 0;
}

void
nova_drm_free_outputs(nova_drm_output_info_t *outputs)
{
	free(outputs);
}

/*
 * Get available modes for an output
 */
int
nova_drm_get_modes(struct nova_drm *drm, uint32_t output_id,
    nova_drm_mode_t **modes, size_t *count)
{
	size_t i;
	struct nova_drm_output *output = NULL;
	nova_drm_mode_t *mode_list;

	if (drm == NULL || modes == NULL || count == NULL)
		return -1;

	/* Find output */
	for (i = 0; i < drm->output_count; i++) {
		if (drm->outputs[i].connector.id == output_id) {
			output = &drm->outputs[i];
			break;
		}
	}

	if (output == NULL || output->connector.conn == NULL) {
		*modes = NULL;
		*count = 0;
		return -1;
	}

	drmModeConnector *conn = output->connector.conn;
	mode_list = calloc(conn->count_modes, sizeof(*mode_list));
	if (mode_list == NULL)
		return -1;

	for (i = 0; i < (size_t)conn->count_modes; i++) {
		drmModeModeInfo *m = &conn->modes[i];
		mode_list[i].width = m->hdisplay;
		mode_list[i].height = m->vdisplay;
		mode_list[i].refresh_hz = m->vrefresh;
		mode_list[i].preferred = (m->type & DRM_MODE_TYPE_PREFERRED) !=
		    0;
	}

	*modes = mode_list;
	*count = conn->count_modes;
	return 0;
}

void
nova_drm_free_modes(nova_drm_mode_t *modes)
{
	free(modes);
}

/*
 * Set mode for an output
 */
int
nova_drm_set_mode(struct nova_drm *drm, uint32_t output_id,
    const nova_drm_mode_t *mode)
{
	/* TODO: Implement mode setting */
	(void)drm;
	(void)output_id;
	(void)mode;
	return 0;
}

/*
 * Set callbacks
 */
void
nova_drm_set_page_flip_callback(struct nova_drm *drm,
    nova_drm_page_flip_callback_t callback, void *data)
{
	if (drm != NULL) {
		drm->flip_callback = callback;
		drm->flip_callback_data = data;
	}
}

void
nova_drm_set_hotplug_callback(struct nova_drm *drm,
    nova_drm_hotplug_callback_t callback, void *data)
{
	if (drm != NULL) {
		drm->hotplug_callback = callback;
		drm->hotplug_callback_data = data;
	}
}
