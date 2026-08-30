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

#include <stdarg.h>
#include <stdio.h>

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

void log_vline(const char *tag, const char *fmt, va_list ap) {
    char line[LOG_LINE_MAX];
    int n;

    if (lachesis_quiet) {
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
    fputs(tag, stderr);
    fwrite(line, 1, (size_t)n, stderr);
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
    log_av_default(avcl, level, "%s", line);
}

void log_init(void) {
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
