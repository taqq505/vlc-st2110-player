PREFIX    = /usr
CC        = cc
CFLAGS    = -std=gnu99 -O2 -Wall -Wextra -fPIC -DPIC $(shell pkg-config --cflags vlc-plugin)
LIBS      = $(shell pkg-config --libs vlc-plugin)
CPPFLAGS  = -D__PLUGIN__ -DMODULE_STRING=\"st2110\"
plugindir = $(PREFIX)/lib/vlc/plugins/access

libst2110_plugin.so: st2110.o
	$(CC) -shared -o $@ $< $(LIBS)

st2110.o: st2110.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

install: libst2110_plugin.so
	install -D $< $(plugindir)/$<

clean:
	rm -f *.o *.so
