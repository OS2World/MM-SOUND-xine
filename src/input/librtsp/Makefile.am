include $(top_srcdir)/misc/Makefile.common

AM_CPPFLAGS = -D_LARGEFILE64_SOURCE -I$(srcdir)/../libreal

noinst_LTLIBRARIES = librtsp.la

librtsp_la_SOURCES = \
  rtsp.c \
  rtsp_session.c

librtsp_la_LDFLAGS = $(THREAD_LIBS) -avoid-version -module

noinst_HEADERS = \
  rtsp.h \
  rtsp_session.h
