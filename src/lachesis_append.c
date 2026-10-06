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

#include "lachesis_append.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

#include "lachesis_alloc.h"
#include "lachesis_log.h"

#define APPEND_RETRY_TIMEOUT_MS 200
#define APPEND_MAX_RETRIES 10
#define APPEND_WAIT_SLICE_MS 10
#define APPEND_EOF_TRIES 2
#define APPEND_AVIO_BUFSIZE (64 * 1024)

struct AppendIO {
    AVIOContext *inner;
    AVIOContext *pb;
    int64_t eof_size;
    int64_t known_size;
    unsigned ops;
    int appending;
    int warned;
    int past_end;
    int eof_tries;
    int indirect;
    VideoState *is;
};

static int append_interrupt(void *arg) {
    VideoState *is = arg;
    return is && is->abort_request;
}

static int append_aborted(const struct AppendIO *a) {
    return a->is && a->is->abort_request;
}

static int append_wait_over(const struct AppendIO *a) {
    return a->is && (a->is->abort_request || a->is->seek_req);
}

static int64_t append_get_size(struct AppendIO *a) {
    int64_t size = avio_size(a->inner);

    if (size < 0) {
        return -1;
    }
    a->known_size = size;

    return size;
}

static int append_wait(struct AppendIO *a, int ms) {
    for (int waited = 0; waited < ms; waited += APPEND_WAIT_SLICE_MS) {
        if (append_wait_over(a)) {
            return 1;
        }
        SDL_Delay(APPEND_WAIT_SLICE_MS);
    }

    return append_wait_over(a);
}

static void append_note_growth(struct AppendIO *a, int64_t size) {
    if (size <= a->eof_size || a->appending) {
        return;
    }
    if (!a->warned) {
        log_warn("Periodically retrying on input still being appended to.\n");
        a->warned = 1;
    }
    a->appending = 1;
    if (a->pb) {
        a->pb->direct = 1;
    }
}

static void append_clear_eof(struct AppendIO *a) {
    a->inner->eof_reached = 0;
    a->inner->error = 0;
}

static int append_read_retry(struct AppendIO *a, uint8_t *buf, int size,
                             int expect_more) {
    int64_t size_now;

    a->ops++;
    for (int retries = 0; retries < APPEND_MAX_RETRIES; retries++) {
        int r;

        if (append_aborted(a)) {
            return AVERROR_EXIT;
        }
        append_clear_eof(a);
        r = avio_read(a->inner, buf, size);
        if (r > 0) {
            a->eof_tries = 0;
            return r;
        }
        if (r < 0 && r != AVERROR_EOF) {
            return r;
        }

        append_note_growth(a, append_get_size(a));

        if (!a->appending && !expect_more) {
            break;
        }
        if (append_wait(a, APPEND_RETRY_TIMEOUT_MS)) {
            if (append_aborted(a)) {
                return AVERROR_EXIT;
            }
            break;
        }
    }
    size_now = append_get_size(a);
    if (size_now >= 0) {
        a->eof_size = size_now;
    }
    if (expect_more) {
        a->past_end = 1;
    }

    return 0;
}

static int append_read(void *opaque, uint8_t *buf, int size) {
    int r = append_read_retry(opaque, buf, size, 0);

    return r ? r : AVERROR_EOF;
}

static int64_t append_seek(void *opaque, int64_t offset, int whence) {
    struct AppendIO *a = opaque;

    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE || whence == SEEK_END) {
        int64_t size = append_get_size(a);

        if (whence == AVSEEK_SIZE) {
            return size >= 0 ? size : AVERROR(ENOSYS);
        }
        if (size < 0 || offset > INT64_MAX - size) {
            return AVERROR(EINVAL);
        }
        offset += size;
        whence = SEEK_SET;
    }
    if (whence == SEEK_CUR) {
        int64_t pos = avio_tell(a->inner);

        if (pos < 0 || offset > INT64_MAX - pos) {
            return AVERROR(EINVAL);
        }
        offset += pos;
        whence = SEEK_SET;
    }
    a->ops++;
    if (whence == SEEK_SET && offset > a->known_size) {
        int64_t size = append_get_size(a);

        if (size >= 0 && offset > size) {
            a->past_end = 1;
            a->eof_size = size;
        }
    }
    append_clear_eof(a);

    return avio_seek(a->inner, offset, whence);
}

