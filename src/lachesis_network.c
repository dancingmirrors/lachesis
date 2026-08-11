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

#include "lachesis_network.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <libavformat/avio.h>
#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

#include "lachesis_alloc.h"
#include "lachesis_options.h"
#include "lachesis_renderer.h"

static char *build_default_ytdl_format(void) {
    unsigned caps = renderer_video_decode_caps(renderer);
    char sel[512];
    size_t off = 0;

    /* clang-format off */
#define ADD_SEL(codec)                                    \
    do {                                                  \
        int n = snprintf(sel + off, sizeof(sel) - off,    \
                         "bv*[vcodec^=%s]+ba/", (codec)); \
        if (n > 0 && (size_t)n < sizeof(sel) - off) {     \
            off += (size_t)n;                             \
        }                                                 \
    } while (0)
    /* clang-format on */

    sel[0] = '\0';
    if (caps & RENDERER_DECODE_CAP_AV1) {
        ADD_SEL("av01");
    }
    if (caps & RENDERER_DECODE_CAP_VP9) {
        ADD_SEL("vp09");
        ADD_SEL("vp9");
    }
    if (caps & RENDERER_DECODE_CAP_HEVC) {
        ADD_SEL("hev1");
        ADD_SEL("hvc1");
    }
    ADD_SEL("avc1");
    ADD_SEL("h264");
#undef ADD_SEL

    return av_asprintf("%sb", sel);
}

void set_ytdl_http_opts(AVDictionary **opts) {
    av_dict_set(opts, "reconnect", "1", 0);
    av_dict_set(opts, "reconnect_streamed", "1", 0);
    av_dict_set(opts, "reconnect_on_network_error", "1", 0);
    av_dict_set(opts, "reconnect_on_http_error", "4xx,5xx", 0);
    av_dict_set(opts, "reconnect_delay_max", "7", 0);
    av_dict_set(opts, "multiple_requests", "1", 0);
}

#define YTDL_CHUNK_BYTES ((int64_t)10 * 1024 * 1024)
#define YTDL_AVIO_BUFSZ (64 * 1024)
#define YTDL_CHUNK_MAX_RETRIES 5

struct YtdlChunkedIO {
    char *url;
    int64_t pos; /* The current logical byte position. */
    int64_t size; /* The total resource size, or -1 if unknown. */
    int64_t chunk; /* The request size in bytes. */
    int64_t inner_read; /* The bytes consumed from the current inner request. */
    AVIOContext *inner; /* The current chunk's HTTP context, or NULL. */
    AVIOContext *pb; /* The wrapper context handed to the demuxer. */
    VideoState *is;
};

static int ytdl_chunked_interrupt(void *arg) {
    VideoState *is = arg;
    return is && is->abort_request;
}

static int ytdl_chunked_open_inner(struct YtdlChunkedIO *c) {
    AVDictionary *opts = NULL;
    set_ytdl_http_opts(&opts);
    AVIOInterruptCB cb = {ytdl_chunked_interrupt, c->is};
    int ret = avio_open2(&c->inner, c->url, AVIO_FLAG_READ, &cb, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        c->inner = NULL;
        return ret;
    }
    if (c->size < 0) {
        int64_t sz = avio_size(c->inner);
        if (sz > 0) {
            c->size = sz;
        }
    }
    /* This should work with any lavf version. */
    if (c->pos > 0 && avio_seek(c->inner, c->pos, SEEK_SET) < 0) {
        avio_closep(&c->inner);
        return AVERROR(EIO);
    }
    c->inner_read = 0;

    return 0;
}

static int ytdl_chunked_read(void *opaque, uint8_t *buf, int size) {
    struct YtdlChunkedIO *c = opaque;
    int retries = 0;

    for (;;) {
        if (c->is && c->is->abort_request) {
            return AVERROR_EXIT;
        }
        if (!c->inner) {
            if (c->size >= 0 && c->pos >= c->size) {
                return AVERROR_EOF;
            }
            if (ytdl_chunked_open_inner(c) < 0) {
                if (++retries > YTDL_CHUNK_MAX_RETRIES) {
                    return AVERROR(EIO);
                }
                continue;
            }
        }
        int want = size;
        if (c->chunk > 0) {
            int64_t rem = c->chunk - c->inner_read;
            if (rem <= 0) {
                avio_closep(&c->inner);
                continue;
            }
            if (want > rem) {
                want = (int)rem;
            }
        }
        int r = avio_read(c->inner, buf, want);
        if (r > 0) {
            c->pos += r;
            c->inner_read += r;
            return r;
        }
        avio_closep(&c->inner);
        if (r == AVERROR_EOF) {
            if (c->size > 0 && c->pos < c->size) {
                continue;
            }
            return AVERROR_EOF;
        }
        if (++retries > YTDL_CHUNK_MAX_RETRIES) {
            return r < 0 ? r : AVERROR(EIO);
        }
    }
}

static int64_t ytdl_chunked_seek(void *opaque, int64_t offset, int whence) {
    struct YtdlChunkedIO *c = opaque;
    int64_t newpos;

    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) {
        return c->size >= 0 ? c->size : AVERROR(ENOSYS);
    }
    if (whence == SEEK_SET) {
        newpos = offset;
    } else if (whence == SEEK_CUR) {
        newpos = c->pos + offset;
    } else if (whence == SEEK_END) {
        if (c->size < 0) {
            return AVERROR(EINVAL);
        }
        newpos = c->size + offset;
    } else {
        return AVERROR(EINVAL);
    }
    if (newpos < 0) {
        return AVERROR(EINVAL);
    }
    avio_closep(&c->inner);
    c->pos = newpos;

    return newpos;
}

