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

#include "lachesis_playlist.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <libavformat/avio.h>
#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/macros.h>
#include <libavutil/mem.h>

#include "lachesis_archive.h"
#include "lachesis_log.h"

#define UNSAFE_MAX_BYTES (1 << 20)
#define UNSAFE_MAX_ENTRIES 10000
#define UNSAFE_MAX_LINE 4096
#define UNSAFE_MAX_WARNINGS 8

#define UNSAFE_FILE_WHITELIST "file,crypto,data"
#define UNSAFE_HTTP_WHITELIST "http,https,httpproxy,tcp,tls,crypto,data"

static const struct {
    const char *scheme;
    const char *whitelist;
} unsafe_protocols[] = {
    {"file", UNSAFE_FILE_WHITELIST},
    {"http", UNSAFE_HTTP_WHITELIST},
    {"https", UNSAFE_HTTP_WHITELIST},
    {"rtmp", "rtmp,rtmps,tcp,tls,crypto,data"},
    {"rtmps", "rtmp,rtmps,tcp,tls,crypto,data"},
    {"rtsp", "rtsp,rtsps,rtp,udp,tcp,tls,crypto,data"},
    {"rtsps", "rtsp,rtsps,rtp,udp,tcp,tls,crypto,data"},
    {"srt", "srt,crypto,data"},
    {"udp", "udp,rtp,crypto,data"},
    {"rtp", "rtp,udp,crypto,data"},
    {"mms", "mms,mmsh,mmst,http,tcp,crypto,data"},
    {"mmsh", "mms,mmsh,mmst,http,tcp,crypto,data"},
    {"mmst", "mms,mmsh,mmst,http,tcp,crypto,data"},
};

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int has_url_scheme(const char *s) {
    if (!is_alpha(s[0])) {
        return 0;
    }
    for (size_t i = 1; s[i]; i++) {
        char c = s[i];

        if (c == ':') {
            return i >= 2;
        }
        if (!is_alpha(c) && !(c >= '0' && c <= '9') &&
            c != '+' && c != '-' && c != '.') {
            return 0;
        }
    }

    return 0;
}

static int is_absolute_path(const char *p) {
    if (p[0] == '/' || p[0] == '\\') {
        return 1;
    }
    if (is_alpha(p[0]) && p[1] == ':') {
        return 1;
    }

    return 0;
}

const char *playlist_protocol_whitelist(const char *url) {
    const char *proto = avio_find_protocol_name(url);

    if (!proto) {
        return NULL;
    }
    for (size_t i = 0; i < FF_ARRAY_ELEMS(unsafe_protocols); i++) {
        if (!av_strcasecmp(proto, unsafe_protocols[i].scheme)) {
            return unsafe_protocols[i].whitelist;
        }
    }

    return NULL;
}

int is_supported_playlist(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) {
        return 0;
    }
    ext++;

    /* XXX: Finish me. */
    return av_strcasecmp(ext, "m3u") == 0 || av_strcasecmp(ext, "m3u8") == 0;
}

static int is_hls_tag(const char *line) {
    static const char *const tags[] = {
        "#EXT-X-STREAM-INF:",
        "#EXT-X-TARGETDURATION:",
        "#EXT-X-MEDIA-SEQUENCE:",
    };

    for (size_t i = 0; i < FF_ARRAY_ELEMS(tags); i++) {
        if (av_strstart(line, tags[i], NULL)) {
            return 1;
        }
    }

    return 0;
}

static int line_is_printable(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c < 0x20 || c == 0x7f) {
            return 0;
        }
    }

    return 1;
}

typedef struct M3uReader {
    const char *name;
    char *buf;
    size_t len;
    size_t pos;
    int lineno;
    int warnings;
    int hls;
} M3uReader;

static void reader_init(M3uReader *r, const char *name, char *buf, size_t len) {
    r->name = name;
    r->buf = buf;
    r->len = len;
    /* A UTF-8 BOM. */
    r->pos = (len >= 3 && !memcmp(buf, "\xef\xbb\xbf", 3)) ? 3 : 0;
    r->lineno = 0;
    r->warnings = 0;
    r->hls = 0;
}

static void reader_skip(M3uReader *r, const char *why, const char *what) {
    if (r->warnings++ >= UNSAFE_MAX_WARNINGS) {
        return;
    }
    if (what) {
        log_warn("%s:%d: skipping %s: '%s'.\n", r->name, r->lineno, why, what);
    } else {
        log_warn("%s:%d: skipping %s.\n", r->name, r->lineno, why);
    }
}

static void reader_summarize(const M3uReader *r) {
    if (r->warnings > UNSAFE_MAX_WARNINGS) {
        log_warn("%s: %d further entries were skipped.\n",
                 r->name, r->warnings - UNSAFE_MAX_WARNINGS);
    }
}

