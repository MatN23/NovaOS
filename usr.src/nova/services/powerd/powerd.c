/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * nova-powerd - Power Management Daemon Implementation
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/un.h>

#include <dev/acpica/acpiio.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "nova_power.h"

#define NOVA_POWERD_SOCKET "/var/run/nova/powerd.sock"
#define ACPI_DEVICE	   "/dev/acpi"

/*
 * Power daemon state (client-side)
 */
struct nova_powerd_client {
	int sock_fd;

	nova_power_source_callback_t source_callback;
	void *source_callback_data;
	nova_battery_callback_t battery_callback;
	void *battery_callback_data;
	nova_lid_callback_t lid_callback;
	void *lid_callback_data;
	nova_profile_callback_t profile_callback;
	void *profile_callback_data;
};

static struct nova_powerd_client *g_client = NULL;

/*
 * Power daemon state (server-side)
 */
struct nova_powerd {
	int sock_fd;
	int kq;
	int acpi_fd;
	volatile bool running;

	/* Current state */
	nova_power_source_t power_source;
	nova_power_profile_t current_profile;
	nova_lid_state_t lid_state;
	int brightness;

	/* Battery info */
	nova_battery_info_t *batteries;
	size_t battery_count;

	/* Profile configurations */
	nova_power_profile_config_t profiles[3];

	/* Idle configuration */
	nova_idle_config_t idle_config;

	/* Inhibitors */
	struct {
		uint32_t cookie;
		char reason[128];
	} inhibitors[16];
	size_t inhibitor_count;
	uint32_t next_cookie;
};

/*
 * Read ACPI battery info
 */
static int
acpi_get_battery(int acpi_fd, int unit, nova_battery_info_t *info)
{
	union acpi_battery_ioctl_arg arg;

	memset(info, 0, sizeof(*info));
	snprintf(info->id, sizeof(info->id), "BAT%d", unit);

	arg.unit = unit;
	if (ioctl(acpi_fd, ACPIIO_BATT_GET_BIF, &arg) < 0)
		return -1;

	info->present = true;
	info->energy_full = (float)arg.bif.lfcap / 1000.0f;
	strlcpy(info->vendor, arg.bif.oeminfo, sizeof(info->vendor));
	strlcpy(info->model, arg.bif.model, sizeof(info->model));

	arg.unit = unit;
	if (ioctl(acpi_fd, ACPIIO_BATT_GET_BST, &arg) >= 0) {
		info->energy = (float)arg.bst.cap / 1000.0f;
		info->power_rate = (float)arg.bst.rate / 1000.0f;
		info->voltage = (float)arg.bst.volt / 1000.0f;

		if (info->energy_full > 0)
			info->percentage =
			    (int)((info->energy / info->energy_full) * 100);

		info->charging = (arg.bst.state & ACPI_BATT_STAT_CHARGING) != 0;
		info->discharging = (arg.bst.state & ACPI_BATT_STAT_DISCHARG) !=
		    0;

		/* Estimate time remaining */
		if (info->discharging && info->power_rate > 0)
			info->time_to_empty = (int)(info->energy /
			    info->power_rate * 3600);
		else
			info->time_to_empty = -1;

		if (info->charging && info->power_rate > 0)
			info->time_to_full = (int)((info->energy_full -
						       info->energy) /
			    info->power_rate * 3600);
		else
			info->time_to_full = -1;
	}

	return 0;
}

/*
 * Read AC adapter state
 */
static nova_power_source_t
acpi_get_power_source(int acpi_fd)
{
	int state;

	if (ioctl(acpi_fd, ACPIIO_ACAD_GET_STATUS, &state) < 0)
		return NOVA_POWER_AC; /* Default to AC if unknown */

	return state ? NOVA_POWER_AC : NOVA_POWER_BATTERY;
}

/*
 * Get display brightness via sysctl
 */
static int
get_brightness(void)
{
	int brightness = 100;
	size_t len = sizeof(brightness);

	/* Try ACPI video brightness first */
	if (sysctlbyname("hw.acpi.video.lcd0.brightness", &brightness, &len,
		NULL, 0) == 0)
		return brightness;

	/* Try backlight */
	if (sysctlbyname("dev.backlight.0.brightness", &brightness, &len, NULL,
		0) == 0)
		return brightness;

	return 100; /* Default */
}

/*
 * Set display brightness
 */
