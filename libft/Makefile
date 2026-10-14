# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
SRC = ft.c ft_geom.c ft_holder.c ft_focus.c ft_calib.c ft_job.c ft_png.c ft_detect.c ft_preview.c ft_plan.c ft_maint.c ft_upload.c ft_image.c ft_develop.c ft_dng.c ft_out.c ft_curve.c ft_render.c ft_render_auto.c ft_share.c ft_share_cut.c ft_output.c ft_icc.c
OBJ = $(SRC:.c=.o)
TESTS = test/test_ft test/test_geom test/test_focus test/test_calib test/test_job test/test_png test/test_detect test/test_preview test/test_plan test/test_maint test/test_upload test/test_develop test/test_curve test/test_render test/test_render_auto test/test_share test/test_share_cut test/test_dng test/test_output test/test_icc
# the render core, the share planner and the output colour units also
# build for the browser (WASM): libm and mem* only
PURE = ft_curve.o ft_render.o ft_render_auto.o ft_plan.o ft_share.o ft_output.o ft_icc.o
# lcms2 is a test oracle only: libft itself never links it
LCMS_LIBS ?= $(shell pkg-config --libs lcms2 2>/dev/null || echo -llcms2)

libft.a: $(OBJ)
	$(AR) rcs $@ $(OBJ)

%.o: %.c *.h
	$(CC) $(CFLAGS) -c -o $@ $<

test/test_detect: test/test_detect.c test/synth.c test/synth.h test/ftest.h libft.a
	$(CC) $(CFLAGS) -I. -o $@ test/test_detect.c test/synth.c libft.a -lm

test/test_share_cut: test/test_share_cut.c test/synth.c test/synth.h test/ftest.h libft.a
	$(CC) $(CFLAGS) -I. -o $@ test/test_share_cut.c test/synth.c libft.a -lm

test/test_icc: test/test_icc.c test/ftest.h libft.a
	$(CC) $(CFLAGS) -I. -o $@ test/test_icc.c libft.a $(LCMS_LIBS) -lm

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
