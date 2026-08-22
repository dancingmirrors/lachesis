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

#include <libavformat/avformat.h>
#include <libavutil/common.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
#include <libavutil/time.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include "lachesis_delete.h"
#include "lachesis_equalizer.h"
#include "lachesis_internal.h"
#include "lachesis_keys.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_osd.h"
#include "lachesis_present.h"
#include "lachesis_renderer.h"
#include "lachesis_screenshot.h"

#define SDL_VOLUME_STEP (10.0)
#define SEEK_MIN_STEP (0.001)
#define SEEK_EXACT_STEP (1.0)

static int sbs360_drag = 0;
static int sbs360_drag_last_x = 0;
static int sbs360_drag_last_y = 0;
static int pan_drag = 0;
static int pan_drag_last_x = 0;
static int pan_drag_last_y = 0;
static int delete_confirm_pending = 0;
static int delete_paused_by_prompt = 0;
static int delete_prearm_paused = 0;
static int delete_advance_deferred = 0;

static int demuxer_ready(const VideoState *is) {
    return is->ic != NULL;
}

static int seeking_by_bytes(VideoState *is) {
    return SDL_GetAtomicInt(&is->seek_by_bytes) > 0;
}

static int refuse_without_video(VideoState *is) {
    if (is->video_st) {
        return 0;
    }
    osd_show_message("No video track");
    is->force_refresh = 1;

    return 1;
}

static int sbs360_active(VideoState *is) {
    return view360_enabled() && !refuse_without_video(is);
}

static void seek_chapter(VideoState *is, int incr) {
    int64_t pos, target;
    int i, cur = -1;
    double now;

    if (!demuxer_ready(is) || !is->ic->nb_chapters) {
        return;
    }

    now = effective_playhead(is);
    if (isnan(now)) {
        return;
    }
    pos = (int64_t)(now * AV_TIME_BASE);
    for (i = 0; i < (int)is->ic->nb_chapters; i++) {
        AVChapter *ch = is->ic->chapters[i];

        if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
            break;
        }
        cur = i;
    }

    i = cur + incr;
    if (i >= (int)is->ic->nb_chapters) {
        return;
    }
    if (i < 0) {
        if (incr > 0) {
            return;
        }
        target = (int64_t)(playhead_origin(is) * AV_TIME_BASE);
    } else {
        target = av_rescale_q(is->ic->chapters[i]->start,
                              is->ic->chapters[i]->time_base, AV_TIME_BASE_Q);
    }

    if (incr > 0 ? target <= pos : target >= pos) {
        return;
    }
    stream_seek(is, target, 0, 0);
}

static void seek_relative_exact(VideoState *is, double incr, int exact) {
    double pos;

    if (!demuxer_ready(is)) {
        return;
    }
    osd_show_seek();
    if (seeking_by_bytes(is)) {
        pos = -1;
        if (pos < 0 && is->video_stream >= 0) {
            pos = frame_queue_last_pos(&is->pictq);
        }
        if (pos < 0 && is->audio_stream >= 0) {
            pos = frame_queue_last_pos(&is->sampq);
        }
        if (pos < 0) {
            pos = avio_tell(is->ic->pb);
        }
        if (is->ic->bit_rate) {
            incr *= is->ic->bit_rate / 8.0;
        } else {
            incr *= 180000.0;
        }
        pos += incr;
        stream_seek(is, pos, incr, 1);
    } else {
        double target;

        pos = effective_playhead(is);
        if (isnan(pos)) {
            pos = playhead_origin(is);
        }
        target = pos + incr;
        if (incr < 0.0) {
            double clamped = playhead_clamp(is, target);

            if ((clamped - pos) * incr >= 0.0 && fabs(clamped - pos) <= fabs(incr)) {
                target = clamped;
            }
        }
        if (fabs(target - pos) < SEEK_MIN_STEP) {
            return;
        }
        if (exact) {
            stream_seek_exact(is, (int64_t)(target * AV_TIME_BASE));
        } else {
            stream_seek(is, (int64_t)(target * AV_TIME_BASE),
                        (int64_t)((target - pos) * AV_TIME_BASE), 0);
        }
    }
}

static void seek_relative(VideoState *is, double incr) {
    seek_relative_exact(is, incr, 0);
}

