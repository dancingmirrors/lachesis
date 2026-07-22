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

#include <math.h>
#include <string.h>

#include <libavutil/macros.h>
#include <libavutil/mem.h>

#include "lachesis_config.h"
#include "lachesis_view360.h"

#if LACHESIS_HAVE_LIBPLACEBO
#include <libplacebo/shaders/custom.h>
#endif

#define VIEW360_PANINI_LO 80.0
#define VIEW360_PANINI_HI 160.0
#define VIEW360_VC_LO 130.0
#define VIEW360_VC_HI 180.0

#if LACHESIS_HAVE_LIBPLACEBO

/* clang-format off */
static const char view360_shader[] =
    "//!PARAM yaw\n"
    "//!DESC Horizontal view angle (degrees, positive = right)\n"
    "//!TYPE DYNAMIC float\n"
    "//!MINIMUM -180.0\n"
    "//!MAXIMUM 180.0\n"
    "0.0\n"
    "\n"
    "//!PARAM pitch\n"
    "//!DESC Vertical view angle (degrees, positive = up)\n"
    "//!TYPE DYNAMIC float\n"
    "//!MINIMUM -90.0\n"
    "//!MAXIMUM 90.0\n"
    "0.0\n"
    "\n"
    "//!PARAM hfov\n"
    "//!DESC Horizontal field of view (degrees)\n"
    "//!TYPE DYNAMIC float\n"
    "//!MINIMUM 10.0\n"
    "//!MAXIMUM 180.0\n"
    "90.0\n"
    "\n"
    "//!PARAM tb\n"
    "//!DESC Frame layout (0 = SBS, 1 = TB)\n"
    "//!TYPE DYNAMIC float\n"
    "//!MINIMUM 0.0\n"
    "//!MAXIMUM 1.0\n"
    "0.0\n"
    "\n"
    "//!HOOK MAIN\n"
    "//!BIND HOOKED\n"
    "//!DESC 360 Panini projection with zoom-coupled vertical fit\n"
    "//!WIDTH OUTPUT.w\n"
    "//!HEIGHT OUTPUT.h\n"
    "\n"
    "#define PI 3.14159265358979323846\n"
    "\n"
    "#define D_LO " AV_STRINGIFY(VIEW360_PANINI_LO) "\n"
    "#define D_HI " AV_STRINGIFY(VIEW360_PANINI_HI) "\n"
    "#define VC_LO " AV_STRINGIFY(VIEW360_VC_LO) "\n"
    "#define VC_HI " AV_STRINGIFY(VIEW360_VC_HI) "\n"
    "\n"
    "vec4 sample_catmull_rom(vec2 uv) {\n"
    "    vec2 sz  = HOOKED_size;\n"
    "    vec2 sp  = uv * sz;\n"
    "    vec2 tc1 = floor(sp - 0.5) + 0.5;\n"
    "    vec2 f   = sp - tc1;\n"
    "\n"
    "    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
    "    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
    "    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
    "    vec2 w3 = f * f * (-0.5 + 0.5 * f);\n"
    "\n"
    "    vec2 w12 = w1 + w2;\n"
    "    vec2 off = w2 / w12;\n"
    "    vec2 t0  = (tc1 - 1.0) / sz;\n"
    "    vec2 t3  = (tc1 + 2.0) / sz;\n"
    "    vec2 t12 = (tc1 + off) / sz;\n"
    "\n"
    "    vec4 r = vec4(0.0);\n"
    "    r += HOOKED_tex(vec2(t0.x,  t0.y )) * w0.x  * w0.y;\n"
    "    r += HOOKED_tex(vec2(t12.x, t0.y )) * w12.x * w0.y;\n"
    "    r += HOOKED_tex(vec2(t3.x,  t0.y )) * w3.x  * w0.y;\n"
    "    r += HOOKED_tex(vec2(t0.x,  t12.y)) * w0.x  * w12.y;\n"
    "    r += HOOKED_tex(vec2(t12.x, t12.y)) * w12.x * w12.y;\n"
    "    r += HOOKED_tex(vec2(t3.x,  t12.y)) * w3.x  * w12.y;\n"
    "    r += HOOKED_tex(vec2(t0.x,  t3.y )) * w0.x  * w3.y;\n"
    "    r += HOOKED_tex(vec2(t12.x, t3.y )) * w12.x * w3.y;\n"
    "    r += HOOKED_tex(vec2(t3.x,  t3.y )) * w3.x  * w3.y;\n"
    "    return r;\n"
    "}\n"
    "\n"
    "vec3 view_ray(vec2 ndc, float aspect) {\n"
    "    float hfov_rad = hfov * (PI / 180.0);\n"
    "    float hh       = hfov_rad * 0.5;\n"
    "    float sh       = sin(hh);\n"
    "    float ch       = cos(hh);\n"
    "\n"
    "    float d = smoothstep(D_LO, D_HI, hfov);\n"
    "\n"
    "    float kx = ndc.x * sh / (d + ch);\n"
    "    float ky = ndc.y * sh / ((d + ch) * aspect);\n"
    "    float kk = kx * kx;\n"
    "\n"
    "    float cphi = (-kk * d + sqrt(1.0 + kk * (1.0 - d * d))) / (1.0 + kk);\n"
    "    float sphi = kx * (d + cphi);\n"
    "\n"
    "    float rv = ky * (d + cphi);\n"
    "\n"
    "    float vcomp = smoothstep(VC_LO, VC_HI, hfov);\n"
    "    float theta = (1.0 + vcomp) * atan(rv);\n"
    "    float sinth = sin(theta);\n"
    "    float costh = cos(theta);\n"
    "\n"
    "    return vec3(costh * sphi, sinth, costh * cphi);\n"
    "}\n"
    "\n"
    "vec4 hook() {\n"
    "    vec2 ndc = HOOKED_pos * 2.0 - 1.0;\n"
    "    ndc.y    = -ndc.y;\n"
    "\n"
    "    float aspect = target_size.x / target_size.y;\n"
    "    vec3 ray = view_ray(ndc, aspect);\n"
    "\n"
    "    float p  = pitch * (PI / 180.0);\n"
    "    float cp = cos(p), sp = sin(p);\n"
    "    mat3 Rx  = mat3(\n"
    "        1.0, 0.0,  0.0,\n"
    "        0.0,  cp,  -sp,\n"
    "        0.0,  sp,   cp\n"
    "    );\n"
    "\n"
    "    float ya = yaw * (PI / 180.0);\n"
    "    float cy = cos(ya), sy = sin(ya);\n"
    "    mat3 Ry  = mat3(\n"
    "        cy, 0.0, -sy,\n"
    "        0.0, 1.0, 0.0,\n"
    "        sy, 0.0, cy\n"
    "    );\n"
    "\n"
    "    vec3 dir = Ry * Rx * ray;\n"
    "\n"
    "    float lon = atan(dir.x, dir.z);\n"
    "    float lat = asin(clamp(dir.y, -1.0, 1.0));\n"
    "    float u   = lon / (2.0 * PI) + 0.5;\n"
    "    float v   = (0.5 - lat / PI) * (1.0 - 0.5 * tb);\n"
    "\n"
    "    return sample_catmull_rom(vec2(u, v));\n"
    "}\n";
