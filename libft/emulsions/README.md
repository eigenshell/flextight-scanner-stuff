# Emulsion tables

`.cube` 3D tables for the `lut3d` print module (`libft/ft_pm.h`): the input
is the print exposure e (D), mapped to 0..1 by the emulsion entry's
`lo`/`hi`; the output is sRGB-encoded RGB. After adding or changing a
table, run `make luts` in `libft/` (needs Python 3), add or update its
entry in `ft_emulsion.c` and bump `FT_EMULSION_REV`. The generated
`ft_emulsion_luts.h` is committed, so building libft never needs Python.

Only commit tables you may publish: this is a public GPL repository.