static int
set_brightness(int brightness)
{
	if (brightness < 0)
		brightness = 0;
	if (brightness > 100)
		brightness = 100;

	/* Try ACPI video brightness first */
	if (sysctlbyname("hw.acpi.video.lcd0.brightness", NULL, NULL,
		&brightness, sizeof(brightness)) == 0)
		return 0;

	/* Try backlight */
	if (sysctlbyname("dev.backlight.0.brightness", NULL, NULL, &brightness,
		sizeof(brightness)) == 0)
		return 0;

	return -1;
}

/*
 * Apply power profile
 */
static int
apply_profile(struct nova_powerd *pd, nova_power_profile_t profile)
{
	nova_power_profile_config_t *cfg = &pd->profiles[profile];
	int freq_level;
	size_t len;

	/* Set CPU frequency scaling */
	if (strcmp(cfg->cpu_governor, "performance") == 0)
		freq_level = 0; /* Maximum frequency */
	else if (strcmp(cfg->cpu_governor, "powersave") == 0)
		freq_level = 100; /* Minimum frequency */
	else
		freq_level = -1; /* Adaptive */

	len = sizeof(freq_level);
	sysctlbyname("dev.cpu.0.freq_levels", NULL, NULL, &freq_level, len);

	/* Set brightness if configured */
	if (cfg->display_brightness >= 0)
		set_brightness(cfg->display_brightness);

	pd->current_profile = profile;
	syslog(LOG_INFO, "Applied power profile: %s", cfg->cpu_governor);

	return 0;
}

/*
 * Initialize default profiles
 */
static void
init_default_profiles(struct nova_powerd *pd)
{
	/* Performance */
	pd->profiles[NOVA_POWER_PERFORMANCE].profile = NOVA_POWER_PERFORMANCE;
	strlcpy(pd->profiles[NOVA_POWER_PERFORMANCE].cpu_governor,
	    "performance", sizeof(pd->profiles[0].cpu_governor));
	pd->profiles[NOVA_POWER_PERFORMANCE].gpu_power_level = 100;
	pd->profiles[NOVA_POWER_PERFORMANCE].display_brightness = -1;
	pd->profiles[NOVA_POWER_PERFORMANCE].usb_autosuspend = false;

	/* Balanced */
	pd->profiles[NOVA_POWER_BALANCED].profile = NOVA_POWER_BALANCED;
	strlcpy(pd->profiles[NOVA_POWER_BALANCED].cpu_governor, "ondemand",
	    sizeof(pd->profiles[1].cpu_governor));
	pd->profiles[NOVA_POWER_BALANCED].gpu_power_level = 50;
	pd->profiles[NOVA_POWER_BALANCED].display_brightness = -1;
	pd->profiles[NOVA_POWER_BALANCED].usb_autosuspend = true;
	pd->profiles[NOVA_POWER_BALANCED].usb_autosuspend_delay_ms = 2000;

	/* Power Saver */
	pd->profiles[NOVA_POWER_POWER_SAVER].profile = NOVA_POWER_POWER_SAVER;
	strlcpy(pd->profiles[NOVA_POWER_POWER_SAVER].cpu_governor, "powersave",
	    sizeof(pd->profiles[2].cpu_governor));
	pd->profiles[NOVA_POWER_POWER_SAVER].gpu_power_level = 0;
	pd->profiles[NOVA_POWER_POWER_SAVER].display_brightness = 50;
	pd->profiles[NOVA_POWER_POWER_SAVER].usb_autosuspend = true;
	pd->profiles[NOVA_POWER_POWER_SAVER].usb_autosuspend_delay_ms = 500;
}

/*
 * Create power daemon
 */
