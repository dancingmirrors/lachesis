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
#include "lachesis_internal.h"
#include "lachesis_keys.h"
#include "lachesis_options.h"
#include "lachesis_osd.h"
#include "lachesis_renderer.h"
#include "lachesis_screenshot.h"

#define SDL_VOLUME_STEP (0.75)

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

static void seek_chapter(VideoState *is, int incr) {
    int64_t pos = get_master_clock(is) * AV_TIME_BASE;
    int i;

    if (!is->ic->nb_chapters) {
        return;
    }

    for (i = 0; i < (int)is->ic->nb_chapters; i++) {
        AVChapter *ch = is->ic->chapters[i];
        if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
            i--;
            break;
        }
    }

    i += incr;
    i = FFMAX(i, 0);
    if (i >= (int)is->ic->nb_chapters) {
        return;
    }

    stream_seek(is, av_rescale_q(is->ic->chapters[i]->start, is->ic->chapters[i]->time_base, AV_TIME_BASE_Q), 0, 0);
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
    double incr, pos, frac;

    for (;;) {
        double x;
        cur_stream = *pis;
        refresh_loop_wait_event(cur_stream, &event);
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
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
                    if (!cur_stream->paused) {
                        toggle_pause(cur_stream);
                        delete_paused_by_prompt = 1;
                    }
                    osd_show_delete_prompt(delete_current_name(cur_stream));
                }
                cur_stream->force_refresh = 1;
                break;
            }
            if (exit_on_keydown || event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q) {
                do_exit(cur_stream);
                break;
            }
            if (!cur_stream->width) {
                continue;
            }
            if ((event.key.mod & SDL_KMOD_ALT) && (event.key.mod & SDL_KMOD_SHIFT)) {
                SDL_Scancode sc = event.key.scancode;
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
                [[fallthrough]];
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
                step_to_next_frame(cur_stream);
                break;
            case SDLK_S:
                take_screenshot(cur_stream, (event.key.mod & SDL_KMOD_CTRL) != 0);
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
            case SDLK_D:
                if (!vk_renderer) {
                    osd_show_message("Deinterlacing requires the Vulkan renderer");
                } else {
                    static const char *const deint_names[DEINTERLACE_MODE_COUNT] = {
                        "off",
                        "yadif",
                        "bob",
                    };
                    deinterlace = (deinterlace + 1) % DEINTERLACE_MODE_COUNT;
                    osd_show_message("Deinterlace: %s", deint_names[deinterlace]);
                }
                cur_stream->force_refresh = 1;
                break;
            case SDLK_R:
            case SDLK_KP_9:
                video_rotate = (video_rotate + 90) % 360;
                osd_show_message("Rotate: %d\xc2\xb0", video_rotate);
                cur_stream->force_refresh = 1;
                break;
            case SDLK_W:
                if (nb_vfilters > 0) {
                    cur_stream->vfilter_idx = (cur_stream->vfilter_idx + 1) % nb_vfilters;
                }
                break;
            case SDLK_PAGEUP:
                if (cur_stream->ic->nb_chapters <= 1) {
                    incr = 600.0;
                    goto do_seek;
                }
                seek_chapter(cur_stream, 1);
                break;
            case SDLK_PAGEDOWN:
                if (cur_stream->ic->nb_chapters <= 1) {
                    incr = -600.0;
                    goto do_seek;
                }
                seek_chapter(cur_stream, -1);
                break;
            case SDLK_LEFT:
                incr = seek_interval ? -seek_interval : -5.0;
                goto do_seek;
            case SDLK_RIGHT:
                incr = seek_interval ? seek_interval : 5.0;
                goto do_seek;
            case SDLK_UP:
                incr = 60.0;
                goto do_seek;
            case SDLK_DOWN:
                incr = -60.0;
            do_seek:
                osd_show_seek();
                if (seek_by_bytes) {
                    pos = -1;
                    if (pos < 0 && cur_stream->video_stream >= 0) {
                        pos = frame_queue_last_pos(&cur_stream->pictq);
                    }
                    if (pos < 0 && cur_stream->audio_stream >= 0) {
                        pos = frame_queue_last_pos(&cur_stream->sampq);
                    }
                    if (pos < 0) {
                        pos = avio_tell(cur_stream->ic->pb);
                    }
                    if (cur_stream->ic->bit_rate) {
                        incr *= cur_stream->ic->bit_rate / 8.0;
                    } else {
                        incr *= 180000.0;
                    }
                    pos += incr;
                    stream_seek(cur_stream, pos, incr, 1);
                } else {
                    pos = get_master_clock(cur_stream);
                    if (isnan(pos)) {
                        pos = (double)cur_stream->seek_pos / AV_TIME_BASE;
                    }
                    pos += incr;
                    if (cur_stream->ic->start_time != AV_NOPTS_VALUE && pos < cur_stream->ic->start_time / (double)AV_TIME_BASE) {
                        pos = cur_stream->ic->start_time / (double)AV_TIME_BASE;
                    }
                    stream_seek(cur_stream, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
                }
                break;
            case SDLK_1:
                osd_toggle_info_page(1);
                cur_stream->force_refresh = 1;
                break;
            case SDLK_2:
                osd_toggle_info_page(2);
                cur_stream->force_refresh = 1;
                break;
            case SDLK_I:
                if (event.key.mod & SDL_KMOD_SHIFT) {
                    osd_toggle_info_page(1);
                    cur_stream->force_refresh = 1;
                    break;
                }
                if (enable_360sbs) {
                    sbs360_pitch = FFMIN(sbs360_pitch + 5.0f, 90.0f);
                    cur_stream->force_refresh = 1;
                }
                break;
            case SDLK_K:
                if (enable_360sbs) {
                    sbs360_pitch = FFMAX(sbs360_pitch - 5.0f, -90.0f);
                    cur_stream->force_refresh = 1;
                }
                break;
            case SDLK_J:
                if (enable_360sbs) {
                    sbs360_yaw -= 5.0f;
                    cur_stream->force_refresh = 1;
                }
                break;
            case SDLK_L:
                if (event.key.mod & SDL_KMOD_SHIFT) {
                    ab_loop_toggle(cur_stream);
                    cur_stream->force_refresh = 1;
                } else if (enable_360sbs) {
                    sbs360_yaw += 5.0f;
                    cur_stream->force_refresh = 1;
                }
                break;
            case SDLK_LEFTBRACKET:
                set_playback_speed(playback_speed - PLAYBACK_SPEED_STEP);
                cur_stream->force_refresh = 1;
                break;
            case SDLK_RIGHTBRACKET:
                set_playback_speed(playback_speed + PLAYBACK_SPEED_STEP);
                cur_stream->force_refresh = 1;
                break;
            case SDLK_EQUALS:
            case SDLK_KP_PLUS:
                if (enable_360sbs) {
                    sbs360_hfov = FFMAX(sbs360_hfov - 10.0f, 10.0f);
                    cur_stream->force_refresh = 1;
                }
                break;
            case SDLK_MINUS:
            case SDLK_KP_MINUS:
                if (enable_360sbs) {
                    sbs360_hfov = FFMIN(sbs360_hfov + 10.0f, 180.0f);
                    cur_stream->force_refresh = 1;
                }
                break;
            case SDLK_KP_3:
                if (vk_renderer) {
                    if (!enable_360sbs) {
                        enable_360sbs = 1;
                        view360_layout = VK_360_LAYOUT_FULL;
                        osd_show_message("360: side-by-side");
                    } else if (view360_layout == VK_360_LAYOUT_FULL) {
                        view360_layout = VK_360_LAYOUT_TB;
                        osd_show_message("360: top-bottom");
                    } else {
                        enable_360sbs = 0;
                        osd_show_message("360: off");
                    }
                    if (enable_360sbs) {
                        sbs360_reset_view();
                    }
                    vk_renderer_enable_360(vk_renderer, enable_360sbs ? view360_layout : VK_360_LAYOUT_OFF);
                    cur_stream->force_refresh = 1;
                }
                break;
            default:
                break;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (enable_360sbs && event.button.button == SDL_BUTTON_LEFT) {
                sbs360_drag = 0;
            }
            if (event.button.button == SDL_BUTTON_LEFT) {
                pan_drag = 0;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (exit_on_mousedown) {
                do_exit(cur_stream);
                break;
            }
            if (enable_360sbs && event.button.button == SDL_BUTTON_LEFT) {
                sbs360_drag = 1;
                sbs360_drag_last_x = event.button.x;
                sbs360_drag_last_y = event.button.y;
                break;
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
                    toggle_fullscreen(cur_stream);
                    cur_stream->force_refresh = 1;
                    last_mouse_left_click = 0;
                } else {
                    last_mouse_left_click = av_gettime_relative();
                }
            }
            [[fallthrough]];
        case SDL_EVENT_MOUSE_MOTION:
            if (cursor_hidden) {
                SDL_ShowCursor();
                cursor_hidden = 0;
            }
            cursor_last_shown = av_gettime_relative();
            if (enable_360sbs && sbs360_drag && event.type == SDL_EVENT_MOUSE_MOTION) {
                int dx = event.motion.x - sbs360_drag_last_x;
                int dy = event.motion.y - sbs360_drag_last_y;
                sbs360_drag_last_x = event.motion.x;
                sbs360_drag_last_y = event.motion.y;
                if (dx || dy) {
                    float deg_per_px = sbs360_hfov / (float)(cur_stream->width ? cur_stream->width : 1) * 1.0f;
                    sbs360_yaw += dx * deg_per_px;
                    sbs360_pitch = av_clipf(sbs360_pitch - dy * deg_per_px, -90.0f, 90.0f);
                    cur_stream->force_refresh = 1;
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
                x = event.button.x;
            } else {
                if (!(event.motion.state & SDL_BUTTON_RMASK)) {
                    break;
                }
                x = event.motion.x;
            }
            if (seek_by_bytes || cur_stream->ic->duration <= 0) {
                uint64_t size = avio_size(cur_stream->ic->pb);
                stream_seek(cur_stream, size * x / cur_stream->width, 0, 1);
            } else {
                int64_t ts;
                frac = x / cur_stream->width;
                ts = frac * cur_stream->ic->duration;
                if (cur_stream->ic->start_time != AV_NOPTS_VALUE) {
                    ts += cur_stream->ic->start_time;
                }
                stream_seek(cur_stream, ts, 0, 0);
            }
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            screen_width = event.window.data1;
            screen_height = event.window.data2;
            if (is_fullscreen) {
                cur_stream->width = screen_width;
                cur_stream->height = screen_height;
            } else {
                cur_stream->width = default_width;
                cur_stream->height = default_height;
            }
            if (vk_renderer) {
                vk_renderer_resize(vk_renderer, screen_width, screen_height);
            }
            [[fallthrough]];
        case SDL_EVENT_WINDOW_EXPOSED:
            cur_stream->force_refresh = 1;
            break;
        case SDL_EVENT_QUIT:
            do_exit(*pis);
            break;
        case FF_QUIT_EVENT: {
            VideoState *old = event.user.data1;
            if (old && old != *pis) {
                break;
            }
            if (fatal_error_pending) {
                do_exit(*pis);
                break;
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
        default:
            break;
        }
    }
}
