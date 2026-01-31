/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * nova-powerd - Power Management Daemon
 */

#ifndef _NOVA_POWER_H_
#define _NOVA_POWER_H_

#include <sys/types.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Nova Power Management
 *
 * Desktop-friendly power and hardware management:
 * - Battery monitoring and status
 * - Power profiles (Performance / Balanced / Power Saver)
 * - Display brightness control
 * - Lid and power button handling
 * - Suspend/hibernate coordination
 */

/*
 * Power source
 */

typedef enum nova_power_source {
	NOVA_POWER_AC,
	NOVA_POWER_BATTERY,
	NOVA_POWER_UPS,
} nova_power_source_t;

/*
 * Battery information
 */

typedef struct nova_battery_info {
	char id[32]; /* Battery ID */
	bool present;
	int percentage; /* 0-100 */
	bool charging;
	bool discharging;
	int time_to_full;  /* Seconds, -1 if unknown */
	int time_to_empty; /* Seconds, -1 if unknown */
	float voltage;	   /* Volts */
	float energy;	   /* Wh */
	float energy_full; /* Wh */
	float power_rate;  /* Watts (positive = charging) */
	int cycle_count;
	char vendor[64];
	char model[64];
} nova_battery_info_t;

/*
 * Power profiles
 */

typedef enum nova_power_profile {
	NOVA_POWER_PERFORMANCE,
	NOVA_POWER_BALANCED,
	NOVA_POWER_POWER_SAVER,
} nova_power_profile_t;

typedef struct nova_power_profile_config {
	nova_power_profile_t profile;
	char cpu_governor[32];	/* e.g., "performance" */
	int gpu_power_level;	/* 0-100 */
	int display_brightness; /* 0-100, -1 for no change */
	int disk_apm;		/* 0-254 */
	bool usb_autosuspend;
	int usb_autosuspend_delay_ms;
} nova_power_profile_config_t;

/*
 * Connection to power daemon
 */

int nova_power_connect(void);
void nova_power_disconnect(void);
int nova_power_dispatch(void);
int nova_power_get_fd(void);

/*
 * Power source and battery
 */

/* Get current power source */
nova_power_source_t nova_power_get_source(void);

/* Get battery information */
int nova_power_get_battery(nova_battery_info_t **batteries, size_t *count);
void nova_power_free_batteries(nova_battery_info_t *batteries);

/* Get overall battery percentage (combined for multi-battery systems) */
int nova_power_get_battery_percentage(void);

/* Get estimated time remaining (seconds, -1 if unknown) */
int nova_power_get_time_remaining(void);

/*
 * Power profiles
 */

/* Get current power profile */
nova_power_profile_t nova_power_get_profile(void);

/* Set power profile */
int nova_power_set_profile(nova_power_profile_t profile);

/* Get profile configuration */
int nova_power_get_profile_config(nova_power_profile_t profile,
    nova_power_profile_config_t *config);

/* Set profile configuration */
int nova_power_set_profile_config(const nova_power_profile_config_t *config);

/*
 * Display brightness
 */

/* Get display brightness (0-100) */
int nova_power_get_brightness(void);

/* Set display brightness (0-100) */
int nova_power_set_brightness(int brightness);

/* Adjust brightness relatively */
int nova_power_adjust_brightness(int delta);

/*
 * Sleep/Hibernate
 */

typedef enum nova_sleep_action {
	NOVA_SLEEP_SUSPEND,   /* Suspend to RAM */
	NOVA_SLEEP_HIBERNATE, /* Suspend to disk */
	NOVA_SLEEP_HYBRID,    /* Suspend + hibernate */
} nova_sleep_action_t;

/* Check if sleep action is available */
bool nova_power_can_sleep(nova_sleep_action_t action);

/* Initiate sleep */
int nova_power_sleep(nova_sleep_action_t action);

/* Lock screen before sleep */
int nova_power_lock_and_sleep(nova_sleep_action_t action);

/*
 * Lid and power button
 */

typedef enum nova_lid_state {
	NOVA_LID_OPEN,
	NOVA_LID_CLOSED,
} nova_lid_state_t;

nova_lid_state_t nova_power_get_lid_state(void);

typedef enum nova_lid_action {
	NOVA_LID_ACTION_NOTHING,
	NOVA_LID_ACTION_SUSPEND,
	NOVA_LID_ACTION_HIBERNATE,
	NOVA_LID_ACTION_LOCK,
} nova_lid_action_t;

/* Configure lid close behavior */
int nova_power_set_lid_action(nova_lid_action_t on_ac,
    nova_lid_action_t on_battery);

typedef enum nova_power_button_action {
	NOVA_POWER_BTN_NOTHING,
	NOVA_POWER_BTN_SUSPEND,
	NOVA_POWER_BTN_HIBERNATE,
	NOVA_POWER_BTN_SHUTDOWN,
	NOVA_POWER_BTN_SHOW_DIALOG,
} nova_power_button_action_t;

/* Configure power button behavior */
int nova_power_set_power_button_action(nova_power_button_action_t action);

/*
 * Idle detection
 */

typedef struct nova_idle_config {
	int dim_delay_sec; /* Dim screen after */
	int dim_percent;   /* Dim to this brightness */
	int screen_off_delay_sec;
	int lock_delay_sec;
	int sleep_delay_sec; /* -1 to disable */
} nova_idle_config_t;

int nova_power_get_idle_config(nova_idle_config_t *config);
int nova_power_set_idle_config(const nova_idle_config_t *config);

/* Simulate user activity (reset idle timer) */
int nova_power_simulate_activity(void);

/* Inhibit idle actions temporarily */
int nova_power_inhibit_idle(const char *reason, uint32_t *cookie);
int nova_power_uninhibit_idle(uint32_t cookie);

/*
 * Callbacks
 */

typedef void (
    *nova_power_source_callback_t)(void *data, nova_power_source_t source);

typedef void (
    *nova_battery_callback_t)(void *data, const nova_battery_info_t *battery);

typedef void (*nova_lid_callback_t)(void *data, nova_lid_state_t state);

typedef void (
    *nova_profile_callback_t)(void *data, nova_power_profile_t profile);

void nova_power_set_source_callback(nova_power_source_callback_t callback,
    void *data);

void nova_power_set_battery_callback(nova_battery_callback_t callback,
    void *data);

void nova_power_set_lid_callback(nova_lid_callback_t callback, void *data);

void nova_power_set_profile_callback(nova_profile_callback_t callback,
    void *data);

#ifdef __cplusplus
}
#endif

#endif /* _NOVA_POWER_H_ */
