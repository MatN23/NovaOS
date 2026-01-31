/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * NovaCompositor - Input Backend Implementation
 */

#include <sys/types.h>
#include <sys/event.h>
#include <sys/ioctl.h>

#include <dev/evdev/input.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "nova_input.h"

/* XKB for keyboard handling */
#include <xkbcommon/xkbcommon.h>

#define MAX_DEVICES    32
#define INPUT_DEV_PATH "/dev/input"

/*
 * Internal device structure
 */
struct nova_input_device_internal {
	uint32_t id;
	int fd;
	char path[64];
	char name[128];
	nova_device_type_t types;
	uint16_t vendor_id;
	uint16_t product_id;
	bool active;

	/* Pointer state */
	double x, y;
	uint32_t buttons;

	/* Touch state */
	struct {
		int32_t slot;
		double x, y;
		bool active;
	} touches[10];
	int active_touches;
};

/*
 * Gesture recognizer state
 */
struct nova_gesture_state {
	bool active;
	nova_gesture_type_t type;
	int fingers;
	double start_x[5];
	double start_y[5];
	double delta;
};

/*
 * Input manager
 */
struct nova_input {
	/* Configuration */
	nova_input_config_t config;

	/* XKB keyboard state */
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;

	/* Keyboard settings */
	nova_keyboard_config_t keyboard_config;

	/* Pointer settings */
	nova_pointer_config_t pointer_config;

	/* Touchpad settings */
	nova_touchpad_config_t touchpad_config;

	/* Devices */
	struct nova_input_device_internal *devices[MAX_DEVICES];
	size_t device_count;
	uint32_t next_device_id;

	/* Event loop */
	int kq;

	/* Gesture recognition */
	struct nova_gesture_state gesture;
	int gesture_min_fingers;
	int gesture_max_fingers;

	/* Callbacks */
	nova_key_callback_t key_callback;
	void *key_callback_data;
	nova_pointer_callback_t pointer_callback;
	void *pointer_callback_data;
	nova_button_callback_t button_callback;
	void *button_callback_data;
	nova_scroll_callback_t scroll_callback;
	void *scroll_callback_data;
	nova_touch_callback_t touch_callback;
	void *touch_callback_data;
	nova_gesture_callback_t gesture_callback;
	void *gesture_callback_data;
	nova_device_callback_t device_callback;
	void *device_callback_data;
};

/*
 * Detect device capabilities
 */
static uint32_t
detect_device_types(int fd)
{
	unsigned long evbit[NBITS(EV_MAX)] = { 0 };
	unsigned long keybit[NBITS(KEY_MAX)] = { 0 };
	unsigned long relbit[NBITS(REL_MAX)] = { 0 };
	unsigned long absbit[NBITS(ABS_MAX)] = { 0 };
	uint32_t types = 0;

	if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0)
		return 0;

	/* Check for keyboard */
	if (test_bit(EV_KEY, evbit)) {
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit);

		/* Has alphabetic keys */
		if (test_bit(KEY_A, keybit) && test_bit(KEY_Z, keybit))
			types |= NOVA_DEVICE_KEYBOARD;
	}

	/* Check for pointer */
	if (test_bit(EV_REL, evbit)) {
		ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbit)), relbit);

		if (test_bit(REL_X, relbit) && test_bit(REL_Y, relbit))
			types |= NOVA_DEVICE_POINTER;
	}

	/* Check for touchpad/touchscreen */
	if (test_bit(EV_ABS, evbit)) {
		ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit);

		if (test_bit(ABS_MT_SLOT, absbit) ||
		    test_bit(ABS_MT_POSITION_X, absbit))
			types |= NOVA_DEVICE_TOUCH;
		else if (test_bit(ABS_X, absbit) && test_bit(ABS_Y, absbit))
			types |= NOVA_DEVICE_TABLET;
	}

	/* Check for switches (lid, etc.) */
	if (test_bit(EV_SW, evbit))
		types |= NOVA_DEVICE_SWITCH;

	return types;
}

/*
 * Open and add an input device
 */
