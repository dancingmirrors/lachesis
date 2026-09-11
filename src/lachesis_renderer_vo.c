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

/* clang-format off */
#include "lachesis_alloc.h"
#include "lachesis_config.h"
#include "lachesis_log.h"
#include "lachesis_renderer.h"
#include "lachesis_renderer_internal.h"
#include "lachesis_supersample.h"
#include "lachesis_view360.h"
/* clang-format on */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>

static void vo_pin_gpu(RendererContext *ctx) {
#if LACHESIS_HAVE_OPENGL
    gl_pin_current(ctx);
#else
    (void)ctx;
#endif
}

static void vo_unpin_gpu(RendererContext *ctx) {
#if LACHESIS_HAVE_OPENGL
    gl_unpin_current(ctx);
#else
    (void)ctx;
#endif
}

static void vo_overlay_free(VoOverlay *ov) {
    av_freep(&ov->pixels);
    ov->size = 0;
    ov->generation = 0;
}

static void *vo_overlay_take(VoOverlay *ov, const void *pixels, int height,
                             int stride, unsigned generation) {
    size_t need;

    if (!pixels || height <= 0 || stride <= 0) {
        return NULL;
    }
    need = (size_t)height * (size_t)stride;
    if (ov->pixels && ov->size == need && ov->generation == generation) {
        return ov->pixels;
    }
    if (!ov->pixels || ov->size != need) {
        av_freep(&ov->pixels);
        if (!(ov->pixels = av_malloc(need))) {
            ov->size = 0;
            ov->generation = 0;
            return NULL;
        }
        ov->size = need;
    }
    memcpy(ov->pixels, pixels, need);
    ov->generation = generation;

    return ov->pixels;
}

static int vo_frame_take(VoFrame *dst, AVFrame *src, uint64_t id) {
    if (!src) {
        if (dst->frame) {
            av_frame_unref(dst->frame);
        }
        dst->id = 0;
        return 0;
    }
    if (dst->frame && dst->frame->buf[0] && id && dst->id == id) {
        return 0;
    }
    if (!dst->frame && !(dst->frame = av_frame_alloc())) {
        return AVERROR(ENOMEM);
    }
    av_frame_unref(dst->frame);
    dst->id = 0;
    if (av_frame_ref(dst->frame, src) < 0) {
        return AVERROR(ENOMEM);
    }
    dst->id = id;

    return 0;
}

static void vo_frame_drop(VoFrame *vf) {
    if (vf->frame) {
        av_frame_unref(vf->frame);
    }
    vf->id = 0;
}

static void vo_frame_free(VoFrame *vf) {
    av_frame_free(&vf->frame);
    vf->id = 0;
}

static void vo_job_release(Vo *vo) {
    vo->params.mix_frames = NULL;
    vo->params.mix_num_frames = 0;
    vo->params.prev_frame = NULL;
    vo->params.next_frame = NULL;
}

static void vo_frames_drop(Vo *vo) {
    vo_frame_drop(&vo->frame);
    vo_frame_drop(&vo->prev_frame);
    vo_frame_drop(&vo->next_frame);
    for (size_t i = 0; i < FF_ARRAY_ELEMS(vo->mix_frame); i++) {
        vo_frame_drop(&vo->mix_frame[i]);
    }
}

