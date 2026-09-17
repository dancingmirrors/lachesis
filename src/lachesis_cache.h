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

#ifndef LACHESIS_CACHE_H
#define LACHESIS_CACHE_H

#include <libavutil/dict.h>
#include <libplacebo/cache.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>

typedef struct ShaderCache {
    pl_cache cache;
    char *dir;
    char *prefix;
    char leaf[24];
} ShaderCache;

void shader_cache_open(ShaderCache *sc, pl_gpu gpu, pl_log log,
                       const char *backend, const AVDictionary *opt);
void shader_cache_close(ShaderCache *sc, pl_gpu gpu);

#endif /* LACHESIS_CACHE_H */