static struct nova_input_device_internal *
add_device(struct nova_input *input, const char *path)
{
	struct nova_input_device_internal *dev;
	struct input_id id;
	char name[128] = "Unknown";
	int fd;
	uint32_t types;

	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return NULL;

	types = detect_device_types(fd);
	if (types == 0) {
		close(fd);
		return NULL;
	}

	/* Get device info */
	ioctl(fd, EVIOCGNAME(sizeof(name)), name);
	ioctl(fd, EVIOCGID, &id);

	/* Allocate device */
	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		close(fd);
		return NULL;
	}

	dev->id = input->next_device_id++;
	dev->fd = fd;
	strlcpy(dev->path, path, sizeof(dev->path));
	strlcpy(dev->name, name, sizeof(dev->name));
	dev->types = types;
	dev->vendor_id = id.vendor;
	dev->product_id = id.product;
	dev->active = true;

	/* Add to kqueue */
	struct kevent ev;
	EV_SET(&ev, fd, EVFILT_READ, EV_ADD, 0, 0, dev);
	kevent(input->kq, &ev, 1, NULL, 0, NULL);

	/* Store device */
	if (input->device_count < MAX_DEVICES) {
		input->devices[input->device_count++] = dev;
	}

	syslog(LOG_INFO, "Added input device: %s (%s) types=0x%x", dev->name,
	    path, types);

	/* Notify callback */
	if (input->device_callback) {
		nova_device_info_t info = {
			.id = dev->id,
			.types = dev->types,
			.vendor_id = dev->vendor_id,
			.product_id = dev->product_id,
		};
		strlcpy(info.name, dev->name, sizeof(info.name));
		strlcpy(info.sysname, dev->path, sizeof(info.sysname));
		input->device_callback(input->device_callback_data, &info,
		    true);
	}

	return dev;
}

/*
 * Enumerate input devices
 */
static int
enumerate_devices(struct nova_input *input)
{
	DIR *dir;
	struct dirent *ent;
	char path[128];

	dir = opendir(INPUT_DEV_PATH);
	if (dir == NULL) {
		syslog(LOG_WARNING, "Failed to open %s: %s", INPUT_DEV_PATH,
		    strerror(errno));
		return -1;
	}

	while ((ent = readdir(dir)) != NULL) {
		if (strncmp(ent->d_name, "event", 5) != 0)
			continue;

		snprintf(path, sizeof(path), "%s/%s", INPUT_DEV_PATH,
		    ent->d_name);
		add_device(input, path);
	}

	closedir(dir);

	syslog(LOG_INFO, "Enumerated %zu input devices", input->device_count);
	return 0;
}

/*
 * Initialize XKB keyboard handling
 */
