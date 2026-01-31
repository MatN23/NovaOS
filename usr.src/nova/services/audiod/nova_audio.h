/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * nova-audiod - Modern Audio Daemon
 */

#ifndef _NOVA_AUDIO_H_
#define _NOVA_AUDIO_H_

#include <sys/types.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Nova Audio Daemon
 *
 * Modern audio management built on FreeBSD's OSS subsystem:
 * - Per-application volume control
 * - Device hot-plug support (USB, Bluetooth)
 * - Low-latency audio path for professional work
 * - Audio routing between devices
 * - Persistent device preferences
 */

/*
 * Device types
 */

typedef enum nova_audio_device_type {
	NOVA_AUDIO_DEVICE_OUTPUT,
	NOVA_AUDIO_DEVICE_INPUT,
} nova_audio_device_type_t;

/*
 * Device information
 */

typedef struct nova_audio_device {
	char id[64];	/* Unique device ID */
	char name[128]; /* Friendly name */
	char description[256];
	nova_audio_device_type_t type;
	bool available;	 /* Currently connected */
	bool is_default; /* Current default device */
	int priority;	 /* Auto-switch priority */
	float volume;	 /* 0.0 - 1.0 */
	bool muted;
} nova_audio_device_t;

/*
 * Audio stream information
 */

typedef struct nova_audio_stream {
	uint32_t id;	    /* Stream ID */
	pid_t pid;	    /* Owning process */
	char app_id[128];   /* Application ID */
	char app_name[128]; /* Application name */
	char device_id[64]; /* Output/input device */
	float volume;	    /* 0.0 - 1.0 */
	bool muted;
	bool corked; /* Paused */
	int sample_rate;
	int channels;
} nova_audio_stream_t;

/*
 * Connection to audio daemon
 */

int nova_audio_connect(void);
void nova_audio_disconnect(void);
int nova_audio_dispatch(void);
int nova_audio_get_fd(void);

/*
 * Device management
 */

/* List audio devices */
int nova_audio_list_devices(nova_audio_device_type_t type,
    nova_audio_device_t **devices, size_t *count);

void nova_audio_free_devices(nova_audio_device_t *devices);

/* Get/set default device */
int nova_audio_get_default_device(nova_audio_device_type_t type,
    char *device_id, size_t len);

int nova_audio_set_default_device(nova_audio_device_type_t type,
    const char *device_id);

/* Device volume control */
int nova_audio_get_device_volume(const char *device_id, float *volume);
int nova_audio_set_device_volume(const char *device_id, float volume);
int nova_audio_get_device_mute(const char *device_id, bool *muted);
int nova_audio_set_device_mute(const char *device_id, bool muted);

/*
 * Stream management
 */

/* List active streams */
int nova_audio_list_streams(nova_audio_stream_t **streams, size_t *count);
void nova_audio_free_streams(nova_audio_stream_t *streams);

/* Stream volume control */
int nova_audio_get_stream_volume(uint32_t stream_id, float *volume);
int nova_audio_set_stream_volume(uint32_t stream_id, float volume);
int nova_audio_get_stream_mute(uint32_t stream_id, bool *muted);
int nova_audio_set_stream_mute(uint32_t stream_id, bool muted);

/* Move stream to different device */
int nova_audio_move_stream(uint32_t stream_id, const char *device_id);

/*
 * Application preferences
 */

typedef struct nova_audio_app_prefs {
	char app_id[128];
	char preferred_output[64];
	char preferred_input[64];
	float default_volume;
	bool noise_suppression;
	float input_boost_db;
} nova_audio_app_prefs_t;

int nova_audio_get_app_prefs(const char *app_id, nova_audio_app_prefs_t *prefs);
int nova_audio_set_app_prefs(const nova_audio_app_prefs_t *prefs);

/*
 * Callbacks
 */

typedef void (*nova_audio_device_callback_t)(void *data,
    const nova_audio_device_t *device, const char *event);

typedef void (*nova_audio_stream_callback_t)(void *data,
    const nova_audio_stream_t *stream, const char *event);

void nova_audio_set_device_callback(nova_audio_device_callback_t callback,
    void *data);

void nova_audio_set_stream_callback(nova_audio_stream_callback_t callback,
    void *data);

/*
 * Low-latency mode
 */

typedef struct nova_audio_low_latency_config {
	int buffer_size; /* Frames */
	int periods;
	int sample_rate;
	bool exclusive; /* Exclusive device access */
} nova_audio_low_latency_config_t;

/* Request low-latency audio mode for an application */
int nova_audio_request_low_latency(
    const nova_audio_low_latency_config_t *config);

/* Release low-latency mode */
int nova_audio_release_low_latency(void);

#ifdef __cplusplus
}
#endif

#endif /* _NOVA_AUDIO_H_ */
