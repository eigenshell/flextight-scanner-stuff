# SPDX-License-Identifier: GPL-2.0-or-later
# The ring and pass worker of detailed scans (sub-project 2b). ftpoc and
# ftdev compile ring.c and worker.c themselves; this builds the ring test.
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
LIBFT = ../../libft

test_ring: test_ring.c ring.c ring.h
	$(CC) $(CFLAGS) -I$(LIBFT) -o $@ test_ring.c ring.c -pthread

test: test_ring
	./test_ring

clean:
	rm -f test_ring

.PHONY: test clean
