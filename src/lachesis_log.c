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

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <io.h>
#include <windows.h>
// clang-format on
#define LACHESIS_STDERR_ISATTY() _isatty(_fileno(stderr))
#define LACHESIS_STDERR_WRITE(buf, len) _write(_fileno(stderr), buf, len)
#else
#include <sys/ioctl.h>
#include <unistd.h>
#define LACHESIS_STDERR_ISATTY() isatty(STDERR_FILENO)
#define LACHESIS_STDERR_WRITE(buf, len) write(STDERR_FILENO, buf, len)
#endif

#include <SDL3/SDL.h>

#include <libavutil/log.h>

#include "lachesis_log.h"

#define LOG_LINE_MAX 4096

static void log_sanitize(char *line, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)line[i];

        if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7f) {
            line[i] = '?';
            continue;
        }
        if (c == 0xc2 && i + 1 < len && (unsigned char)line[i + 1] >= 0x80 &&
            (unsigned char)line[i + 1] <= 0x9f) {
            line[i] = line[i + 1] = '?';
            i++;
        }
    }
}

static char log_av_prev[LOG_LINE_MAX];
static const void *log_av_prev_ctx;
static int log_av_prev_level;
static int log_av_open;
static int log_repeats;
static int log_skip_repeats = 1;
static int log_stderr_tty;

static SDL_Mutex *log_mutex;

static void log_lock(void) {
    if (log_mutex) {
        SDL_LockMutex(log_mutex);
    }
}

static void log_unlock(void) {
    if (log_mutex) {
        SDL_UnlockMutex(log_mutex);
    }
}

void log_set_skip_repeated(int skip) {
    log_lock();
    log_skip_repeats = skip;
    log_unlock();
}

#define LOG_STATUS_MAX 256
#define LOG_STATUS_TAG "INFO: "

enum {
    LOG_LIVE_NONE,
    LOG_LIVE_STATUS,
    LOG_LIVE_REPEATS,
};

static char log_status_text[LOG_STATUS_MAX];
static size_t log_live_shown;
static volatile sig_atomic_t log_live;

int log_status_available(void) {
    return log_stderr_tty && !lachesis_quiet;
}

static size_t log_status_budget(void) {
    int columns = 0;

#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);

    if (handle != INVALID_HANDLE_VALUE &&
        GetConsoleScreenBufferInfo(handle, &info)) {
        columns = info.srWindow.Right - info.srWindow.Left + 1;
    }
#else
    struct winsize ws;

    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0) {
        columns = ws.ws_col;
    }
#endif

    return columns > 1 ? (size_t)columns - 1 : 0;
}

// All of these expect the caller to hold log_mutex.
static void log_live_erase(void) {
    size_t cols = log_live_shown;
    size_t budget;
    size_t i;

    if (log_live == LOG_LIVE_NONE) {
        return;
    }
    budget = log_status_budget();
    if (budget && cols > budget) {
        cols = budget;
    }
    fputc('\r', stderr);
    for (i = 0; i < cols; i++) {
        fputc(' ', stderr);
    }
    fputc('\r', stderr);
    log_live_shown = 0;
    log_live = LOG_LIVE_NONE;
}

static void log_live_draw(int what, const char *tag, const char *text) {
    size_t tag_len = strlen(tag);
    size_t len = strlen(text);
    size_t budget, cols;
    size_t i;

    if (log_av_open) {
        fputc('\n', stderr);
        log_av_open = 0;
    }
    budget = log_status_budget();
    if (budget) {
        if (budget <= tag_len) {
            return;
        }
        if (len > budget - tag_len) {
            len = budget - tag_len;
        }
    }
    cols = tag_len + len;
    fputc('\r', stderr);
    fputs(tag, stderr);
    fwrite(text, 1, len, stderr);
    for (i = cols; i < log_live_shown; i++) {
        fputc(' ', stderr);
    }
    for (i = cols; i < log_live_shown; i++) {
        fputc('\b', stderr);
    }
    log_live_shown = cols;
    log_live = what;
    fflush(stderr);
}

static void log_status_draw(void) {
    if (log_status_text[0] && log_status_available()) {
        log_live_draw(LOG_LIVE_STATUS, LOG_STATUS_TAG, log_status_text);
    }
}

static void log_repeats_draw(void) {
    char text[64];

    snprintf(text, sizeof(text), "    Last message repeated %d times", log_repeats);
    log_live_draw(LOG_LIVE_REPEATS, "", text);
}

