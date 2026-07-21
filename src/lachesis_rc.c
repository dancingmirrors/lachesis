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

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/error.h>
#include <libavutil/mem.h>

#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_rc.h"

#define LACHESIS_RC_NAME ".lachesis.rc"
#define LACHESIS_RC_MAX_BYTES (1 << 20)
#define LACHESIS_RC_PATH_MAX 4096
#define LACHESIS_RC_SRC_MAX (LACHESIS_RC_PATH_MAX + 16)

static int resolve_rc_path(char *buf, size_t size) {
    const char *home;

#ifdef _WIN32
    home = getenv("USERPROFILE");
    if (!home || !home[0]) {
        home = getenv("HOME");
    }
    if (!home || !home[0]) {
        return -1;
    }
    snprintf(buf, size, "%s\\%s", home, LACHESIS_RC_NAME);
#else
    home = getenv("HOME");
    if (!home || !home[0]) {
        return -1;
    }
    snprintf(buf, size, "%s/%s", home, LACHESIS_RC_NAME);
#endif
    return 0;
}

static char *read_entire_file(const char *path, int *err_out) {
    *err_out = 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        *err_out = errno;
        return NULL;
    }

    char *buf = NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        *err_out = errno;
        goto done;
    }
    long size = ftell(f);
    if (size < 0) {
        *err_out = errno;
        goto done;
    }
    if (size > LACHESIS_RC_MAX_BYTES) {
        *err_out = EFBIG;
        goto done;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        *err_out = errno;
        goto done;
    }

    buf = av_malloc((size_t)size + 1);
    if (!buf) {
        *err_out = ENOMEM;
        goto done;
    }

    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';

done:
    fclose(f);
    return buf;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return s;
}

static char *parse_value(char *v) {
    while (*v && isspace((unsigned char)*v)) {
        v++;
    }

    if (*v == '"' || *v == '\'') {
        char *close = strchr(v + 1, *v);
        if (close) {
            *close = '\0';
            return v + 1;
        }
        /* Fall through and take the value literally. */
    }

    int seen = 0;
    for (char *p = v; *p; p++) {
        if (*p == '#' && seen && p > v && isspace((unsigned char)p[-1])) {
            *p = '\0';
            break;
        }
        if (!isspace((unsigned char)*p)) {
            seen = 1;
        }
    }

    char *end = v + strlen(v);
    while (end > v && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return v;
}

static void apply_line(void *optctx, const OptionDef *defs, char *line,
                       const char *path, int lineno) {
    char *s = trim(line);
    if (!*s || *s == '#') {
        return;
    }

    char *eq = strchr(s, '=');
    if (!eq) {
        log_warn("%s:%d: expected 'key = value', ignoring.\n", path, lineno);
        return;
    }
    *eq = '\0';

    char *key = trim(s);
    if (!*key) {
        log_warn("%s:%d: missing option name, ignoring.\n", path, lineno);
        return;
    }
    char *value = parse_value(eq + 1);

    char src[LACHESIS_RC_SRC_MAX];
    snprintf(src, sizeof(src), "%s:%d", path, lineno);
    parse_config_option(optctx, key, value, defs, src);
}

int load_config_file(void *optctx, const OptionDef *defs) {
    char path[LACHESIS_RC_PATH_MAX];
    if (resolve_rc_path(path, sizeof(path)) < 0) {
        return 0;
    }

    int err = 0;
    char *buf = read_entire_file(path, &err);
    if (!buf) {
        if (err && err != ENOENT) {
            log_warn("Could not read %s: %s.\n", path, strerror(err));
        }
        return 0;
    }

    int lineno = 0;
    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        char *line = p;
        if (nl) {
            *nl = '\0';
            p = nl + 1;
        } else {
            p = line + strlen(line);
        }

        /* Tolerate CRLF. */
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') {
            line[len - 1] = '\0';
        }

        apply_line(optctx, defs, line, path, ++lineno);
    }

    av_free(buf);
    return 0;
}