int append_io_local_file(const char *url) {
    const char *proto;

    if (!url || !*url) {
        return 0;
    }
    proto = avio_find_protocol_name(url);

    return proto && !strcmp(proto, "file");
}

int append_io_applies(const char *url, const AVInputFormat *forced) {
    AVProbeData pd = {.filename = url};
    int score = AVPROBE_SCORE_RETRY;

    if (!append_io_local_file(url)) {
        return 0;
    }
    if (forced) {
        return !(forced->flags & AVFMT_NOFILE);
    }

    return !av_probe_input_format2(&pd, 0, &score);
}

static struct AppendIO *append_io_open(const char *url, VideoState *is,
                                       int with_pb) {
    AVIOInterruptCB cb = {append_interrupt, is};
    struct AppendIO *a = av_mallocz(sizeof(*a));
    uint8_t *buffer;

    if (!a) {
        return NULL;
    }
    a->is = is;
    a->indirect = !with_pb;
    if (avio_open2(&a->inner, url, AVIO_FLAG_READ, &cb, NULL) < 0) {
        a->inner = NULL;
        av_free(a);
        return NULL;
    }
    a->inner->direct = 1;
    a->eof_size = append_get_size(a);
    if (!(a->inner->seekable & AVIO_SEEKABLE_NORMAL) || a->eof_size < 0) {
        avio_closep(&a->inner);
        av_free(a);
        return NULL;
    }
    if (!with_pb) {
        return a;
    }
    buffer = av_malloc(APPEND_AVIO_BUFSIZE);
    if (!buffer) {
        avio_closep(&a->inner);
        av_free(a);
        return NULL;
    }
    a->pb = avio_alloc_context(buffer, APPEND_AVIO_BUFSIZE, 0, a, append_read,
                               NULL, append_seek);
    if (!a->pb) {
        av_free(buffer);
        avio_closep(&a->inner);
        av_free(a);
        return NULL;
    }
    alloc_track_disown(buffer);
    a->pb->seekable = AVIO_SEEKABLE_NORMAL;

    return a;
}

struct AppendIO *append_io_create(const char *url, VideoState *is) {
    return append_io_open(url, is, 1);
}

struct AppendIO *append_io_create_reader(const char *url, VideoState *is) {
    return append_io_open(url, is, 0);
}

void append_io_free(struct AppendIO **pa) {
    struct AppendIO *a = pa ? *pa : NULL;

    if (!a) {
        return;
    }
    if (a->pb) {
        av_freep(&a->pb->buffer);
        avio_context_free(&a->pb);
    }
    avio_closep(&a->inner);
    av_freep(pa);
}

AVIOContext *append_io_pb(struct AppendIO *a) {
    return a ? a->pb : NULL;
}

unsigned append_io_ops(const struct AppendIO *a) {
    return a ? a->ops : 0;
}

int64_t append_io_read(struct AppendIO *a, void *buf, size_t size,
                       int expect_more) {
    if (!a || !size) {
        return 0;
    }
    if (size > INT_MAX) {
        size = INT_MAX;
    }

    return append_read_retry(a, buf, (int)size, expect_more);
}

int64_t append_io_seek(struct AppendIO *a, int64_t offset, int whence) {
    return a ? append_seek(a, offset, whence) : AVERROR(EINVAL);
}

int append_io_wait_growth(struct AppendIO *a) {
    int budget = APPEND_RETRY_TIMEOUT_MS * APPEND_MAX_RETRIES;

    if (!a) {
        return 0;
    }
    if (!a->past_end && (a->indirect || !a->appending)) {
        if (a->eof_tries >= APPEND_EOF_TRIES) {
            return 0;
        }
        a->eof_tries++;
        return 1;
    }
    a->past_end = 0;
    for (int waited = 0; waited < budget; waited += APPEND_WAIT_SLICE_MS) {
        int64_t size = append_get_size(a);

        if (size > a->eof_size) {
            append_note_growth(a, size);
            a->eof_size = size;
            return 1;
        }
        if (append_wait_over(a)) {
            break;
        }
        SDL_Delay(APPEND_WAIT_SLICE_MS);
    }
    a->appending = 0;

    return 0;
}
