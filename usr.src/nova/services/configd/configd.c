/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * nova-configd - Configuration Daemon Implementation
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "nova_config.h"

#define NOVA_CONFIGD_SOCKET "/var/run/nova/configd.sock"
#define SYSTEM_CONFIG_DIR   "/etc/nova"
#define USER_CONFIG_DIR	    ".config/nova"
#define MAX_CONFIG_ENTRIES  256
#define MAX_WATCHES	    64

/*
 * Configuration entry
 */
struct config_entry {
	char key[128];
	nova_config_value_t value;
	nova_config_domain_t domain;
	bool user_modifiable;
};

/*
 * Watch entry
 */
struct config_watch {
	uint32_t id;
	char prefix[128];
	nova_config_callback_t callback;
	void *data;
	bool active;
};

/*
 * Configuration daemon state (client-side)
 */
struct nova_configd_client {
	int sock_fd;
	struct config_watch watches[MAX_WATCHES];
	size_t watch_count;
	uint32_t next_watch_id;
};

static struct nova_configd_client *g_client = NULL;

/*
 * Configuration daemon state (server-side)
 */
struct nova_configd {
	int sock_fd;
	int kq;
	volatile bool running;

	/* Configuration entries */
	struct config_entry *entries;
	size_t entry_count;
	size_t entry_capacity;

	/* File paths */
	char system_dir[256];
	char user_dir[256];
};

/*
 * Simple TOML value parser (key = value format)
 */
static int
parse_toml_line(const char *line, char *key, size_t key_len,
    nova_config_value_t *value)
{
	const char *eq, *val_start;
	char val_buf[256];
	size_t klen;

	/* Skip comments and empty lines */
	while (*line == ' ' || *line == '\t')
		line++;
	if (*line == '#' || *line == '\0' || *line == '[')
		return -1;

	/* Find = */
	eq = strchr(line, '=');
	if (eq == NULL)
		return -1;

	/* Extract key */
	klen = eq - line;
	while (klen > 0 && (line[klen - 1] == ' ' || line[klen - 1] == '\t'))
		klen--;
	if (klen >= key_len)
		klen = key_len - 1;
	memcpy(key, line, klen);
	key[klen] = '\0';

	/* Extract value */
	val_start = eq + 1;
	while (*val_start == ' ' || *val_start == '\t')
		val_start++;

	strlcpy(val_buf, val_start, sizeof(val_buf));

	/* Trim trailing whitespace/newline */
	size_t vlen = strlen(val_buf);
	while (vlen > 0 &&
	    (val_buf[vlen - 1] == '\n' || val_buf[vlen - 1] == '\r' ||
		val_buf[vlen - 1] == ' ' || val_buf[vlen - 1] == '\t'))
		val_buf[--vlen] = '\0';

	/* Determine type */
	if (val_buf[0] == '"') {
		/* String */
		value->type = NOVA_CONFIG_STRING;
		char *end = strchr(val_buf + 1, '"');
		if (end)
			*end = '\0';
		value->v.string = strdup(val_buf + 1);
	} else if (strcmp(val_buf, "true") == 0) {
		value->type = NOVA_CONFIG_BOOL;
		value->v.boolean = true;
	} else if (strcmp(val_buf, "false") == 0) {
		value->type = NOVA_CONFIG_BOOL;
		value->v.boolean = false;
	} else if (strchr(val_buf, '.') != NULL) {
		/* Float */
		value->type = NOVA_CONFIG_FLOAT;
		value->v.floating = strtod(val_buf, NULL);
	} else {
		/* Integer */
		value->type = NOVA_CONFIG_INT;
		value->v.integer = strtoll(val_buf, NULL, 0);
	}

	return 0;
}

/*
 * Load a TOML configuration file
 */
static int
load_config_file(struct nova_configd *cd, const char *path, const char *prefix,
    nova_config_domain_t domain)
{
	FILE *fp;
	char line[512];
	char section[64] = "";
	char key[128], full_key[256];
	nova_config_value_t value;

	fp = fopen(path, "r");
	if (fp == NULL)
		return -1;

	while (fgets(line, sizeof(line), fp) != NULL) {
		/* Check for section header */
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;

		if (*p == '[') {
			char *end = strchr(p, ']');
			if (end) {
				*end = '\0';
				strlcpy(section, p + 1, sizeof(section));
			}
			continue;
		}

		if (parse_toml_line(line, key, sizeof(key), &value) == 0) {
			/* Build full key */
			if (prefix[0] && section[0])
				snprintf(full_key, sizeof(full_key), "%s.%s.%s",
				    prefix, section, key);
			else if (prefix[0])
				snprintf(full_key, sizeof(full_key), "%s.%s",
				    prefix, key);
			else if (section[0])
				snprintf(full_key, sizeof(full_key), "%s.%s",
				    section, key);
			else
				strlcpy(full_key, key, sizeof(full_key));

			/* Store entry */
			if (cd->entry_count < cd->entry_capacity) {
				struct config_entry *ent =
				    &cd->entries[cd->entry_count++];
				strlcpy(ent->key, full_key, sizeof(ent->key));
				ent->value = value;
				ent->domain = domain;
				ent->user_modifiable = true;
			}
		}
	}

	fclose(fp);
	return 0;
}

