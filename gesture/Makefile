CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror
LDLIBS ?= -lX11 -lXext -lXtst

all: q2-display-gesture

q2-display-gesture: q2-display-gesture.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f q2-display-gesture

.PHONY: all clean
