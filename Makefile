
# Makefile for H1A EEPROM Tool
# (c) 2022--2023 Adnacom, Inc.
# Based on
# Makefile for The PCI Utilities
# (c) 1998--2020 Martin Mares <mj@ucw.cz>

OPT=-O0
CFLAGS=$(OPT) -Wall -W -Wno-parentheses -Wstrict-prototypes -Wmissing-prototypes -Wno-missing-braces -g -std=gnu99

VERSION=3.7.0
DATE=2020-05-31

# Host OS and release (override if you are cross-compiling)
HOST=
RELEASE=
CROSS_COMPILE=

# Support for compressed pci.ids (yes/no, default: detect)
ZLIB=

# Support for resolving ID's by DNS (yes/no, default: detect)
DNS=

# Build libpci as a shared library (yes/no; or local for testing; requires GCC)
SHARED=no

# Use libkmod to resolve kernel modules on Linux (yes/no, default: detect)
LIBKMOD=

# Use libudev to resolve device names using hwdb on Linux (yes/no, default: detect)
HWDB=

# ABI version suffix in the name of the shared library
# (as we use proper symbol versioning, this seldom needs changing)
ABI_VERSION=.3

# Installation directories
PREFIX=/usr/local
SBINDIR=$(PREFIX)/sbin
SHAREDIR=$(PREFIX)/share
IDSDIR=$(SHAREDIR)
MANDIR:=$(shell if [ -d $(PREFIX)/share/man ] ; then echo $(PREFIX)/share/man ; else echo $(PREFIX)/man ; fi)
INCDIR=$(PREFIX)/include
LIBDIR=$(PREFIX)/lib
PKGCFDIR=$(LIBDIR)/pkgconfig

# 
TARGET_EXEC := h1a_ee
BUILD_DIR := ./build
SRC_DIRS := ./src
SRCS := $(shell find $(SRC_DIRS) -name '*.cpp' -or -name '*.c')
OBJS := $(SRCS:%=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

# Commands
INSTALL=install
DIRINSTALL=install -d
STRIP=-s
CC=$(CROSS_COMPILE)gcc
AR=$(CROSS_COMPILE)ar
RANLIB=$(CROSS_COMPILE)ranlib

# Base name of the library (overridden on NetBSD, which has its own libpci)
LIBNAME=libpci

-include lib/config.mk

PCIINC=lib/config.h lib/header.h lib/pci.h lib/types.h lib/sysdep.h
PCIINC_INS=lib/config.h lib/header.h lib/pci.h lib/types.h

export

all: lib/$(PCILIB) $(TARGET_EXEC)

lib/$(PCILIB): $(PCIINC) force
	$(MAKE) -C lib all

force:

lib/config.h lib/config.mk:
	cd lib && ./configure

$(TARGET_EXEC): LDLIBS+=$(LIBKMOD_LIBS)
$(BUILD_DIR)/ls-kernel.c.o: CFLAGS+=$(LIBKMOD_CFLAGS)

LSPCIINC=$(SRC_DIRS)/adna.h $(SRC_DIRS)/pciutils.h $(PCIINC)

$(TARGET_EXEC): $(OBJS) lib/$(PCILIB)
	$(CC) $(LDFLAGS) $(TARGET_ARCH) $^ $(LDLIBS) -o $@ 

$(BUILD_DIR)/%.c.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

%.8 %.7 %.5: %.man
	M=`echo $(DATE) | sed 's/-01-/-January-/;s/-02-/-February-/;s/-03-/-March-/;s/-04-/-April-/;s/-05-/-May-/;s/-06-/-June-/;s/-07-/-July-/;s/-08-/-August-/;s/-09-/-September-/;s/-10-/-October-/;s/-11-/-November-/;s/-12-/-December-/;s/\(.*\)-\(.*\)-\(.*\)/\3 \2 \1/'` ; sed <$< >$@ "s/@TODAY@/$$M/;s/@VERSION@/pciutils-$(VERSION)/;s#@IDSDIR@#$(IDSDIR)#"

ctags:
	rm -f tags
	find . -name '*.[hc]' -exec ctags --append {} +

TAGS:
	rm -f TAGS
	find . -name '*.[hc]' -exec etags --append {} +

clean:
	rm -f `find . -name "*~" -o -name "*.[oa]" -o -name "\#*\#" -o -name TAGS -o -name core -o -name "*.orig"`
	rm -f $(TARGET_EXEC) lib/config.* *.[578] lib/*.pc lib/*.so lib/*.so.* tags
	rm -rf maint/dist

distclean: clean

install: all
# -c is ignored on Linux, but required on FreeBSD
	$(INSTALL) -c -m 755 $(STRIP) $(TARGET_EXEC) $(DESTDIR)$(SBINDIR)

ifeq ($(SHARED),yes)
ifeq ($(LIBEXT),dylib)
	ln -sf $(PCILIB) $(DESTDIR)$(LIBDIR)/$(LIBNAME)$(ABI_VERSION).$(LIBEXT)
else
	ln -sf $(PCILIB) $(DESTDIR)$(LIBDIR)/$(LIBNAME).$(LIBEXT)$(ABI_VERSION)
endif
endif

ifeq ($(SHARED),yes)
install: install-pcilib
endif

install-pcilib: lib/$(PCILIB)
	$(DIRINSTALL) -m 755 $(DESTDIR)$(LIBDIR)
	$(INSTALL) -c -m 644 lib/$(PCILIB) $(DESTDIR)$(LIBDIR)

install-lib: $(PCIINC_INS) lib/$(PCILIBPC) install-pcilib
	$(DIRINSTALL) -m 755 $(DESTDIR)$(INCDIR)/pci $(DESTDIR)$(PKGCFDIR)
	$(INSTALL) -c -m 644 $(PCIINC_INS) $(DESTDIR)$(INCDIR)/pci
	$(INSTALL) -c -m 644 lib/$(PCILIBPC) $(DESTDIR)$(PKGCFDIR)
ifeq ($(SHARED),yes)
ifeq ($(LIBEXT),dylib)
	ln -sf $(LIBNAME)$(ABI_VERSION).$(LIBEXT) $(DESTDIR)$(LIBDIR)/$(LIBNAME).$(LIBEXT)
else
	ln -sf $(LIBNAME).$(LIBEXT)$(ABI_VERSION) $(DESTDIR)$(LIBDIR)/$(LIBNAME).$(LIBEXT)
endif
endif

uninstall: all
	rm -f $(DESTDIR)$(SBINDIR)/$(TARGET_EXEC) $(DESTDIR)$(SBINDIR)/setpci $(DESTDIR)$(SBINDIR)/update-pciids
ifeq ($(SHARED),yes)
	rm -f $(DESTDIR)$(LIBDIR)/$(PCILIB) $(DESTDIR)$(LIBDIR)/$(LIBNAME).so$(ABI_VERSION)
endif

.PHONY: all clean distclean install install-lib uninstall force tags TAGS
