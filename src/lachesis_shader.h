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

#ifndef LACHESIS_SHADER_H
#define LACHESIS_SHADER_H

#include <stddef.h>
#include <stdint.h>

#define SHADER_MAX 32

extern int shader_enabled;

int shader_add(const char *path);
void shader_clear(void);
void shader_config_done(void);
int shader_count(void);
const char *shader_path(int i);
const char *shader_name(int i);
void shader_free_all(void);

int shader_params_set(const char *spec);

enum ShaderState {
    SHADER_RUNNING,
    SHADER_NO_RENDERER,
    SHADER_BENCHMARKING,
    SHADER_OFF,
};

const char *shader_status(enum ShaderState state, int num_failed);

struct pl_gpu_t;
struct pl_hook;

typedef struct ShaderSet ShaderSet;

ShaderSet *shader_set_load(const struct pl_gpu_t *gpu);

void shader_set_free(ShaderSet **set);

int shader_set_hooks(const ShaderSet *set, const struct pl_hook **hooks,
                     int max);

void shader_set_note_disabled(ShaderSet *set, const uint64_t *signatures,
                              int num);

int shader_set_num_failed(const ShaderSet *set);

#endif // LACHESIS_SHADER_H
