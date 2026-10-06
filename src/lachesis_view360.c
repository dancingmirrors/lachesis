/*
 * Copyright © 2026 dancingmirrors@icloud.com
 * Based on code © 2026 the VLC authors.
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

#include <string.h>

#include <libavutil/macros.h>

#include "lachesis_config.h"
#include "lachesis_view360.h"

#include <libplacebo/shaders/custom.h>

#define VIEW360_PANINI_LO 80.0
#define VIEW360_PANINI_HI 160.0
#define VIEW360_VC_LO 130.0
#define VIEW360_VC_HI 180.0

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
    "//!PARAM roll\n"
    "//!DESC View roll angle (degrees, positive = clockwise)\n"
    "//!TYPE DYNAMIC float\n"
    "//!MINIMUM -180.0\n"
    "//!MAXIMUM 180.0\n"
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
    "//!PARAM sphere\n"
    "//!DESC Projection (0 = panini, 1 = sphere)\n"
    "//!TYPE DYNAMIC float\n"
    "//!MINIMUM 0.0\n"
    "//!MAXIMUM 1.0\n"
    "0.0\n"
    "\n"
    "//!PARAM rot\n"
    "//!DESC Source rotation in quarter turns\n"
    "//!TYPE DYNAMIC float\n"
    "//!MINIMUM 0.0\n"
    "//!MAXIMUM 3.0\n"
    "0.0\n"
    "\n"
    "//!PARAM view_off_x\n"
    "//!DESC Left edge of the visible part of the view, 0..1\n"
    "//!TYPE DYNAMIC float\n"
    "//!MINIMUM 0.0\n"
    "//!MAXIMUM 1.0\n"
    "0.0\n"
    "\n"
    "//!PARAM view_off_y\n"
    "//!DESC Top edge of the visible part of the view, 0..1\n"
    "//!TYPE DYNAMIC float\n"
    "//!MINIMUM 0.0\n"
    "//!MAXIMUM 1.0\n"
    "0.0\n"
    "\n"
    "//!PARAM view_scale_x\n"
    "//!DESC How much of the view is visible horizontally, 0..1\n"
    "//!TYPE DYNAMIC float\n"
    "//!MINIMUM 0.0\n"
    "//!MAXIMUM 1.0\n"
    "1.0\n"
    "\n"
    "//!PARAM view_scale_y\n"
    "//!DESC How much of the view is visible vertically, 0..1\n"
    "//!TYPE DYNAMIC float\n"
    "//!MINIMUM 0.0\n"
    "//!MAXIMUM 1.0\n"
    "1.0\n"
    "\n"
    "//!PARAM view_aspect\n"
    "//!DESC Aspect of the whole view\n"
    "//!TYPE DYNAMIC float\n"
    "//!MINIMUM 0.01\n"
    "//!MAXIMUM 100.0\n"
    "1.0\n"
    "\n"
    "//!HOOK MAIN\n"
    "//!BIND HOOKED\n"
    "//!DESC 360 Panini/sphere projection with zoom-coupled vertical fit\n"
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
    "#define SPH_MAX " AV_STRINGIFY(VIEW360_SPHERE_HFOV_MAX) "\n"
    "#define SPH_PULLBACK " AV_STRINGIFY(VIEW360_SPHERE_HFOV_PULLBACK) "\n"
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
    "vec3 sphere_ray(vec2 ndc, float aspect) {\n"
    "    float tx = tan(min(hfov, SPH_MAX) * (PI / 360.0));\n"
    "    float ty = tx / aspect;\n"
    "\n"
    "    float tt   = tx * tx + ty * ty;\n"
    "    float dmax = sqrt((1.0 + tt) / tt);\n"
    "\n"
    "    float ramp = (hfov - SPH_PULLBACK) / (SPH_MAX - SPH_PULLBACK);\n"
    "    float dist = dmax * clamp(ramp, 0.0, 1.0);\n"
    "\n"
    "    vec3 d = normalize(vec3(ndc.x * tx, ndc.y * ty, 1.0));\n"
    "\n"
    "    float b = d.z * dist;\n"
    "    float c = dist * dist - 1.0;\n"
    "    float t = b + sqrt(max(b * b - c, 0.0));\n"
    "\n"
    "    return normalize(d * t - vec3(0.0, 0.0, dist));\n"
    "}\n"
    "\n"
    "vec4 hook() {\n"
    "    vec2 view  = vec2(view_off_x, view_off_y) +\n"
    "                 HOOKED_pos * vec2(view_scale_x, view_scale_y);\n"
    "    vec2 ndc   = view * 2.0 - 1.0;\n"
    "    ndc.y      = -ndc.y;\n"
    "\n"
    "    vec3 ray;\n"
    "    if (sphere > 0.5) {\n"
    "        ray = sphere_ray(ndc, view_aspect);\n"
    "    } else {\n"
    "        ray = view_ray(ndc, view_aspect);\n"
    "    }\n"
    "\n"
    "    float r  = roll * (PI / 180.0);\n"
    "    float cr = cos(r), sr = sin(r);\n"
    "    mat3 Rz  = mat3(\n"
    "         cr,  sr, 0.0,\n"
    "        -sr,  cr, 0.0,\n"
    "        0.0, 0.0, 1.0\n"
    "    );\n"
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
    "    vec3 dir = Ry * Rx * Rz * ray;\n"
    "\n"
    "    float lon = atan(dir.x, dir.z);\n"
    "    float lat = asin(clamp(dir.y, -1.0, 1.0));\n"
    "    float u   = lon / (2.0 * PI) + 0.5;\n"
    "    float v   = (0.5 - lat / PI) * (1.0 - 0.5 * tb);\n"
    "\n"
    "    vec2 st = vec2(u, v);\n"
    "    if (rot == 1.0) {\n"
    "        st = vec2(v, 1.0 - u);\n"
    "    } else if (rot == 2.0) {\n"
    "        st = vec2(1.0 - u, 1.0 - v);\n"
    "    } else if (rot == 3.0) {\n"
    "        st = vec2(1.0 - v, u);\n"
    "    }\n"
    "\n"
    "    return HOOKED_tex(st);\n"
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
                            float roll, float hfov, enum View360Layout layout,
                            enum View360Projection projection, int rotate,
                            const View360Viewport *viewport) {
    float tb = layout == VIEW360_LAYOUT_TB ? 1.0f : 0.0f;
    float sphere = projection == VIEW360_PROJECTION_SPHERE ? 1.0f : 0.0f;

    for (int i = 0; i < hook->num_parameters; i++) {
        const struct pl_hook_par *par = &hook->parameters[i];
        if (!strcmp(par->name, "yaw")) {
            par->data->f = yaw;
        }
        if (!strcmp(par->name, "pitch")) {
            par->data->f = pitch;
        }
        if (!strcmp(par->name, "roll")) {
            par->data->f = roll;
        }
        if (!strcmp(par->name, "hfov")) {
            par->data->f = hfov;
        }
        if (!strcmp(par->name, "tb")) {
            par->data->f = tb;
        }
        if (!strcmp(par->name, "sphere")) {
            par->data->f = sphere;
        }
        if (!strcmp(par->name, "rot")) {
            par->data->f = (float)(rotate / 90);
        }
        if (!strcmp(par->name, "view_off_x")) {
            par->data->f = viewport->off_x;
        }
        if (!strcmp(par->name, "view_off_y")) {
            par->data->f = viewport->off_y;
        }
        if (!strcmp(par->name, "view_scale_x")) {
            par->data->f = viewport->scale_x;
        }
        if (!strcmp(par->name, "view_scale_y")) {
            par->data->f = viewport->scale_y;
        }
        if (!strcmp(par->name, "view_aspect")) {
            par->data->f = viewport->aspect;
        }
    }
}
