# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
SRC = ft.c ft_geom.c ft_holder.c ft_focus.c ft_calib.c ft_job.c ft_png.c ft_detect.c ft_preview.c ft_plan.c ft_maint.c
OBJ = $(SRC:.c=.o)
TESTS = test/test_ft test/test_geom test/test_focus test/test_calib test/test_job test/test_png test/test_detect test/test_preview test/test_plan test/test_maint

libft.a: $(OBJ)
	$(AR) rcs $@ $(OBJ)

%.o: %.c *.h
	$(CC) $(CFLAGS) -c -o $@ $<

test/test_detect: test/test_detect.c test/synth.c test/synth.h test/ftest.h libft.a
	$(CC) $(CFLAGS) -I. -o $@ test/test_detect.c test/synth.c libft.a -lm

test/%: test/%.c test/ftest.h libft.a
	$(CC) $(CFLAGS) -I. -o $@ $< libft.a -lm

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(OBJ) libft.a $(TESTS) test/png_dump

.PHONY: test clean
