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

#ifndef LACHESIS_SINGLE_H
#define LACHESIS_SINGLE_H

#include "lachesis_internal.h"

enum SingleMode {
    SINGLE_REPLACE = 0,
    SINGLE_OFF,
    SINGLE_QUEUE,
};

enum SingleRole {
    SINGLE_ROLE_ALONE = 0,
    SINGLE_ROLE_PRIMARY,
    SINGLE_ROLE_HANDED_OFF,
};

extern const char *const single_modes[];
extern int single_mode;

enum SingleRole single_claim(char **paths, int n_paths);

/* These two are main thread only. */
void single_poll(void);
void single_handle_event(VideoState **pis);

void single_shutdown(void);

#endif /* LACHESIS_SINGLE_H */
