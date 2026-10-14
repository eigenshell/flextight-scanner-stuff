# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
SRC = ft.c ft_geom.c ft_holder.c ft_focus.c ft_calib.c ft_job.c ft_png.c ft_detect.c ft_preview.c ft_plan.c ft_maint.c ft_upload.c ft_image.c ft_develop.c ft_dng.c ft_tiff_ifd.c ft_tiff.c ft_out.c ft_curve.c ft_render.c ft_render_auto.c
OBJ = $(SRC:.c=.o)
TESTS = test/test_ft test/test_geom test/test_focus test/test_calib test/test_job test/test_png test/test_detect test/test_preview test/test_plan test/test_maint test/test_upload test/test_develop test/test_curve test/test_render test/test_render_auto test/test_tiff
# the render core also builds for the browser (WASM): libm and mem* only
PURE = ft_curve.o ft_render.o ft_render_auto.o

libft.a: $(OBJ)
	$(AR) rcs $@ $(OBJ)

%.o: %.c *.h
	$(CC) $(CFLAGS) -c -o $@ $<

test/test_detect: test/test_detect.c test/synth.c test/synth.h test/ftest.h libft.a
	$(CC) $(CFLAGS) -I. -o $@ test/test_detect.c test/synth.c libft.a -lm

# libtiff reads the tiled writer's files back (tests only, an independent
# reader); libft itself never links it
test/test_tiff: test/test_tiff.c test/ftest.h libft.a
	$(CC) $(CFLAGS) -I. $$(pkg-config --cflags libtiff-4) -o $@ test/test_tiff.c libft.a $$(pkg-config --libs libtiff-4) -lm

test/%: test/%.c test/ftest.h libft.a
	$(CC) $(CFLAGS) -I. -o $@ $< libft.a -lm

# the check must pass the render core and catch an allocating object
purity: $(PURE) ft_image.o
	@sh test/purity.sh $(PURE)
	@sh test/purity.sh ft_image.o >/dev/null; test $$? -eq 1 || \
	    { echo "purity: the check did not reject calloc in ft_image.o"; exit 1; }

test: $(TESTS) purity
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(OBJ) libft.a $(TESTS) test/png_dump

.PHONY: test clean purity
