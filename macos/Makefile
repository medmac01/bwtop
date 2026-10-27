CFLAGS = -O2 -Wall
LIBS = -framework CoreFoundation -framework IOKit -lIOReport

all: mbtop membw

mbtop: mbtop.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

membw: membw.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

install: all
	install -m 755 mbtop membw /usr/local/bin/

clean:
	rm -f mbtop membw

.PHONY: all install clean
