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

#ifndef LACHESIS_LOG_H
#define LACHESIS_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <libavutil/attributes.h>

extern int lachesis_quiet;

/* I disagree with av_log's loglevels so here we are. */
static av_unused av_printf_format(2, 0) void log_vline(const char *tag, const char *fmt, va_list ap) {
    if (lachesis_quiet) {
        return;
    }
    fputs(tag, stderr);
    vfprintf(stderr, fmt, ap);
}

static av_unused av_printf_format(1, 2) void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vline("INFO: ", fmt, ap);
    va_end(ap);
}

static av_unused av_printf_format(1, 2) void log_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vline("WARN: ", fmt, ap);
    va_end(ap);
}

static av_unused av_printf_format(1, 2) void log_dead(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vline("DEAD: ", fmt, ap);
    va_end(ap);
}

/* Like log_dead(), but never returns. */
static av_unused av_printf_format(1, 2) void fatal_quit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vline("DEAD: ", fmt, ap);
    va_end(ap);
    exit(1);
}

#endif /* LACHESIS_LOG_H */
