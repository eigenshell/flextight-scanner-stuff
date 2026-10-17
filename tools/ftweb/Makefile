# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
LIBFT = ../../libft

all: ftweb_cli purity-web

# the engine natively, driven from files (tests/test_ftweb.py)
ftweb_cli: ftweb_cli.c ftweb.c ftweb.h $(LIBFT)/libft.a
	$(CC) $(CFLAGS) -I$(LIBFT) -o $@ ftweb_cli.c ftweb.c $(LIBFT)/libft.a -lm

ftweb.o: ftweb.c ftweb.h
	$(CC) $(CFLAGS) -I$(LIBFT) -c -o $@ ftweb.c

# the engine is browser tier
purity-web: ftweb.o
	@sh $(LIBFT)/test/purity_web.sh ftweb.o

$(LIBFT)/libft.a: FORCE
	$(MAKE) -C $(LIBFT) libft.a

clean:
	rm -f ftweb_cli ftweb.o

FORCE:
.PHONY: all clean purity-web FORCE
