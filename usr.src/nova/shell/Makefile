# NovaShell - Makefile

.include <bsd.own.mk>

PROG=		novashell
SRCS=		main.c \
		shell.c \
		components/panel.c \
		components/launcher.c \
		components/notifications.c \
		components/workspaces.c \
		components/switcher.c \
		components/settings.c \
		widgets/clock.c \
		widgets/battery.c \
		widgets/network.c \
		widgets/audio.c \
		widgets/systray.c

BINDIR=		/usr/libexec/nova

LDADD=		-lwayland-client \
		-lnova-ui \
		-lnova-config \
		-lxkbcommon \
		-lm

CFLAGS+=	-I${.CURDIR} \
		-I${.CURDIR}/../lib/libnova-ui \
		-I${.CURDIR}/../lib/libnova-config \
		-I/usr/local/include \
		-DNOVA_VERSION=\"0.1.0\"

LDFLAGS+=	-L/usr/local/lib \
		-L${.CURDIR}/../lib/libnova-ui \
		-L${.CURDIR}/../lib/libnova-config

MAN=		novashell.1

.include <bsd.prog.mk>