static char *reader_next(M3uReader *r) {
    while (r->pos < r->len) {
        size_t end = r->pos;
        while (end < r->len && r->buf[end] != '\n') {
            end++;
        }

        char *line = r->buf + r->pos;
        size_t line_len = end - r->pos;
        r->pos = end < r->len ? end + 1 : r->len;
        r->lineno++;

        if (line_len && line[line_len - 1] == '\r') {
            /* Tolerate CRLF. */
            line_len--;
        }
        while (line_len && (unsigned char)line[0] <= ' ') {
            line++;
            line_len--;
        }
        while (line_len && (unsigned char)line[line_len - 1] <= ' ') {
            line_len--;
        }
        line[line_len] = '\0';

        if (!line_len) {
            continue;
        }
        if (line[0] == '#') {
            if (is_hls_tag(line)) {
                r->hls = 1;
                return NULL;
            }
            /* The #EXTINF title is intentionally dropped. */
            continue;
        }
        if (line_len > UNSAFE_MAX_LINE || !line_is_printable(line, line_len)) {
            reader_skip(r, "a malformed entry", NULL);
            continue;
        }
        /* "\\host\share" and "//host/share" would leak credentials to a peer. */
        if ((line[0] == '\\' || line[0] == '/') && line[1] == line[0]) {
            reader_skip(r, "a network share", line);
            continue;
        }

        return line;
    }

    return NULL;
}

static int add_entry(PlaylistEntry **entries, int *n, int *cap,
                     char *display_path, char *archive_path, char *entry_name,
                     int from_playlist) {
    if (*n >= *cap) {
        int new_cap = *cap ? *cap * 2 : 16;
        PlaylistEntry *tmp = av_realloc_array(*entries, new_cap, sizeof(**entries));
        if (!tmp) {
            return AVERROR(ENOMEM);
        }
        *entries = tmp;
        *cap = new_cap;
    }

    PlaylistEntry *e = &(*entries)[(*n)++];
    e->display_path = display_path;
    e->archive_path = archive_path;
    e->entry_name = entry_name;
    e->from_playlist = from_playlist;

    return 0;
}

static void free_entries(PlaylistEntry *entries, int n) {
    for (int i = 0; i < n; i++) {
        av_free(entries[i].display_path);
        av_free(entries[i].archive_path);
        av_free(entries[i].entry_name);
    }
    av_free(entries);
}

static char *read_playlist_file(const char *path, size_t *out_len) {
    char *buf = NULL;
    long size;
    size_t got;

    *out_len = 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        log_warn("%s: %s.\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        log_warn("%s: %s.\n", path, strerror(errno));
        goto done;
    }
    if (size > UNSAFE_MAX_BYTES) {
        log_warn("%s: the playlist is larger than %d bytes.\n", path, UNSAFE_MAX_BYTES);
        goto done;
    }

    buf = av_malloc((size_t)size + 1);
    if (!buf) {
        goto done;
    }
    got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    *out_len = got;

done:
    fclose(f);
    return buf;
}

int playlist_from_unsafe(const char *unsafe_path, PlaylistEntry **out, int *count) {
    PlaylistEntry *entries = NULL;
    M3uReader r;
    int n = 0, cap = 0;
    size_t len, dir_len = 0;
    int ret = AVERROR_INVALIDDATA;
    char *line;

    *out = NULL;
    *count = 0;

    if (has_url_scheme(unsafe_path)) {
        log_warn("%s: only local playlist files can be expanded.\n", unsafe_path);
        return AVERROR(EINVAL);
    }

    char *buf = read_playlist_file(unsafe_path, &len);
    if (!buf) {
        return AVERROR(EIO);
    }

    for (size_t i = 0; unsafe_path[i]; i++) {
        if (unsafe_path[i] == '/' || unsafe_path[i] == '\\') {
            dir_len = i + 1;
        }
    }

    reader_init(&r, unsafe_path, buf, len);
    while ((line = reader_next(&r))) {
        if (!playlist_protocol_whitelist(line)) {
            reader_skip(&r, "an entry with a disallowed protocol", line);
            continue;
        }
        if (n >= UNSAFE_MAX_ENTRIES) {
            log_warn("%s: stopping after %d entries.\n", unsafe_path, UNSAFE_MAX_ENTRIES);
            break;
        }

        char *full = (dir_len && !has_url_scheme(line) && !is_absolute_path(line))
            ? av_asprintf("%.*s%s", (int)dir_len, unsafe_path, line)
            : av_strdup(line);
        if (!full || add_entry(&entries, &n, &cap, full, NULL, NULL, 1) < 0) {
            av_free(full);
            ret = AVERROR(ENOMEM);
            goto fail;
        }
    }
    if (r.hls) {
        goto fail;
    }
    reader_summarize(&r);

    if (!n) {
    } else {
        log_verbose("%s: %d playlist entries.\n", unsafe_path, n);
    }

    av_free(buf);
    *out = entries;
    *count = n;

    return 0;

fail:
    free_entries(entries, n);
    av_free(buf);

    return ret;
}

