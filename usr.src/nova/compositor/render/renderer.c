/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * NovaCompositor - Vulkan Renderer Implementation
 */

#include <sys/types.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

/* Include Vulkan */
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>

#include "nova_renderer.h"

/*
 * Shader sources (SPIR-V would be embedded in production)
 */

/* Vertex shader for textured quads */
static const char *vertex_shader_glsl =
    "#version 450\n"
    "layout(location = 0) in vec2 inPosition;\n"
    "layout(location = 1) in vec2 inTexCoord;\n"
    "layout(location = 0) out vec2 fragTexCoord;\n"
    "layout(push_constant) uniform PushConstants {\n"
    "    mat4 transform;\n"
    "    vec4 color;\n"
    "    float opacity;\n"
    "    float cornerRadius;\n"
    "} pc;\n"
    "void main() {\n"
    "    gl_Position = pc.transform * vec4(inPosition, 0.0, 1.0);\n"
    "    fragTexCoord = inTexCoord;\n"
    "}\n";

/* Fragment shader for textured quads with rounded corners */
static const char *fragment_shader_glsl =
    "#version 450\n"
    "layout(location = 0) in vec2 fragTexCoord;\n"
    "layout(location = 0) out vec4 outColor;\n"
    "layout(binding = 0) uniform sampler2D texSampler;\n"
    "layout(push_constant) uniform PushConstants {\n"
    "    mat4 transform;\n"
    "    vec4 color;\n"
    "    float opacity;\n"
    "    float cornerRadius;\n"
    "} pc;\n"
    "void main() {\n"
    "    vec4 texColor = texture(texSampler, fragTexCoord);\n"
    "    outColor = texColor * pc.color * vec4(1.0, 1.0, 1.0, pc.opacity);\n"
    "}\n";

/*
 * Internal structures
 */

struct nova_renderer {
	/* Configuration */
	nova_renderer_config_t config;

	/* Vulkan instance and device */
	VkInstance instance;
	VkPhysicalDevice physical_device;
	VkDevice device;
	VkQueue graphics_queue;
	VkQueue present_queue;
	uint32_t graphics_family;
	uint32_t present_family;

	/* Command pools and buffers */
	VkCommandPool command_pool;
	VkCommandBuffer *command_buffers;
	int command_buffer_count;

	/* Synchronization */
	VkSemaphore *image_available_semaphores;
	VkSemaphore *render_finished_semaphores;
	VkFence *in_flight_fences;
	int max_frames_in_flight;
	int current_frame;

	/* Pipeline */
	VkRenderPass render_pass;
	VkPipelineLayout pipeline_layout;
	VkPipeline graphics_pipeline;
	VkDescriptorSetLayout descriptor_set_layout;
	VkDescriptorPool descriptor_pool;

	/* Default resources */
	VkSampler default_sampler;

	/* Frame pacing */
	uint64_t target_frame_time_ns;
	uint64_t last_frame_time;
};

struct nova_render_target {
	struct nova_renderer *renderer;

	/* Surface */
	VkSurfaceKHR surface;
	VkSurfaceCapabilitiesKHR capabilities;

	/* Swapchain */
	VkSwapchainKHR swapchain;
	VkFormat format;
	VkExtent2D extent;
	VkImage *images;
	VkImageView *image_views;
	VkFramebuffer *framebuffers;
	uint32_t image_count;

	/* Configuration */
	bool adaptive_sync;
	int refresh_hz;
};

struct nova_texture {
	struct nova_renderer *renderer;

	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkDescriptorSet descriptor_set;

	int width;
	int height;
	nova_texture_format_t format;
};

/*
 * Helper: Check Vulkan result
 */
#define VK_CHECK(call)                                                      \
	do {                                                                \
		VkResult result = (call);                                   \
		if (result != VK_SUCCESS) {                                 \
			syslog(LOG_ERR, "Vulkan error %d at %s:%d", result, \
			    __FILE__, __LINE__);                            \
			return NULL;                                        \
		}                                                           \
	} while (0)

#define VK_CHECK_VOID(call)                                                 \
	do {                                                                \
		VkResult result = (call);                                   \
		if (result != VK_SUCCESS) {                                 \
			syslog(LOG_ERR, "Vulkan error %d at %s:%d", result, \
			    __FILE__, __LINE__);                            \
			return;                                             \
		}                                                           \
	} while (0)

