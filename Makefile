SHELL := /bin/bash
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror

.PHONY: check check-files check-c

check: check-files check-c

check-files:
	./scripts/check.sh

check-c:
	@if [[ "$$(uname -s)" == "Linux" ]]; then \
		$(CC) $(CFLAGS) -fsyntax-only bridge/q2-x11-fb-bridge.c; \
		$(CC) $(CFLAGS) -fsyntax-only gesture/q2-display-gesture.c; \
		printf '%s\n' "C sources: OK"; \
	else \
		printf '%s\n' "C sources: skipped (Linux headers required)"; \
	fi
