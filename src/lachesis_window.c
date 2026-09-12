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
#include <stdlib.h>
#include <string.h>

#include <libavutil/avstring.h>
#include <libavutil/macros.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/rational.h>
#include <libavutil/time.h>

#include <SDL3/SDL.h>

#include "lachesis_alloc.h"
#include "lachesis_aspect.h"
#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_playlist.h"
#include "lachesis_present.h"
#include "lachesis_queue.h"
#include "lachesis_renderer.h"
#include "lachesis_window.h"

#define WINDOW_INSIST_US (2000 * 1000)
#define WINDOW_INSIST_TRIES 3
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

static void window_size_for_content(int pic_width, int pic_height,
                                    AVRational sar, int rotate, int *out_w,
                                    int *out_h) {
    AVRational aspect_ratio;
    int64_t width, height;
    int64_t max_width = INT64_MAX, max_height = INT64_MAX;
    float density = window_points_scale();
    SDL_Rect display_bounds;

    if (pic_width < 1) {
        pic_width = 1;
    }
    if (pic_height < 1) {
        pic_height = 1;
    }

    aspect_ratio = sar;

    if (rotate == 90 || rotate == 270) {
        int tmp = pic_width;
        pic_width = pic_height;
        pic_height = tmp;
        if (aspect_ratio.num > 0 && aspect_ratio.den > 0) {
            aspect_ratio = av_make_q(aspect_ratio.den, aspect_ratio.num);
        }
    }

    aspect_ratio = aspect_override_sar(pic_width, pic_height, aspect_ratio);

    if (aspect_ratio.num <= 0 || aspect_ratio.den <= 0) {
        aspect_ratio = av_make_q(1, 1);
    }

    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    height = pic_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;

    if (SDL_GetDisplayBounds(window ? SDL_GetDisplayForWindow(window)
                                    : SDL_GetPrimaryDisplay(),
                             &display_bounds)) {
        max_width = (int64_t)(display_bounds.w * density * autofit_larger);
        max_height = (int64_t)(display_bounds.h * density * autofit_larger);
    }
    if (width > max_width || height > max_height) {
        height = max_height;
        width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
        if (width > max_width) {
            width = max_width;
            height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
        }
    }

    *out_w = FFMAX((int)lrintf((float)width / density), 1);
    *out_h = FFMAX((int)lrintf((float)height / density), 1);
}

static int sized_for_width;
static int sized_for_height;
static AVRational sized_for_sar;

static int window_rotate;

static int noted_rotate;

static AVRational content_sar(const Frame *vp) {
    return aspect_override_sar(vp->width, vp->height, vp->sar);
}

static int content_size_is_current(const Frame *vp) {
    AVRational sar = content_sar(vp);

    return vp->width == sized_for_width && vp->height == sized_for_height &&
        sar.num == sized_for_sar.num && sar.den == sized_for_sar.den;
}

static void note_content_size(const Frame *vp) {
    sized_for_width = vp->width;
    sized_for_height = vp->height;
    sized_for_sar = content_sar(vp);
}

static int window_content_sized;

void size_default_for_content(const Frame *vp) {
    window_content_sized = 1;
    note_content_size(vp);
    window_rotate = video_rotate;
    window_size_for_content(vp->width, vp->height, vp->sar, window_rotate,
                            &default_width, &default_height);
}

void init_default_window_size(void) {
    window_size_for_content(1920, 1080, (AVRational){1, 1}, 0, &default_width,
                            &default_height);
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

    char *title;
    if (playlist_size > 1) {
        title = av_asprintf("%s - %s [%d/%d]", program_name, display,
                            playlist_pos + 1, playlist_size);
    } else {
        title = av_asprintf("%s - %s", program_name, display);
    }
    av_free(owned);

    return title;
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
    const char *title = current_window_title(is);

    if (!window || !title ||
        (window_title_shown && !strcmp(window_title_shown, title))) {
        return;
    }
    av_free(window_title_shown);
    window_title_shown = av_strdup(title);
    SDL_SetWindowTitle(window, title);
}

#define WINDOW_SIZE_SLACK 4

static int window_is_ours_to_size(void) {
    return !(SDL_GetWindowFlags(window) &
             (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED |
              SDL_WINDOW_MINIMIZED));
}

static int window_want_w;
static int window_want_h;
static int64_t window_want_us;
static int window_want_tries;

static void forget_window_size(void) {
    window_want_w = 0;
    window_want_h = 0;
    window_want_us = 0;
    window_want_tries = 0;
}

static void want_window_size(int w, int h) {
    window_want_w = w;
    window_want_h = h;
    window_want_us = av_gettime_relative();
    window_want_tries = 0;
}

static int window_size_still_wanted(void) {
    return window_want_w > 0 && window_want_h > 0 &&
        av_gettime_relative() - window_want_us <= WINDOW_INSIST_US;
}

