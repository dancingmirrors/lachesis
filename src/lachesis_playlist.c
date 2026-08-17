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

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
/* clang-format off */
#include <io.h>
#include <windows.h>
/* clang-format on */
#else
#include <unistd.h>
#endif

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avstring.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/lfg.h>
#include <libavutil/log.h>
#include <libavutil/macros.h>
#include <libavutil/mem.h>
#include <libavutil/random_seed.h>

#include "lachesis_alloc.h"
#include "lachesis_archive.h"
#include "lachesis_log.h"
#include "lachesis_options.h"

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

static int is_separator(char c) {
    return c == '/' || c == '\\';
}

static int is_absolute_path(const char *p) {
    if (is_separator(p[0])) {
        return 1;
    }
    if (is_alpha(p[0]) && p[1] == ':') {
        return 1;
    }

    return 0;
}

static int is_printable(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c < 0x20 || c == 0x7f) {
            return 0;
        }
    }

    return 1;
}

const char *playlist_protocol_whitelist(const char *url) {
    const char *proto;
    char scheme[16];

    if (has_url_scheme(url)) {
        size_t len = strcspn(url, ":");

        if (len >= sizeof(scheme)) {
            return NULL;
        }
        memcpy(scheme, url, len);
        scheme[len] = '\0';
        proto = scheme;
    } else {
        proto = avio_find_protocol_name(url);
    }

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

static PlaylistEntry *playlist_entries;

int playlist_size;
int playlist_pos;
int playlist_nav_dir = 1;

const PlaylistEntry *playlist_get(int pos) {
    if (pos < 0 || pos >= playlist_size) {
        return NULL;
    }

    return &playlist_entries[pos];
}

static int playlist_add(char *display_path, char *archive_path, char *entry_name,
                        int from_playlist) {
    PlaylistEntry *tmp;

    if (!display_path || (!!archive_path != !!entry_name)) {
        goto fail;
    }
    tmp = av_realloc_array(playlist_entries, playlist_size + 1,
                           sizeof(*playlist_entries));
    if (!tmp) {
        goto fail;
    }
    playlist_entries = tmp;

    PlaylistEntry *e = &playlist_entries[playlist_size++];
    e->display_path = display_path;
    e->archive_path = archive_path;
    e->entry_name = entry_name;
    e->from_playlist = from_playlist;

    return 0;

fail:
    av_free(display_path);
    av_free(archive_path);
    av_free(entry_name);

    return AVERROR(ENOMEM);
}

static int playlist_add_file(const char *path, int from_playlist) {
    return playlist_add(av_strdup(path), NULL, NULL, from_playlist);
}

static int playlist_add_archive_entry(const char *archive_path,
                                      const char *entry_name) {
    return playlist_add(av_asprintf("%s|%s", archive_path, entry_name),
                        av_strdup(archive_path), av_strdup(entry_name), 0);
}

static void playlist_truncate(int size) {
    while (playlist_size > size) {
        PlaylistEntry *e = &playlist_entries[--playlist_size];

        av_freep(&e->display_path);
        av_freep(&e->archive_path);
        av_freep(&e->entry_name);
    }
}

void playlist_remove_at(int pos) {
    if (pos < 0 || pos >= playlist_size) {
        return;
    }

    PlaylistEntry *e = &playlist_entries[pos];
    av_free(e->display_path);
    av_free(e->archive_path);
    av_free(e->entry_name);
    memmove(e, e + 1, (playlist_size - pos - 1) * sizeof(*playlist_entries));
    playlist_size--;
}

void playlist_clear(void) {
    playlist_truncate(0);
    av_freep(&playlist_entries);
}

/* A Fisher-Yates shuffle. */
void playlist_shuffle(void) {
    if (playlist_size < 2) {
        return;
    }

    AVLFG lfg;
    av_lfg_init(&lfg, av_get_random_seed());

    for (int i = playlist_size - 1; i > 0; i--) {
        int j = av_lfg_get(&lfg) % (unsigned)(i + 1);
        PlaylistEntry tmp = playlist_entries[i];
        playlist_entries[i] = playlist_entries[j];
        playlist_entries[j] = tmp;
    }
}

void playlist_reverse(void) {
    if (playlist_size < 2) {
        return;
    }

    for (int i = 0, j = playlist_size - 1; i < j; i++, j--) {
        PlaylistEntry tmp = playlist_entries[i];
        playlist_entries[i] = playlist_entries[j];
        playlist_entries[j] = tmp;
    }
}

static int path_is_readable(const char *path) {
#if defined(_WIN32)
    return _access(path, 4) == 0;
#else
    return access(path, R_OK) == 0;
#endif
}

int playlist_path_is_usable(const char *path) {
    const char *proto;
    struct stat st;

    if (!path || !*path) {
        return 0;
    }
    if (!strcmp(path, "-")) {
        return 1;
    }
    proto = avio_find_protocol_name(path);
    if (!proto || strcmp(proto, "file")) {
        return 1;
    }
    if (!strncmp(path, "file:", 5)) {
        path += 5;
    }

    return stat(path, &st) == 0;
}

int playlist_entry_is_reachable(int pos) {
    const PlaylistEntry *e = playlist_get(pos);
    const char *path;
    const char *proto;
    struct stat st;
    int err;

    if (!e) {
        return 0;
    }
    path = e->archive_path ? e->archive_path : e->display_path;
    if (!path) {
        return 1;
    }
    proto = avio_find_protocol_name(path);
    if (!proto || strcmp(proto, "file")) {
        return 1;
    }
    if (!strncmp(path, "file:", 5)) {
        path += 5;
    }

    if (stat(path, &st) != 0) {
        err = AVERROR(errno);
    } else if (S_ISDIR(st.st_mode)) {
        err = AVERROR(EISDIR);
    } else if (!path_is_readable(path)) {
        err = AVERROR(errno);
    } else {
        return 1;
    }
    av_log(NULL, AV_LOG_ERROR, "%s: %s\n", e->display_path, av_err2str(err));

    return 0;
}

static int entry_is_inherited_terminal(const char *url, const char *proto) {
#if defined(_WIN32)
    (void)url;
    (void)proto;

    return 0;
#else
    const char *spec = url;
    char *end;
    long fd;

    if (!strcmp(proto, "fd")) {
        av_strstart(spec, "fd:", &spec);
        if (*spec) {
            return 0;
        }
        fd = STDIN_FILENO;
    } else if (!strcmp(proto, "pipe")) {
        av_strstart(spec, "pipe:", &spec);
        fd = strtol(spec, &end, 10);
        if (!*spec) {
            fd = STDIN_FILENO;
        } else if (end == spec || *end || fd < 0 || fd > INT_MAX) {
            return 0;
        }
    } else {
        return 0;
    }

    return isatty((int)fd);
#endif
}

void playlist_drop_character_devices(int format_forced) {
    if (format_forced) {
        return;
    }

    for (int i = playlist_size - 1; i >= 0; i--) {
        const PlaylistEntry *e = &playlist_entries[i];
        const char *path = e->archive_path ? e->archive_path : e->display_path;
        const char *proto;
        struct stat st;

        if (!path) {
            continue;
        }
        proto = avio_find_protocol_name(path);
        if (!proto) {
            continue;
        }
        if (strcmp(proto, "file")) {
            if (!entry_is_inherited_terminal(path, proto)) {
                continue;
            }
            playlist_remove_at(i);
            continue;
        }
        if (!strncmp(path, "file:", 5)) {
            path += 5;
        }
        if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode)) {
            continue;
        }
        playlist_remove_at(i);
    }
}

