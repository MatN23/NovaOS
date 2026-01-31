/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * NovaCompositor - Input Handling
 */

#ifndef _NOVA_INPUT_H_
#define _NOVA_INPUT_H_

#include <sys/types.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Nova Input Manager
 *
 * Handles input devices via FreeBSD's evdev subsystem:
 * - Keyboards, mice, touchpads, tablets
 * - Device hotplug detection via devd
 * - XKB keyboard layout handling
 * - Pointer acceleration profiles
 * - Multi-touch gesture recognition
 */

struct nova_input;
struct nova_input_device;
struct nova_gesture_recognizer;

/*
 * Input manager initialization
 */

typedef struct nova_input_config {
	const char *xkb_rules;	 /* XKB rules file */
	const char *xkb_model;	 /* Keyboard model */
	const char *xkb_layout;	 /* Layout (e.g., "us") */
	const char *xkb_variant; /* Variant */
	const char *xkb_options; /* Options (e.g., "ctrl:nocaps") */
} nova_input_config_t;

struct nova_input *nova_input_create(const nova_input_config_t *config);
void nova_input_destroy(struct nova_input *input);

/* Get file descriptors for event polling */
int nova_input_get_fd_count(struct nova_input *input);
int nova_input_get_fds(struct nova_input *input, int *fds, int max_fds);

/* Process pending input events */
int nova_input_dispatch(struct nova_input *input);

/*
 * Device enumeration
 */

typedef enum nova_device_type {
	NOVA_DEVICE_KEYBOARD = 0x01,
	NOVA_DEVICE_POINTER = 0x02,
	NOVA_DEVICE_TOUCH = 0x04,
	NOVA_DEVICE_TABLET = 0x08,
	NOVA_DEVICE_SWITCH = 0x10, /* Lid switch, etc. */
} nova_device_type_t;

typedef struct nova_device_info {
	uint32_t id;
	char name[128];
	char sysname[64]; /* /dev/input/eventX */
	uint32_t types;	  /* Bitmask of nova_device_type_t */
	uint16_t vendor_id;
	uint16_t product_id;
} nova_device_info_t;

int nova_input_get_devices(struct nova_input *input,
    nova_device_info_t **devices, size_t *count);

void nova_input_free_devices(nova_device_info_t *devices);

/*
 * Device configuration
 */

typedef struct nova_pointer_config {
	float accel_speed; /* -1.0 to 1.0 */
	bool natural_scroll;
	bool left_handed;
	float scroll_factor;
} nova_pointer_config_t;

typedef struct nova_touchpad_config {
	bool enabled;
	bool tap_to_click;
	bool tap_and_drag;
	bool two_finger_scroll;
	bool natural_scroll;
	bool disable_while_typing;
	float accel_speed;
	int click_method; /* 0=areas, 1=fingers */
} nova_touchpad_config_t;

typedef struct nova_keyboard_config {
	int repeat_delay_ms;
	int repeat_rate; /* chars/sec */
} nova_keyboard_config_t;

int nova_input_configure_pointer(struct nova_input *input, uint32_t device_id,
    const nova_pointer_config_t *config);

int nova_input_configure_touchpad(struct nova_input *input, uint32_t device_id,
    const nova_touchpad_config_t *config);

int nova_input_configure_keyboard(struct nova_input *input,
    const nova_keyboard_config_t *config);

/*
 * Input events
 */

typedef enum nova_key_state {
	NOVA_KEY_RELEASED = 0,
	NOVA_KEY_PRESSED = 1,
	NOVA_KEY_REPEAT = 2,
} nova_key_state_t;

typedef enum nova_button_state {
	NOVA_BUTTON_RELEASED = 0,
	NOVA_BUTTON_PRESSED = 1,
} nova_button_state_t;

typedef struct nova_key_event {
	uint32_t device_id;
	uint32_t time_ms;
	uint32_t keycode; /* Linux keycode */
	uint32_t keysym;  /* XKB keysym */
	nova_key_state_t state;
	uint32_t mods; /* Modifier state */
	char utf8[8];  /* UTF-8 character, if printable */
} nova_key_event_t;

typedef struct nova_pointer_event {
	uint32_t device_id;
	uint32_t time_ms;
	double x, y;   /* Absolute position */
	double dx, dy; /* Relative motion */
} nova_pointer_event_t;

typedef struct nova_button_event {
	uint32_t device_id;
	uint32_t time_ms;
	uint32_t button; /* Button code */
	nova_button_state_t state;
} nova_button_event_t;

typedef struct nova_scroll_event {
	uint32_t device_id;
	uint32_t time_ms;
	double dx, dy; /* Scroll delta */
	bool discrete; /* Discrete scroll wheel */
} nova_scroll_event_t;

typedef struct nova_touch_event {
	uint32_t device_id;
	uint32_t time_ms;
	int32_t slot; /* Touch point ID */
	double x, y;  /* Position (0.0-1.0) */
	enum { NOVA_TOUCH_DOWN, NOVA_TOUCH_UP, NOVA_TOUCH_MOTION } type;
} nova_touch_event_t;

/*
 * Event callbacks
 */

typedef void (*nova_key_callback_t)(void *data, const nova_key_event_t *event);
typedef void (
    *nova_pointer_callback_t)(void *data, const nova_pointer_event_t *event);
typedef void (
    *nova_button_callback_t)(void *data, const nova_button_event_t *event);
typedef void (
    *nova_scroll_callback_t)(void *data, const nova_scroll_event_t *event);
typedef void (
    *nova_touch_callback_t)(void *data, const nova_touch_event_t *event);
typedef void (*nova_device_callback_t)(void *data,
    const nova_device_info_t *device, bool added);

void nova_input_set_key_callback(struct nova_input *input,
    nova_key_callback_t callback, void *data);

void nova_input_set_pointer_callback(struct nova_input *input,
    nova_pointer_callback_t callback, void *data);

void nova_input_set_button_callback(struct nova_input *input,
    nova_button_callback_t callback, void *data);

void nova_input_set_scroll_callback(struct nova_input *input,
    nova_scroll_callback_t callback, void *data);

void nova_input_set_touch_callback(struct nova_input *input,
    nova_touch_callback_t callback, void *data);

void nova_input_set_device_callback(struct nova_input *input,
    nova_device_callback_t callback, void *data);

/*
 * Gesture recognition
 */

typedef enum nova_gesture_type {
	NOVA_GESTURE_NONE,
	NOVA_GESTURE_SWIPE_LEFT,
	NOVA_GESTURE_SWIPE_RIGHT,
	NOVA_GESTURE_SWIPE_UP,
	NOVA_GESTURE_SWIPE_DOWN,
	NOVA_GESTURE_PINCH_IN,
	NOVA_GESTURE_PINCH_OUT,
	NOVA_GESTURE_ROTATE,
} nova_gesture_type_t;

typedef struct nova_gesture_event {
	nova_gesture_type_t type;
	int fingers;	/* Number of fingers */
	float delta;	/* Progress (0.0-1.0) */
	bool ended;	/* Gesture completed */
	bool cancelled; /* Gesture cancelled */
} nova_gesture_event_t;

typedef void (
    *nova_gesture_callback_t)(void *data, const nova_gesture_event_t *event);

void nova_input_set_gesture_callback(struct nova_input *input,
    nova_gesture_callback_t callback, void *data);

/* Enable/disable gesture recognition for N-finger gestures */
void nova_input_enable_gestures(struct nova_input *input, int min_fingers,
    int max_fingers);

#ifdef __cplusplus
}
#endif

#endif /* _NOVA_INPUT_H_ */
