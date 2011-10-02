DESTDIR ?=
NSSDIR ?= /usr/lib

CC = gcc
CFLAGS = --std=gnu99 -fPIC -O2 -g -ggdb -Wall
LDFLAGS = -lunbound

MODULE = libnss_ubdns.so.2

BINS = $(MODULE)

all: $(BINS)

OBJS = arpa.o domain_to_str.o lookup.o ubdns.o

$(MODULE): $(OBJS)
	$(CC) -fPIC -shared -Wl,-h,$(MODULE) -Wl,--version-script,nss_ubdns.map -o $@ $(LDFLAGS) $^

clean:
	rm -f $(BINS) $(OBJS)

install:
	mkdir -p $(DESTDIR)$(NSSDIR)
	install -m 0644 $(MODULE) $(DESTDIR)$(NSSDIR)/$(MODULE)

.PHONY: all clean install