/* clang-format on */

const struct pl_hook *view360_pl_hook_create(const struct pl_gpu_t *gpu) {
    return pl_mpv_user_shader_parse(gpu, view360_shader,
                                    sizeof(view360_shader) - 1);
}

void view360_pl_hook_destroy(const struct pl_hook **hook) {
    pl_mpv_user_shader_destroy(hook);
}

void view360_pl_hook_update(const struct pl_hook *hook, float yaw, float pitch,
                            float hfov, enum View360Layout layout) {
    float tb = layout == VIEW360_LAYOUT_TB ? 1.0f : 0.0f;

    for (int i = 0; i < hook->num_parameters; i++) {
        const struct pl_hook_par *par = &hook->parameters[i];
        if (!strcmp(par->name, "yaw")) {
            par->data->f = yaw;
        }
        if (!strcmp(par->name, "pitch")) {
            par->data->f = pitch;
        }
        if (!strcmp(par->name, "hfov")) {
            par->data->f = hfov;
        }
        if (!strcmp(par->name, "tb")) {
            par->data->f = tb;
        }
    }
}

#endif /* LACHESIS_HAVE_LIBPLACEBO */

#define VIEW360_GRID_W 128
#define VIEW360_GRID_H 72

#define VIEW360_PI 3.14159265358979323846f

typedef struct View360Mesh {
    SDL_Vertex *verts;
    int *indices;
    int num_verts;
    int num_indices;

    float *grid_u;
    float *grid_v;

    SDL_Rect rect;
    float yaw;
    float pitch;
    float hfov;
    int layout;
    int flip_v;
    int valid;
} View360Mesh;

static View360Mesh mesh;

