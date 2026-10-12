# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
SRC = ft.c
OBJ = $(SRC:.c=.o)
TESTS = test/test_ft

libft.a: $(OBJ)
	$(AR) rcs $@ $(OBJ)

%.o: %.c *.h
	$(CC) $(CFLAGS) -c -o $@ $<

test/%: test/%.c libft.a
	$(CC) $(CFLAGS) -I. -o $@ $< libft.a -lm

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(OBJ) libft.a $(TESTS) test/png_dump

.PHONY: test clean
