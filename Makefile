WAYLAND_PROTOCOLS=$(shell pkg-config --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER=$(shell pkg-config --variable=wayland_scanner wayland-scanner)
SCDOC=$(shell pkg-config --variable=scdoc scdoc)
LIBS=\
	 $(shell pkg-config --cflags --libs wayland-client) \
	 $(shell pkg-config --cflags --libs xkbcommon)

xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.c: xdg-shell-protocol.h
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

wev: wev.c shm.c xdg-shell-protocol.h xdg-shell-protocol.c
	$(CC) $(CFLAGS) \
		-g -std=c11 -I. \
		-o wev wev.c shm.c xdg-shell-protocol.c \
		$(LIBS) -lrt

wev.1: wev.1.scd
	$(SCDOC) < wev.1.scd > wev.1

all: wev wev.1

PREFIX?=/usr/local
BINDIR?=$(PREFIX)/bin
SHAREDIR?=$(PREFIX)/share
MANDIR?=$(SHAREDIR)/man
DESTDIR?=

install: wev
	mkdir -p $(DESTDIR)$(BINDIR)
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	install -m755 wev $(DESTDIR)$(BINDIR)/wev
	install -m644 wev.1 $(DESTDIR)$(MANDIR)/man1/wev.1

clean:
	rm -f wev wev.1 xdg-shell-protocol.h xdg-shell-protocol.c

.DEFAULT_GOAL=all
.PHONY: all install clean