/*
 * Load all configuration files
 */
static int
load_all_configs(struct nova_configd *cd)
{
	DIR *dir;
	struct dirent *ent;
	char path[512];

	/* Load system configs */
	dir = opendir(cd->system_dir);
	if (dir != NULL) {
		while ((ent = readdir(dir)) != NULL) {
			size_t len = strlen(ent->d_name);
			if (len > 5 &&
			    strcmp(ent->d_name + len - 5, ".toml") == 0) {
				snprintf(path, sizeof(path), "%s/%s",
				    cd->system_dir, ent->d_name);

				/* Use filename without .toml as prefix */
				char prefix[64];
				strlcpy(prefix, ent->d_name, sizeof(prefix));
				prefix[len - 5] = '\0';

				load_config_file(cd, path, prefix,
				    NOVA_CONFIG_DOMAIN_SYSTEM);
				syslog(LOG_DEBUG, "Loaded config: %s", path);
			}
		}
		closedir(dir);
	}

	/* Load user configs (override system) */
	dir = opendir(cd->user_dir);
	if (dir != NULL) {
		while ((ent = readdir(dir)) != NULL) {
			size_t len = strlen(ent->d_name);
			if (len > 5 &&
			    strcmp(ent->d_name + len - 5, ".toml") == 0) {
				snprintf(path, sizeof(path), "%s/%s",
				    cd->user_dir, ent->d_name);

				char prefix[64];
				strlcpy(prefix, ent->d_name, sizeof(prefix));
				prefix[len - 5] = '\0';

				load_config_file(cd, path, prefix,
				    NOVA_CONFIG_DOMAIN_USER);
				syslog(LOG_DEBUG, "Loaded user config: %s",
				    path);
			}
		}
		closedir(dir);
	}

	syslog(LOG_INFO, "Loaded %zu configuration entries", cd->entry_count);
	return 0;
}

/*
 * Find entry by key
 */
static struct config_entry *
find_entry(struct nova_configd *cd, const char *key)
{
	/* Search in reverse to get user overrides first */
	for (size_t i = cd->entry_count; i > 0; i--) {
		if (strcmp(cd->entries[i - 1].key, key) == 0)
			return &cd->entries[i - 1];
	}
	return NULL;
}

/*
 * Create configuration daemon
 */
struct nova_configd *
nova_configd_create(const char *user_home)
{
	struct nova_configd *cd;
	struct sockaddr_un addr;

	cd = calloc(1, sizeof(*cd));
	if (cd == NULL) {
		syslog(LOG_ERR, "Failed to allocate config daemon");
		return NULL;
	}

	cd->sock_fd = -1;
	cd->kq = -1;

	/* Set directories */
	strlcpy(cd->system_dir, SYSTEM_CONFIG_DIR, sizeof(cd->system_dir));
	if (user_home != NULL)
		snprintf(cd->user_dir, sizeof(cd->user_dir), "%s/%s", user_home,
		    USER_CONFIG_DIR);

	/* Allocate entry storage */
	cd->entry_capacity = MAX_CONFIG_ENTRIES;
	cd->entries = calloc(cd->entry_capacity, sizeof(struct config_entry));
	if (cd->entries == NULL) {
		free(cd);
		return NULL;
	}

	/* Load configurations */
	load_all_configs(cd);

	/* Create kqueue */
	cd->kq = kqueue();
	if (cd->kq < 0) {
		syslog(LOG_ERR, "Failed to create kqueue");
		free(cd->entries);
		free(cd);
		return NULL;
	}

	/* Create IPC socket */
	cd->sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (cd->sock_fd >= 0) {
		unlink(NOVA_CONFIGD_SOCKET);

		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strlcpy(addr.sun_path, NOVA_CONFIGD_SOCKET,
		    sizeof(addr.sun_path));

		bind(cd->sock_fd, (struct sockaddr *)&addr, sizeof(addr));
		listen(cd->sock_fd, 5);
	}

	syslog(LOG_INFO, "Configuration daemon created");
	return cd;
}

/*
 * Destroy configuration daemon
 */
void
nova_configd_destroy(struct nova_configd *cd)
{
	if (cd == NULL)
		return;

	/* Free string values */
	for (size_t i = 0; i < cd->entry_count; i++) {
		if (cd->entries[i].value.type == NOVA_CONFIG_STRING)
			free(cd->entries[i].value.v.string);
	}
	free(cd->entries);

	if (cd->sock_fd >= 0) {
		close(cd->sock_fd);
		unlink(NOVA_CONFIGD_SOCKET);
	}

	if (cd->kq >= 0)
		close(cd->kq);

	free(cd);
}

/*
 * Client API: Connect to configuration daemon
 */
int
nova_config_connect(void)
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

	g_client->next_watch_id = 1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strlcpy(addr.sun_path, NOVA_CONFIGD_SOCKET, sizeof(addr.sun_path));

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
nova_config_disconnect(void)
{
	if (g_client != NULL) {
		if (g_client->sock_fd >= 0)
			close(g_client->sock_fd);
		free(g_client);
		g_client = NULL;
	}
}

