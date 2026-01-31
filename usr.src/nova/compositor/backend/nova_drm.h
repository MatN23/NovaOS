/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * NovaCompositor - DRM/KMS Backend
 */

#ifndef _NOVA_DRM_H_
#define _NOVA_DRM_H_

#include <sys/types.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Nova DRM/KMS Backend
 *
 * Interfaces with FreeBSD's DRM subsystem (sys/dev/drm2) for:
 * - GPU device discovery and initialization
 * - Display output enumeration (CRTCs, connectors, encoders)
 * - Mode setting and resolution changes
 * - GBM buffer allocation for rendering
 * - VSync and page flipping
 */

struct nova_drm;
struct nova_drm_output;
struct nova_drm_buffer;

/*
 * DRM backend initialization
 */

typedef struct nova_drm_config {
	const char *device_path; /* e.g., "/dev/dri/card0" */
	bool prefer_atomic;	 /* Use atomic modesetting if available */
	bool allow_modifiers;	 /* Allow format modifiers */
} nova_drm_config_t;

struct nova_drm *nova_drm_create(const nova_drm_config_t *config);
void nova_drm_destroy(struct nova_drm *drm);

/* Get file descriptor for event polling */
int nova_drm_get_fd(struct nova_drm *drm);

/* Process pending DRM events (page flip, hotplug) */
int nova_drm_dispatch_events(struct nova_drm *drm);

/*
 * GPU information
 */

typedef struct nova_gpu_info {
	char name[128];	 /* GPU name */
	char driver[64]; /* Driver name (e.g., "amdgpu") */
	uint32_t vendor_id;
	uint32_t device_id;
	bool atomic_supported;
	bool vblank_supported;
	bool vrr_supported; /* Variable refresh rate */
} nova_gpu_info_t;

int nova_drm_get_gpu_info(struct nova_drm *drm, nova_gpu_info_t *info);

/*
 * Output (display) management
 */

typedef struct nova_drm_mode {
	int width;
	int height;
	int refresh; /* mHz (e.g., 60000 for 60Hz) */
	bool preferred;
	bool current;
	char name[32]; /* Mode name string */
} nova_drm_mode_t;

typedef struct nova_drm_output_info {
	uint32_t connector_id;
	uint32_t crtc_id;
	char name[32]; /* e.g., "HDMI-A-1" */
	char make[64];
	char model[64];
	char serial[64];
	int phys_width;	 /* mm */
	int phys_height; /* mm */
	bool connected;
	nova_drm_mode_t *modes;
	size_t mode_count;
} nova_drm_output_info_t;

/* Enumerate connected outputs */
int nova_drm_get_outputs(struct nova_drm *drm, nova_drm_output_info_t **outputs,
    size_t *count);

void nova_drm_free_outputs(nova_drm_output_info_t *outputs, size_t count);

/* Create output context for an active display */
struct nova_drm_output *nova_drm_output_create(struct nova_drm *drm,
    uint32_t connector_id, const nova_drm_mode_t *mode);

void nova_drm_output_destroy(struct nova_drm_output *output);

/* Change output mode */
int nova_drm_output_set_mode(struct nova_drm_output *output,
    const nova_drm_mode_t *mode);

/* Get output dimensions */
int nova_drm_output_get_size(struct nova_drm_output *output, int *width,
    int *height, int *refresh_hz);

/*
 * Buffer management (GBM)
 */

typedef struct nova_drm_buffer_config {
	int width;
	int height;
	uint32_t format; /* DRM_FORMAT_* */
	uint32_t flags;	 /* GBM_BO_USE_* */
} nova_drm_buffer_config_t;

struct nova_drm_buffer *nova_drm_buffer_create(struct nova_drm *drm,
    const nova_drm_buffer_config_t *config);

struct nova_drm_buffer *nova_drm_buffer_import_dmabuf(struct nova_drm *drm,
    int fd, int width, int height, int stride, uint32_t format,
    uint64_t modifier);

void nova_drm_buffer_destroy(struct nova_drm_buffer *buffer);

/* Get buffer properties */
int nova_drm_buffer_get_dmabuf_fd(struct nova_drm_buffer *buffer);
int nova_drm_buffer_get_stride(struct nova_drm_buffer *buffer);
uint32_t nova_drm_buffer_get_format(struct nova_drm_buffer *buffer);

/*
 * Page flipping and presentation
 */

typedef void (*nova_drm_page_flip_callback_t)(void *data,
    struct nova_drm_output *output, uint64_t timestamp_ns);

/* Queue a page flip */
int nova_drm_output_page_flip(struct nova_drm_output *output,
    struct nova_drm_buffer *buffer, nova_drm_page_flip_callback_t callback,
    void *data);

/* Wait for vblank */
int nova_drm_output_wait_vblank(struct nova_drm_output *output,
    uint64_t *timestamp_ns);

/*
 * Hotplug handling
 */

typedef void (*nova_drm_hotplug_callback_t)(void *data, uint32_t connector_id,
    bool connected);

void nova_drm_set_hotplug_callback(struct nova_drm *drm,
    nova_drm_hotplug_callback_t callback, void *data);

#ifdef __cplusplus
}
#endif

#endif /* _NOVA_DRM_H_ */
