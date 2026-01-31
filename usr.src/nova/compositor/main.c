/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * NovaCompositor - Main Entry Point
 */

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "backend/nova_drm.h"
#include "backend/nova_input.h"
#include "nova_compositor.h"
#include "render/nova_renderer.h"

#define NOVA_VERSION "0.1.0"

static volatile sig_atomic_t g_running = 1;
static struct nova_compositor *g_compositor = NULL;

static void
signal_handler(int sig)
{
	switch (sig) {
	case SIGINT:
	case SIGTERM:
		g_running = 0;
		if (g_compositor != NULL)
			nova_compositor_terminate(g_compositor);
		break;
	case SIGHUP:
		/* Reload configuration */
		syslog(LOG_INFO, "Received SIGHUP, reloading configuration");
		break;
	}
}

static void
usage(void)
{
	fprintf(stderr, "usage: novacompositor [options]\n");
	fprintf(stderr, "options:\n");
	fprintf(stderr, "  -c, --config <file>    Configuration file\n");
	fprintf(stderr,
	    "  -d, --drm-device <dev> DRM device (default: /dev/dri/card0)\n");
	fprintf(stderr, "  -s, --socket <name>    Wayland socket name\n");
	fprintf(stderr, "  -X, --xwayland         Enable XWayland\n");
	fprintf(stderr, "  -D, --debug            Enable debug logging\n");
	fprintf(stderr, "  -v, --version          Show version\n");
	fprintf(stderr, "  -h, --help             Show this help\n");
	exit(1);
}

static void
version(void)
{
	printf("NovaCompositor %s\n", NOVA_VERSION);
	printf("Wayland compositor for Nova Desktop on FreeBSD\n");
	exit(0);
}

int
main(int argc, char *argv[])
{
	nova_compositor_config_t config = {
		.drm_device = "/dev/dri/card0",
		.socket_name = "nova-0",
		.enable_xwayland = false,
		.debug_mode = false,
	};
	const char *config_file = NULL;
	int ch, ret;

	static struct option longopts[] = { { "config", required_argument, NULL,
						'c' },
		{ "drm-device", required_argument, NULL, 'd' },
		{ "socket", required_argument, NULL, 's' },
		{ "xwayland", no_argument, NULL, 'X' },
		{ "debug", no_argument, NULL, 'D' },
		{ "version", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' }, { NULL, 0, NULL, 0 } };

	while ((ch = getopt_long(argc, argv, "c:d:s:XDvh", longopts, NULL)) !=
	    -1) {
		switch (ch) {
		case 'c':
			config_file = optarg;
			break;
		case 'd':
			config.drm_device = optarg;
			break;
		case 's':
			config.socket_name = optarg;
			break;
		case 'X':
			config.enable_xwayland = true;
			break;
		case 'D':
			config.debug_mode = true;
			break;
		case 'v':
			version();
			break;
		case 'h':
		default:
			usage();
		}
	}

	/* Initialize syslog */
	openlog("novacompositor", LOG_PID | LOG_NDELAY, LOG_DAEMON);
	if (config.debug_mode)
		setlogmask(LOG_UPTO(LOG_DEBUG));
	else
		setlogmask(LOG_UPTO(LOG_INFO));

	syslog(LOG_INFO, "NovaCompositor %s starting", NOVA_VERSION);

	/* Set up signal handlers */
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;

	if (sigaction(SIGINT, &sa, NULL) < 0)
		err(1, "sigaction(SIGINT)");
	if (sigaction(SIGTERM, &sa, NULL) < 0)
		err(1, "sigaction(SIGTERM)");
	if (sigaction(SIGHUP, &sa, NULL) < 0)
		err(1, "sigaction(SIGHUP)");

	/* Ignore SIGPIPE */
	signal(SIGPIPE, SIG_IGN);

	/* Verify DRM device exists */
	if (access(config.drm_device, R_OK | W_OK) != 0) {
		syslog(LOG_ERR, "Cannot access DRM device %s: %s",
		    config.drm_device, strerror(errno));
		errx(1, "Cannot access DRM device %s", config.drm_device);
	}

	/* Create compositor */
	g_compositor = nova_compositor_create(&config);
	if (g_compositor == NULL) {
		syslog(LOG_ERR, "Failed to create compositor");
		errx(1, "Failed to create compositor");
	}

	syslog(LOG_INFO, "Compositor initialized, entering main loop");

	/* Notify service manager we're ready (sd_notify compatible) */
	const char *notify_socket = getenv("NOTIFY_SOCKET");
	if (notify_socket != NULL) {
		/* Send ready notification */
		/* TODO: Implement sd_notify protocol */
	}

	/* Run compositor main loop */
	ret = nova_compositor_run(g_compositor);

	syslog(LOG_INFO, "Compositor shutting down");

	/* Cleanup */
	nova_compositor_destroy(g_compositor);
	g_compositor = NULL;

	closelog();

	return ret;
}
