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

#include <libavutil/log.h>

#include "lachesis_log.h"

static _Thread_local int (*log_interrupt_cb)(void *);
static _Thread_local void *log_interrupt_ctx;

static void log_av_callback(void *avcl, int level, const char *fmt, va_list ap) {
    int plain = level & 0xff;

    if (level >= 0 && plain >= AV_LOG_ERROR && plain <= AV_LOG_WARNING &&
        log_interrupt_cb && log_interrupt_cb(log_interrupt_ctx)) {
        level = (level & ~0xff) | AV_LOG_VERBOSE;
    }

    av_log_default_callback(avcl, level, fmt, ap);
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
