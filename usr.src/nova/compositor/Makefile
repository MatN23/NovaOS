# NovaCompositor - Makefile

.include <bsd.own.mk>

PROG=		novacompositor
SRCS=		main.c \
		compositor.c \
		backend/drm.c \
		backend/input.c \
		protocol/wl_compositor.c \
		protocol/wl_surface.c \
		protocol/wl_seat.c \
		protocol/xdg_shell.c \
		render/renderer.c \
		render/shaders.c \
		effects/blur.c \
		effects/shadow.c \
		effects/animation.c

BINDIR=		/usr/libexec/nova

# Wayland protocol headers (generated)
WAYLAND_PROTOCOLS=	${.CURDIR}/protocol/wayland.xml \
			${.CURDIR}/protocol/xdg-shell.xml

# Libraries
LDADD=		-lwayland-server \
		-lvulkan \
		-ldrm \
		-lgbm \
		-lxkbcommon \
		-linput \
		-ludev \
		-lm

CFLAGS+=	-I${.CURDIR} \
		-I${.CURDIR}/../lib/libnova-config \
		-I/usr/local/include \
		-I/usr/local/include/libdrm \
		-DNOVA_VERSION=\"0.1.0\"

LDFLAGS+=	-L/usr/local/lib

# Vulkan shader compilation
SHADERS=	shaders/blit.vert \
		shaders/blit.frag \
		shaders/blur.comp \
		shaders/shadow.frag \
		shaders/rounded.frag

.for shader in ${SHADERS}
CLEANFILES+=	${shader:.glsl=.spv}
.endfor

MAN=		novacompositor.8

.include <bsd.prog.mk>
