/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * nova-audiod - Audio Daemon Implementation
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/soundcard.h>
#include <sys/un.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "nova_audio.h"

#define NOVA_AUDIOD_SOCKET "/var/run/nova/audiod.sock"
#define MAX_DEVICES	   16
#define MAX_STREAMS	   64

/*
 * Internal device structure
 */
struct nova_audio_device_internal {
	char id[64];
	char name[128];
	char dev_path[64];
	nova_audio_device_type_t type;
	bool available;
	bool is_default;
	float volume;
	bool muted;
	int fd;
};

/*
 * Internal stream structure
 */
struct nova_audio_stream_internal {
	uint32_t id;
	pid_t pid;
	char app_id[128];
	char device_id[64];
	float volume;
	bool muted;
	int sample_rate;
	int channels;
	bool active;
};

/*
 * Audio daemon state (client-side connection)
 */
struct nova_audiod_client {
	int sock_fd;

	/* Device callback */
	nova_audio_device_callback_t device_callback;
	void *device_callback_data;

	/* Stream callback */
	nova_audio_stream_callback_t stream_callback;
	void *stream_callback_data;
};

static struct nova_audiod_client *g_client = NULL;

/*
 * Audio daemon state (server-side)
 */
struct nova_audiod {
	int sock_fd;
	int kq;
	volatile bool running;

	/* Devices */
	struct nova_audio_device_internal *devices[MAX_DEVICES];
	size_t device_count;

	/* Streams */
	struct nova_audio_stream_internal *streams[MAX_STREAMS];
	size_t stream_count;
	uint32_t next_stream_id;

	/* Default devices */
	char default_output[64];
	char default_input[64];
};

/*
 * OSS mixer control
 */
static int
oss_get_volume(const char *dev_path, int *left, int *right)
{
	int fd, vol;

	fd = open(dev_path, O_RDONLY);
	if (fd < 0)
		return -1;

	if (ioctl(fd, MIXER_READ(SOUND_MIXER_VOLUME), &vol) < 0) {
		close(fd);
		return -1;
	}

	*left = vol & 0xff;
	*right = (vol >> 8) & 0xff;

	close(fd);
	return 0;
}

