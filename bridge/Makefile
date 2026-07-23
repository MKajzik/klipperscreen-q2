CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror
LDLIBS ?= -lX11 -lXext -lXtst

all: q2-x11-fb-bridge

q2-x11-fb-bridge: q2-x11-fb-bridge.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f q2-x11-fb-bridge

.PHONY: all clean
