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

#ifndef LACHESIS_ICC_H
#define LACHESIS_ICC_H

#include <stddef.h>

typedef struct IccProfileInfo {
    char device_class[5]; /* 'mntr', 'scnr', 'prtr'... */
    char color_space[5]; /* 'RGB ', 'GRAY', 'Lab '... */
    int version_major;
    int version_minor;
    int has_vcgt;
} IccProfileInfo;

typedef struct IccGammaRamp {
    int size;
    float *data;
} IccGammaRamp;

int icc_profile_inspect(const void *data, size_t len, IccProfileInfo *info,
                        const char **why);

int icc_profile_read_vcgt(const void *data, size_t len, IccGammaRamp *ramp);

int icc_gamma_ramp_identity(int size, IccGammaRamp *ramp);
void icc_gamma_ramp_scale(IccGammaRamp *ramp, const float scale[3]);
int icc_gamma_ramp_is_identity(const IccGammaRamp *ramp);
void icc_gamma_ramp_free(IccGammaRamp *ramp);

#endif /* LACHESIS_ICC_H */
