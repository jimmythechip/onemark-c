include config.mk

SRC = core/buf.c core/file.c core/vim.c core/nav.c core/edit.c core/search.c
SRC += plat/$(PLATFORM).c main.c

OBJ = $(SRC:.c=.o)

onemark: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

.c.o:
	$(CC) $(CFLAGS) -c -o $@ $<

config.h:
	cp config.def.h config.h

clean:
	rm -f onemark $(OBJ)

install: onemark
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp onemark $(DESTDIR)$(PREFIX)/bin/
	chmod 755 $(DESTDIR)$(PREFIX)/bin/onemark

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/onemark

.PHONY: clean install uninstall
