include config.mk

# Core model (platform-independent)
CORE = core/buf.c core/file.c core/vim.c core/edit.c core/conf.c

# Controller (platform-independent)
APP = app.c

# Terminal frontend (default)
SRC = $(CORE) $(APP) plat/$(PLATFORM).c main.c
OBJ = $(SRC:.c=.o)

onemark: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

# Windows cross-compile target (requires mingw-w64)
WIN_CC      = x86_64-w64-mingw32-gcc
WIN_CFLAGS  = -std=c99 -Wall -Wextra -Os -DWIN32 -D_WIN32 -DUNICODE -D_UNICODE
WIN_LDFLAGS = -lgdi32 -luser32 -lole32 -luuid -mwindows
WIN_SRC     = $(CORE) $(APP) main_win32.c
WIN_OBJ     = $(WIN_SRC:.c=.win32.o)

%.win32.o: %.c
	$(WIN_CC) $(WIN_CFLAGS) -c -o $@ $<

onemark.exe: $(WIN_OBJ)
	$(WIN_CC) $(WIN_CFLAGS) -o $@ $(WIN_OBJ) $(WIN_LDFLAGS)

.c.o:
	$(CC) $(CFLAGS) -c -o $@ $<

config.h:
	cp config.def.h config.h

clean:
	rm -f onemark onemark.exe $(OBJ) $(WIN_OBJ)

install: onemark
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp onemark $(DESTDIR)$(PREFIX)/bin/
	chmod 755 $(DESTDIR)$(PREFIX)/bin/onemark

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/onemark

.PHONY: clean install uninstall
