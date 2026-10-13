# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
LIBFT = ../../libft

ftdev: ftdev.c $(LIBFT)/libft.a
	$(CC) $(CFLAGS) -I$(LIBFT) -o $@ ftdev.c $(LIBFT)/libft.a -lm

$(LIBFT)/libft.a: FORCE
	$(MAKE) -C $(LIBFT) libft.a

clean:
	rm -f ftdev

FORCE:
.PHONY: clean FORCE
