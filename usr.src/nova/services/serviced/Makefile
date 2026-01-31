# nova-serviced - Service Manager Makefile

.include <bsd.own.mk>

PROG=		nova-serviced
SRCS=		main.c \
		serviced.c \
		service.c \
		dependency.c \
		health.c \
		logging.c

BINDIR=		/usr/libexec/nova

LDADD=		-lnova-config \
		-lutil

CFLAGS+=	-I${.CURDIR} \
		-I${.CURDIR}/../../lib/libnova-config \
		-DNOVA_VERSION=\"0.1.0\"

LDFLAGS+=	-L${.CURDIR}/../../lib/libnova-config

MAN=		nova-serviced.8 \
		nova-ctl.1

# CLI tool
SCRIPTS=	nova-ctl
SCRIPTSDIR=	/usr/bin

.include <bsd.prog.mk>