/* We "roll" as a 90° rotation we can keep the controls sane. */
static void sbs360_view_move(VideoState *cur_stream, float sx, float sy) {
    int quadrant = (((int)lroundf(sbs360_roll / 90.0f)) % 4 + 4) % 4;
    float dyaw, dpitch;

    switch (quadrant) {
    case 1:
        dyaw = -sy;
        dpitch = sx;
        break;
    case 2:
        dyaw = -sx;
        dpitch = -sy;
        break;
    case 3:
        dyaw = sy;
        dpitch = -sx;
        break;
    default:
        dyaw = sx;
        dpitch = sy;
        break;
    }

    sbs360_yaw += dyaw;
    sbs360_pitch = av_clipf(sbs360_pitch + dpitch, -90.0f, 90.0f);
    cur_stream->force_refresh = 1;
}

static void pan_display(VideoState *cur_stream, SDL_Scancode sc) {
    float step_x = FFMAX(cur_stream->width, 1) * 0.1f;
    float step_y = FFMAX(cur_stream->height, 1) * 0.1f;
    if (sc == SDL_SCANCODE_LEFT) {
        display_pan_x += step_x;
    } else if (sc == SDL_SCANCODE_RIGHT) {
        display_pan_x -= step_x;
    } else if (sc == SDL_SCANCODE_UP) {
        display_pan_y += step_y;
    } else if (sc == SDL_SCANCODE_DOWN) {
        display_pan_y -= step_y;
    }

    cur_stream->force_refresh = 1;
}

static void playlist_advance_or_exit(VideoState **pis) {
    playlist_nav_dir = 1;
    if (playlist_pos + 1 < playlist_size) {
        playlist_switch(pis, playlist_pos + 1);
    } else {
        do_exit(*pis);
    }
}

void event_loop(VideoState **pis) {
    VideoState *cur_stream;
    SDL_Event event;

    double incr, frac, length;

    for (;;) {
        double x;
        cur_stream = *pis;
        refresh_loop_wait_event(cur_stream, &event);
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            float density = window_pixel_density();
            if (density != 1.0f) {
                if (event.type == SDL_EVENT_MOUSE_MOTION) {
                    event.motion.x *= density;
                    event.motion.y *= density;
                } else {
                    event.button.x *= density;
                    event.button.y *= density;
                }
            }
            break;
        }
        default:
            break;
        }
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
#ifdef __APPLE__
            if (event.key.mod & SDL_KMOD_GUI) {
                break;
            }
