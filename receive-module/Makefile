VLC_PLUGIN_CFLAGS := $(shell pkg-config --cflags vlc-plugin)
VLC_PLUGIN_LIBS   := $(shell pkg-config --libs vlc-plugin)

CC     ?= gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2 -fPIC -DPIC $(VLC_PLUGIN_CFLAGS)
LDFLAGS =
LIBS    = $(VLC_PLUGIN_LIBS)

PREFIX    ?= /usr
plugindir  = $(PREFIX)/lib/vlc/plugins/access

TARGET = libst2110_plugin.so

all: $(TARGET)

$(TARGET): st2110.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDFLAGS) $(LIBS)

install: $(TARGET)
	install -d "$(DESTDIR)$(plugindir)"
	install -m 755 $(TARGET) "$(DESTDIR)$(plugindir)/"

clean:
	rm -f $(TARGET)

.PHONY: all install clean