static int
init_xkb(struct nova_input *input)
{
	struct xkb_rule_names names = {
		.rules = input->config.xkb_rules,
		.model = input->config.xkb_model,
		.layout = input->config.xkb_layout,
		.variant = input->config.xkb_variant,
		.options = input->config.xkb_options,
	};

	input->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (input->xkb_context == NULL) {
		syslog(LOG_ERR, "Failed to create XKB context");
		return -1;
	}

	input->xkb_keymap = xkb_keymap_new_from_names(input->xkb_context,
	    &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (input->xkb_keymap == NULL) {
		syslog(LOG_ERR, "Failed to create XKB keymap");
		return -1;
	}

	input->xkb_state = xkb_state_new(input->xkb_keymap);
	if (input->xkb_state == NULL) {
		syslog(LOG_ERR, "Failed to create XKB state");
		return -1;
	}

	syslog(LOG_INFO, "XKB initialized: layout=%s",
	    input->config.xkb_layout ? input->config.xkb_layout : "us");

	return 0;
}

/*
 * Create input manager
 */
struct nova_input *
nova_input_create(const nova_input_config_t *config)
{
	struct nova_input *input;

	input = calloc(1, sizeof(*input));
	if (input == NULL) {
		syslog(LOG_ERR, "Failed to allocate input manager");
		return NULL;
	}

	/* Copy configuration */
	if (config != NULL)
		memcpy(&input->config, config, sizeof(*config));

	/* Default values */
	input->next_device_id = 1;
	input->keyboard_config.repeat_delay_ms = 400;
	input->keyboard_config.repeat_rate = 30;
	input->pointer_config.accel_speed = 0.0f;
	input->pointer_config.natural_scroll = false;
	input->touchpad_config.enabled = true;
	input->touchpad_config.tap_to_click = true;
	input->touchpad_config.two_finger_scroll = true;
	input->touchpad_config.natural_scroll = true;
	input->gesture_min_fingers = 3;
	input->gesture_max_fingers = 4;

	/* Create kqueue */
	input->kq = kqueue();
	if (input->kq < 0) {
		syslog(LOG_ERR, "Failed to create kqueue: %s", strerror(errno));
		free(input);
		return NULL;
	}

	/* Initialize XKB */
	if (init_xkb(input) != 0) {
		close(input->kq);
		free(input);
		return NULL;
	}

	/* Enumerate devices */
	enumerate_devices(input);

	syslog(LOG_INFO, "Input manager created");
	return input;
}

/*
 * Destroy input manager
 */
void
nova_input_destroy(struct nova_input *input)
{
	if (input == NULL)
		return;

	/* Close devices */
	for (size_t i = 0; i < input->device_count; i++) {
		if (input->devices[i] != NULL) {
			if (input->devices[i]->fd >= 0)
				close(input->devices[i]->fd);
			free(input->devices[i]);
		}
	}

	/* Clean up XKB */
	if (input->xkb_state)
		xkb_state_unref(input->xkb_state);
	if (input->xkb_keymap)
		xkb_keymap_unref(input->xkb_keymap);
	if (input->xkb_context)
		xkb_context_unref(input->xkb_context);

	if (input->kq >= 0)
		close(input->kq);

	free(input);
}

/*
 * Get file descriptors for polling
 */
int
nova_input_get_fd_count(struct nova_input *input)
{
	return input ? (int)input->device_count : 0;
}

int
nova_input_get_fds(struct nova_input *input, int *fds, int max_fds)
{
	int count = 0;

	if (input == NULL || fds == NULL)
		return 0;

	for (size_t i = 0; i < input->device_count && count < max_fds; i++) {
		if (input->devices[i] != NULL && input->devices[i]->fd >= 0)
			fds[count++] = input->devices[i]->fd;
	}

	return count;
}

/*
 * Process keyboard event
 */
static void
process_key_event(struct nova_input *input,
    struct nova_input_device_internal *dev, const struct input_event *ev)
{
	nova_key_event_t key_event;
	xkb_keysym_t keysym;

	memset(&key_event, 0, sizeof(key_event));
	key_event.device_id = dev->id;
	key_event.time_ms = ev->time.tv_sec * 1000 + ev->time.tv_usec / 1000;
	key_event.keycode = ev->code;

	/* Update XKB state and get keysym */
	xkb_state_update_key(input->xkb_state, ev->code + 8,
	    ev->value ? XKB_KEY_DOWN : XKB_KEY_UP);

	keysym = xkb_state_key_get_one_sym(input->xkb_state, ev->code + 8);
	key_event.keysym = keysym;

	/* Get UTF-8 character */
	xkb_state_key_get_utf8(input->xkb_state, ev->code + 8, key_event.utf8,
	    sizeof(key_event.utf8));

	/* Get modifier state */
	key_event.mods = 0;
	if (xkb_state_mod_name_is_active(input->xkb_state, XKB_MOD_NAME_SHIFT,
		XKB_STATE_MODS_EFFECTIVE))
		key_event.mods |= 1;
	if (xkb_state_mod_name_is_active(input->xkb_state, XKB_MOD_NAME_CTRL,
		XKB_STATE_MODS_EFFECTIVE))
		key_event.mods |= 2;
	if (xkb_state_mod_name_is_active(input->xkb_state, XKB_MOD_NAME_ALT,
		XKB_STATE_MODS_EFFECTIVE))
		key_event.mods |= 4;
	if (xkb_state_mod_name_is_active(input->xkb_state, XKB_MOD_NAME_LOGO,
		XKB_STATE_MODS_EFFECTIVE))
		key_event.mods |= 8;

	/* Set state */
	switch (ev->value) {
	case 0:
		key_event.state = NOVA_KEY_RELEASED;
		break;
	case 1:
		key_event.state = NOVA_KEY_PRESSED;
		break;
	case 2:
		key_event.state = NOVA_KEY_REPEAT;
		break;
	}

	if (input->key_callback)
		input->key_callback(input->key_callback_data, &key_event);
}

/*
 * Process pointer motion event
 */
static void
process_pointer_motion(struct nova_input *input,
    struct nova_input_device_internal *dev, const struct input_event *ev,
    double dx, double dy)
{
	nova_pointer_event_t pointer_event;

	/* Apply acceleration */
	float accel = 1.0f + input->pointer_config.accel_speed;
	dx *= accel;
	dy *= accel;

	/* Update position */
	dev->x += dx;
	dev->y += dy;

	/* Clamp to screen bounds (TODO: Get actual screen size) */
	if (dev->x < 0)
		dev->x = 0;
	if (dev->y < 0)
		dev->y = 0;
	if (dev->x > 1920)
		dev->x = 1920;
	if (dev->y > 1080)
		dev->y = 1080;

	memset(&pointer_event, 0, sizeof(pointer_event));
	pointer_event.device_id = dev->id;
	pointer_event.time_ms = ev->time.tv_sec * 1000 +
	    ev->time.tv_usec / 1000;
	pointer_event.x = dev->x;
	pointer_event.y = dev->y;
	pointer_event.dx = dx;
	pointer_event.dy = dy;

	if (input->pointer_callback)
		input->pointer_callback(input->pointer_callback_data,
		    &pointer_event);
}

/*
 * Process button event
 */
static void
process_button_event(struct nova_input *input,
    struct nova_input_device_internal *dev, const struct input_event *ev)
{
	nova_button_event_t button_event;

	memset(&button_event, 0, sizeof(button_event));
	button_event.device_id = dev->id;
	button_event.time_ms = ev->time.tv_sec * 1000 + ev->time.tv_usec / 1000;
	button_event.button = ev->code;
	button_event.state = ev->value ? NOVA_BUTTON_PRESSED :
					 NOVA_BUTTON_RELEASED;

	/* Update button state */
	if (ev->value)
		dev->buttons |= (1 << (ev->code - BTN_LEFT));
	else
		dev->buttons &= ~(1 << (ev->code - BTN_LEFT));

	if (input->button_callback)
		input->button_callback(input->button_callback_data,
		    &button_event);
}

/*
 * Process scroll event
 */
static void
process_scroll_event(struct nova_input *input,
    struct nova_input_device_internal *dev, const struct input_event *ev)
{
	nova_scroll_event_t scroll_event;

	memset(&scroll_event, 0, sizeof(scroll_event));
	scroll_event.device_id = dev->id;
	scroll_event.time_ms = ev->time.tv_sec * 1000 + ev->time.tv_usec / 1000;

	if (ev->code == REL_WHEEL) {
		scroll_event.dy = ev->value *
		    input->pointer_config.scroll_factor;
		if (input->pointer_config.natural_scroll)
			scroll_event.dy = -scroll_event.dy;
		scroll_event.discrete = true;
	} else if (ev->code == REL_HWHEEL) {
		scroll_event.dx = ev->value *
		    input->pointer_config.scroll_factor;
		scroll_event.discrete = true;
	}

	if (input->scroll_callback)
		input->scroll_callback(input->scroll_callback_data,
		    &scroll_event);
}

/*
 * Process events from a device
 */
static void
process_device_events(struct nova_input *input,
    struct nova_input_device_internal *dev)
{
	struct input_event events[32];
	ssize_t len;
	double dx = 0, dy = 0;
	bool has_motion = false;

	len = read(dev->fd, events, sizeof(events));
	if (len <= 0)
		return;

	size_t count = len / sizeof(struct input_event);

	for (size_t i = 0; i < count; i++) {
		struct input_event *ev = &events[i];

		switch (ev->type) {
		case EV_KEY:
			if (ev->code >= BTN_MISC && ev->code < KEY_OK) {
				/* Button */
				process_button_event(input, dev, ev);
			} else {
				/* Keyboard key */
				process_key_event(input, dev, ev);
			}
			break;

		case EV_REL:
			switch (ev->code) {
			case REL_X:
				dx += ev->value;
				has_motion = true;
				break;
			case REL_Y:
				dy += ev->value;
				has_motion = true;
				break;
			case REL_WHEEL:
			case REL_HWHEEL:
				process_scroll_event(input, dev, ev);
				break;
			}
			break;

		case EV_ABS:
			/* TODO: Handle touch/tablet events */
			break;

		case EV_SYN:
			if (ev->code == SYN_REPORT && has_motion) {
				process_pointer_motion(input, dev, ev, dx, dy);
				dx = dy = 0;
				has_motion = false;
			}
			break;
		}
	}
}

/*
 * Dispatch pending input events
 */
int
nova_input_dispatch(struct nova_input *input)
{
	struct kevent events[16];
	struct timespec timeout = { 0, 0 };
	int nevents;

	if (input == NULL)
		return -1;

	nevents = kevent(input->kq, NULL, 0, events, 16, &timeout);

	for (int i = 0; i < nevents; i++) {
		struct nova_input_device_internal *dev = events[i].udata;

		if (dev != NULL && events[i].filter == EVFILT_READ) {
			process_device_events(input, dev);
		}
	}

	return 0;
}

/*
 * Get device list
 */
int
nova_input_get_devices(struct nova_input *input, nova_device_info_t **devices,
    size_t *count)
{
	nova_device_info_t *list;

	if (input == NULL || devices == NULL || count == NULL)
		return -1;

	if (input->device_count == 0) {
		*devices = NULL;
		*count = 0;
		return 0;
	}

	list = calloc(input->device_count, sizeof(*list));
	if (list == NULL)
		return -1;

	for (size_t i = 0; i < input->device_count; i++) {
		struct nova_input_device_internal *dev = input->devices[i];
		if (dev != NULL) {
			list[i].id = dev->id;
			strlcpy(list[i].name, dev->name, sizeof(list[i].name));
			strlcpy(list[i].sysname, dev->path,
			    sizeof(list[i].sysname));
			list[i].types = dev->types;
			list[i].vendor_id = dev->vendor_id;
			list[i].product_id = dev->product_id;
		}
	}

	*devices = list;
	*count = input->device_count;
	return 0;
}

void
nova_input_free_devices(nova_device_info_t *devices)
{
	free(devices);
}

/*
 * Configure devices
 */
int
nova_input_configure_pointer(struct nova_input *input, uint32_t device_id,
    const nova_pointer_config_t *config)
{
	if (input == NULL || config == NULL)
		return -1;

	memcpy(&input->pointer_config, config, sizeof(*config));
	return 0;
}

int
nova_input_configure_touchpad(struct nova_input *input, uint32_t device_id,
    const nova_touchpad_config_t *config)
{
	if (input == NULL || config == NULL)
		return -1;

	memcpy(&input->touchpad_config, config, sizeof(*config));
	return 0;
}

int
nova_input_configure_keyboard(struct nova_input *input,
    const nova_keyboard_config_t *config)
{
	if (input == NULL || config == NULL)
		return -1;

	memcpy(&input->keyboard_config, config, sizeof(*config));
	return 0;
}

/*
 * Set callbacks
 */
void
nova_input_set_key_callback(struct nova_input *input,
    nova_key_callback_t callback, void *data)
{
	if (input != NULL) {
		input->key_callback = callback;
		input->key_callback_data = data;
	}
}

void
nova_input_set_pointer_callback(struct nova_input *input,
    nova_pointer_callback_t callback, void *data)
{
	if (input != NULL) {
		input->pointer_callback = callback;
		input->pointer_callback_data = data;
	}
}

void
nova_input_set_button_callback(struct nova_input *input,
    nova_button_callback_t callback, void *data)
{
	if (input != NULL) {
		input->button_callback = callback;
		input->button_callback_data = data;
	}
}

void
nova_input_set_scroll_callback(struct nova_input *input,
    nova_scroll_callback_t callback, void *data)
{
	if (input != NULL) {
		input->scroll_callback = callback;
		input->scroll_callback_data = data;
	}
}

void
nova_input_set_touch_callback(struct nova_input *input,
    nova_touch_callback_t callback, void *data)
{
	if (input != NULL) {
		input->touch_callback = callback;
		input->touch_callback_data = data;
	}
}

void
nova_input_set_device_callback(struct nova_input *input,
    nova_device_callback_t callback, void *data)
{
	if (input != NULL) {
		input->device_callback = callback;
		input->device_callback_data = data;
	}
}

void
nova_input_set_gesture_callback(struct nova_input *input,
    nova_gesture_callback_t callback, void *data)
{
	if (input != NULL) {
		input->gesture_callback = callback;
		input->gesture_callback_data = data;
	}
}

void
nova_input_enable_gestures(struct nova_input *input, int min_fingers,
    int max_fingers)
{
	if (input != NULL) {
		input->gesture_min_fingers = min_fingers;
		input->gesture_max_fingers = max_fingers;
	}
}