#endif
            if (event.key.mod & SDL_KMOD_CTRL) {
                switch (event.key.key) {
                case SDLK_C:
                    do_exit(cur_stream);
                case SDLK_S:
                    take_screenshot(cur_stream, 1);
                    break;
                default:
                    break;
                }
                break;
            }
            if (delete_confirm_pending) {
                delete_confirm_pending = 0;
                osd_hide_delete_prompt();
                int was_paused = delete_prearm_paused;
                int we_paused = delete_paused_by_prompt;
                int advance_deferred = delete_advance_deferred;
                delete_prearm_paused = 0;
                delete_paused_by_prompt = 0;
                delete_advance_deferred = 0;
                if (event.key.key == SDLK_Y) {
                    if (!delete_current_file(pis, was_paused)) {
                        if (we_paused && (*pis)->paused) {
                            toggle_pause(*pis);
                        }
                        if (advance_deferred) {
                            playlist_advance_or_exit(pis);
                        }
                    }
                } else if (advance_deferred) {
                    if (we_paused && (*pis)->paused) {
                        toggle_pause(*pis);
                    }
                    playlist_advance_or_exit(pis);
                } else {
                    if (we_paused && (*pis)->paused) {
                        toggle_pause(*pis);
                    }
                    osd_show_message("Delete cancelled");
                    (*pis)->force_refresh = 1;
                }
                break;
            }
            if (allow_delete && event.key.key == SDLK_DELETE &&
                (event.key.mod & SDL_KMOD_SHIFT)) {
                const char *reason = delete_current_reason(cur_stream);
                if (reason) {
                    osd_show_message("Can't delete: %s", reason);
                } else {
                    delete_confirm_pending = 1;
                    delete_prearm_paused = cur_stream->paused;
                    delete_paused_by_prompt = 0;
                    cur_stream->step_from_play = 0;
                    if (!cur_stream->paused) {
                        toggle_pause(cur_stream);
                        delete_paused_by_prompt = 1;
                    }
                    osd_show_delete_prompt(delete_current_name(cur_stream));
                }
                cur_stream->force_refresh = 1;
                break;
            }
            if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q) {
                do_exit(cur_stream);
            }
            if ((event.key.mod & SDL_KMOD_ALT) && (event.key.mod & SDL_KMOD_SHIFT)) {
                SDL_Scancode sc = event.key.scancode;
                if ((sc == SDL_SCANCODE_EQUALS || sc == SDL_SCANCODE_MINUS ||
                     sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT ||
                     sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN ||
                     sc == SDL_SCANCODE_BACKSPACE) &&
                    refuse_without_video(cur_stream)) {
                    break;
                }
                if (sc == SDL_SCANCODE_EQUALS) {
                    display_scale = FFMIN(display_scale + 0.1f, 4.0f);
                    cur_stream->force_refresh = 1;
                    break;
                } else if (sc == SDL_SCANCODE_MINUS) {
                    display_scale = FFMAX(display_scale - 0.1f, 0.1f);
                    cur_stream->force_refresh = 1;
                    break;
                } else if (sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT ||
                           sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN) {
                    pan_display(cur_stream, sc);
                    break;
                } else if (sc == SDL_SCANCODE_BACKSPACE) {
                    display_scale = 1.0f;
                    display_pan_x = 0.0f;
                    display_pan_y = 0.0f;
                    cur_stream->force_refresh = 1;
                    break;
                }
            }
            switch (event.key.key) {
            case SDLK_F:
                toggle_fullscreen(cur_stream);
                cur_stream->force_refresh = 1;
                break;
            case SDLK_P:
                if (event.key.mod & SDL_KMOD_SHIFT) {
                    if (cur_stream->ic && cur_stream->ic->duration != AV_NOPTS_VALUE) {
                        osd_show_seek();
                    } else {
                        osd_show_status();
                    }
                    cur_stream->force_refresh = 1;
                    break;
                }
                __attribute__((fallthrough));
            case SDLK_SPACE:
                toggle_pause(cur_stream);
                osd_show_seek();
                cur_stream->force_refresh = 1;
                break;
            case SDLK_M:
                toggle_mute(cur_stream);
                cur_stream->force_refresh = 1;
                break;
            case SDLK_KP_MULTIPLY:
            case SDLK_0:
                update_volume(cur_stream, 1, SDL_VOLUME_STEP);
                osd_show_status();
                cur_stream->force_refresh = 1;
                break;
            case SDLK_KP_DIVIDE:
            case SDLK_9:
                update_volume(cur_stream, -1, SDL_VOLUME_STEP);
                osd_show_status();
                cur_stream->force_refresh = 1;
                break;
            case SDLK_N:
                frame_step(cur_stream);
                cur_stream->force_refresh = 1;
                break;
            case SDLK_S:
                take_screenshot(cur_stream, 0);
                break;
            case SDLK_PERIOD:
            case SDLK_GREATER:
                playlist_nav_dir = 1;
                if (playlist_pos + 1 < playlist_size) {
                    playlist_switch(pis, playlist_pos + 1);
                } else if (!keep_open) {
                    do_exit(cur_stream);
                }
                break;
            case SDLK_COMMA:
            case SDLK_LESS:
                playlist_nav_dir = -1;
                if (playlist_pos > 0) {
                    playlist_switch(pis, playlist_pos - 1);
                }
                break;
            case SDLK_A:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                break;
            case SDLK_V:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                break;
            case SDLK_C:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_T:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_D: {
                if (refuse_without_video(cur_stream)) {
                    break;
                }
                deinterlace = !deinterlace;
                osd_show_message("Deinterlace: %s", deinterlace ? "on" : "off");
                cur_stream->force_refresh = 1;
                break;
            }
            case SDLK_B:
                if (refuse_without_video(cur_stream)) {
                    break;
                }
                frame_interpolation = !frame_interpolation;
                osd_show_message("Interpolation: %s",
                                 frame_interpolation ? "on" : "off");
                cur_stream->force_refresh = 1;
                break;
            case SDLK_R:
            case SDLK_KP_9:
                if (refuse_without_video(cur_stream)) {
                    break;
                }
                video_rotate = (video_rotate + 90) % 360;
                osd_show_message("Rotate: %d\xc2\xb0", video_rotate);
                cur_stream->force_refresh = 1;
                break;
            case SDLK_W:
                if (refuse_without_video(cur_stream)) {
                    break;
                }
                if (nb_vfilters > 0) {
                    cur_stream->vfilter_idx = (cur_stream->vfilter_idx + 1) % nb_vfilters;
                    osd_show_message("Video filter: %s",
                                     vfilters_list[cur_stream->vfilter_idx]);
                    cur_stream->force_refresh = 1;
                }
                break;
            case SDLK_PAGEUP:
                if (!demuxer_ready(cur_stream) ||
                    cur_stream->ic->nb_chapters <= 1) {
                    incr = 600.0;
                    goto do_seek;
                }
                seek_chapter(cur_stream, 1);
                break;
            case SDLK_PAGEDOWN:
                if (!demuxer_ready(cur_stream) ||
                    cur_stream->ic->nb_chapters <= 1) {
                    incr = -600.0;
                    goto do_seek;
                }
                seek_chapter(cur_stream, -1);
                break;
            case SDLK_LEFT:
                if (event.key.mod & SDL_KMOD_SHIFT) {
                    seek_relative_exact(cur_stream, -SEEK_EXACT_STEP, 1);
                    break;
                }
                incr = seek_interval ? -(double)seek_interval : -5.0;
                goto do_seek;
            case SDLK_RIGHT:
                if (event.key.mod & SDL_KMOD_SHIFT) {
                    seek_relative_exact(cur_stream, SEEK_EXACT_STEP, 1);
                    break;
                }
                incr = seek_interval ? (double)seek_interval : 5.0;
                goto do_seek;
            case SDLK_UP:
                incr = 60.0;
                goto do_seek;
            case SDLK_DOWN:
                incr = -60.0;
            do_seek:
                seek_relative(cur_stream, incr);
                break;
            case SDLK_1:
            case SDLK_2:
            case SDLK_3:
            case SDLK_4:
            case SDLK_5:
            case SDLK_6:
            case SDLK_7:
            case SDLK_8:
                if (refuse_without_video(cur_stream)) {
                    break;
                }
                switch (event.key.key) {
                case SDLK_1:
                    equalizer_adjust(EQ_BRIGHTNESS, -1);
                    break;
                case SDLK_2:
                    equalizer_adjust(EQ_BRIGHTNESS, 1);
                    break;
                case SDLK_3:
                    equalizer_adjust(EQ_GAMMA, -1);
                    break;
                case SDLK_4:
                    equalizer_adjust(EQ_GAMMA, 1);
                    break;
                case SDLK_5:
                    equalizer_adjust(EQ_CONTRAST, -1);
                    break;
                case SDLK_6:
                    equalizer_adjust(EQ_CONTRAST, 1);
                    break;
                case SDLK_7:
                    equalizer_adjust(EQ_SATURATION, -1);
                    break;
                case SDLK_8:
                    equalizer_adjust(EQ_SATURATION, 1);
                    break;
                default:
                    break;
                }
                cur_stream->force_refresh = 1;
                break;
            case SDLK_I:
                if (event.key.mod & SDL_KMOD_SHIFT) {
                    osd_cycle_info();
                    cur_stream->force_refresh = 1;
                    break;
                }
                if (sbs360_active(cur_stream)) {
                    sbs360_view_move(cur_stream, 0.0f, 5.0f);
                }
                break;
            case SDLK_K:
                if (sbs360_active(cur_stream)) {
                    sbs360_view_move(cur_stream, 0.0f, -5.0f);
                }
                break;
            case SDLK_J:
                if (sbs360_active(cur_stream)) {
                    sbs360_view_move(cur_stream, -5.0f, 0.0f);
                }
                break;
            case SDLK_L:
                if (event.key.mod & SDL_KMOD_SHIFT) {
                    ab_loop_toggle(cur_stream);
                    cur_stream->force_refresh = 1;
                } else if (sbs360_active(cur_stream)) {
                    sbs360_view_move(cur_stream, 5.0f, 0.0f);
                }
                break;
            case SDLK_U:
                if (sbs360_active(cur_stream)) {
                    sbs360_roll -= 90.0f;
                    if (sbs360_roll <= -180.0f) {
                        sbs360_roll += 360.0f;
                    }
                    osd_show_message("Roll: %d\xc2\xb0", (int)sbs360_roll);
                    cur_stream->force_refresh = 1;
                }
                break;
            case SDLK_O:
                if (sbs360_active(cur_stream)) {
                    sbs360_roll += 90.0f;
                    if (sbs360_roll > 180.0f) {
                        sbs360_roll -= 360.0f;
                    }
                    osd_show_message("Roll: %d\xc2\xb0", (int)sbs360_roll);
                    cur_stream->force_refresh = 1;
                }
                break;
            case SDLK_LEFTBRACKET:
                set_playback_speed(cur_stream, playback_speed - PLAYBACK_SPEED_STEP);
                cur_stream->force_refresh = 1;
                break;
            case SDLK_RIGHTBRACKET:
                set_playback_speed(cur_stream, playback_speed + PLAYBACK_SPEED_STEP);
                cur_stream->force_refresh = 1;
                break;
            case SDLK_EQUALS:
            case SDLK_KP_PLUS:
                if (sbs360_active(cur_stream)) {
                    sbs360_hfov =
                        view360_clamp_hfov(view360_projection, sbs360_hfov - 10.0f);
                    cur_stream->force_refresh = 1;
                }
                break;
            case SDLK_MINUS:
            case SDLK_KP_MINUS:
                if (sbs360_active(cur_stream)) {
                    sbs360_hfov =
                        view360_clamp_hfov(view360_projection, sbs360_hfov + 10.0f);
                    cur_stream->force_refresh = 1;
                }
                break;
            case SDLK_KP_1: {
                enum SupersampleLevel next;

                if (refuse_without_video(cur_stream)) {
                    break;
                }
                next = supersample_level_next(supersample_level);
                if (renderer && renderer_set_supersample(renderer, next) < 0) {
                    osd_show_message("Supersample: unavailable");
                    break;
                }
                supersample_level = next;
                osd_show_message("Supersample: %s",
                                 supersample_level_name(supersample_level));
                cur_stream->force_refresh = 1;
                break;
            }
            case SDLK_KP_3:
                if (refuse_without_video(cur_stream)) {
                    break;
                }
                view360_mode_next(&view360_layout, &view360_projection);
                osd_show_message(
                    "360: %s",
                    view360_mode_name(view360_layout, view360_projection));
                if (view360_enabled()) {
                    sbs360_reset_view();
                }
                if (renderer) {
                    renderer_enable_360(renderer, view360_layout,
                                        view360_projection);
                }
                cur_stream->force_refresh = 1;
                break;
            default:
                break;
            }
            break;
        case SDL_EVENT_KEY_UP:
            if (event.key.key == SDLK_N) {
                cur_stream->step_key_held = 0;
                if (cur_stream->step_from_play && cur_stream->paused) {
                    toggle_pause(cur_stream);
                }
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT) {
                sbs360_drag = 0;
                pan_drag = 0;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (view360_enabled() && event.button.button == SDL_BUTTON_LEFT &&
                !(SDL_GetModState() & SDL_KMOD_CTRL)) {
                sbs360_drag = 1;
                sbs360_drag_last_x = event.button.x;
                sbs360_drag_last_y = event.button.y;
                /* Deliberately no break. */
            }
            if ((SDL_GetModState() & SDL_KMOD_CTRL) &&
                event.button.button == SDL_BUTTON_LEFT) {
                pan_drag = 1;
                pan_drag_last_x = event.button.x;
                pan_drag_last_y = event.button.y;
                break;
            }
            if (event.button.button == SDL_BUTTON_LEFT) {
                static int64_t last_mouse_left_click = 0;
                if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                    sbs360_drag = 0;
                    toggle_fullscreen(cur_stream);
                    cur_stream->force_refresh = 1;
                    last_mouse_left_click = 0;
                } else {
                    last_mouse_left_click = av_gettime_relative();
                }
            }
            __attribute__((fallthrough));
        case SDL_EVENT_MOUSE_MOTION:
            if (cursor_hidden) {
                SDL_ShowCursor();
                cursor_hidden = 0;
            }
            cursor_last_shown = av_gettime_relative();
            if (view360_enabled() && sbs360_drag && event.type == SDL_EVENT_MOUSE_MOTION) {
                int dx = event.motion.x - sbs360_drag_last_x;
                int dy = event.motion.y - sbs360_drag_last_y;
                sbs360_drag_last_x = event.motion.x;
                sbs360_drag_last_y = event.motion.y;
                if (dx || dy) {
                    float deg_per_px = sbs360_hfov / (float)(cur_stream->width ? cur_stream->width : 1) * 1.0f;
                    sbs360_view_move(cur_stream, dx * deg_per_px, -dy * deg_per_px);
                }
                break;
            }
            if (pan_drag && event.type == SDL_EVENT_MOUSE_MOTION) {
                int dx = event.motion.x - pan_drag_last_x;
                int dy = event.motion.y - pan_drag_last_y;
                pan_drag_last_x = event.motion.x;
                pan_drag_last_y = event.motion.y;
                if (dx || dy) {
                    display_pan_x += dx;
                    display_pan_y += dy;
                    cur_stream->force_refresh = 1;
                }
                break;
            }
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (event.button.button != SDL_BUTTON_RIGHT) {
                    break;
                }
                x = (double)event.button.x;
            } else {
                if (!(event.motion.state & SDL_BUTTON_RMASK)) {
                    break;
                }
                x = (double)event.motion.x;
            }
            if (!demuxer_ready(cur_stream) || cur_stream->width <= 0) {
                break;
            }
            frac = av_clipd(x / cur_stream->width, 0.0, 1.0);
            length = playhead_length(cur_stream);
            if (seeking_by_bytes(cur_stream) || length <= 0.0) {
                int64_t size = avio_size(cur_stream->ic->pb);
                if (size <= 0) {
                    break;
                }
                stream_seek(cur_stream, (int64_t)(size * frac), 0, 1);
            } else {
                double ts = playhead_origin(cur_stream) + frac * length;

                stream_seek(cur_stream, (int64_t)(ts * AV_TIME_BASE), 0, 0);
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL: {
            /* Try to account for the precise delta of trackpads. */
            static float wheel_bank;
            float dy = event.wheel.y;
            int notches;

            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                dy = -dy;
            }
            if ((dy < 0.0f) != (wheel_bank < 0.0f)) {
                wheel_bank = 0.0f;
            }
            wheel_bank += dy;

            notches = (int)wheel_bank;
            if (!notches) {
                break;
            }
            wheel_bank -= (float)notches;

            incr = seek_interval ? (double)seek_interval : 5.0;
            seek_relative(cur_stream, incr * notches);
            break;
        }
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            if (note_window_pixel_size(event.window.data1, event.window.data2)) {
                video_adopt_window_size(cur_stream);
                if (renderer) {
                    renderer_resize(renderer, screen_width, screen_height);
                }
                present_reset();
            }
            __attribute__((fallthrough));
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            cur_stream->force_refresh = 1;
            break;
        case SDL_EVENT_WINDOW_ICCPROF_CHANGED:
        case SDL_EVENT_WINDOW_HDR_STATE_CHANGED:
            if (renderer_refresh_display_info(renderer, window)) {
                cur_stream->force_refresh = 1;
            }
            break;
        case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
            present_update_display_mode();
            break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            present_update_display_mode();
            present_reset();
            renderer_refresh_display_info(renderer, window);
            cur_stream->force_refresh = 1;
            break;
        case SDL_EVENT_QUIT:
            do_exit(*pis);
        case FF_QUIT_EVENT: {
            VideoState *old = event.user.data1;
            if (old && old != *pis) {
                break;
            }
            if (fatal_error_pending) {
                exit_status = 1;
                do_exit(*pis);
            }
            if (delete_confirm_pending) {
                delete_advance_deferred = 1;
                break;
            }
            if (event.user.code == FF_QUIT_REASON_ERROR && playlist_nav_dir < 0 &&
                playlist_pos > 0) {
                /* Also handle "previous" entries in case we encounter a broken file. */
                playlist_switch(pis, playlist_pos - 1);
                break;
            }
            playlist_advance_or_exit(pis);
            break;
        }
        case FF_RENDER_FAULT_EVENT:
            render_fault_fallback(pis);
            break;
        case FF_SCREENSHOT_EVENT:
            screenshot_report(&event);
            break;
        default:
            break;
        }
    }
}
