# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
LIBFT = ../../libft
DETAIL = ../detail
# ftdev export writes JPEGs: libjpeg (libjpeg-turbo). libft.a itself has
# no dependencies; ft_jpeg.c is built here.
JPEG_LIBS ?= $(shell pkg-config --libs libjpeg 2>/dev/null || echo -ljpeg)
SRC = ftdev.c detail.c util.c render_flags.c share.c stock.c print.c plan.c \
	$(LIBFT)/ft_jpeg.c $(DETAIL)/ring.c $(DETAIL)/worker.c

ftdev: $(SRC) *.h $(DETAIL)/ring.h $(DETAIL)/worker.h $(LIBFT)/ft_jpeg.h \
		$(LIBFT)/libft.a
	$(CC) $(CFLAGS) -I$(LIBFT) -I. -I$(DETAIL) -o $@ $(SRC) \
	    $(LIBFT)/libft.a $(JPEG_LIBS) -lm -pthread

$(LIBFT)/libft.a: FORCE
	$(MAKE) -C $(LIBFT) libft.a

clean:
	rm -f ftdev

FORCE:
.PHONY: clean FORCE
