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
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
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

static int win_ytdl_spawn(const char *path, const char *fmt, const char *url,
                          HANDLE *out_proc, HANDLE *out_rd) {
    SECURITY_ATTRIBUTES sa = {.nLength = sizeof(sa), .bInheritHandle = TRUE};
    STARTUPINFOA si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    HANDLE rd = NULL, wr = NULL, nul = INVALID_HANDLE_VALUE;
    AVBPrint cmdline;
    char *cmd = NULL;
    BOOL ok;

    *out_proc = NULL;
    *out_rd = NULL;

    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        return 0;
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

    *out_proc = pi.hProcess;
    *out_rd = rd;
    return 1;

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
    return 0;
}

static int ytdl_read_output(HANDLE rd, VideoState *is, AVBPrint *out) {
    for (;;) {
        if (is && is->abort_request) {
            return 0;
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) {
            return 1;
        }
        if (!avail) {
            Sleep(YTDL_POLL_MS);
            continue;
        }
        char chunk[4096];
        DWORD got = 0;
        if (avail > sizeof(chunk)) {
            avail = sizeof(chunk);
        }
        if (!ReadFile(rd, chunk, avail, &got, NULL) || !got) {
            return 1;
        }
        av_bprint_append_data(out, chunk, got);
        if (!av_bprint_is_complete(out)) {
            return 0;
        }
    }
}
#else /* !_WIN32 */

static int posix_ytdl_spawn(const char *path, const char *fmt, const char *url,
                            pid_t *out_pid) {
    int fds[2];

    if (pipe(fds) != 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    if (pid == 0) {
        char *const argv[] = {
            (char *)path,
            (char *)"-g",
            (char *)"--no-warnings",
            (char *)"--no-playlist",
            (char *)"-f",
            (char *)fmt,
            (char *)"--",
            (char *)url,
            NULL,
        };

        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(fds[1]);

        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) {
            dup2(nul, STDERR_FILENO);
            close(nul);
        }

        execvp(path, argv);
        _exit(127);
    }

    close(fds[1]);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    *out_pid = pid;

    return fds[0];
}

static void posix_ytdl_reap(pid_t pid, int kill_it) {
    if (pid <= 0) {
        return;
    }
    if (kill_it) {
        kill(pid, SIGTERM);
        for (int i = 0; i < 50; i++) {
            pid_t r = waitpid(pid, NULL, WNOHANG);
            if (r == pid || (r < 0 && errno != EINTR)) {
                return;
            }
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
        kill(pid, SIGKILL);
    }
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
}

static int ytdl_read_output(int fd, VideoState *is, AVBPrint *out) {
    for (;;) {
        if (is && is->abort_request) {
            return 0;
        }
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, YTDL_POLL_MS);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        if (pr == 0) {
            continue;
        }
        char chunk[4096];
        ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return 0;
        }
        if (got == 0) {
            return 1;
        }
        av_bprint_append_data(out, chunk, (unsigned)got);
        if (!av_bprint_is_complete(out)) {
            return 0;
        }
    }
}

#endif /* _WIN32 */

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

#if defined(_WIN32)
    /* Spawn yt-dlp directly so cmd.exe never expands %VAR% in the URL. */
    HANDLE proc = NULL, rd = NULL;
    int spawned = win_ytdl_spawn(path, fmt, url, &proc, &rd);
    av_free(auto_fmt);
    if (!spawned) {
        av_bprint_finalize(&out, NULL);
        return 0;
    }
    complete = ytdl_read_output(rd, is, &out);
    CloseHandle(rd);
    if (proc) {
        if (!complete) {
            TerminateProcess(proc, 1);
        }
        WaitForSingleObject(proc, INFINITE);
        CloseHandle(proc);
    }
#else
    pid_t pid = -1;
    int fd = posix_ytdl_spawn(path, fmt, url, &pid);
    av_free(auto_fmt);
    if (fd < 0) {
        av_bprint_finalize(&out, NULL);
        return 0;
    }
    complete = ytdl_read_output(fd, is, &out);
    close(fd);
    posix_ytdl_reap(pid, !complete);
#endif

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