/*
 * Find suitable memory type
 */
static uint32_t
find_memory_type(struct nova_renderer *renderer, uint32_t type_filter,
    VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties mem_props;
	vkGetPhysicalDeviceMemoryProperties(renderer->physical_device,
	    &mem_props);

	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((type_filter & (1 << i)) &&
		    (mem_props.memoryTypes[i].propertyFlags & properties) ==
			properties) {
			return i;
		}
	}

	return UINT32_MAX;
}

/*
 * Create Vulkan instance
 */
static int
create_instance(struct nova_renderer *renderer)
{
	VkApplicationInfo app_info = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "NovaCompositor",
		.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
		.pEngineName = "Nova",
		.engineVersion = VK_MAKE_VERSION(1, 0, 0),
		.apiVersion = VK_API_VERSION_1_2,
	};

	const char *extensions[] = {
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
		VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
	};

	const char *layers[] = {
		"VK_LAYER_KHRONOS_validation",
	};

	VkInstanceCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app_info,
		.enabledExtensionCount = renderer->config.enable_validation ?
		    3 :
		    2,
		.ppEnabledExtensionNames = extensions,
		.enabledLayerCount = renderer->config.enable_validation ? 1 : 0,
		.ppEnabledLayerNames = layers,
	};

	if (vkCreateInstance(&create_info, NULL, &renderer->instance) !=
	    VK_SUCCESS) {
		syslog(LOG_ERR, "Failed to create Vulkan instance");
		return -1;
	}

	return 0;
}

/*
 * Select physical device
 */
static int
select_physical_device(struct nova_renderer *renderer)
{
	uint32_t device_count = 0;
	vkEnumeratePhysicalDevices(renderer->instance, &device_count, NULL);

	if (device_count == 0) {
		syslog(LOG_ERR, "No Vulkan-capable GPUs found");
		return -1;
	}

	VkPhysicalDevice *devices = calloc(device_count,
	    sizeof(VkPhysicalDevice));
	vkEnumeratePhysicalDevices(renderer->instance, &device_count, devices);

	/* Use preferred GPU or first discrete GPU */
	if (renderer->config.preferred_gpu != NULL) {
		renderer->physical_device = renderer->config.preferred_gpu;
	} else {
		renderer->physical_device = devices[0];

		for (uint32_t i = 0; i < device_count; i++) {
			VkPhysicalDeviceProperties props;
			vkGetPhysicalDeviceProperties(devices[i], &props);

			if (props.deviceType ==
			    VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
				renderer->physical_device = devices[i];
				syslog(LOG_INFO, "Selected GPU: %s",
				    props.deviceName);
				break;
			}
		}
	}

	free(devices);

	/* Find queue families */
	uint32_t queue_family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(renderer->physical_device,
	    &queue_family_count, NULL);

	VkQueueFamilyProperties *queue_families = calloc(queue_family_count,
	    sizeof(VkQueueFamilyProperties));
	vkGetPhysicalDeviceQueueFamilyProperties(renderer->physical_device,
	    &queue_family_count, queue_families);

	renderer->graphics_family = UINT32_MAX;
	for (uint32_t i = 0; i < queue_family_count; i++) {
		if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			renderer->graphics_family = i;
			renderer->present_family = i; /* Assume same for now */
			break;
		}
	}

	free(queue_families);

	if (renderer->graphics_family == UINT32_MAX) {
		syslog(LOG_ERR, "No suitable graphics queue family");
		return -1;
	}

	return 0;
}

/*
 * Create logical device
 */
static int
create_device(struct nova_renderer *renderer)
{
	float queue_priority = 1.0f;
	VkDeviceQueueCreateInfo queue_create_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = renderer->graphics_family,
		.queueCount = 1,
		.pQueuePriorities = &queue_priority,
	};

	VkPhysicalDeviceFeatures device_features = {
		.samplerAnisotropy = VK_TRUE,
	};

	const char *extensions[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	};

	VkDeviceCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queue_create_info,
		.pEnabledFeatures = &device_features,
		.enabledExtensionCount = 1,
		.ppEnabledExtensionNames = extensions,
	};

	if (vkCreateDevice(renderer->physical_device, &create_info, NULL,
		&renderer->device) != VK_SUCCESS) {
		syslog(LOG_ERR, "Failed to create logical device");
		return -1;
	}

	vkGetDeviceQueue(renderer->device, renderer->graphics_family, 0,
	    &renderer->graphics_queue);
	renderer->present_queue = renderer->graphics_queue;

	return 0;
}

