/*
 * Copyright © 2003 Fabrice Bellard
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

#include "lachesis_config.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libavutil/avstring.h>
#include <libavutil/macros.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>

#include <SDL3/SDL.h>

#include "lachesis_alloc.h"
#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_playlist.h"
#include "lachesis_present.h"
#include "lachesis_renderer.h"
#include "lachesis_wayland.h"
#include "lachesis_window.h"

#define WINDOW_FALLBACK_FIT 0.85f
#define RAISE_GRACE_US (300 * 1000)

static char *window_title_path;
static char *window_title_shown;

int window_placed;

static int raise_wanted;
static int raise_asked;
static char *raise_token;
static int64_t raise_started_us;

static float window_points_scale(void) {
    float density = 0.0f;

    if (window) {
        density = SDL_GetWindowPixelDensity(window);
    } else {
        const SDL_DisplayMode *mode =
            SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());

        density = mode ? mode->pixel_density : 0.0f;
    }

    return density > 0.0f ? density : 1.0f;
}

static void fallback_size_for_display(SDL_DisplayID id, int *out_w, int *out_h) {
    const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(id);
    float density = mode && mode->pixel_density > 0.0f ? mode->pixel_density : 1.0f;
    int64_t width = 1920, height = 1080;
    SDL_Rect display_bounds;

    if (SDL_GetDisplayBounds(id, &display_bounds) && display_bounds.w > 0 &&
        display_bounds.h > 0) {
        int64_t max_width = (int64_t)(display_bounds.w * density * WINDOW_FALLBACK_FIT);
        int64_t max_height = (int64_t)(display_bounds.h * density * WINDOW_FALLBACK_FIT);

        if (width > max_width || height > max_height) {
            height = max_height;
            width = av_rescale(height, 16, 9) & ~1;
            if (width > max_width) {
                width = max_width;
                height = av_rescale(width, 9, 16) & ~1;
            }
        }
    }

    *out_w = FFMAX((int)lrintf((float)width / density), 1);
    *out_h = FFMAX((int)lrintf((float)height / density), 1);
}

void init_default_window_size(void) {
    const char *driver = SDL_GetCurrentVideoDriver();
    SDL_DisplayID *displays;

    if (driver && !strcmp(driver, "kmsdrm")) {
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());

        if (mode && mode->w > 0 && mode->h > 0) {
            default_width = mode->w;
            default_height = mode->h;
            return;
        }
    }

    displays = SDL_GetDisplays(NULL);
    default_width = 1920;
    default_height = 1080;
    for (int i = 0; displays && displays[i]; i++) {
        int w, h;

        fallback_size_for_display(displays[i], &w, &h);
        if (i == 0 || w < default_width) {
            default_width = w;
            default_height = h;
        }
    }
    SDL_free(displays);
    log_verbose("Outside of fullscreen the window falls back to %dx%d.\n",
                default_width, default_height);
}

int note_window_pixel_size(int w, int h) {
    if (w <= 0 || h <= 0) {
        return 0;
    }
    screen_width = w;
    screen_height = h;

    return 1;
}

void update_screen_size(void) {
    int w = 0, h = 0;

    if (!window) {
        return;
    }
    SDL_GetWindowSizeInPixels(window, &w, &h);
    note_window_pixel_size(w, h);
}

int video_adopt_window_size(VideoState *is) {
    if (screen_width <= 0 || screen_height <= 0) {
        return 0;
    }
    is->width = screen_width;
    is->height = screen_height;

    return 1;
}

float window_pixel_density(void) {
    float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;

    return density > 0.0f ? density : 1.0f;
}

static const char *file_basename(const char *path) {
    const char *slash = strrchr(path, '/');
#if defined(_WIN32)
    const char *backslash = strrchr(path, '\\');

    if (backslash && (!slash || backslash > slash)) {
        slash = backslash;
    }
#endif
    return (slash && slash[1]) ? slash + 1 : path;
}

static char *make_default_window_title(const char *path,
                                       const char *archive_path,
                                       const char *entry_name) {
    const char *display;
    char *owned = NULL;

    if (archive_path && entry_name) {
        owned = av_asprintf("%s | %s", file_basename(archive_path), entry_name);
        display = owned ? owned : entry_name;
    } else {
        if (!path) {
            return NULL;
        }
        display = strstr(path, "://") ? path : file_basename(path);
    }

    char *title = av_asprintf("%s - %s", program_name, display);
    av_free(owned);

    return title;
}

static void playlist_counter(char *buf, size_t size) {
    buf[0] = '\0';
    if (playlist_size > 1) {
        snprintf(buf, size, " [%d/%d]", playlist_pos + 1, playlist_size);
    }
}

void note_media_window_title(const char *path, const char *archive_path,
                             const char *entry_name) {
    av_freep(&window_title_path);
    window_title_path = make_default_window_title(path, archive_path,
                                                  entry_name);
}

static const char *current_window_title(VideoState *is) {
    if (window_title) {
        return window_title;
    }
    if (SDL_GetAtomicInt(&is->open_phase) == STREAM_OPEN_DONE && window_title_auto) {
        return window_title_auto;
    }

    return window_title_path;
}

void refresh_window_title(VideoState *is) {
    const char *base = current_window_title(is);
    char counter[32] = "";
    char *title;
    size_t len;

    if (!window || !base) {
        return;
    }
    if (base != window_title) {
        playlist_counter(counter, sizeof(counter));
    }
    len = strlen(base);
    if (window_title_shown && !strncmp(window_title_shown, base, len) &&
        !strcmp(window_title_shown + len, counter)) {
        return;
    }
    if (!(title = av_asprintf("%s%s", base, counter))) {
        return;
    }
    av_free(window_title_shown);
    window_title_shown = title;
    SDL_SetWindowTitle(window, title);
}

static int window_fullscreen_now(void) {
    return !!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
}

static int hold_output(void) {
    return renderer_pause_output(renderer);
}

static void drop_output(int held) {
    if (held) {
        renderer_resume_output(renderer);
    }
}

void present_pacing_reset(void) {
    renderer_drop_present_feedback(renderer);
    present_reset();
}

static int display_info_stale;

void refresh_display_info(VideoState *is) {
    int ret;

    if (!display_info_stale || !renderer) {
        return;
    }
    ret = renderer_refresh_display_info(renderer, window);
    if (ret < 0) {
        return;
    }
    display_info_stale = 0;
    if (ret > 0) {
        is->force_refresh = 1;
    }
}

void note_display_info_change(VideoState *is) {
    display_info_stale = 1;
    refresh_display_info(is);
}

void window_want_raise(const char *token) {
    av_freep(&raise_token);
    if (token && *token) {
        raise_token = av_strdup(token);
    }
    raise_started_us = av_gettime_relative();
    raise_asked = 0;
    raise_wanted = 1;
}

static int raise_arm(void) {
    if (!raise_wanted) {
        return 0;
    }
    if (raise_token) {
        SDL_SetEnvironmentVariable(SDL_GetEnvironment(),
                                   "XDG_ACTIVATION_TOKEN", raise_token, true);
    }

    return 1;
}

static void raise_spent(void) {
    raise_wanted = 0;
    raise_asked = 0;
    av_freep(&raise_token);
    SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), "XDG_ACTIVATION_TOKEN");
}

void finish_raise(void) {
    if (!raise_wanted || !window) {
        return;
    }
    if (!raise_asked) {
        raise_asked = 1;
        if ((SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) ||
            !wayland_activate(window, raise_token)) {
            raise_arm();
        }
        SDL_RaiseWindow(window);
    }
    if (!raise_token || (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) {
        raise_spent();
        return;
    }
    if (av_gettime_relative() - raise_started_us < RAISE_GRACE_US) {
        return;
    }

    raise_spent();
}

static void place_window(void) {
    int held;

    if (window_placed) {
        return;
    }
    // Asking again can move it to another output on Wayland.
    held = hold_output();
    window_placed = 1;
    SDL_ShowWindow(window);
    drop_output(held);
}

void window_open_bare(VideoState *is) {
    int held;

    place_window();
    held = hold_output();
    SDL_SyncWindow(window);
    is_fullscreen = window_fullscreen_now();
    present_update_display_mode();

    update_screen_size();
    if (!video_adopt_window_size(is)) {
        float scale = window_points_scale();

        is->width = FFMAX((int)lrintf(default_width * scale), 1);
        is->height = FFMAX((int)lrintf(default_height * scale), 1);
    }
    drop_output(held);
}

int window_occluded(void) {
    return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_OCCLUDED);
}

char *startup_window_title(const char *fallback_path) {
    const char *base = window_title_auto;
    const PlaylistEntry *e;
    char counter[32];
    char *owned = NULL;
    char *title;

    if (window_title) {
        return av_strdup(window_title);
    }
    if (!base) {
        e = playlist_get(playlist_pos);
        owned = e ? make_default_window_title(e->display_path, e->archive_path,
                                              e->entry_name)
                  : make_default_window_title(fallback_path, NULL, NULL);
        base = owned;
    }
    if (!base) {
        return NULL;
    }
    playlist_counter(counter, sizeof(counter));
    title = av_asprintf("%s%s", base, counter);
    av_free(owned);

    return title;
}

void note_fullscreen_state(VideoState *is) {
    int fullscreen;
    int held;

    if (!window || !is || !window_placed) {
        return;
    }
    fullscreen = window_fullscreen_now();
    if (is_fullscreen == fullscreen) {
        return;
    }
    log_verbose("The compositor put the window %s fullscreen.\n",
                fullscreen ? "into" : "out of");
    held = hold_output();
    is_fullscreen = fullscreen;
    update_screen_size();
    video_adopt_window_size(is);
    is->force_refresh = 1;
    present_update_display_mode();
    present_pacing_reset();
    drop_output(held);
}

// On Windows SDL doesn't refit a fullscreen window when the display's resolution changes.
void enter_fullscreen(VideoState *is) {
    const char *driver = SDL_GetCurrentVideoDriver();
    int refit = driver && !strcmp(driver, "windows");
    int synced;
    int held;

    if (!window || (window_fullscreen_now() && !refit)) {
        return;
    }
    held = hold_output();
    SDL_SetWindowFullscreen(window, true);
    synced = SDL_SyncWindow(window);
    if (!window_fullscreen_now()) {
        log_verbose(synced ? "The compositor kept the window out of fullscreen.\n"
                           : "The window is not fullscreen yet.\n");
    }
    is_fullscreen = window_fullscreen_now();
    update_screen_size();
    video_adopt_window_size(is);
    is->force_refresh = 1;
    present_update_display_mode();
    present_pacing_reset();
    drop_output(held);
}

void window_uninit(void) {
    av_freep(&window_title_path);
    av_freep(&window_title_shown);
    av_freep(&raise_token);
}
