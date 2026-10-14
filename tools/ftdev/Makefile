# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
LIBFT = ../../libft
# ftdev export writes JPEGs: libjpeg (libjpeg-turbo). libft.a itself has
# no dependencies; ft_jpeg.c is built here.
JPEG_LIBS ?= $(shell pkg-config --libs libjpeg 2>/dev/null || echo -ljpeg)
SRC = ftdev.c render_flags.c share.c $(LIBFT)/ft_jpeg.c

ftdev: $(SRC) *.h $(LIBFT)/ft_jpeg.h $(LIBFT)/libft.a
	$(CC) $(CFLAGS) -I$(LIBFT) -I. -o $@ $(SRC) $(LIBFT)/libft.a \
	    $(JPEG_LIBS) -lm

$(LIBFT)/libft.a: FORCE
	$(MAKE) -C $(LIBFT) libft.a

clean:
	rm -f ftdev

FORCE:
.PHONY: clean FORCE