static int
oss_set_volume(const char *dev_path, int left, int right)
{
	int fd, vol;

	fd = open(dev_path, O_RDWR);
	if (fd < 0)
		return -1;

	vol = (left & 0xff) | ((right & 0xff) << 8);

	if (ioctl(fd, MIXER_WRITE(SOUND_MIXER_VOLUME), &vol) < 0) {
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

/*
 * Enumerate OSS audio devices
 */
static int
audiod_enumerate_devices(struct nova_audiod *ad)
{
	struct nova_audio_device_internal *dev;
	oss_sysinfo si;
	int mixer_fd, i;

	mixer_fd = open("/dev/mixer", O_RDONLY);
	if (mixer_fd < 0) {
		syslog(LOG_WARNING, "Failed to open /dev/mixer: %s",
		    strerror(errno));
		/* Create a default device anyway */
		dev = calloc(1, sizeof(*dev));
		if (dev != NULL) {
			strlcpy(dev->id, "default", sizeof(dev->id));
			strlcpy(dev->name, "Default Audio Device",
			    sizeof(dev->name));
			strlcpy(dev->dev_path, "/dev/dsp",
			    sizeof(dev->dev_path));
			dev->type = NOVA_AUDIO_DEVICE_OUTPUT;
			dev->available = true;
			dev->is_default = true;
			dev->volume = 1.0f;
			dev->fd = -1;
			ad->devices[ad->device_count++] = dev;
			strlcpy(ad->default_output, dev->id,
			    sizeof(ad->default_output));
		}
		return 0;
	}

	/* Get system info */
	if (ioctl(mixer_fd, SNDCTL_SYSINFO, &si) >= 0) {
		syslog(LOG_INFO, "OSS: %d audio devices, %d mixers",
		    si.numaudios, si.nummixers);

		for (i = 0; i < si.numaudios && ad->device_count < MAX_DEVICES;
		    i++) {
			oss_audioinfo ai;
			ai.dev = i;

			if (ioctl(mixer_fd, SNDCTL_AUDIOINFO, &ai) >= 0) {
				dev = calloc(1, sizeof(*dev));
				if (dev == NULL)
					continue;

				snprintf(dev->id, sizeof(dev->id), "oss-%d", i);
				strlcpy(dev->name, ai.name, sizeof(dev->name));
				strlcpy(dev->dev_path, ai.devnode,
				    sizeof(dev->dev_path));

				if (ai.caps & PCM_CAP_OUTPUT)
					dev->type = NOVA_AUDIO_DEVICE_OUTPUT;
				else
					dev->type = NOVA_AUDIO_DEVICE_INPUT;

				dev->available = (ai.enabled != 0);
				dev->volume = 1.0f;
				dev->fd = -1;

				/* First output is default */
				if (ad->default_output[0] == '\0' &&
				    dev->type == NOVA_AUDIO_DEVICE_OUTPUT) {
					dev->is_default = true;
					strlcpy(ad->default_output, dev->id,
					    sizeof(ad->default_output));
				}

				ad->devices[ad->device_count++] = dev;
				syslog(LOG_INFO, "Found audio device: %s (%s)",
				    dev->name, dev->id);
			}
		}
	}

	close(mixer_fd);
	return 0;
}

/*
 * Create audio daemon
 */
struct nova_audiod *
nova_audiod_create(void)
{
	struct nova_audiod *ad;
	struct sockaddr_un addr;

	ad = calloc(1, sizeof(*ad));
	if (ad == NULL) {
		syslog(LOG_ERR, "Failed to allocate audio daemon");
		return NULL;
	}

	ad->sock_fd = -1;
	ad->kq = -1;
	ad->next_stream_id = 1;

	/* Create kqueue */
	ad->kq = kqueue();
	if (ad->kq < 0) {
		syslog(LOG_ERR, "Failed to create kqueue: %s", strerror(errno));
		free(ad);
		return NULL;
	}

	/* Create IPC socket */
	ad->sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ad->sock_fd < 0) {
		syslog(LOG_ERR, "Failed to create socket: %s", strerror(errno));
		close(ad->kq);
		free(ad);
		return NULL;
	}

	unlink(NOVA_AUDIOD_SOCKET);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strlcpy(addr.sun_path, NOVA_AUDIOD_SOCKET, sizeof(addr.sun_path));

	if (bind(ad->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		syslog(LOG_ERR, "Failed to bind socket: %s", strerror(errno));
		close(ad->sock_fd);
		close(ad->kq);
		free(ad);
		return NULL;
	}

	listen(ad->sock_fd, 5);

	/* Enumerate audio devices */
	audiod_enumerate_devices(ad);

	syslog(LOG_INFO, "Audio daemon created with %zu devices",
	    ad->device_count);
	return ad;
}

/*
 * Destroy audio daemon
 */
void
nova_audiod_destroy(struct nova_audiod *ad)
{
	size_t i;

	if (ad == NULL)
		return;

	for (i = 0; i < ad->device_count; i++) {
		if (ad->devices[i] != NULL) {
			if (ad->devices[i]->fd >= 0)
				close(ad->devices[i]->fd);
			free(ad->devices[i]);
		}
	}

	for (i = 0; i < ad->stream_count; i++)
		free(ad->streams[i]);

	if (ad->sock_fd >= 0) {
		close(ad->sock_fd);
		unlink(NOVA_AUDIOD_SOCKET);
	}

	if (ad->kq >= 0)
		close(ad->kq);

	free(ad);
}

/*
 * Client API: Connect to audio daemon
 */
int
nova_audio_connect(void)
{
	struct sockaddr_un addr;

	if (g_client != NULL)
		return 0; /* Already connected */

	g_client = calloc(1, sizeof(*g_client));
	if (g_client == NULL)
		return -1;

	g_client->sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (g_client->sock_fd < 0) {
		free(g_client);
		g_client = NULL;
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strlcpy(addr.sun_path, NOVA_AUDIOD_SOCKET, sizeof(addr.sun_path));

	if (connect(g_client->sock_fd, (struct sockaddr *)&addr, sizeof(addr)) <
	    0) {
		close(g_client->sock_fd);
		free(g_client);
		g_client = NULL;
		return -1;
	}

	return 0;
}

void
nova_audio_disconnect(void)
{
	if (g_client != NULL) {
		if (g_client->sock_fd >= 0)
			close(g_client->sock_fd);
		free(g_client);
		g_client = NULL;
	}
}

int
nova_audio_get_fd(void)
{
	return g_client ? g_client->sock_fd : -1;
}

int
nova_audio_dispatch(void)
{
	/* TODO: Implement message processing */
	return 0;
}

/*
 * Device management - stub implementations
 */
int
nova_audio_list_devices(nova_audio_device_type_t type,
    nova_audio_device_t **devices, size_t *count)
{
	/* TODO: Request device list from daemon */
	*devices = NULL;
	*count = 0;
	return 0;
}

void
nova_audio_free_devices(nova_audio_device_t *devices)
{
	free(devices);
}

int
nova_audio_get_default_device(nova_audio_device_type_t type, char *device_id,
    size_t len)
{
	/* TODO: Query daemon */
	strlcpy(device_id, "default", len);
	return 0;
}

int
nova_audio_set_default_device(nova_audio_device_type_t type,
    const char *device_id)
{
	/* TODO: Send to daemon */
	return 0;
}

int
nova_audio_get_device_volume(const char *device_id, float *volume)
{
	int left, right;

	if (oss_get_volume("/dev/mixer", &left, &right) < 0)
		return -1;

	*volume = (float)(left + right) / 200.0f;
	return 0;
}

int
nova_audio_set_device_volume(const char *device_id, float volume)
{
	int level = (int)(volume * 100.0f);
	if (level < 0)
		level = 0;
	if (level > 100)
		level = 100;

	return oss_set_volume("/dev/mixer", level, level);
}

int
nova_audio_get_device_mute(const char *device_id, bool *muted)
{
	float vol;
	if (nova_audio_get_device_volume(device_id, &vol) < 0)
		return -1;
	*muted = (vol < 0.001f);
	return 0;
}

int
nova_audio_set_device_mute(const char *device_id, bool muted)
{
	if (muted)
		return nova_audio_set_device_volume(device_id, 0.0f);
	return 0; /* Unmuting would need stored volume */
}

/*
 * Stream management stubs
 */
int
nova_audio_list_streams(nova_audio_stream_t **streams, size_t *count)
{
	*streams = NULL;
	*count = 0;
	return 0;
}

void
nova_audio_free_streams(nova_audio_stream_t *streams)
{
	free(streams);
}

int
nova_audio_get_stream_volume(uint32_t stream_id, float *volume)
{
	*volume = 1.0f;
	return 0;
}

int
nova_audio_set_stream_volume(uint32_t stream_id, float volume)
{
	return 0;
}

int
nova_audio_get_stream_mute(uint32_t stream_id, bool *muted)
{
	*muted = false;
	return 0;
}

int
nova_audio_set_stream_mute(uint32_t stream_id, bool muted)
{
	return 0;
}

int
nova_audio_move_stream(uint32_t stream_id, const char *device_id)
{
	return 0;
}

/*
 * Callbacks
 */
void
nova_audio_set_device_callback(nova_audio_device_callback_t callback,
    void *data)
{
	if (g_client != NULL) {
		g_client->device_callback = callback;
		g_client->device_callback_data = data;
	}
}

void
nova_audio_set_stream_callback(nova_audio_stream_callback_t callback,
    void *data)
{
	if (g_client != NULL) {
		g_client->stream_callback = callback;
		g_client->stream_callback_data = data;
	}
}

/*
 * Low-latency mode
 */
int
nova_audio_request_low_latency(const nova_audio_low_latency_config_t *config)
{
	/* TODO: Implement low-latency mode */
	return 0;
}

int
nova_audio_release_low_latency(void)
{
	return 0;
}