/* Called with the lock held. */
static int vo_job_take(Vo *vo, AVFrame *frame, const RenderParams *params,
                       int blank) {
    int num_mix = params->mix_frames ? params->mix_num_frames : 0;

    vo->params = *params;
    vo->blank = blank;

    if (num_mix > (int)FF_ARRAY_ELEMS(vo->mix_frame)) {
        num_mix = FF_ARRAY_ELEMS(vo->mix_frame);
    }
    if (vo_frame_take(&vo->frame, blank ? NULL : frame, params->frame_id) < 0 ||
        vo_frame_take(&vo->prev_frame, params->prev_frame,
                      params->prev_frame_id) < 0 ||
        vo_frame_take(&vo->next_frame, params->next_frame,
                      params->next_frame_id) < 0) {
        vo_frames_drop(vo);
        return AVERROR(ENOMEM);
    }
    vo->params.prev_frame = params->prev_frame ? vo->prev_frame.frame : NULL;
    vo->params.next_frame = params->next_frame ? vo->next_frame.frame : NULL;

    for (int i = 0; i < num_mix; i++) {
        AVFrame *src = params->mix_frames[i].frame;

        vo->mix[i] = params->mix_frames[i];
        if (src && src == frame) {
            vo->mix[i].frame = vo->frame.frame;
            continue;
        }
        if (vo_frame_take(&vo->mix_frame[i], src,
                          params->mix_frames[i].signature) < 0) {
            vo_frames_drop(vo);
            return AVERROR(ENOMEM);
        }
        vo->mix[i].frame = src ? vo->mix_frame[i].frame : NULL;
    }
    vo->params.mix_frames = num_mix ? vo->mix : NULL;
    vo->params.mix_num_frames = num_mix;

    vo->params.osd_pixels =
        vo_overlay_take(&vo->osd, params->osd_pixels, params->osd_height,
                        params->osd_stride, params->osd_generation);
    vo->params.sub_pixels =
        vo_overlay_take(&vo->sub, params->sub_pixels, params->sub_height,
                        params->sub_stride, params->sub_generation);
    vo->params.text_sub_pixels =
        vo_overlay_take(&vo->text_sub, params->text_sub_pixels,
                        params->text_sub_height, params->text_sub_stride,
                        params->text_sub_generation);

    return 0;
}

static void vo_note_feedback(Vo *vo, const RenderParams *rp, unsigned epoch) {
    unsigned next = (vo->feedback_head + 1) % VO_FEEDBACK_RING;
    VoFeedback *fb;

    if (rp->present_done_us <= 0 && rp->present_display_us <= 0) {
        return;
    }
    if (epoch != vo->feedback_epoch) {
        return;
    }
    if (next == vo->feedback_tail) {
        vo->feedback_tail = (vo->feedback_tail + 1) % VO_FEEDBACK_RING;
    }
    fb = &vo->feedback[vo->feedback_head];
    fb->done_us = rp->present_done_us;
    fb->block_us = rp->present_block_us;
    fb->source = rp->present_source;
    fb->display_us = rp->present_display_us;
    fb->refresh_us = rp->present_refresh_us;
    vo->feedback_head = next;
}

static int vo_thread(void *arg) {
    RendererContext *ctx = arg;
    Vo *vo = &ctx->vo;

    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);

    for (;;) {
        enum View360Layout p360_layout;
        enum View360Projection p360_projection;
        enum SupersampleLevel p_supersample;
        AVFrame *frame;
        float yaw, pitch, roll, hfov;
        unsigned epoch;
        unsigned pending;
        int blank;
        int status;

        SDL_LockMutex(vo->lock);
        while (!vo->have_job && !vo->quit) {
            SDL_WaitCondition(vo->wake, vo->lock);
        }
        if (vo->quit && !vo->have_job) {
            SDL_UnlockMutex(vo->lock);
            break;
        }
        vo->have_job = 0;
        blank = vo->blank;
        frame = vo->frame.frame;
        pending = vo->pending;
        vo->pending = 0;
        p360_layout = vo->pending_360_layout;
        p360_projection = vo->pending_360_projection;
        p_supersample = vo->pending_supersample;
        epoch = vo->feedback_epoch;
        yaw = vo->view360_yaw;
        pitch = vo->view360_pitch;
        roll = vo->view360_roll;
        hfov = vo->view360_hfov;
        SDL_UnlockMutex(vo->lock);

        if (pending & VO_PENDING_360) {
            renderer_apply_360(ctx, p360_layout, p360_projection);
        }
        ctx->sbs360_yaw = yaw;
        ctx->sbs360_pitch = pitch;
        ctx->sbs360_roll = roll;
        ctx->sbs360_hfov = hfov;
        if (pending & VO_PENDING_SUPERSAMPLE) {
            if (p_supersample != SUPERSAMPLE_OFF && !ctx->supersample_hook) {
                ctx->supersample_hook = supersample_pl_hook_create(ctx->gpu);
                if (!ctx->supersample_hook) {
                    log_warn("Supersampling is unavailable.\n");
                }
            }
            ctx->supersample_level =
                ctx->supersample_hook ? p_supersample : SUPERSAMPLE_OFF;
        }

        vo_pin_gpu(ctx);
        vo_pin_gpu(ctx);
        status = blank ? renderer_draw_blank(&ctx->api, &vo->params)
                       : renderer_draw_frame(&ctx->api, frame, &vo->params);
        vo_unpin_gpu(ctx);
        vo_unpin_gpu(ctx);

        SDL_LockMutex(vo->lock);
        vo->last_status = status;
        vo->have_status = 1;
        vo_note_feedback(vo, &vo->params, epoch);
        vo_job_release(vo);
        vo->busy = 0;
        SDL_BroadcastCondition(vo->idle);
        SDL_UnlockMutex(vo->lock);
    }

    return 0;
}