static char *read_archive_member(const char *archive_path, const char *entry_name,
                                 size_t *out_len) {
    *out_len = 0;

    AVIOContext *avio = archive_entry_open_avio(archive_path, entry_name);
    if (!avio) {
        log_warn("%s|%s: could not be read.\n", archive_path, entry_name);
        return NULL;
    }

    char *buf = av_malloc(UNSAFE_MAX_BYTES + 1);
    int got = 0;
    if (!buf) {
        goto fail;
    }
    while (got < UNSAFE_MAX_BYTES + 1) {
        int r = avio_read(avio, (unsigned char *)buf + got, UNSAFE_MAX_BYTES + 1 - got);
        if (r <= 0) {
            break;
        }
        got += r;
    }
    if (got > UNSAFE_MAX_BYTES) {
        log_warn("%s|%s: the playlist is larger than %d bytes.\n",
                 archive_path, entry_name, UNSAFE_MAX_BYTES);
        goto fail;
    }
    buf[got] = '\0';

    archive_entry_close_avio(avio);
    *out_len = (size_t)got;

    return buf;

fail:
    archive_entry_close_avio(avio);
    av_free(buf);

    return NULL;
}

static char *archive_member_resolve(const char *base, size_t base_len,
                                    const char *entry) {
    if (has_url_scheme(entry) || is_absolute_path(entry)) {
        return NULL;
    }

    char *joined = av_asprintf("%.*s%s", (int)base_len, base, entry);
    if (!joined) {
        return NULL;
    }
    char *out = av_mallocz(strlen(joined) + 1);
    if (!out) {
        av_free(joined);
        return NULL;
    }

    size_t len = 0;
    int depth = 0, ok = 1;
    char *save = NULL;
    for (char *c = av_strtok(joined, "/", &save); c;
         c = av_strtok(NULL, "/", &save)) {
        if (!strcmp(c, ".")) {
            continue;
        }
        if (!strcmp(c, "..")) {
            if (!depth) {
                ok = 0;
                break;
            }
            depth--;
            while (len && out[len - 1] != '/') {
                len--;
            }
            if (len) {
                len--;
            }
            out[len] = '\0';
            continue;
        }
        if (len) {
            out[len++] = '/';
        }
        size_t n = strlen(c);
        memcpy(out + len, c, n);
        len += n;
        out[len] = '\0';
        depth++;
    }
    av_free(joined);

    if (!ok || !len) {
        av_freep(&out);
    }

    return out;
}

int playlist_from_archive_unsafe(const char *archive_path, const char *entry_name,
                                 const PlaylistEntry *members, int nb_members,
                                 PlaylistEntry **out, int *count) {
    PlaylistEntry *entries = NULL;
    M3uReader r;
    int n = 0, cap = 0;
    size_t len, base_len = 0;
    int ret = AVERROR_INVALIDDATA;
    char *line;

    *out = NULL;
    *count = 0;

    char *buf = read_archive_member(archive_path, entry_name, &len);
    if (!buf) {
        return AVERROR(EIO);
    }

    for (size_t i = 0; entry_name[i]; i++) {
        if (entry_name[i] == '/') {
            base_len = i + 1;
        }
    }

    char *name = av_asprintf("%s|%s", archive_path, entry_name);
    reader_init(&r, name ? name : entry_name, buf, len);

    while ((line = reader_next(&r))) {
        char *resolved = archive_member_resolve(entry_name, base_len, line);
        if (!resolved) {
            reader_skip(&r, "an entry that leaves the archive", line);
            continue;
        }

        const PlaylistEntry *hit = NULL;
        for (int i = 0; i < nb_members; i++) {
            if (!strcmp(members[i].entry_name, resolved)) {
                hit = &members[i];
                break;
            }
        }
        av_free(resolved);
        if (!hit) {
            reader_skip(&r, "an entry that is not in the archive", line);
            continue;
        }
        if (n >= UNSAFE_MAX_ENTRIES) {
            log_warn("%s: stopping after %d entries.\n", r.name, UNSAFE_MAX_ENTRIES);
            break;
        }

        char *display = av_strdup(hit->display_path);
        char *archive = av_strdup(hit->archive_path);
        char *member = av_strdup(hit->entry_name);
        if (!display || !archive || !member ||
            add_entry(&entries, &n, &cap, display, archive, member, 0) < 0) {
            av_free(display);
            av_free(archive);
            av_free(member);
            ret = AVERROR(ENOMEM);
            goto fail;
        }
    }
    if (r.hls) {
        goto fail;
    }
    reader_summarize(&r);

    if (n) {
        log_verbose("%s: %d playlist entries.\n", r.name, n);
    }

    av_free(name);
    av_free(buf);
    *out = entries;
    *count = n;

    return 0;

fail:
    free_entries(entries, n);
    av_free(name);
    av_free(buf);

    return ret;
}
