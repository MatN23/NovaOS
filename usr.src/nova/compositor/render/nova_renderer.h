/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * NovaCompositor - Vulkan Renderer
 */

#ifndef _NOVA_RENDERER_H_
#define _NOVA_RENDERER_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Vulkan types - include vulkan.h if available, otherwise use opaque handles.
 * Applications using the full Vulkan API should include vulkan.h before this
 * header.
 */
#ifndef VK_DEFINE_HANDLE
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct VkSurfaceKHR_T *VkSurfaceKHR;
typedef struct VkCommandBuffer_T *VkCommandBuffer;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Nova Vulkan Renderer
 *
 * GPU-accelerated rendering backend using Vulkan for all compositing
 * operations. Features include:
 * - Hardware-accelerated window compositing
 * - Shader-based visual effects (blur, shadows, rounded corners)
 * - Efficient damage tracking and partial updates
 * - Frame pacing for tear-free presentation
 */

struct nova_renderer;
struct nova_render_target;
struct nova_texture;

/*
 * Renderer configuration
 */

typedef struct nova_renderer_config {
	VkPhysicalDevice preferred_gpu; /* NULL for auto-select */
	bool enable_validation;		/* Vulkan validation layers */
	int max_frames_in_flight;
} nova_renderer_config_t;

/*
 * Create the Vulkan renderer.
 */
struct nova_renderer *nova_renderer_create(
    const nova_renderer_config_t *config);

void nova_renderer_destroy(struct nova_renderer *renderer);

/*
 * Render target management (one per output/display)
 */

typedef struct nova_render_target_config {
	int width;
	int height;
	int refresh_hz;
	VkSurfaceKHR surface; /* From DRM/KMS */
	bool adaptive_sync;   /* VRR/FreeSync support */
} nova_render_target_config_t;

struct nova_render_target *
nova_render_target_create(struct nova_renderer *renderer,
    const nova_render_target_config_t *config);

void nova_render_target_destroy(struct nova_render_target *target);

int nova_render_target_resize(struct nova_render_target *target, int width,
    int height);

/*
 * Texture management
 */

typedef enum nova_texture_format {
	NOVA_FORMAT_RGBA8,
	NOVA_FORMAT_BGRA8,
	NOVA_FORMAT_RGB10A2,
	NOVA_FORMAT_RGBA16F,
} nova_texture_format_t;

struct nova_texture *nova_texture_create(struct nova_renderer *renderer,
    int width, int height, nova_texture_format_t format);

struct nova_texture *nova_texture_from_dmabuf(struct nova_renderer *renderer,
    int fd, int width, int height, int stride, uint32_t format,
    uint64_t modifier);

void nova_texture_destroy(struct nova_texture *texture);

/*
 * Frame rendering
 */

typedef struct nova_frame_context {
	struct nova_render_target *target;
	VkCommandBuffer cmd;
	uint32_t frame_index;
	uint64_t frame_time_ns;
} nova_frame_context_t;

int nova_renderer_begin_frame(struct nova_renderer *renderer,
    struct nova_render_target *target, nova_frame_context_t *ctx);

int nova_renderer_end_frame(struct nova_renderer *renderer,
    nova_frame_context_t *ctx);

/*
 * Drawing primitives
 */

typedef struct nova_rect {
	int x, y;
	int width, height;
} nova_rect_t;

typedef struct nova_color {
	float r, g, b, a;
} nova_color_t;

/* Clear the render target */
void nova_renderer_clear(nova_frame_context_t *ctx, const nova_color_t *color);

/* Draw a textured quad (window surface) */
void nova_renderer_draw_texture(nova_frame_context_t *ctx,
    struct nova_texture *texture, const nova_rect_t *src_rect,
    const nova_rect_t *dst_rect, float opacity, float corner_radius);

/* Draw with blur effect */
void nova_renderer_draw_blur(nova_frame_context_t *ctx, const nova_rect_t *rect,
    float radius, float opacity);

/* Draw drop shadow */
void nova_renderer_draw_shadow(nova_frame_context_t *ctx,
    const nova_rect_t *rect, float radius, float opacity, int offset_x,
    int offset_y, float corner_radius);

/* Draw solid rectangle */
void nova_renderer_draw_rect(nova_frame_context_t *ctx, const nova_rect_t *rect,
    const nova_color_t *color, float corner_radius);

/*
 * Frame pacing and timing
 */

typedef struct nova_frame_stats {
	uint64_t frame_time_ns;	  /* Total frame time */
	uint64_t render_time_ns;  /* GPU render time */
	uint64_t present_time_ns; /* Presentation time */
	uint64_t latency_ns;	  /* Input-to-display latency */
	int missed_frames;	  /* Frames missed this second */
	float fps;		  /* Current FPS */
} nova_frame_stats_t;

int nova_renderer_get_stats(struct nova_renderer *renderer,
    struct nova_render_target *target, nova_frame_stats_t *stats);

/* Target a specific frame time (e.g., 16666666 ns for 60fps) */
void nova_renderer_set_target_frame_time(struct nova_renderer *renderer,
    uint64_t target_ns);

#ifdef __cplusplus
}
#endif

#endif /* _NOVA_RENDERER_H_ */