struct YtdlChunkedIO *ytdl_chunked_create(const char *url, VideoState *is) {
    struct YtdlChunkedIO *c = av_mallocz(sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->url = av_strdup(url);
    c->pos = 0;
    c->size = -1;
    c->chunk = YTDL_CHUNK_BYTES;
    c->is = is;
    if (!c->url) {
        av_free(c);
        return NULL;
    }
    if (ytdl_chunked_open_inner(c) < 0) {
        av_free(c->url);
        av_free(c);
        return NULL;
    }
    if (c->size <= 0) {
        avio_closep(&c->inner);
        av_free(c->url);
        av_free(c);
        return NULL;
    }
    unsigned char *buffer = av_malloc(YTDL_AVIO_BUFSZ);
    if (!buffer) {
        avio_closep(&c->inner);
        av_free(c->url);
        av_free(c);
        return NULL;
    }
    c->pb = avio_alloc_context(buffer, YTDL_AVIO_BUFSZ, 0, c, ytdl_chunked_read,
                               NULL, ytdl_chunked_seek);
    if (!c->pb) {
        av_free(buffer);
        avio_closep(&c->inner);
        av_free(c->url);
        av_free(c);
        return NULL;
    }
    alloc_track_disown(buffer);
    c->pb->seekable = c->size > 0 ? AVIO_SEEKABLE_NORMAL : 0;

    return c;
}

void ytdl_chunked_free(struct YtdlChunkedIO **pc) {
    struct YtdlChunkedIO *c = pc ? *pc : NULL;
    if (!c) {
        return;
    }
    avio_closep(&c->inner);
    if (c->pb) {
        av_freep(&c->pb->buffer);
        avio_context_free(&c->pb);
    }
    av_free(c->url);
    av_freep(pc);
}

AVIOContext *ytdl_chunked_pb(struct YtdlChunkedIO *c) {
    return c ? c->pb : NULL;
}

#define YTDL_POLL_MS 100
#define YTDL_OUTPUT_MAX (256 * 1024)

static SDL_Process *ytdl_spawn(const char *path, const char *fmt, const char *url) {
    const char *args[] = {
        path,
        "-g",
        "--no-warnings",
        "--no-playlist",
        "-f",
        fmt,
        "--",
        url,
        NULL,
    };
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_Process *proc;

    if (!props) {
        return NULL;
    }
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                          SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER,
                          SDL_PROCESS_STDIO_NULL);
#if defined(_WIN32)
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#endif
    proc = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);

    return proc;
}

static int ytdl_read_output(SDL_Process *proc, VideoState *is, AVBPrint *out) {
    SDL_IOStream *io = SDL_GetProcessOutput(proc);

    if (!io) {
        return 0;
    }
    for (;;) {
        char chunk[4096];
        size_t got;

        if (is && is->abort_request) {
            return 0;
        }
        got = SDL_ReadIO(io, chunk, sizeof(chunk));
        if (got > 0) {
            av_bprint_append_data(out, chunk, (unsigned)got);
            if (!av_bprint_is_complete(out)) {
                return 0;
            }
            continue;
        }
        switch (SDL_GetIOStatus(io)) {
        case SDL_IO_STATUS_NOT_READY:
            SDL_Delay(YTDL_POLL_MS);
            continue;
        case SDL_IO_STATUS_EOF:
            return 1;
        default:
            return 0;
        }
    }
}

static void ytdl_reap(SDL_Process *proc, int kill_it) {
    if (!proc) {
        return;
    }
    if (kill_it) {
        SDL_KillProcess(proc, false);
        for (int i = 0; i < 50; i++) {
            if (SDL_WaitProcess(proc, false, NULL)) {
                SDL_DestroyProcess(proc);
                return;
            }
            SDL_Delay(10);
        }
        SDL_KillProcess(proc, true);
    }
    SDL_WaitProcess(proc, true, NULL);
    SDL_DestroyProcess(proc);
}

int ytdl_resolve(VideoState *is, const char *url, char **video_url,
                 char **audio_url) {
    *video_url = NULL;
    *audio_url = NULL;
    const char *path = ytdl_path ? ytdl_path : "yt-dlp";
    char *auto_fmt = ytdl_format ? NULL : build_default_ytdl_format();
    const char *fmt = ytdl_format ? ytdl_format
                                  : (auto_fmt ? auto_fmt : "bestvideo+bestaudio/best");
    AVBPrint out;
    int complete;

    av_bprint_init(&out, 0, YTDL_OUTPUT_MAX);

    SDL_Process *proc = ytdl_spawn(path, fmt, url);
    av_free(auto_fmt);
    if (!proc) {
        av_bprint_finalize(&out, NULL);
        return 0;
    }
    complete = ytdl_read_output(proc, is, &out);
    ytdl_reap(proc, !complete);

    int n = 0;
    if (complete && av_bprint_is_complete(&out)) {
        char *save = NULL;
        for (char *line = av_strtok(out.str, "\r\n", &save); line;
             line = av_strtok(NULL, "\r\n", &save)) {
            if (n == 0) {
                *video_url = av_strdup(line);
            } else if (n == 1) {
                *audio_url = av_strdup(line);
            }
            n++;
        }
    }
    av_bprint_finalize(&out, NULL);

    if (!*video_url) {
        av_freep(audio_url);
        return 0;
    }

    return n;
}
