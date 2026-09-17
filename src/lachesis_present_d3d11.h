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

#ifndef LACHESIS_PRESENT_D3D11_H
#define LACHESIS_PRESENT_D3D11_H

#include "lachesis_config.h"

#if LACHESIS_HAVE_D3D11

#include <stdint.h>

#include <libplacebo/d3d11.h>

#include "lachesis_present.h"

typedef struct D3DPresentSample {
    int64_t display_us;
    double refresh_us;
    int source;
} D3DPresentSample;

void d3dpresent_attach(pl_swapchain swapchain);

int d3dpresent_source(void);
int d3dpresent_poll(D3DPresentSample *out);

void d3dpresent_disable(void);
void d3dpresent_shutdown(void);

#endif /* LACHESIS_HAVE_D3D11 */

#endif /* LACHESIS_PRESENT_D3D11_H */