/*
 * Create command pool
 */
static int
create_command_pool(struct nova_renderer *renderer)
{
	VkCommandPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = renderer->graphics_family,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};

	if (vkCreateCommandPool(renderer->device, &pool_info, NULL,
		&renderer->command_pool) != VK_SUCCESS) {
		syslog(LOG_ERR, "Failed to create command pool");
		return -1;
	}

	return 0;
}

/*
 * Create synchronization objects
 */
static int
create_sync_objects(struct nova_renderer *renderer)
{
	int n = renderer->max_frames_in_flight;

	renderer->image_available_semaphores = calloc(n, sizeof(VkSemaphore));
	renderer->render_finished_semaphores = calloc(n, sizeof(VkSemaphore));
	renderer->in_flight_fences = calloc(n, sizeof(VkFence));

	VkSemaphoreCreateInfo sem_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};

	VkFenceCreateInfo fence_info = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT,
	};

	for (int i = 0; i < n; i++) {
		if (vkCreateSemaphore(renderer->device, &sem_info, NULL,
			&renderer->image_available_semaphores[i]) !=
			VK_SUCCESS ||
		    vkCreateSemaphore(renderer->device, &sem_info, NULL,
			&renderer->render_finished_semaphores[i]) !=
			VK_SUCCESS ||
		    vkCreateFence(renderer->device, &fence_info, NULL,
			&renderer->in_flight_fences[i]) != VK_SUCCESS) {
			syslog(LOG_ERR, "Failed to create sync objects");
			return -1;
		}
	}

	return 0;
}

/*
 * Create default sampler
 */
static int
create_default_sampler(struct nova_renderer *renderer)
{
	VkSamplerCreateInfo sampler_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.anisotropyEnable = VK_TRUE,
		.maxAnisotropy = 16.0f,
		.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
		.unnormalizedCoordinates = VK_FALSE,
		.compareEnable = VK_FALSE,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
	};

	if (vkCreateSampler(renderer->device, &sampler_info, NULL,
		&renderer->default_sampler) != VK_SUCCESS) {
		syslog(LOG_ERR, "Failed to create sampler");
		return -1;
	}

	return 0;
}

/*
 * Create renderer
 */
struct nova_renderer *
nova_renderer_create(const nova_renderer_config_t *config)
{
	struct nova_renderer *renderer;

	renderer = calloc(1, sizeof(*renderer));
	if (renderer == NULL) {
		syslog(LOG_ERR, "Failed to allocate renderer");
		return NULL;
	}

	memcpy(&renderer->config, config, sizeof(*config));
	renderer->max_frames_in_flight = config->max_frames_in_flight > 0 ?
	    config->max_frames_in_flight :
	    2;
	renderer->target_frame_time_ns = 16666666; /* 60 FPS default */

	if (create_instance(renderer) != 0 ||
	    select_physical_device(renderer) != 0 ||
	    create_device(renderer) != 0 ||
	    create_command_pool(renderer) != 0 ||
	    create_sync_objects(renderer) != 0 ||
	    create_default_sampler(renderer) != 0) {
		nova_renderer_destroy(renderer);
		return NULL;
	}

	syslog(LOG_INFO, "Vulkan renderer initialized");
	return renderer;
}

/*
 * Destroy renderer
 */