static float view360_smoothstep(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

static float view360_clamp(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static void view360_project_grid(const SDL_Rect *rect, float yaw, float pitch,
                                 float hfov, float tb, int flip_v) {
    float aspect = (float)rect->w / (float)rect->h;
    float hfov_rad = hfov * (VIEW360_PI / 180.0f);
    float hh = hfov_rad * 0.5f;
    float sh = sinf(hh);
    float ch = cosf(hh);
    float d = view360_smoothstep(VIEW360_PANINI_LO, VIEW360_PANINI_HI, hfov);
    float vscale = 1.0f + view360_smoothstep(VIEW360_VC_LO, VIEW360_VC_HI, hfov);
    float vmul = 1.0f - 0.5f * tb;

    float p = pitch * (VIEW360_PI / 180.0f);
    float cp = cosf(p), sp = sinf(p);
    float ya = yaw * (VIEW360_PI / 180.0f);
    float cy = cosf(ya), sy = sinf(ya);

    float col_sphi[VIEW360_GRID_W + 1];
    float col_cphi[VIEW360_GRID_W + 1];
    float col_dpc[VIEW360_GRID_W + 1];

    for (int i = 0; i <= VIEW360_GRID_W; i++) {
        float ndc_x = (float)i / VIEW360_GRID_W * 2.0f - 1.0f;
        float kx = ndc_x * sh / (d + ch);
        float kk = kx * kx;
        float cphi = (-kk * d + sqrtf(1.0f + kk * (1.0f - d * d))) / (1.0f + kk);

        col_cphi[i] = cphi;
        col_sphi[i] = kx * (d + cphi);
        col_dpc[i] = d + cphi;
    }

    for (int j = 0; j <= VIEW360_GRID_H; j++) {
        float ndc_y = -((float)j / VIEW360_GRID_H * 2.0f - 1.0f);
        float ky = ndc_y * sh / ((d + ch) * aspect);
        float *row_u = &mesh.grid_u[j * (VIEW360_GRID_W + 1)];
        float *row_v = &mesh.grid_v[j * (VIEW360_GRID_W + 1)];

        for (int i = 0; i <= VIEW360_GRID_W; i++) {
            float rv = ky * col_dpc[i];
            float theta = vscale * atanf(rv);
            float sinth = sinf(theta);
            float costh = cosf(theta);

            float x = costh * col_sphi[i];
            float y = sinth;
            float z = costh * col_cphi[i];

            float y2 = cp * y + sp * z;
            float z2 = -sp * y + cp * z;
            float x3 = cy * x + sy * z2;
            float z3 = -sy * x + cy * z2;

            float lon = atan2f(x3, z3);
            float lat = asinf(view360_clamp(y2, -1.0f, 1.0f));
            float v = (0.5f - lat / VIEW360_PI) * vmul;

            row_u[i] = lon / (2.0f * VIEW360_PI) + 0.5f;
            row_v[i] = flip_v ? 1.0f - v : v;
        }
    }
}

static void view360_emit_quad(const SDL_FPoint pos[4], const float u[4],
                              const float v[4]) {
    int base = mesh.num_verts;
    int *ix = &mesh.indices[mesh.num_indices];

    for (int k = 0; k < 4; k++) {
        SDL_Vertex *vt = &mesh.verts[mesh.num_verts++];
        vt->position = pos[k];
        vt->color = (SDL_FColor){1.0f, 1.0f, 1.0f, 1.0f};
        vt->tex_coord = (SDL_FPoint){u[k], v[k]};
    }

    ix[0] = base;
    ix[1] = base + 1;
    ix[2] = base + 2;
    ix[3] = base + 1;
    ix[4] = base + 3;
    ix[5] = base + 2;
    mesh.num_indices += 6;
}

static float view360_split_t(float a, float b) {
    float d = b - a;
    if (fabsf(d) < 1e-6f) {
        return 0.5f;
    }
    return view360_clamp((1.0f - a) / d, 0.0f, 1.0f);
}

static int view360_build(const SDL_Rect *rect, enum View360Layout layout,
                         float yaw, float pitch, float hfov, int flip_v) {
    const int cells = VIEW360_GRID_W * VIEW360_GRID_H;
    const int grid_points = (VIEW360_GRID_W + 1) * (VIEW360_GRID_H + 1);

    if (!mesh.verts) {
        mesh.verts = av_malloc(cells * 8 * sizeof(*mesh.verts));
        mesh.indices = av_malloc(cells * 12 * sizeof(*mesh.indices));
        mesh.grid_u = av_malloc(grid_points * sizeof(*mesh.grid_u));
        mesh.grid_v = av_malloc(grid_points * sizeof(*mesh.grid_v));
        if (!mesh.verts || !mesh.indices || !mesh.grid_u || !mesh.grid_v) {
            view360_free();
            return -1;
        }
    }

    view360_project_grid(rect, yaw, pitch, hfov,
                         layout == VIEW360_LAYOUT_TB ? 1.0f : 0.0f, flip_v);

    mesh.num_verts = 0;
    mesh.num_indices = 0;

    for (int j = 0; j < VIEW360_GRID_H; j++) {
        float py0 = rect->y + rect->h * ((float)j / VIEW360_GRID_H);
        float py1 = rect->y + rect->h * ((float)(j + 1) / VIEW360_GRID_H);
        const float *row0_u = &mesh.grid_u[j * (VIEW360_GRID_W + 1)];
        const float *row0_v = &mesh.grid_v[j * (VIEW360_GRID_W + 1)];
        const float *row1_u = &mesh.grid_u[(j + 1) * (VIEW360_GRID_W + 1)];
        const float *row1_v = &mesh.grid_v[(j + 1) * (VIEW360_GRID_W + 1)];

        for (int i = 0; i < VIEW360_GRID_W; i++) {
            float px0 = rect->x + rect->w * ((float)i / VIEW360_GRID_W);
            float px1 = rect->x + rect->w * ((float)(i + 1) / VIEW360_GRID_W);
            SDL_FPoint pos[4] = {
                {px0, py0}, {px1, py0}, {px0, py1}, {px1, py1}};
            float u[4] = {row0_u[i], row0_u[i + 1], row1_u[i], row1_u[i + 1]};
            float v[4] = {row0_v[i], row0_v[i + 1], row1_v[i], row1_v[i + 1]};
            float umin, umax, base;

            for (int k = 1; k < 4; k++) {
                u[k] += rintf(u[0] - u[k]);
            }
            umin = umax = u[0];
            for (int k = 1; k < 4; k++) {
                umin = u[k] < umin ? u[k] : umin;
                umax = u[k] > umax ? u[k] : umax;
            }

            if (umax - umin > 0.9f) {
                base = floorf(umin);
                for (int k = 0; k < 4; k++) {
                    u[k] = view360_clamp(u[k] - base, 0.0f, 1.0f);
                }
                view360_emit_quad(pos, u, v);
                continue;
            }

            base = floorf(umin);
            for (int k = 0; k < 4; k++) {
                u[k] -= base;
            }

            if (umax - base <= 1.0f) {
                view360_emit_quad(pos, u, v);
                continue;
            }

            {
                float t_top = view360_split_t(u[0], u[1]);
                float t_bot = view360_split_t(u[2], u[3]);
                SDL_FPoint ptop = {px0 + (px1 - px0) * t_top, py0};
                SDL_FPoint pbot = {px0 + (px1 - px0) * t_bot, py1};
                float vtop = v[0] + (v[1] - v[0]) * t_top;
                float vbot = v[2] + (v[3] - v[2]) * t_bot;

                SDL_FPoint lpos[4] = {pos[0], ptop, pos[2], pbot};
                float lu[4] = {fminf(u[0], 1.0f), 1.0f, fminf(u[2], 1.0f), 1.0f};
                float lv[4] = {v[0], vtop, v[2], vbot};
                view360_emit_quad(lpos, lu, lv);

                SDL_FPoint rpos[4] = {ptop, pos[1], pbot, pos[3]};
                float ru[4] = {0.0f, fmaxf(u[1] - 1.0f, 0.0f), 0.0f,
                               fmaxf(u[3] - 1.0f, 0.0f)};
                float rv[4] = {vtop, v[1], vbot, v[3]};
                view360_emit_quad(rpos, ru, rv);
            }
        }
    }

    mesh.rect = *rect;
    mesh.yaw = yaw;
    mesh.pitch = pitch;
    mesh.hfov = hfov;
    mesh.layout = layout;
    mesh.flip_v = flip_v;
    mesh.valid = 1;

    return 0;
}

int view360_draw(SDL_Renderer *renderer, SDL_Texture *texture,
                 const SDL_Rect *rect, enum View360Layout layout,
                 float yaw, float pitch, float hfov, int flip_v) {
    if (!renderer || !texture || !rect || rect->w <= 0 || rect->h <= 0) {
        return -1;
    }

    if (!mesh.valid || mesh.yaw != yaw || mesh.pitch != pitch ||
        mesh.hfov != hfov || mesh.layout != (int)layout ||
        mesh.flip_v != flip_v ||
        memcmp(&mesh.rect, rect, sizeof(*rect)) != 0) {
        if (view360_build(rect, layout, yaw, pitch, hfov, flip_v) < 0) {
            return -1;
        }
    }

    if (!SDL_RenderGeometry(renderer, texture, mesh.verts, mesh.num_verts,
                            mesh.indices, mesh.num_indices)) {
        return -1;
    }

    return 0;
}

void view360_free(void) {
    av_freep(&mesh.verts);
    av_freep(&mesh.indices);
    av_freep(&mesh.grid_u);
    av_freep(&mesh.grid_v);
    mesh.num_verts = 0;
    mesh.num_indices = 0;
    mesh.valid = 0;
}