typedef struct PlaylistParser PlaylistParser;
typedef int (*PlaylistSink)(PlaylistParser *p, const char *location);

#define PLAYLIST_DEMUXED (1 << 0)
#define PLAYLIST_DIRECTIVES (1 << 1)

typedef struct PlaylistFormat {
    const char *name;
    const char *const *extensions;
    int (*parse)(PlaylistParser *p);
    int flags;
} PlaylistFormat;

struct PlaylistParser {
    const char *name;
    const PlaylistFormat *fmt;
    char *buf;
    size_t len;
    size_t pos;
    int lineno;
    int warnings;
    int count;
    int stopped;
    PlaylistSink sink;
    void *opaque;
};

static void parser_init(PlaylistParser *p, const char *name,
                        const PlaylistFormat *fmt, char *buf,
                        size_t len, PlaylistSink sink, void *opaque) {
    memset(p, 0, sizeof(*p));
    p->name = name;
    p->fmt = fmt;
    p->buf = buf;
    p->len = len;
    /* A UTF-8 BOM. */
    p->pos = (len >= 3 && !memcmp(buf, "\xef\xbb\xbf", 3)) ? 3 : 0;
    p->sink = sink;
    p->opaque = opaque;
}

static void parser_skip(PlaylistParser *p, const char *why, const char *what) {
    if (p->warnings++ >= UNSAFE_MAX_WARNINGS) {
        return;
    }
    if (p->lineno) {
        log_warn("%s:%d: skipping %s%s%s.\n", p->name, p->lineno, why,
                 what ? ": " : "", what ? what : "");
    } else {
        log_warn("%s: skipping %s%s%s.\n", p->name, why,
                 what ? ": " : "", what ? what : "");
    }
}

static void parser_summarize(const PlaylistParser *p) {
    if (p->warnings > UNSAFE_MAX_WARNINGS) {
        log_warn("%s: %d further entries were skipped.\n",
                 p->name, p->warnings - UNSAFE_MAX_WARNINGS);
    }
}

static char *parser_line(PlaylistParser *p) {
    while (p->pos < p->len) {
        size_t end = p->pos;
        while (end < p->len && p->buf[end] != '\n') {
            end++;
        }

        char *line = p->buf + p->pos;
        size_t line_len = end - p->pos;
        p->pos = end < p->len ? end + 1 : p->len;
        p->lineno++;

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

        if (line_len) {
            return line;
        }
    }

    return NULL;
}

static int parser_add(PlaylistParser *p, const char *location) {
    size_t len = strlen(location);
    int ret;

    if (!len) {
        return 0;
    }
    if (len > UNSAFE_MAX_LINE || !is_printable(location, len)) {
        parser_skip(p, "a malformed entry", NULL);
        return 0;
    }
    /* "\\host\share" and "//host/share" would leak credentials to a peer. */
    if (is_separator(location[0]) && is_separator(location[1])) {
        parser_skip(p, "a network share", location);
        return 0;
    }
    if (p->count >= UNSAFE_MAX_ENTRIES) {
        log_warn("%s: stopping after %d entries.\n", p->name, UNSAFE_MAX_ENTRIES);
        p->stopped = 1;
        return 0;
    }

    ret = p->sink(p, location);
    if (ret > 0) {
        p->count++;
    }

    return ret;
}