void
nova_renderer_destroy(struct nova_renderer *renderer)
{
	if (renderer == NULL)
		return;

	if (renderer->device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(renderer->device);

		if (renderer->default_sampler != VK_NULL_HANDLE)
			vkDestroySampler(renderer->device,
			    renderer->default_sampler, NULL);

		for (int i = 0; i < renderer->max_frames_in_flight; i++) {
			if (renderer->image_available_semaphores)
				vkDestroySemaphore(renderer->device,
				    renderer->image_available_semaphores[i],
				    NULL);
			if (renderer->render_finished_semaphores)
				vkDestroySemaphore(renderer->device,
				    renderer->render_finished_semaphores[i],
				    NULL);
			if (renderer->in_flight_fences)
				vkDestroyFence(renderer->device,
				    renderer->in_flight_fences[i], NULL);
		}

		free(renderer->image_available_semaphores);
		free(renderer->render_finished_semaphores);
		free(renderer->in_flight_fences);

		if (renderer->graphics_pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(renderer->device,
			    renderer->graphics_pipeline, NULL);
		if (renderer->pipeline_layout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(renderer->device,
			    renderer->pipeline_layout, NULL);
		if (renderer->render_pass != VK_NULL_HANDLE)
			vkDestroyRenderPass(renderer->device,
			    renderer->render_pass, NULL);
		if (renderer->command_pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(renderer->device,
			    renderer->command_pool, NULL);

		vkDestroyDevice(renderer->device, NULL);
	}

	if (renderer->instance != VK_NULL_HANDLE)
		vkDestroyInstance(renderer->instance, NULL);

	free(renderer);
}

/*
 * Create render target (swapchain for a display)
 */
struct nova_render_target *
nova_render_target_create(struct nova_renderer *renderer,
    const nova_render_target_config_t *config)
{
	struct nova_render_target *target;

	target = calloc(1, sizeof(*target));
	if (target == NULL)
		return NULL;

	target->renderer = renderer;
	target->surface = config->surface;
	target->adaptive_sync = config->adaptive_sync;
	target->refresh_hz = config->refresh_hz;
	target->extent.width = config->width;
	target->extent.height = config->height;

	/* Get surface capabilities */
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(renderer->physical_device,
	    target->surface, &target->capabilities);

	/* Choose format */
	uint32_t format_count;
	vkGetPhysicalDeviceSurfaceFormatsKHR(renderer->physical_device,
	    target->surface, &format_count, NULL);

	VkSurfaceFormatKHR *formats = calloc(format_count,
	    sizeof(VkSurfaceFormatKHR));
	vkGetPhysicalDeviceSurfaceFormatsKHR(renderer->physical_device,
	    target->surface, &format_count, formats);

	target->format = VK_FORMAT_B8G8R8A8_SRGB; /* Default */
	for (uint32_t i = 0; i < format_count; i++) {
		if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
		    formats[i].colorSpace ==
			VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			target->format = formats[i].format;
			break;
		}
	}
	free(formats);

	/* Create swapchain */
	target->image_count = target->capabilities.minImageCount + 1;
	if (target->capabilities.maxImageCount > 0 &&
	    target->image_count > target->capabilities.maxImageCount) {
		target->image_count = target->capabilities.maxImageCount;
	}

	VkSwapchainCreateInfoKHR swapchain_info = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = target->surface,
		.minImageCount = target->image_count,
		.imageFormat = target->format,
		.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
		.imageExtent = target->extent,
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.preTransform = target->capabilities.currentTransform,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = target->adaptive_sync ?
		    VK_PRESENT_MODE_MAILBOX_KHR :
		    VK_PRESENT_MODE_FIFO_KHR,
		.clipped = VK_TRUE,
	};

	if (vkCreateSwapchainKHR(renderer->device, &swapchain_info, NULL,
		&target->swapchain) != VK_SUCCESS) {
		syslog(LOG_ERR, "Failed to create swapchain");
		free(target);
		return NULL;
	}

	/* Get swapchain images */
	vkGetSwapchainImagesKHR(renderer->device, target->swapchain,
	    &target->image_count, NULL);
	target->images = calloc(target->image_count, sizeof(VkImage));
	vkGetSwapchainImagesKHR(renderer->device, target->swapchain,
	    &target->image_count, target->images);

	/* Create image views */
	target->image_views = calloc(target->image_count, sizeof(VkImageView));
	for (uint32_t i = 0; i < target->image_count; i++) {
		VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = target->images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = target->format,
            .components = {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };

		if (vkCreateImageView(renderer->device, &view_info, NULL,
			&target->image_views[i]) != VK_SUCCESS) {
			syslog(LOG_ERR, "Failed to create image view");
		}
	}

	syslog(LOG_INFO, "Render target created: %dx%d, %d images",
	    target->extent.width, target->extent.height, target->image_count);

	return target;
}

/*
 * Destroy render target
 */
void
nova_render_target_destroy(struct nova_render_target *target)
{
	if (target == NULL)
		return;

	struct nova_renderer *renderer = target->renderer;

	vkDeviceWaitIdle(renderer->device);

	for (uint32_t i = 0; i < target->image_count; i++) {
		if (target->framebuffers)
			vkDestroyFramebuffer(renderer->device,
			    target->framebuffers[i], NULL);
		if (target->image_views)
			vkDestroyImageView(renderer->device,
			    target->image_views[i], NULL);
	}

	free(target->framebuffers);
	free(target->image_views);
	free(target->images);

	if (target->swapchain != VK_NULL_HANDLE)
		vkDestroySwapchainKHR(renderer->device, target->swapchain,
		    NULL);

	free(target);
}

/*
 * Resize render target
 */
int
nova_render_target_resize(struct nova_render_target *target, int width,
    int height)
{
	/* Recreate swapchain with new size */
	target->extent.width = width;
	target->extent.height = height;

	/* TODO: Implement swapchain recreation */

	return 0;
}

/*
 * Create texture
 */
struct nova_texture *
nova_texture_create(struct nova_renderer *renderer, int width, int height,
    nova_texture_format_t format)
{
	struct nova_texture *tex;
	VkFormat vk_format;

	tex = calloc(1, sizeof(*tex));
	if (tex == NULL)
		return NULL;

	tex->renderer = renderer;
	tex->width = width;
	tex->height = height;
	tex->format = format;

	switch (format) {
	case NOVA_FORMAT_RGBA8:
		vk_format = VK_FORMAT_R8G8B8A8_UNORM;
		break;
	case NOVA_FORMAT_BGRA8:
		vk_format = VK_FORMAT_B8G8R8A8_UNORM;
		break;
	case NOVA_FORMAT_RGB10A2:
		vk_format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		break;
	case NOVA_FORMAT_RGBA16F:
		vk_format = VK_FORMAT_R16G16B16A16_SFLOAT;
		break;
	default:
		vk_format = VK_FORMAT_R8G8B8A8_UNORM;
	}

	/* Create image */
	VkImageCreateInfo image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.extent = { width, height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.format = vk_format,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		    VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.samples = VK_SAMPLE_COUNT_1_BIT,
	};

	if (vkCreateImage(renderer->device, &image_info, NULL, &tex->image) !=
	    VK_SUCCESS) {
		free(tex);
		return NULL;
	}

	/* Allocate memory */
	VkMemoryRequirements mem_reqs;
	vkGetImageMemoryRequirements(renderer->device, tex->image, &mem_reqs);

	VkMemoryAllocateInfo alloc_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mem_reqs.size,
		.memoryTypeIndex = find_memory_type(renderer,
		    mem_reqs.memoryTypeBits,
		    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
	};

	if (vkAllocateMemory(renderer->device, &alloc_info, NULL,
		&tex->memory) != VK_SUCCESS) {
		vkDestroyImage(renderer->device, tex->image, NULL);
		free(tex);
		return NULL;
	}

	vkBindImageMemory(renderer->device, tex->image, tex->memory, 0);

	/* Create image view */
	VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = tex->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = vk_format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

	if (vkCreateImageView(renderer->device, &view_info, NULL, &tex->view) !=
	    VK_SUCCESS) {
		vkFreeMemory(renderer->device, tex->memory, NULL);
		vkDestroyImage(renderer->device, tex->image, NULL);
		free(tex);
		return NULL;
	}

	return tex;
}

/*
 * Create texture from DMA-BUF (for Wayland client buffers)
 */
struct nova_texture *
nova_texture_from_dmabuf(struct nova_renderer *renderer, int fd, int width,
    int height, int stride, uint32_t format, uint64_t modifier)
{
	/* TODO: Implement DMA-BUF import using VK_EXT_external_memory_dma_buf
	 */
	return nova_texture_create(renderer, width, height, NOVA_FORMAT_BGRA8);
}

/*
 * Destroy texture
 */
void
nova_texture_destroy(struct nova_texture *texture)
{
	if (texture == NULL)
		return;

	struct nova_renderer *renderer = texture->renderer;

	if (texture->view != VK_NULL_HANDLE)
		vkDestroyImageView(renderer->device, texture->view, NULL);
	if (texture->memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer->device, texture->memory, NULL);
	if (texture->image != VK_NULL_HANDLE)
		vkDestroyImage(renderer->device, texture->image, NULL);

	free(texture);
}

/*
 * Begin frame rendering
 */
int
nova_renderer_begin_frame(struct nova_renderer *renderer,
    struct nova_render_target *target, nova_frame_context_t *ctx)
{
	int current_frame = renderer->current_frame;

	/* Wait for previous frame */
	vkWaitForFences(renderer->device, 1,
	    &renderer->in_flight_fences[current_frame], VK_TRUE, UINT64_MAX);

	/* Acquire next image */
	uint32_t image_index;
	VkResult result = vkAcquireNextImageKHR(renderer->device,
	    target->swapchain, UINT64_MAX,
	    renderer->image_available_semaphores[current_frame], VK_NULL_HANDLE,
	    &image_index);

	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		/* Swapchain needs recreation */
		return -1;
	}

	vkResetFences(renderer->device, 1,
	    &renderer->in_flight_fences[current_frame]);

	ctx->target = target;
	ctx->frame_index = image_index;
	ctx->frame_time_ns = 0; /* TODO: Get actual time */

	return 0;
}

