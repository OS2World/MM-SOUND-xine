include $(top_srcdir)/misc/Makefile.common

LIBTOOL = $(SHELL) $(top_builddir)/libtool
noinst_LTLIBRARIES = libxineutils.la

EXTRA_DIST = ppcasm_string.S ppc_asm.tmpl

if PPC_ARCH
pppc_files = ppcasm_string.S 
endif

AM_CFLAGS = $(THREAD_CFLAGS) $(X_CFLAGS)

libxineutils_la_SOURCES = $(pppc_files) \
	cpu_accel.c \
	color.c \
	list.c \
	memcpy.c \
	monitor.c \
	utils.c \
	xine_check.c \
	xine_mutex.c \
	xmllexer.c \
	xmlparser.c \
	xine_buffer.c 

libxineutils_la_LIBADD = $(THREAD_LIBS)

include_HEADERS = attributes.h \
	compat.h \
	xineutils.h \
	xine_check.h \
	xmllexer.h \
	xmlparser.h \
	xine_buffer.h

noinst_HEADERS = ppcasm_string.h