#define XML_NAME_MAX 32
#define XML_DEPTH_MAX 16

typedef struct XmlScanner {
    const char *pos;
    const char *end;
} XmlScanner;

typedef struct XmlElement {
    char name[XML_NAME_MAX];
    const char *attrs;
    size_t attrs_len;
    int closing;
    int empty;
} XmlElement;

static void xml_name(char *dst, size_t dst_size, const char *src, size_t len) {
    for (size_t i = len; i > 0; i--) {
        if (src[i - 1] == ':') {
            src += i;
            len -= i;
            break;
        }
    }
    if (len > dst_size - 1) {
        len = dst_size - 1;
    }
    for (size_t i = 0; i < len; i++) {
        dst[i] = (char)av_tolower((unsigned char)src[i]);
    }
    dst[len] = '\0';
}

static const char *xml_find(const char *s, size_t len, const char *marker) {
    size_t n = strlen(marker);

    while (len >= n) {
        const char *hit = memchr(s, marker[0], len - n + 1);
        if (!hit) {
            break;
        }
        if (!memcmp(hit, marker, n)) {
            return hit;
        }
        len -= (size_t)(hit - s) + 1;
        s = hit + 1;
    }

    return NULL;
}

static void xml_skip_past(XmlScanner *s, const char *marker) {
    const char *hit = xml_find(s->pos, (size_t)(s->end - s->pos), marker);

    s->pos = hit ? hit + strlen(marker) : s->end;
}

