/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * nova-configd - Configuration Service
 */

#ifndef _NOVA_CONFIG_H_
#define _NOVA_CONFIG_H_

#include <sys/types.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Nova Configuration System
 *
 * Unified configuration management:
 * - Human-readable TOML configuration files
 * - Layered: system defaults → user overrides
 * - Live configuration updates with notifications
 * - CLI and programmatic access
 */

/*
 * Configuration value types
 */

typedef enum nova_config_type {
	NOVA_CONFIG_STRING,
	NOVA_CONFIG_INT,
	NOVA_CONFIG_FLOAT,
	NOVA_CONFIG_BOOL,
	NOVA_CONFIG_ARRAY,
	NOVA_CONFIG_TABLE,
} nova_config_type_t;

typedef struct nova_config_value {
	nova_config_type_t type;
	union {
		char *string;
		int64_t integer;
		double floating;
		bool boolean;
		struct {
			struct nova_config_value *items;
			size_t count;
		} array;
		struct {
			char **keys;
			struct nova_config_value *values;
			size_t count;
		} table;
	} v;
} nova_config_value_t;

/*
 * Connection to configuration daemon
 */

int nova_config_connect(void);
void nova_config_disconnect(void);
int nova_config_dispatch(void);
int nova_config_get_fd(void);

/*
 * Configuration access
 */

/* Get a configuration value by key path (e.g., "appearance.theme") */
int nova_config_get(const char *key, nova_config_value_t *value);

/* Set a configuration value */
int nova_config_set(const char *key, const nova_config_value_t *value);

/* Delete a configuration key (reverts to default) */
int nova_config_unset(const char *key);

/* Check if a key exists */
bool nova_config_exists(const char *key);

/* Free a configuration value */
void nova_config_value_free(nova_config_value_t *value);

/*
 * Typed getters (convenience functions)
 */

int nova_config_get_string(const char *key, char *buf, size_t len);
int nova_config_get_int(const char *key, int64_t *value);
int nova_config_get_float(const char *key, double *value);
int nova_config_get_bool(const char *key, bool *value);

/*
 * Typed setters (convenience functions)
 */

int nova_config_set_string(const char *key, const char *value);
int nova_config_set_int(const char *key, int64_t value);
int nova_config_set_float(const char *key, double value);
int nova_config_set_bool(const char *key, bool value);

/*
 * Configuration domains
 */

typedef enum nova_config_domain {
	NOVA_CONFIG_DOMAIN_SYSTEM,  /* /etc/nova/ */
	NOVA_CONFIG_DOMAIN_USER,    /* ~/.config/nova/ */
	NOVA_CONFIG_DOMAIN_RUNTIME, /* Transient, memory only */
} nova_config_domain_t;

/* Get value from specific domain (ignoring layering) */
int nova_config_get_from(nova_config_domain_t domain, const char *key,
    nova_config_value_t *value);

/* Set value in specific domain */
int nova_config_set_in(nova_config_domain_t domain, const char *key,
    const nova_config_value_t *value);

/*
 * Configuration schema
 */

typedef struct nova_config_schema_entry {
	char key[128];
	nova_config_type_t type;
	char description[256];
	nova_config_value_t default_value;
	bool user_modifiable;
} nova_config_schema_entry_t;

/* Get schema for a configuration domain */
int nova_config_get_schema(const char *prefix,
    nova_config_schema_entry_t **entries, size_t *count);

void nova_config_free_schema(nova_config_schema_entry_t *entries, size_t count);

/*
 * Configuration watch/notify
 */

typedef void (*nova_config_callback_t)(void *data, const char *key,
    const nova_config_value_t *old_value, const nova_config_value_t *new_value);

/* Watch for changes to keys matching a prefix */
int nova_config_watch(const char *key_prefix, nova_config_callback_t callback,
    void *data, uint32_t *watch_id);

/* Stop watching */
int nova_config_unwatch(uint32_t watch_id);

/*
 * Configuration file paths
 */

/* Get path to system configuration directory */
const char *nova_config_system_dir(void);

/* Get path to user configuration directory */
const char *nova_config_user_dir(void);

/* Reload configuration from disk */
int nova_config_reload(void);

/* Write pending changes to disk */
int nova_config_sync(void);

#ifdef __cplusplus
}
#endif

#endif /* _NOVA_CONFIG_H_ */