int vo_start(RendererContext *ctx) {
    Vo *vo = &ctx->vo;

    if (ctx->api.backend == RENDERER_API_OPENGL) {
        return 0;
    }
    vo->view360_hfov = 90.0f;
    if (!(vo->lock = SDL_CreateMutex()) || !(vo->wake = SDL_CreateCondition()) ||
        !(vo->idle = SDL_CreateCondition())) {
        return AVERROR(ENOMEM);
    }
    if (!(vo->thread = SDL_CreateThread(vo_thread, "video output", ctx))) {
        return AVERROR(ENOMEM);
    }

    return 0;
}

int vo_stop(RendererContext *ctx) {
    Vo *vo = &ctx->vo;
    int stopped = 1;

    if (vo->abandoned) {
        return 0;
    }
    if (vo->thread) {
        SDL_LockMutex(vo->lock);
        vo->quit = 1;
        SDL_SignalCondition(vo->wake);
        while (vo->busy) {
            if (!SDL_WaitConditionTimeout(vo->idle, vo->lock, VO_STOP_WAIT_MS)) {
                break;
            }
        }
        stopped = !vo->busy;
        SDL_UnlockMutex(vo->lock);

        if (stopped) {
            SDL_WaitThread(vo->thread, NULL);
        } else {
            SDL_DetachThread(vo->thread);
            vo->abandoned = 1;
        }
        vo->thread = NULL;
    }
    if (!stopped) {
        return 0;
    }

    vo_job_release(vo);
    vo_frame_free(&vo->frame);
    vo_frame_free(&vo->prev_frame);
    vo_frame_free(&vo->next_frame);
    for (size_t i = 0; i < FF_ARRAY_ELEMS(vo->mix_frame); i++) {
        vo_frame_free(&vo->mix_frame[i]);
    }
    vo_overlay_free(&vo->osd);
    vo_overlay_free(&vo->sub);
    vo_overlay_free(&vo->text_sub);
    if (vo->wake) {
        SDL_DestroyCondition(vo->wake);
        vo->wake = NULL;
    }
    if (vo->idle) {
        SDL_DestroyCondition(vo->idle);
        vo->idle = NULL;
    }
    if (vo->lock) {
        SDL_DestroyMutex(vo->lock);
        vo->lock = NULL;
    }

    return 1;
}

int vo_borrow(RendererContext *ctx, int timeout_ms) {
    Vo *vo = &ctx->vo;

    if (ctx->quiesced) {
        return 0;
    }

    if (!vo->thread) {
        return !vo->abandoned;
    }
    SDL_LockMutex(vo->lock);
    if (vo->borrowed) {
        vo->borrowed++;
        SDL_UnlockMutex(vo->lock);
        return 1;
    }
    while (vo->busy) {
        if (!SDL_WaitConditionTimeout(vo->idle, vo->lock, timeout_ms)) {
            break;
        }
    }
    if (vo->busy) {
        SDL_UnlockMutex(vo->lock);
        return 0;
    }
    vo->busy = 1;
    vo->borrowed = 1;
    SDL_UnlockMutex(vo->lock);

    return 1;
}

