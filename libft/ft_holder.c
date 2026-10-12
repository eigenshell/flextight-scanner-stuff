/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "ft_holder.h"

static const ft_holder HOLDERS[] = {
    { "24x217", 231.0, 32.0, 2.02, 38.02, 40.0, 6 },
};

const ft_holder *ft_holder_find(const char *name)
{
    size_t i;
    if (!name)
        return NULL;
    for (i = 0; i < sizeof HOLDERS / sizeof HOLDERS[0]; i++)
        if (strcmp(HOLDERS[i].name, name) == 0)
            return &HOLDERS[i];
    return NULL;
}
