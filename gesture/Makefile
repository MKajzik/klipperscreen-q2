CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror
LDLIBS ?= -lX11 -lXext -lXtst

all: gesture

gesture: q2-display-gesture.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f gesture

.PHONY: all clean