/*
 * End frame and present
 */
int
nova_renderer_end_frame(struct nova_renderer *renderer,
    nova_frame_context_t *ctx)
{
	int current_frame = renderer->current_frame;
	struct nova_render_target *target = ctx->target;

	/* Submit command buffer */
	VkSemaphore wait_semaphores[] = {
		renderer->image_available_semaphores[current_frame]
	};
	VkPipelineStageFlags wait_stages[] = {
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
	};
	VkSemaphore signal_semaphores[] = {
		renderer->render_finished_semaphores[current_frame]
	};

	VkSubmitInfo submit_info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = wait_semaphores,
		.pWaitDstStageMask = wait_stages,
		.commandBufferCount = 0, /* TODO: Add command buffers */
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = signal_semaphores,
	};

	vkQueueSubmit(renderer->graphics_queue, 1, &submit_info,
	    renderer->in_flight_fences[current_frame]);

	/* Present */
	VkSwapchainKHR swapchains[] = { target->swapchain };
	uint32_t image_indices[] = { ctx->frame_index };

	VkPresentInfoKHR present_info = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = signal_semaphores,
		.swapchainCount = 1,
		.pSwapchains = swapchains,
		.pImageIndices = image_indices,
	};

	vkQueuePresentKHR(renderer->present_queue, &present_info);

	renderer->current_frame = (current_frame + 1) %
	    renderer->max_frames_in_flight;

	return 0;
}