static void log_repeats_finish(void) {
    if (!log_repeats) {
        return;
    }
    log_live_erase();
    fprintf(stderr, "    Last message repeated %d times\n", log_repeats);
    log_repeats = 0;
}

void log_finish_line(void) {
    log_lock();
    log_repeats_finish();
    log_unlock();
}

void log_status_set(const char *text) {
    if (!text) {
        text = "";
    }
    log_lock();
    if (strcmp(text, log_status_text)) {
        snprintf(log_status_text, sizeof(log_status_text), "%s", text);
        if (log_status_text[0]) {
            log_status_draw();
        } else if (log_live == LOG_LIVE_STATUS) {
            log_live_erase();
        }
    }
    log_unlock();
}

int log_status_finish(void) {
    int drawn;

    log_lock();
    log_repeats_finish();
    if (log_live != LOG_LIVE_STATUS) {
        log_status_draw();
    }
    if ((drawn = log_live == LOG_LIVE_STATUS)) {
        fputc('\n', stderr);
        fflush(stderr);
        log_live_shown = 0;
        log_live = LOG_LIVE_NONE;
        log_status_text[0] = '\0';
    }
    log_unlock();

    return drawn;
}

void log_status_break(void) {
    if (log_live == LOG_LIVE_NONE) {
        return;
    }
    log_live = LOG_LIVE_NONE;
    if (LACHESIS_STDERR_WRITE("\n", 1) < 0) {
        // Nothing to do.
    }
}

void log_vline(const char *tag, const char *fmt, va_list ap) {
    char line[LOG_LINE_MAX];
    int n;

    if (lachesis_quiet) {
        return;
    }
    log_lock();
    log_repeats_finish();
    log_live_erase();
    if (log_av_open) {
        fputc('\n', stderr);
        log_av_open = 0;
    }
    log_av_prev[0] = '\0';
    n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n >= 0) {
        if ((size_t)n >= sizeof(line)) {
            n = (int)sizeof(line) - 1;
        }
        log_sanitize(line, (size_t)n);
        fputs(tag, stderr);
        fwrite(line, 1, (size_t)n, stderr);
    }
    log_status_draw();
    log_unlock();
}

static _Thread_local int (*log_interrupt_cb)(void *);
static _Thread_local void *log_interrupt_ctx;

static av_printf_format(3, 4) void log_av_default(void *avcl, int level,
                                                  const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    av_log_default_callback(avcl, level, fmt, ap);
    va_end(ap);
}

static void log_av_callback(void *avcl, int level, const char *fmt, va_list ap) {
    int plain = level & 0xff;
    char line[LOG_LINE_MAX];
    int n;

    if (level >= 0 && plain >= AV_LOG_ERROR && plain <= AV_LOG_WARNING &&
        log_interrupt_cb && log_interrupt_cb(log_interrupt_ctx)) {
        level = (level & ~0xff) | AV_LOG_VERBOSE;
        plain = AV_LOG_VERBOSE;
    }
    if (level >= 0 && plain > av_log_get_level()) {
        return;
    }
    n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(line)) {
        n = (int)sizeof(line) - 1;
    }
    log_sanitize(line, (size_t)n);

    log_lock();
    if (log_skip_repeats && !log_av_open && n && line[n - 1] == '\n' &&
        avcl == log_av_prev_ctx && level == log_av_prev_level &&
        !strcmp(line, log_av_prev)) {
        log_repeats++;
        if (log_stderr_tty) {
            log_repeats_draw();
        }
        log_unlock();
        return;
    }
    log_repeats_finish();
    log_live_erase();
    memcpy(log_av_prev, line, (size_t)n + 1);
    log_av_prev_ctx = avcl;
    log_av_prev_level = level;

    log_av_default(avcl, level, "%s", line);
    if (n) {
        log_av_open = line[n - 1] != '\n';
    }
    if (!log_av_open) {
        log_status_draw();
    }
    log_unlock();
}

void log_init(void) {
    log_stderr_tty = LACHESIS_STDERR_ISATTY();
    log_mutex = SDL_CreateMutex();
    if (!log_mutex) {
        // Is this reachable anywhere?
    }
    av_log_set_callback(log_av_callback);
}

void log_interrupt_begin(int (*cb)(void *), void *ctx) {
    log_interrupt_cb = cb;
    log_interrupt_ctx = ctx;
}

void log_interrupt_end(void) {
    log_interrupt_cb = NULL;
    log_interrupt_ctx = NULL;
}
