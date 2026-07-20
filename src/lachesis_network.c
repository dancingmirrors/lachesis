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

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#include <libavformat/avio.h>
#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

#include "lachesis_options.h"
#include "lachesis_renderer.h"

static char *build_default_ytdl_format(void) {
    unsigned caps = vk_renderer_video_decode_caps(vk_renderer);
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
    if (caps & VK_DECODE_CAP_AV1) {
        ADD_SEL("av01");
    }
    if (caps & VK_DECODE_CAP_VP9) {
        ADD_SEL("vp09");
        ADD_SEL("vp9");
    }
    if (caps & VK_DECODE_CAP_HEVC) {
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

#if defined(_WIN32)
static void win_append_quoted_arg(AVBPrint *bp, const char *arg) {
    av_bprint_chars(bp, '"', 1);
    for (const char *p = arg;; p++) {
        unsigned backslashes = 0;
        while (*p == '\\') {
            backslashes++;
            p++;
        }
        if (*p == '\0') {
            for (unsigned i = 0; i < backslashes * 2; i++) {
                av_bprint_chars(bp, '\\', 1);
            }
            break;
        } else if (*p == '"') {
            for (unsigned i = 0; i < backslashes * 2 + 1; i++) {
                av_bprint_chars(bp, '\\', 1);
            }
            av_bprint_chars(bp, '"', 1);
        } else {
            for (unsigned i = 0; i < backslashes; i++) {
                av_bprint_chars(bp, '\\', 1);
            }
            av_bprint_chars(bp, *p, 1);
        }
    }
    av_bprint_chars(bp, '"', 1);
}

static FILE *win_ytdl_spawn(const char *path, const char *fmt, const char *url,
                            HANDLE *out_proc) {
    SECURITY_ATTRIBUTES sa = {.nLength = sizeof(sa), .bInheritHandle = TRUE};
    STARTUPINFOA si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    HANDLE rd = NULL, wr = NULL, nul = INVALID_HANDLE_VALUE;
    AVBPrint cmdline;
    char *cmd = NULL;
    int fd = -1;
    FILE *fp = NULL;
    BOOL ok;

    *out_proc = NULL;

    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        return NULL;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);

    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = nul;

    av_bprint_init(&cmdline, 0, AV_BPRINT_SIZE_AUTOMATIC);
    win_append_quoted_arg(&cmdline, path);
    av_bprintf(&cmdline, " -g --no-warnings --no-playlist -f ");
    win_append_quoted_arg(&cmdline, fmt);
    av_bprintf(&cmdline, " -- ");
    win_append_quoted_arg(&cmdline, url);
    if (!av_bprint_is_complete(&cmdline)) {
        av_bprint_finalize(&cmdline, NULL);
        goto fail;
    }
    if (av_bprint_finalize(&cmdline, &cmd) < 0 || !cmd) {
        goto fail;
    }

    ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL,
                        &si, &pi);
    av_free(cmd);
    if (!ok) {
        goto fail;
    }

    CloseHandle(wr);
    wr = NULL;
    CloseHandle(nul);
    nul = INVALID_HANDLE_VALUE;
    CloseHandle(pi.hThread);

    fd = _open_osfhandle((intptr_t)rd, _O_RDONLY);
    if (fd < 0) {
        CloseHandle(rd);
        CloseHandle(pi.hProcess);
        return NULL;
    }
    rd = NULL;

    fp = _fdopen(fd, "r");
    if (!fp) {
        _close(fd);
        CloseHandle(pi.hProcess);
        return NULL;
    }

    *out_proc = pi.hProcess;
    return fp;

fail:
    if (rd) {
        CloseHandle(rd);
    }
    if (wr) {
        CloseHandle(wr);
    }
    if (nul != INVALID_HANDLE_VALUE) {
        CloseHandle(nul);
    }
    return NULL;
}
#endif /* _WIN32 */

int ytdl_resolve(const char *url, char **video_url, char **audio_url) {
    *video_url = NULL;
    *audio_url = NULL;
    const char *path = ytdl_path ? ytdl_path : "yt-dlp";
    char *auto_fmt = ytdl_format ? NULL : build_default_ytdl_format();
    const char *fmt = ytdl_format ? ytdl_format
                                  : (auto_fmt ? auto_fmt : "bestvideo+bestaudio/best");

#if defined(_WIN32)
    /* Spawn yt-dlp directly so cmd.exe never expands %VAR% in the URL. */
    HANDLE proc = NULL;
    FILE *fp = win_ytdl_spawn(path, fmt, url, &proc);
    av_free(auto_fmt);
    if (!fp) {
        return 0;
    }
#else
    char *cmd = av_asprintf("%s -g --no-warnings --no-playlist -f '%s' -- '%s' 2>/dev/null",
                            path, fmt, url);
    av_free(auto_fmt);
    if (!cmd) {
        return 0;
    }
    FILE *fp = popen(cmd, "r");
    av_free(cmd);
    if (!fp) {
        return 0;
    }
#endif

    int n = 0;
    char line[8192];
    while (fgets(line, sizeof(line), fp)) {
        int len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (!len) {
            continue;
        }
        if (n == 0) {
            *video_url = av_strdup(line);
        } else if (n == 1) {
            *audio_url = av_strdup(line);
        }
        n++;
    }
#if defined(_WIN32)
    fclose(fp);
    if (proc) {
        WaitForSingleObject(proc, INFINITE);
        CloseHandle(proc);
    }
#else
    pclose(fp);
#endif

    if (!*video_url) {
        av_freep(audio_url);
        return 0;
    }

    return n;
}
