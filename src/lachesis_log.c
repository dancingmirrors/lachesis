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
#include <io.h>
#include <windows.h>
#define LACHESIS_STDERR_ISATTY() _isatty(_fileno(stderr))
#define LACHESIS_STDERR_WRITE(buf, len) _write(_fileno(stderr), buf, len)
#else
#include <sys/ioctl.h>
#include <unistd.h>
#define LACHESIS_STDERR_ISATTY() isatty(STDERR_FILENO)
#define LACHESIS_STDERR_WRITE(buf, len) write(STDERR_FILENO, buf, len)
#endif

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
static int log_av_repeating;
static int log_stderr_tty;

void log_finish_line(void) {
    if (log_av_repeating) {
        log_av_repeating = 0;
        fputc('\n', stderr);
    }
}

#define LOG_STATUS_MAX 256
#define LOG_STATUS_TAG "INFO: "

static char log_status_text[LOG_STATUS_MAX];
static size_t log_status_shown;
static volatile sig_atomic_t log_status_live;

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

static void log_status_erase(void) {
    size_t cols = log_status_shown;
    size_t budget;
    size_t i;

    if (!log_status_live) {
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
    log_status_shown = 0;
    log_status_live = 0;
}

static void log_status_draw(void) {
    size_t tag = sizeof(LOG_STATUS_TAG) - 1;
    size_t len = strlen(log_status_text);
    size_t budget, cols;
    size_t i;

    if (!len || log_av_repeating || !log_status_available()) {
        return;
    }
    budget = log_status_budget();
    if (budget) {
        if (budget <= tag) {
            return;
        }
        if (len > budget - tag) {
            len = budget - tag;
        }
    }
    cols = tag + len;
    fputc('\r', stderr);
    fputs(LOG_STATUS_TAG, stderr);
    fwrite(log_status_text, 1, len, stderr);
    for (i = cols; i < log_status_shown; i++) {
        fputc(' ', stderr);
    }
    for (i = cols; i < log_status_shown; i++) {
        fputc('\b', stderr);
    }
    log_status_shown = cols;
    log_status_live = 1;
    fflush(stderr);
}

void log_status_set(const char *text) {
    if (!text) {
        text = "";
    }
    if (!strcmp(text, log_status_text)) {
        return;
    }
    snprintf(log_status_text, sizeof(log_status_text), "%s", text);
    if (log_status_text[0]) {
        log_finish_line();
        log_status_draw();
    } else {
        log_status_erase();
    }
}

int log_status_finish(void) {
    log_finish_line();
    if (!log_status_live) {
        log_status_draw();
    }
    if (!log_status_live) {
        return 0;
    }
    fputc('\n', stderr);
    fflush(stderr);
    log_status_shown = 0;
    log_status_live = 0;
    log_status_text[0] = '\0';

    return 1;
}

void log_status_break(void) {
    if (!log_status_live) {
        return;
    }
    log_status_live = 0;
    if (LACHESIS_STDERR_WRITE("\n", 1) < 0) {
        /* Nothing to do. */
    }
}

void log_vline(const char *tag, const char *fmt, va_list ap) {
    char line[LOG_LINE_MAX];
    int n;

    if (lachesis_quiet) {
        return;
    }
    log_status_erase();
    log_finish_line();
    n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n < 0) {
        log_status_draw();
        return;
    }
    if ((size_t)n >= sizeof(line)) {
        n = (int)sizeof(line) - 1;
    }
    log_sanitize(line, (size_t)n);
    fputs(tag, stderr);
    fwrite(line, 1, (size_t)n, stderr);
    log_status_draw();
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

    log_status_erase();
    if (log_stderr_tty && !strcmp(line, log_av_prev) && line[0] &&
        line[n - 1] != '\r') {
        log_av_repeating = 1;
    } else {
        log_av_repeating = 0;
        memcpy(log_av_prev, line, (size_t)n + 1);
    }

    log_av_default(avcl, level, "%s", line);
    log_status_draw();
}

void log_init(void) {
    log_stderr_tty = LACHESIS_STDERR_ISATTY();
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