static void insist_window_size(void) {
    if (window_size_still_wanted() && !is_fullscreen &&
        window_is_ours_to_size()) {
        SDL_SetWindowSize(window, window_want_w, window_want_h);
    }
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

static int window_ever_windowed;

static int recenter_wanted(void) {
    return window_recenter || !window_ever_windowed;
}

static void apply_window_geometry(int w, int h, int recenter) {
    window_ever_windowed = 1;
    want_window_size(w, h);
    SDL_SetWindowSize(window, w, h);
    if (!recenter) {
        return;
    }
    if (!SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED)) {
        SDL_ClearError();
    }
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
        raise_arm();
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

void video_follow_content_size(VideoState *is) {
    int w, h;
    Frame *vp;

    if (!is->video_st) {
        return;
    }

    vp = frame_queue_peek_last(&is->pictq);
    if (video_rotate == noted_rotate && content_size_is_current(vp)) {
        return;
    }
    noted_rotate = window_rotate = video_rotate;
    note_content_size(vp);

    if (window_content_sized && !window_resize) {
        return;
    }
    window_content_sized = 1;

    window_size_for_content(vp->width, vp->height, vp->sar, window_rotate, &w,
                            &h);

    if (w != default_width || h != default_height) {
        default_width = w;
        default_height = h;
        if (!is_fullscreen) {
            int held = hold_output();

            apply_window_geometry(default_width, default_height,
                                  recenter_wanted());
            SDL_SyncWindow(window);
            update_screen_size();
            video_adopt_window_size(is);
            drop_output(held);
        }
    }
}

void note_window_resized(VideoState *is, int w, int h) {
    if (!window || !window_placed || !window_want_w) {
        return;
    }
    if (is_fullscreen || !window_is_ours_to_size() ||
        !window_size_still_wanted()) {
        forget_window_size();
        return;
    }
    if (abs(w - window_want_w) <= WINDOW_SIZE_SLACK &&
        abs(h - window_want_h) <= WINDOW_SIZE_SLACK) {
        return;
    }
    if (window_want_tries >= WINDOW_INSIST_TRIES) {
        forget_window_size();
        return;
    }
    window_want_tries++;
    insist_window_size();
    if (is) {
        update_screen_size();
        video_adopt_window_size(is);
    }
}

static void place_window(void) {
    int held;

    if (window_placed) {
        return;
    }
    held = hold_output();
    window_placed = 1;
    window_rotate = noted_rotate = video_rotate;
    SDL_SetWindowFullscreen(window, is_fullscreen);
    if (!is_fullscreen) {
        apply_window_geometry(default_width, default_height, recenter_wanted());
    }
    SDL_ShowWindow(window);
    drop_output(held);
}

void window_open_bare(VideoState *is) {
    int held;

    place_window();
    held = hold_output();
    SDL_SyncWindow(window);
    present_update_display_mode();

    update_screen_size();
    if (!video_adopt_window_size(is)) {
        float scale = window_points_scale();

        is->width = FFMAX((int)lrintf(default_width * scale), 1);
        is->height = FFMAX((int)lrintf(default_height * scale), 1);
    }
    drop_output(held);
}

int video_open(VideoState *is) {
    if (!window_placed) {
        if (is->video_st) {
            size_default_for_content(frame_queue_peek_last(&is->pictq));
        }
    } else {
        video_follow_content_size(is);
    }
    window_open_bare(is);

    return 0;
}

int window_occluded(void) {
    return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_OCCLUDED);
}

char *startup_window_title(const char *fallback_path) {
    const PlaylistEntry *e;

    if (window_title) {
        return av_strdup(window_title);
    }
    if (window_title_auto) {
        return av_strdup(window_title_auto);
    }

    e = playlist_get(playlist_pos);
    if (e) {
        return make_default_window_title(e->display_path, e->archive_path,
                                         e->entry_name);
    }

    return make_default_window_title(fallback_path, NULL, NULL);
}

void note_fullscreen_state(VideoState *is) {
    int fullscreen;
    int held;

    if (!window || !is || !window_placed) {
        return;
    }
    fullscreen = !!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
    if (is_fullscreen == fullscreen) {
        return;
    }
    log_verbose("The compositor put the window %s fullscreen.\n",
                fullscreen ? "into" : "out of");
    held = hold_output();
    is_fullscreen = fullscreen;
    if (!is_fullscreen) {
        window_ever_windowed = 1;
    }
    if (!is_fullscreen && is->video_st && is->pictq.rindex_shown &&
        (!window_content_sized || window_resize)) {
        size_default_for_content(frame_queue_peek_last(&is->pictq));
    }
    update_screen_size();
    video_adopt_window_size(is);
    is->force_refresh = 1;
    present_update_display_mode();
    present_pacing_reset();
    drop_output(held);
}

void toggle_fullscreen(VideoState *is) {
    int held;

    if (!window) {
        return;
    }
    held = hold_output();
    is_fullscreen = !is_fullscreen;
    SDL_SetWindowFullscreen(window, is_fullscreen);
    if (!is_fullscreen) {
        apply_window_geometry(default_width, default_height, recenter_wanted());
    }
    SDL_SyncWindow(window);
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