/*
 * Drawing primitives
 */
void
nova_renderer_clear(nova_frame_context_t *ctx, const nova_color_t *color)
{
	/* TODO: Record clear command */
}

void
nova_renderer_draw_texture(nova_frame_context_t *ctx,
    struct nova_texture *texture, const nova_rect_t *src_rect,
    const nova_rect_t *dst_rect, float opacity, float corner_radius)
{
	/* TODO: Record draw command */
}

void
nova_renderer_draw_blur(nova_frame_context_t *ctx, const nova_rect_t *rect,
    float radius, float opacity)
{
	/* TODO: Record blur command */
}

void
nova_renderer_draw_shadow(nova_frame_context_t *ctx, const nova_rect_t *rect,
    float radius, float opacity, int offset_x, int offset_y,
    float corner_radius)
{
	/* TODO: Record shadow command */
}

void
nova_renderer_draw_rect(nova_frame_context_t *ctx, const nova_rect_t *rect,
    const nova_color_t *color, float corner_radius)
{
	/* TODO: Record rectangle command */
}

/*
 * Frame stats
 */
int
nova_renderer_get_stats(struct nova_renderer *renderer,
    struct nova_render_target *target, nova_frame_stats_t *stats)
{
	memset(stats, 0, sizeof(*stats));
	stats->fps = 60.0f; /* TODO: Calculate actual FPS */
	return 0;
}

void
nova_renderer_set_target_frame_time(struct nova_renderer *renderer,
    uint64_t target_ns)
{
	if (renderer != NULL)
		renderer->target_frame_time_ns = target_ns;
}
