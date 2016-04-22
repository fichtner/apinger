
EXTRA_DIST = cfgparser1.h apinger.conf
sbin_PROGRAMS = apinger

apinger_SOURCES = \
		apinger.c \
		apinger.h \
		cfgparser1.y \
		cfgparser2.l \
		conf.c \
		conf.h \
		debug.h \
		icmp.c \
		icmp6.c \
		main.c \
		debug.c \
		rrd.c \
		rrd.h \
		tv_macros.h

AM_CFLAGS=-D"SYSCONFDIR=\"$(sysconfdir)\""

AM_YFLAGS=-d