struct nova_powerd *
nova_powerd_create(void)
{
	struct nova_powerd *pd;
	struct sockaddr_un addr;

	pd = calloc(1, sizeof(*pd));
	if (pd == NULL) {
		syslog(LOG_ERR, "Failed to allocate power daemon");
		return NULL;
	}

	pd->sock_fd = -1;
	pd->kq = -1;
	pd->acpi_fd = -1;
	pd->next_cookie = 1;
	pd->brightness = 100;
	pd->current_profile = NOVA_POWER_BALANCED;

	/* Initialize profiles */
	init_default_profiles(pd);

	/* Open ACPI device */
	pd->acpi_fd = open(ACPI_DEVICE, O_RDONLY);
	if (pd->acpi_fd >= 0) {
		pd->power_source = acpi_get_power_source(pd->acpi_fd);

		/* Allocate battery info */
		pd->batteries = calloc(2, sizeof(nova_battery_info_t));
		if (pd->batteries != NULL) {
			for (int i = 0; i < 2; i++) {
				if (acpi_get_battery(pd->acpi_fd, i,
					&pd->batteries[i]) == 0)
					pd->battery_count++;
			}
		}

		syslog(LOG_INFO, "ACPI: %zu batteries, power source: %s",
		    pd->battery_count,
		    pd->power_source == NOVA_POWER_AC ? "AC" : "Battery");
	} else {
		syslog(LOG_WARNING, "Failed to open ACPI device");
		pd->power_source = NOVA_POWER_AC;
	}

	/* Get initial brightness */
	pd->brightness = get_brightness();

	/* Create kqueue */
	pd->kq = kqueue();
	if (pd->kq < 0) {
		syslog(LOG_ERR, "Failed to create kqueue");
		if (pd->acpi_fd >= 0)
			close(pd->acpi_fd);
		free(pd->batteries);
		free(pd);
		return NULL;
	}

	/* Create IPC socket */
	pd->sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (pd->sock_fd >= 0) {
		unlink(NOVA_POWERD_SOCKET);

		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strlcpy(addr.sun_path, NOVA_POWERD_SOCKET,
		    sizeof(addr.sun_path));

		bind(pd->sock_fd, (struct sockaddr *)&addr, sizeof(addr));
		listen(pd->sock_fd, 5);
	}

	/* Apply default profile based on power source */
	if (pd->power_source == NOVA_POWER_BATTERY)
		apply_profile(pd, NOVA_POWER_BALANCED);
	else
		apply_profile(pd, NOVA_POWER_PERFORMANCE);

	syslog(LOG_INFO, "Power daemon created");
	return pd;
}

/*
 * Destroy power daemon
 */
void
nova_powerd_destroy(struct nova_powerd *pd)
{
	if (pd == NULL)
		return;

	free(pd->batteries);

	if (pd->sock_fd >= 0) {
		close(pd->sock_fd);
		unlink(NOVA_POWERD_SOCKET);
	}

	if (pd->acpi_fd >= 0)
		close(pd->acpi_fd);

	if (pd->kq >= 0)
		close(pd->kq);

	free(pd);
}

/*
 * Client API: Connect to power daemon
 */
