/*
 * Copyright © 2026 dancingmirrors@icloud.com
 *
 * This file is part of lachesis.
 *
 * lachesis is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * lachesis is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with lachesis; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef LACHESIS_MATH_H
#define LACHESIS_MATH_H

#include <math.h>

#include "lachesis_config.h"

#if HAVE_BUILTINS
#define LACHESIS_NAN (__builtin_nan(""))
#define LACHESIS_INF (__builtin_inf())
#else
#define LACHESIS_NAN ((double)NAN)
#define LACHESIS_INF ((double)INFINITY)
#endif

#endif /* LACHESIS_MATH_H */