int
nova_config_get_fd(void)
{
	return g_client ? g_client->sock_fd : -1;
}

int
nova_config_dispatch(void)
{
	return 0;
}

/*
 * Configuration access
 */
int
nova_config_get(const char *key, nova_config_value_t *value)
{
	/* TODO: Query daemon */
	(void)key;
	(void)value;
	return -1;
}

int
nova_config_set(const char *key, const nova_config_value_t *value)
{
	/* TODO: Send to daemon */
	(void)key;
	(void)value;
	return 0;
}

int
nova_config_unset(const char *key)
{
	/* TODO: Send to daemon */
	(void)key;
	return 0;
}

bool
nova_config_exists(const char *key)
{
	/* TODO: Query daemon */
	(void)key;
	return false;
}

void
nova_config_value_free(nova_config_value_t *value)
{
	if (value == NULL)
		return;

	if (value->type == NOVA_CONFIG_STRING)
		free(value->v.string);

	memset(value, 0, sizeof(*value));
}

/*
 * Typed getters
 */
int
nova_config_get_string(const char *key, char *buf, size_t len)
{
	nova_config_value_t value;

	if (nova_config_get(key, &value) < 0)
		return -1;

	if (value.type != NOVA_CONFIG_STRING) {
		nova_config_value_free(&value);
		return -1;
	}

	strlcpy(buf, value.v.string, len);
	nova_config_value_free(&value);
	return 0;
}

int
nova_config_get_int(const char *key, int64_t *value)
{
	nova_config_value_t v;

	if (nova_config_get(key, &v) < 0)
		return -1;

	if (v.type != NOVA_CONFIG_INT) {
		nova_config_value_free(&v);
		return -1;
	}

	*value = v.v.integer;
	return 0;
}

int
nova_config_get_float(const char *key, double *value)
{
	nova_config_value_t v;

	if (nova_config_get(key, &v) < 0)
		return -1;

	if (v.type != NOVA_CONFIG_FLOAT) {
		nova_config_value_free(&v);
		return -1;
	}

	*value = v.v.floating;
	return 0;
}

int
nova_config_get_bool(const char *key, bool *value)
{
	nova_config_value_t v;

	if (nova_config_get(key, &v) < 0)
		return -1;

	if (v.type != NOVA_CONFIG_BOOL) {
		nova_config_value_free(&v);
		return -1;
	}

	*value = v.v.boolean;
	return 0;
}

/*
 * Typed setters
 */
int
nova_config_set_string(const char *key, const char *value)
{
	nova_config_value_t v = {
		.type = NOVA_CONFIG_STRING,
		.v.string = (char *)value /* const cast OK, we don't free */
	};
	return nova_config_set(key, &v);
}

int
nova_config_set_int(const char *key, int64_t value)
{
	nova_config_value_t v = { .type = NOVA_CONFIG_INT, .v.integer = value };
	return nova_config_set(key, &v);
}

int
nova_config_set_float(const char *key, double value)
{
	nova_config_value_t v = { .type = NOVA_CONFIG_FLOAT,
		.v.floating = value };
	return nova_config_set(key, &v);
}

int
nova_config_set_bool(const char *key, bool value)
{
	nova_config_value_t v = { .type = NOVA_CONFIG_BOOL,
		.v.boolean = value };
	return nova_config_set(key, &v);
}

/*
 * Configuration paths
 */
const char *
nova_config_system_dir(void)
{
	return SYSTEM_CONFIG_DIR;
}

const char *
nova_config_user_dir(void)
{
	static char path[256];
	const char *home = getenv("HOME");

	if (home != NULL)
		snprintf(path, sizeof(path), "%s/%s", home, USER_CONFIG_DIR);
	else
		strlcpy(path, USER_CONFIG_DIR, sizeof(path));

	return path;
}

/*
 * Watch/notify
 */
int
nova_config_watch(const char *key_prefix, nova_config_callback_t callback,
    void *data, uint32_t *watch_id)
{
	if (g_client == NULL || g_client->watch_count >= MAX_WATCHES)
		return -1;

	struct config_watch *w = &g_client->watches[g_client->watch_count++];
	w->id = g_client->next_watch_id++;
	strlcpy(w->prefix, key_prefix, sizeof(w->prefix));
	w->callback = callback;
	w->data = data;
	w->active = true;

	*watch_id = w->id;
	return 0;
}

int
nova_config_unwatch(uint32_t watch_id)
{
	if (g_client == NULL)
		return -1;

	for (size_t i = 0; i < g_client->watch_count; i++) {
		if (g_client->watches[i].id == watch_id) {
			g_client->watches[i].active = false;
			return 0;
		}
	}

	return -1;
}

int
nova_config_reload(void)
{
	/* TODO: Tell daemon to reload */
	return 0;
}

int
nova_config_sync(void)
{
	/* TODO: Tell daemon to sync to disk */
	return 0;
}