int
nova_power_connect(void)
{
	struct sockaddr_un addr;

	if (g_client != NULL)
		return 0;

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
	strlcpy(addr.sun_path, NOVA_POWERD_SOCKET, sizeof(addr.sun_path));

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
nova_power_disconnect(void)
{
	if (g_client != NULL) {
		if (g_client->sock_fd >= 0)
			close(g_client->sock_fd);
		free(g_client);
		g_client = NULL;
	}
}

int
nova_power_get_fd(void)
{
	return g_client ? g_client->sock_fd : -1;
}

int
nova_power_dispatch(void)
{
	return 0;
}

/*
 * Power source and battery
 */
nova_power_source_t
nova_power_get_source(void)
{
	int acpi_fd, state;

	acpi_fd = open(ACPI_DEVICE, O_RDONLY);
	if (acpi_fd < 0)
		return NOVA_POWER_AC;

	state = acpi_get_power_source(acpi_fd);
	close(acpi_fd);

	return state;
}

int
nova_power_get_battery(nova_battery_info_t **batteries, size_t *count)
{
	nova_battery_info_t *batts;
	int acpi_fd, i, n = 0;

	if (batteries == NULL || count == NULL)
		return -1;

	acpi_fd = open(ACPI_DEVICE, O_RDONLY);
	if (acpi_fd < 0) {
		*batteries = NULL;
		*count = 0;
		return 0;
	}

	batts = calloc(2, sizeof(*batts));
	if (batts == NULL) {
		close(acpi_fd);
		return -1;
	}

	for (i = 0; i < 2; i++) {
		if (acpi_get_battery(acpi_fd, i, &batts[n]) == 0 &&
		    batts[n].present)
			n++;
	}

	close(acpi_fd);

	*batteries = batts;
	*count = n;
	return 0;
}

void
nova_power_free_batteries(nova_battery_info_t *batteries)
{
	free(batteries);
}

int
nova_power_get_battery_percentage(void)
{
	nova_battery_info_t *batts;
	size_t count;
	int total = 0, n = 0;

	if (nova_power_get_battery(&batts, &count) < 0)
		return -1;

	for (size_t i = 0; i < count; i++) {
		if (batts[i].present) {
			total += batts[i].percentage;
			n++;
		}
	}

	nova_power_free_batteries(batts);

	return n > 0 ? total / n : -1;
}

int
nova_power_get_time_remaining(void)
{
	nova_battery_info_t *batts;
	size_t count;
	int time = -1;

	if (nova_power_get_battery(&batts, &count) < 0)
		return -1;

	for (size_t i = 0; i < count; i++) {
		if (batts[i].present && batts[i].discharging) {
			if (batts[i].time_to_empty > 0)
				time = batts[i].time_to_empty;
			break;
		}
	}

	nova_power_free_batteries(batts);
	return time;
}

/*
 * Power profiles
 */
nova_power_profile_t
nova_power_get_profile(void)
{
	/* TODO: Query daemon */
	return NOVA_POWER_BALANCED;
}

int
nova_power_set_profile(nova_power_profile_t profile)
{
	/* TODO: Send to daemon */
	return 0;
}

/*
 * Brightness
 */
int
nova_power_get_brightness(void)
{
	return get_brightness();
}

int
nova_power_set_brightness(int brightness)
{
	return set_brightness(brightness);
}

int
nova_power_adjust_brightness(int delta)
{
	int current = get_brightness();
	return set_brightness(current + delta);
}

/*
 * Sleep/Hibernate
 */
bool
nova_power_can_sleep(nova_sleep_action_t action)
{
	/* Check via sysctl */
	int suspend = 0;
	size_t len = sizeof(suspend);

	switch (action) {
	case NOVA_SLEEP_SUSPEND:
		sysctlbyname("hw.acpi.suspend_state", &suspend, &len, NULL, 0);
		return suspend > 0;
	case NOVA_SLEEP_HIBERNATE:
		return false; /* TODO: Check if swap is configured */
	default:
		return false;
	}
}

int
nova_power_sleep(nova_sleep_action_t action)
{
	int state;

	switch (action) {
	case NOVA_SLEEP_SUSPEND:
		state = 3; /* S3 */
		break;
	case NOVA_SLEEP_HIBERNATE:
		state = 4; /* S4 */
		break;
	default:
		return -1;
	}

	return sysctlbyname("hw.acpi.sleep_state", NULL, NULL, &state,
	    sizeof(state));
}

int
nova_power_lock_and_sleep(nova_sleep_action_t action)
{
	/* TODO: Lock screen first */
	return nova_power_sleep(action);
}

/*
 * Lid state
 */
nova_lid_state_t
nova_power_get_lid_state(void)
{
	int lid = 1; /* Default open */
	size_t len = sizeof(lid);

	sysctlbyname("hw.acpi.lid_switch_state", &lid, &len, NULL, 0);

	return lid ? NOVA_LID_OPEN : NOVA_LID_CLOSED;
}

/*
 * Callbacks
 */
void
nova_power_set_source_callback(nova_power_source_callback_t callback,
    void *data)
{
	if (g_client != NULL) {
		g_client->source_callback = callback;
		g_client->source_callback_data = data;
	}
}

void
nova_power_set_battery_callback(nova_battery_callback_t callback, void *data)
{
	if (g_client != NULL) {
		g_client->battery_callback = callback;
		g_client->battery_callback_data = data;
	}
}

void
nova_power_set_lid_callback(nova_lid_callback_t callback, void *data)
{
	if (g_client != NULL) {
		g_client->lid_callback = callback;
		g_client->lid_callback_data = data;
	}
}

void
nova_power_set_profile_callback(nova_profile_callback_t callback, void *data)
{
	if (g_client != NULL) {
		g_client->profile_callback = callback;
		g_client->profile_callback_data = data;
	}
}

/*
 * Idle inhibition
 */
int
nova_power_inhibit_idle(const char *reason, uint32_t *cookie)
{
	/* TODO: Implement via daemon */
	static uint32_t next = 1;
	*cookie = next++;
	return 0;
}

int
nova_power_uninhibit_idle(uint32_t cookie)
{
	/* TODO: Implement via daemon */
	return 0;
}

int
nova_power_simulate_activity(void)
{
	/* TODO: Reset idle timers */
	return 0;
}