static size_t utf8_encode(char *out, size_t out_size, unsigned cp) {
    size_t n = cp < 0x80 ? 1 : cp < 0x800 ? 2
        : cp < 0x10000                    ? 3
                                          : 4;

    if (n > out_size) {
        return 0;
    }
    switch (n) {
    case 1:
        out[0] = (char)cp;
        break;
    case 2:
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        break;
    case 3:
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        break;
    default:
        out[0] = (char)(0xf0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[3] = (char)(0x80 | (cp & 0x3f));
        break;
    }

    return n;
}

static const struct {
    const char *name;
    char c;
} xml_entities[] = {
    {"amp", '&'},
    {"lt", '<'},
    {"gt", '>'},
    {"quot", '"'},
    {"apos", '\''},
};

static void xml_decode(const char *src, size_t len, char *out, size_t out_size) {
    size_t o = 0;

    for (size_t i = 0; i < len && o + 1 < out_size;) {
        unsigned cp = 0;
        size_t j;

        if (src[i] != '&') {
            out[o++] = src[i++];
            continue;
        }
        for (j = i + 1; j < len && j - i <= 12 && src[j] != ';'; j++) {
            ;
        }
        if (j >= len || src[j] != ';') {
            out[o++] = src[i++];
            continue;
        }

        const char *name = src + i + 1;
        size_t name_len = j - i - 1;

        if (name_len > 1 && name[0] == '#') {
            int hex = name[1] == 'x' || name[1] == 'X';
            size_t k = hex ? 2 : 1;

            for (; k < name_len && cp <= 0x10ffff; k++) {
                int digit;

                if (name[k] >= '0' && name[k] <= '9') {
                    digit = name[k] - '0';
                } else if (hex && (name[k] | 0x20) >= 'a' && (name[k] | 0x20) <= 'f') {
                    digit = (name[k] | 0x20) - 'a' + 10;
                } else {
                    break;
                }
                cp = cp * (hex ? 16 : 10) + (unsigned)digit;
            }
            if (k != name_len || !cp || cp > 0x10ffff ||
                (cp >= 0xd800 && cp <= 0xdfff)) {
                out[o++] = src[i++];
                continue;
            }
        } else {
            for (size_t k = 0; k < FF_ARRAY_ELEMS(xml_entities); k++) {
                if (name_len == strlen(xml_entities[k].name) &&
                    !memcmp(name, xml_entities[k].name, name_len)) {
                    cp = (unsigned char)xml_entities[k].c;
                    break;
                }
            }
            if (!cp) {
                out[o++] = src[i++];
                continue;
            }
        }

        size_t n = utf8_encode(out + o, out_size - o - 1, cp);
        if (!n) {
            break;
        }
        o += n;
        i = j + 1;
    }
    out[o] = '\0';
}

static int xml_next(XmlScanner *s, XmlElement *el) {
    while (s->pos < s->end) {
        const char *lt = memchr(s->pos, '<', (size_t)(s->end - s->pos));
        if (!lt) {
            break;
        }
        s->pos = lt + 1;

        size_t left = (size_t)(s->end - s->pos);
        if (left >= 3 && !memcmp(s->pos, "!--", 3)) {
            xml_skip_past(s, "-->");
            continue;
        }
        if (left >= 8 && !memcmp(s->pos, "![CDATA[", 8)) {
            xml_skip_past(s, "]]>");
            continue;
        }
        if (left && (*s->pos == '?' || *s->pos == '!')) {
            xml_skip_past(s, ">");
            continue;
        }

        if (!left) {
            break;
        }
        el->closing = *s->pos == '/';
        if (el->closing) {
            s->pos++;
        }

        const char *name = s->pos;
        while (s->pos < s->end && (unsigned char)*s->pos > ' ' &&
               *s->pos != '>' && *s->pos != '/') {
            s->pos++;
        }
        if (s->pos == name) {
            /* A stray '<'. */
            continue;
        }
        xml_name(el->name, sizeof(el->name), name, (size_t)(s->pos - name));

        const char *attrs = s->pos;
        char quote = 0;
        int at_value = 0;
        while (s->pos < s->end) {
            char c = *s->pos;

            if (quote) {
                if (c == quote) {
                    quote = 0;
                    at_value = 0;
                }
            } else if (c == '>') {
                break;
            } else if (c == '=') {
                at_value = 1;
            } else if (at_value && (c == '"' || c == '\'')) {
                quote = c;
            } else if ((unsigned char)c > ' ') {
                at_value = 0;
            }
            s->pos++;
        }
        el->attrs = attrs;
        el->attrs_len = (size_t)(s->pos - attrs);
        if (s->pos < s->end) {
            s->pos++;
        }

        el->empty = el->closing;
        for (size_t i = el->attrs_len; i > 0 && !el->empty; i--) {
            if ((unsigned char)attrs[i - 1] > ' ') {
                if (attrs[i - 1] == '/') {
                    el->empty = 1;
                    el->attrs_len = i - 1;
                }
                break;
            }
        }

        return 1;
    }
    s->pos = s->end;

    return 0;
}

static int xml_attr(const XmlElement *el, const char *want, char *out,
                    size_t out_size) {
    const char *p = el->attrs;
    const char *end = el->attrs + el->attrs_len;

    while (p < end) {
        while (p < end && (unsigned char)*p <= ' ') {
            p++;
        }

        const char *name = p;
        while (p < end && (unsigned char)*p > ' ' && *p != '=') {
            p++;
        }
        size_t name_len = (size_t)(p - name);
        if (!name_len) {
            p++;
            continue;
        }

        while (p < end && (unsigned char)*p <= ' ') {
            p++;
        }
        if (p >= end || *p != '=') {
            /* An attribute with no value. */
            continue;
        }
        p++;
        while (p < end && (unsigned char)*p <= ' ') {
            p++;
        }

        const char *value = p;
        size_t value_len;
        if (p < end && (*p == '"' || *p == '\'')) {
            char quote = *p++;
            value = p;
            while (p < end && *p != quote) {
                p++;
            }
            value_len = (size_t)(p - value);
            if (p < end) {
                p++;
            }
        } else {
            while (p < end && (unsigned char)*p > ' ' && *p != '>') {
                p++;
            }
            value_len = (size_t)(p - value);
        }

        if (name_len >= 5 && !av_strncasecmp(name, "xmlns", 5) &&
            (name_len == 5 || name[5] == ':')) {
            continue;
        }

        char lower[XML_NAME_MAX];
        xml_name(lower, sizeof(lower), name, name_len);
        if (!strcmp(lower, want)) {
            xml_decode(value, value_len, out, out_size);
            return 1;
        }
    }

    return 0;
}

static void xml_text(const XmlScanner *s, char *out, size_t out_size) {
    const char *start = s->pos;
    const char *stop;

    while (start < s->end && (unsigned char)*start <= ' ') {
        start++;
    }
    int cdata = (size_t)(s->end - start) >= 9 && !memcmp(start, "<![CDATA[", 9);

    if (cdata) {
        start += 9;
        stop = xml_find(start, (size_t)(s->end - start), "]]>");
    } else {
        stop = memchr(start, '<', (size_t)(s->end - start));
    }
    if (!stop) {
        stop = s->end;
    }

    while (start < stop && (unsigned char)*start <= ' ') {
        start++;
    }
    while (stop > start && (unsigned char)stop[-1] <= ' ') {
        stop--;
    }

    if (cdata) {
        size_t len = FFMIN((size_t)(stop - start), out_size - 1);

        memcpy(out, start, len);
        out[len] = '\0';
    } else {
        xml_decode(start, (size_t)(stop - start), out, out_size);
    }
}

typedef struct XmlRule {
    const char *element;
    const char *attribute;
    const char *parent;
} XmlRule;

static int xml_enclosed_by(const char stack[][XML_NAME_MAX], int depth,
                           const char *name) {
    for (int i = 0; i < depth; i++) {
        if (!strcmp(stack[i], name)) {
            return 1;
        }
    }

    return 0;
}

static int parse_xml(PlaylistParser *p, const XmlRule *rules) {
    XmlScanner s = {p->buf + p->pos, p->buf + p->len};
    char stack[XML_DEPTH_MAX][XML_NAME_MAX];
    char location[UNSAFE_MAX_LINE + 2];
    XmlElement el;
    int depth = 0;

    while (!p->stopped && xml_next(&s, &el)) {
        if (el.closing) {
            for (int i = depth; i > 0; i--) {
                if (!strcmp(stack[i - 1], el.name)) {
                    depth = i - 1;
                    break;
                }
            }
            continue;
        }

        for (const XmlRule *r = rules; r->element; r++) {
            int ret;

            if (strcmp(el.name, r->element) ||
                (r->parent && !xml_enclosed_by(stack, depth, r->parent))) {
                continue;
            }
            if (r->attribute) {
                if (!xml_attr(&el, r->attribute, location, sizeof(location))) {
                    continue;
                }
            } else if (el.empty) {
                continue;
            } else {
                xml_text(&s, location, sizeof(location));
            }
            if ((ret = parser_add(p, location)) < 0) {
                return ret;
            }
            break;
        }

        if (!el.empty && depth < XML_DEPTH_MAX) {
            av_strlcpy(stack[depth++], el.name, sizeof(stack[0]));
        }
    }

    return 0;
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

static int parse_lines(PlaylistParser *p) {
    char *line;

    while (!p->stopped && (line = parser_line(p))) {
        int ret;

        if (line[0] == '#') {
            if (is_hls_tag(line)) {
                return AVERROR_INVALIDDATA;
            }
            /* The #EXTINF title is intentionally dropped. */
            continue;
        }
        if ((p->fmt->flags & PLAYLIST_DIRECTIVES) &&
            (!av_strcasecmp(line, "--stop--") || !av_strcasecmp(line, "--live--"))) {
            continue;
        }
        if ((ret = parser_add(p, line)) < 0) {
            return ret;
        }
    }

    return 0;
}

#if defined(_WIN32)
static char *env_lookup(const char *name, size_t name_len) {
    wchar_t wname[128];
    wchar_t wvalue[1024];
    char *out;
    int n;

    n = MultiByteToWideChar(CP_UTF8, 0, name, (int)name_len, wname,
                            FF_ARRAY_ELEMS(wname) - 1);
    if (n <= 0) {
        return NULL;
    }
    wname[n] = L'\0';

    DWORD got = GetEnvironmentVariableW(wname, wvalue, FF_ARRAY_ELEMS(wvalue));
    if (!got || got >= FF_ARRAY_ELEMS(wvalue)) {
        return NULL;
    }

    n = WideCharToMultiByte(CP_UTF8, 0, wvalue, -1, NULL, 0, NULL, NULL);
    if (n <= 0 || !(out = av_malloc((size_t)n))) {
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, wvalue, -1, out, n, NULL, NULL) <= 0) {
        av_freep(&out);
    }

    return out;
}

static int expand_env_vars(const char *src, char *out, size_t out_size) {
    size_t o = 0;
    int expanded = 0;

    for (size_t i = 0; src[i];) {
        const char *close = src[i] == '%' ? strchr(src + i + 1, '%') : NULL;
        char *value = NULL;

        if (close && close > src + i + 1) {
            value = env_lookup(src + i + 1, (size_t)(close - src) - i - 1);
        }
        if (!value) {
            if (o + 1 >= out_size) {
                return 0;
            }
            out[o++] = src[i++];
            continue;
        }

        size_t len = strlen(value);
        if (o + len + 1 >= out_size) {
            av_free(value);
            return 0;
        }
        memcpy(out + o, value, len);
        av_free(value);
        o += len;
        i = (size_t)(close - src) + 1;
        expanded = 1;
    }
    out[o] = '\0';

    return expanded;
}
#endif

typedef struct PlsEntry {
    long index;
    const char *location;
} PlsEntry;

static int pls_cmp(const void *a, const void *b) {
    long ia = ((const PlsEntry *)a)->index;
    long ib = ((const PlsEntry *)b)->index;

    return ia < ib ? -1 : ia > ib;
}

static int parse_pls(PlaylistParser *p) {
    PlsEntry *list = NULL;
    int n = 0, cap = 0, ret = 0;
    char *line;

    while ((line = parser_line(p))) {
        if (line[0] == '[' || line[0] == ';' || line[0] == '#') {
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        char *value = eq + 1;
        while (eq > line && (unsigned char)eq[-1] <= ' ') {
            eq--;
        }
        *eq = '\0';
        while (*value && (unsigned char)*value <= ' ') {
            value++;
        }
        if (av_strncasecmp(line, "File", 4)) {
            /* TitleN and LengthN are intentionally dropped. */
            continue;
        }

        char *rest;
        long index = strtol(line + 4, &rest, 10);
        if (rest == line + 4 || *rest || index < 0) {
            continue;
        }

        if (n >= cap) {
            int new_cap = cap ? cap * 2 : 16;
            PlsEntry *tmp = av_realloc_array(list, new_cap, sizeof(*list));
            if (!tmp) {
                ret = AVERROR(ENOMEM);
                goto done;
            }
            list = tmp;
            cap = new_cap;
        }
        list[n].index = index;
        list[n].location = value;
        n++;
    }

    if (n > 1) {
        qsort(list, n, sizeof(*list), pls_cmp);
    }

    p->lineno = 0;

    for (int i = 0; i < n && !p->stopped; i++) {
        const char *location = list[i].location;
#if defined(_WIN32)
        char expanded[UNSAFE_MAX_LINE + 1];

        if (expand_env_vars(location, expanded, sizeof(expanded))) {
            location = expanded;
        }
#endif
        if ((ret = parser_add(p, location)) < 0) {
            goto done;
        }
    }
    ret = 0;

done:
    av_free(list);

    return ret;
}

static int parse_xspf(PlaylistParser *p) {
    static const XmlRule rules[] = {
        {"location", NULL, "track"},
        {NULL, NULL, NULL},
    };

    return parse_xml(p, rules);
}

static int parse_asx(PlaylistParser *p) {
    static const XmlRule rules[] = {
        {"ref", "href", NULL},
        {NULL, NULL, NULL},
    };

    return parse_xml(p, rules);
}

static int parse_wpl(PlaylistParser *p) {
    static const XmlRule rules[] = {
        {"media", "src", NULL},
        {NULL, NULL, NULL},
    };

    return parse_xml(p, rules);
}

static int parse_b4s(PlaylistParser *p) {
    static const XmlRule rules[] = {
        {"entry", "playstring", NULL},
        {NULL, NULL, NULL},
    };

    return parse_xml(p, rules);
}

static int parse_qtl(PlaylistParser *p) {
    static const XmlRule rules[] = {
        {"embed", "src", NULL},
        {NULL, NULL, NULL},
    };

    return parse_xml(p, rules);
}

static const char *const m3u_ext[] = {"m3u", "m3u8", NULL};
static const char *const ram_ext[] = {"ram", NULL};
static const char *const pls_ext[] = {"pls", NULL};
static const char *const xspf_ext[] = {"xspf", NULL};
static const char *const asx_ext[] = {"asx", "wax", "wvx", "wmx", NULL};
static const char *const wpl_ext[] = {"wpl", NULL};
static const char *const b4s_ext[] = {"b4s", NULL};
static const char *const qtl_ext[] = {"qtl", NULL};

static const PlaylistFormat playlist_formats[] = {
    {"M3U", m3u_ext, parse_lines, PLAYLIST_DEMUXED},
    {"RAM", ram_ext, parse_lines, PLAYLIST_DIRECTIVES},
    {"PLS", pls_ext, parse_pls, 0},
    {"XSPF", xspf_ext, parse_xspf, 0},
    {"ASX", asx_ext, parse_asx, 0},
    {"WPL", wpl_ext, parse_wpl, 0},
    {"B4S", b4s_ext, parse_b4s, 0},
    {"QTL", qtl_ext, parse_qtl, 0},
};

static const PlaylistFormat *playlist_format_for(const char *path) {
    const char *ext = strrchr(path, '.');

    if (!ext) {
        return NULL;
    }
    ext++;

    for (size_t i = 0; i < FF_ARRAY_ELEMS(playlist_formats); i++) {
        const char *const *e = playlist_formats[i].extensions;

        for (; *e; e++) {
            if (!av_strcasecmp(ext, *e)) {
                return &playlist_formats[i];
            }
        }
    }

    return NULL;
}

static int is_supported_playlist(const char *path) {
    return playlist_format_for(path) != NULL;
}

static const char *const media_ext =
    "3gp2,3gpp,3gpp2,asf,divx,dvr-ms,evo,ivf,m1v,m2p,m2t,m2ts,m2v,m4r,mod,"
    "mp2v,mpe,mpeg,mpg,mpv,mpv2,mqv,mts,mxf,ogm,ogv,qt,rm,rmvb,swf,tod,tp,"
    "trp,ts,vob,vro,weba,wmv,wtv,"
    "3ga,adt,adts,aif,aifc,aiff,alac,amr,ape,au,awb,caf,dff,dsf,dtsma,m1a,"
    "mp+,mpc,mpp,oga,opus,ra,rf64,shn,snd,sox,spx,tak,truehd,tta,voc,w64,"
    "wav,wma,wv,"
    "apng,avif,avifs,bmp,dds,dpx,exr,hdr,heic,heif,hif,ico,j2c,jfif,jls,jp2,"
    "jpe,jpeg,jpf,jpg,jpm,jpx,jxl,pam,pbm,pcx,pfm,pgm,pgmyuv,phm,png,pnm,"
    "ppm,psd,qoi,ras,sgi,sun,sunras,tga,tif,tiff,wbmp,webp,xbm,xpm,xwd";

static const char *const non_media_ext =
    "ans,aqt,art,asc,ass,diz,ice,idx,jss,lrc,mpl2,nfo,pjs,rt,sami,sbg,scc,"
    "smi,srt,ssa,stl,sub,sup,ttml,txt,vt,vtt,webvtt";

static const char *const partial_ext =
    "aria2,crdownload,download,opdownload,part,partial,tmp,ytdl";

static int nb_skipped_non_media;

#define MEDIA_EXT_MAX 16

static int is_media_file(const char *path) {
    const char *name = path;
    const char *ext;
    void *opaque = NULL;
    const AVInputFormat *fmt;
    char probe[MEDIA_EXT_MAX + 3];
    size_t len;

    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }
    /* No dot is not the same as no extension. */
    ext = strrchr(name, '.');
    if (!ext) {
        return 1;
    }
    ext++;
    len = strlen(ext);

    if (av_match_name(ext, partial_ext)) {
        const char *end = ext - 1;

        ext = end;
        while (ext > name && ext[-1] != '.') {
            ext--;
        }
        if (ext == name) {
            return 0;
        }
        len = (size_t)(end - ext);
    }
    if (len == 0 || len > MEDIA_EXT_MAX) {
        return 0;
    }
    probe[0] = 'x';
    probe[1] = '.';
    memcpy(probe + 2, ext, len);
    probe[len + 2] = '\0';

    if (av_match_ext(probe, non_media_ext)) {
        return 0;
    }
    if (av_match_ext(probe, media_ext) || is_supported_archive(probe) ||
        is_supported_playlist(probe)) {
        return 1;
    }
    while ((fmt = av_demuxer_iterate(&opaque))) {
        if (fmt->extensions && av_match_ext(probe, fmt->extensions)) {
            return 1;
        }
    }

    return 0;
}

void playlist_warn_unsafe_disabled(const char *path, int open_failed) {
    static int warned;
    const PlaylistFormat *fmt = playlist_format_for(path);

    if (allow_unsafe || warned || !fmt) {
        return;
    }
    if (!open_failed && (fmt->flags & PLAYLIST_DEMUXED)) {
        return;
    }
    warned = 1;
    log_warn("%s: playlists are only expanded with -allow-unsafe.\n", path);
}

static char *read_avio(AVIOContext *avio, const char *name, size_t *out_len) {
    char *buf = av_malloc(UNSAFE_MAX_BYTES + 1);
    int got = 0;

    *out_len = 0;
    if (!buf) {
        return NULL;
    }
    while (got < UNSAFE_MAX_BYTES + 1) {
        int r = avio_read(avio, (unsigned char *)buf + got, UNSAFE_MAX_BYTES + 1 - got);
        if (r <= 0) {
            break;
        }
        got += r;
    }
    if (got > UNSAFE_MAX_BYTES) {
        log_warn("%s: the playlist is larger than %d bytes.\n", name, UNSAFE_MAX_BYTES);
        av_freep(&buf);
        return NULL;
    }
    buf[got] = '\0';
    *out_len = (size_t)got;

    return buf;
}

static int is_regular_file(const char *path) {
#if defined(_WIN32)
    wchar_t *wpath;
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    DWORD attr;

    if (n <= 0) {
        return 0;
    }
    wpath = av_malloc_array((size_t)n, sizeof(*wpath));
    if (!wpath) {
        return 0;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, n) <= 0) {
        av_free(wpath);
        return 0;
    }
    attr = GetFileAttributesW(wpath);
    av_free(wpath);

    return attr != INVALID_FILE_ATTRIBUTES &&
        !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;

    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static char *read_local_file(const char *path, size_t *out_len) {
    AVDictionary *opts = NULL;
    AVIOContext *avio = NULL;
    char *buf;
    int ret;

    *out_len = 0;
    if (!is_regular_file(path)) {
        log_warn("%s: only a regular file can be expanded as a playlist.\n", path);
        return NULL;
    }
    av_dict_set(&opts, "protocol_whitelist", "file", 0);
    ret = avio_open2(&avio, path, AVIO_FLAG_READ, NULL, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        log_warn("%s: %s.\n", path, av_err2str(ret));
        return NULL;
    }

    buf = read_avio(avio, path, out_len);
    avio_closep(&avio);

    return buf;
}

static char *read_archive_member(const char *archive_path, const char *entry_name,
                                 const char *name, size_t *out_len) {
    AVIOContext *avio = archive_entry_open_avio(archive_path, entry_name);
    char *buf;

    *out_len = 0;
    if (!avio) {
        log_warn("%s: could not be read.\n", name);
        return NULL;
    }

    buf = read_avio(avio, name, out_len);
    archive_entry_close_avio(avio);

    return buf;
}

static int playlist_parse(const PlaylistFormat *fmt, const char *name, char *buf,
                          size_t len, PlaylistSink sink, void *opaque) {
    PlaylistParser p;
    int before = playlist_size;
    int ret;

    if (!fmt) {
        return AVERROR_INVALIDDATA;
    }
    parser_init(&p, name, fmt, buf, len, sink, opaque);
    ret = fmt->parse(&p);
    if (ret < 0) {
        playlist_truncate(before);
        return ret;
    }
    parser_summarize(&p);

    if (p.count) {
        log_verbose("%s: %d entries in the %s playlist.\n", name, p.count, fmt->name);
    }

    return p.count ? 0 : AVERROR_INVALIDDATA;
}

typedef struct FileContext {
    const char *path;
    size_t dir_len;
} FileContext;

static int file_sink(PlaylistParser *p, const char *location) {
    const FileContext *ctx = p->opaque;

    if (!playlist_protocol_whitelist(location)) {
        parser_skip(p, "an entry with a disallowed protocol", location);
        return 0;
    }

    char *full = (ctx->dir_len && !has_url_scheme(location) &&
                  !is_absolute_path(location))
        ? av_asprintf("%.*s%s", (int)ctx->dir_len, ctx->path, location)
        : av_strdup(location);
    if (!full) {
        return AVERROR(ENOMEM);
    }
    if (playlist_add(full, NULL, NULL, 1) < 0) {
        return AVERROR(ENOMEM);
    }

    return 1;
}

static int playlist_add_playlist_file(const char *path) {
    const PlaylistFormat *fmt = playlist_format_for(path);
    FileContext ctx = {path, 0};
    size_t len;
    int ret;

    if (has_url_scheme(path)) {
        log_warn("%s: only local playlist files can be expanded.\n", path);
        return AVERROR(EINVAL);
    }

    char *buf = read_local_file(path, &len);
    if (!buf) {
        return AVERROR(EIO);
    }

    for (size_t i = 0; path[i]; i++) {
        if (path[i] == '/' || path[i] == '\\') {
            ctx.dir_len = i + 1;
        }
    }

    ret = playlist_parse(fmt, path, buf, len, file_sink, &ctx);
    av_free(buf);

    return ret;
}

typedef struct ArchiveContext {
    const char *archive_path;
    const char *entry_name;
    size_t base_len;
    char *const *names;
    int nb_names;
} ArchiveContext;

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

static int archive_sink(PlaylistParser *p, const char *location) {
    const ArchiveContext *ctx = p->opaque;
    const char *hit = NULL;

    char *resolved = archive_member_resolve(ctx->entry_name, ctx->base_len, location);
    if (!resolved) {
        parser_skip(p, "an entry that leaves the archive", location);
        return 0;
    }
    for (int i = 0; i < ctx->nb_names; i++) {
        if (!strcmp(ctx->names[i], resolved)) {
            hit = ctx->names[i];
            break;
        }
    }
    av_free(resolved);

    if (!hit) {
        parser_skip(p, "an entry that is not in the archive", location);
        return 0;
    }
    if (playlist_add_archive_entry(ctx->archive_path, hit) < 0) {
        return AVERROR(ENOMEM);
    }

    return 1;
}

static int playlist_add_archive_playlist(const char *archive_path,
                                         const char *entry_name,
                                         char *const *names, int nb_names) {
    const PlaylistFormat *fmt = playlist_format_for(entry_name);
    ArchiveContext ctx = {archive_path, entry_name, 0, names, nb_names};
    size_t len;
    int ret;

    char *name = av_asprintf("%s|%s", archive_path, entry_name);
    char *buf = read_archive_member(archive_path, entry_name,
                                    name ? name : entry_name, &len);
    if (!buf) {
        av_free(name);
        return AVERROR(EIO);
    }

    for (size_t i = 0; entry_name[i]; i++) {
        if (entry_name[i] == '/') {
            ctx.base_len = i + 1;
        }
    }

    ret = playlist_parse(fmt, name ? name : entry_name, buf, len,
                         archive_sink, &ctx);
    av_free(name);
    av_free(buf);

    return ret;
}

static int playlist_add_archive(const char *archive_path, int filter) {
    char **names = NULL;
    int nb_names = 0;
    int added = 0;

    if (archive_list_entries(archive_path, &names, &nb_names) != 0) {
        return -1;
    }
    if (nb_names <= 0) {
        archive_free_entries(names, nb_names);
        return -1;
    }

    for (int i = 0; allow_unsafe && i < nb_names; i++) {
        int before = playlist_size;

        if (!is_supported_playlist(names[i]) ||
            playlist_add_archive_playlist(archive_path, names[i], names, nb_names) < 0) {
            continue;
        }
        added += playlist_size - before;
    }

    if (!added) {
        for (int i = 0; i < nb_names; i++) {
            if (filter && !is_media_file(names[i])) {
                nb_skipped_non_media++;
                continue;
            }
            playlist_add_archive_entry(archive_path, names[i]);
        }
    }
    archive_free_entries(names, nb_names);

    return 0;
}

void playlist_report_filtered(void) {
    if (!nb_skipped_non_media) {
        return;
    }
    log_warn("Skipped %d file%s that did not look like media.\n",
             nb_skipped_non_media, nb_skipped_non_media == 1 ? "" : "s");
    nb_skipped_non_media = 0;
}

int playlist_add_input(const char *path) {
    if (is_supported_archive(path) && playlist_add_archive(path, !all_files) == 0) {
        return 0;
    }
    if (allow_unsafe && is_supported_playlist(path) &&
        playlist_add_playlist_file(path) == 0) {
        return 0;
    }
    playlist_warn_unsafe_disabled(path, 0);

    return playlist_add_file(path, 0);
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static void free_names(char **names, int count) {
    for (int i = 0; i < count; i++) {
        av_free(names[i]);
    }
    av_free(names);
}

void playlist_add_directory(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) {
        return;
    }

    char **names = NULL;
    int n = 0, cap = 0;

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (n >= cap) {
            int new_cap = cap ? cap * 2 : 16;
            char **tmp = av_realloc_array(names, new_cap, sizeof(*names));
            if (!tmp) {
                goto oom;
            }
            names = tmp;
            cap = new_cap;
        }
        if (!(names[n] = av_strdup(ent->d_name))) {
            goto oom;
        }
        n++;
    }
    closedir(d);

    if (n > 1) {
        qsort(names, n, sizeof(*names), cmp_str);
    }

    size_t dir_len = strlen(dir_path);
    while (dir_len > 0 && dir_path[dir_len - 1] == '/') {
        dir_len--;
    }

    for (int i = 0; i < n; i++) {
        char *full = av_asprintf("%.*s/%s", (int)dir_len, dir_path, names[i]);
        struct stat st;

        if (!full) {
            continue;
        }
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            av_free(full);
            continue;
        }
        if (!all_files && !is_media_file(names[i])) {
            nb_skipped_non_media++;
            av_free(full);
            continue;
        }
        if (!is_supported_archive(full) ||
            playlist_add_archive(full, !all_files) != 0) {
            playlist_warn_unsafe_disabled(full, 0);
            playlist_add_file(full, 0);
        }
        av_free(full);
    }
    free_names(names, n);

    return;

oom:
    closedir(d);
    free_names(names, n);
}
