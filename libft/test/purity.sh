#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# The render core also runs in the browser (WASM): its objects may call
# only libm, mem*, strcmp/strtod and libft's render units, never malloc,
# stdio or threads. Usage: sh test/purity.sh file.o...
bad=$(nm -u "$@" | awk 'NF == 2 { print $2 }' |
      grep -Ev '^(ft_fail|ft_curve_[a-z_]+|ft_recipe_[a-z_]+|ft_render_[a-z_]+|mem(cpy|set|move|cmp)|strcmp|strtod|(exp|log|log10|pow|sqrt|cbrt|ceil|floor|fabs)f?|__stack_chk_fail|__[a-z0-9_]+_finite)$' |
      sort -u)
if [ -n "$bad" ]; then
    echo "purity: $* call" $bad
    exit 1
fi
echo "purity: $* call only libm, mem* and libft"