void vo_release(RendererContext *ctx) {
    Vo *vo = &ctx->vo;

    if (!vo->thread) {
        return;
    }
    SDL_LockMutex(vo->lock);
    if (vo->borrowed && !--vo->borrowed) {
        vo->busy = 0;
        SDL_BroadcastCondition(vo->idle);
    }
    SDL_UnlockMutex(vo->lock);
}

int vo_submit(RendererContext *ctx, AVFrame *frame, RenderParams *params,
              int blank) {
    Vo *vo = &ctx->vo;
    int status;
    int ret;

    if (ctx->quiesced) {
        return AVERROR(EAGAIN);
    }

    if (!vo->thread) {
        if (vo->abandoned) {
            return AVERROR(EAGAIN);
        }
        vo_pin_gpu(ctx);
        status = blank ? renderer_draw_blank(&ctx->api, params)
                       : renderer_draw_frame(&ctx->api, frame, params);
        vo_unpin_gpu(ctx);
        vo_note_feedback(vo, params, vo->feedback_epoch);

        return status;
    }

    SDL_LockMutex(vo->lock);
    if (vo->busy) {
        int64_t deadline = av_gettime_relative() + VO_HANDOFF_WAIT_MS * 1000;

        while (vo->busy) {
            int64_t left = ctx->benchmark ? 1000 : deadline - av_gettime_relative();

            if (left <= 0) {
                break;
            }
            SDL_WaitConditionTimeout(vo->idle, vo->lock,
                                     (Sint32)(left / 1000) + 1);
        }
    }
    if (vo->busy || vo->quit) {
        SDL_UnlockMutex(vo->lock);
        return AVERROR(EAGAIN);
    }
    ret = vo_job_take(vo, frame, params, blank);
    if (ret < 0) {
        SDL_UnlockMutex(vo->lock);
        return ret;
    }
    status = vo->have_status ? vo->last_status : AVERROR(ERANGE);
    vo->have_job = 1;
    vo->busy = 1;
    SDL_SignalCondition(vo->wake);
    SDL_UnlockMutex(vo->lock);

    return status;
}

void renderer_drop_present_feedback(Renderer *renderer) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (!ctx) {
        return;
    }
    vo_state_lock(ctx);
    ctx->vo.feedback_tail = ctx->vo.feedback_head;
    ctx->vo.feedback_epoch++;
    vo_state_unlock(ctx);
}

int renderer_pause_output(Renderer *renderer) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (!ctx) {
        return 1;
    }

    return vo_borrow(ctx, VO_WINDOW_WAIT_MS);
}

void renderer_resume_output(Renderer *renderer) {
    RendererContext *ctx = (RendererContext *)renderer;

    if (ctx) {
        vo_release(ctx);
    }
}

int renderer_take_present_feedback(Renderer *renderer,
                                   RendererPresentFeedback *out) {
    RendererContext *ctx = (RendererContext *)renderer;
    Vo *vo;
    VoFeedback fb;

    if (!ctx) {
        return 0;
    }
    vo = &ctx->vo;
    vo_state_lock(ctx);
    if (vo->feedback_tail == vo->feedback_head) {
        vo_state_unlock(ctx);
        return 0;
    }
    fb = vo->feedback[vo->feedback_tail];
    vo->feedback_tail = (vo->feedback_tail + 1) % VO_FEEDBACK_RING;
    vo_state_unlock(ctx);

    out->done_us = fb.done_us;
    out->block_us = fb.block_us;
    out->source = fb.source;
    out->display_us = fb.display_us;
    out->refresh_us = fb.refresh_us;

    return 1;
}
