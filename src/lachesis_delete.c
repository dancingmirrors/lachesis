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

#include "lachesis_config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <wchar.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <libavutil/avstring.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

#include "lachesis_delete.h"
#include "lachesis_internal.h"
#include "lachesis_osd.h"

static int delete_is_sep(char c) {
    if (c == '/') {
        return 1;
    }
#if defined(_WIN32)
    if (c == '\\') {
        return 1;
    }
#endif

    return 0;
}

static const char *delete_basename(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (delete_is_sep(*p) && p[1]) {
            base = p + 1;
        }
    }

    return base;
}

const char *delete_current_reason(const VideoState *is) {
    if (!is || !is->filename) {
        return "no file";
    }
    if (is->archive_path) {
        return "archive entry";
    }
    if (is->ytdl_source_url || strstr(is->filename, "://")) {
        return "network stream";
    }

    return NULL;
}

const char *delete_current_name(const VideoState *is) {
    if (!is || !is->filename) {
        return NULL;
    }
    return delete_basename(is->filename);
}

#if defined(_WIN32)

static wchar_t *delete_utf8_to_wchar(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (n <= 0) {
        return NULL;
    }
    wchar_t *w = av_malloc_array((size_t)n, sizeof(*w));
    if (!w) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, w, n) != n) {
        av_free(w);
        return NULL;
    }

    return w;
}

static void delete_win_error(DWORD err, char *buf, size_t bufsz) {
    char *msg = NULL;
    DWORD len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                               (LPSTR)&msg, 0, NULL);
    if (len && msg) {
        while (len > 0 && (msg[len - 1] == '\r' || msg[len - 1] == '\n' || msg[len - 1] == '.' || msg[len - 1] == ' ')) {
            msg[--len] = '\0';
        }
        av_strlcpy(buf, msg, bufsz);
    } else {
        snprintf(buf, bufsz, "error %lu", (unsigned long)err);
    }
    if (msg) {
        LocalFree(msg);
    }
}

static int delete_remove(const char *path) {
    wchar_t *wpath = delete_utf8_to_wchar(path);
    if (!wpath) {
        osd_show_message("Delete failed: bad path encoding");
        return 0;
    }

    DWORD attr = GetFileAttributesW(wpath);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        char msg[256];
        delete_win_error(GetLastError(), msg, sizeof(msg));
        osd_show_message("Delete failed: %s", msg);
        av_free(wpath);
        return 0;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        osd_show_message("Won't delete: not a regular file");
        av_free(wpath);
        return 0;
    }
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
        osd_show_message("Refusing to delete symlink");
        av_free(wpath);
        return 0;
    }

    if (!DeleteFileW(wpath)) {
        char msg[256];
        delete_win_error(GetLastError(), msg, sizeof(msg));
        osd_show_message("Delete failed: %s", msg);
        av_free(wpath);
        return 0;
    }
    av_free(wpath);

    return 1;
}

#else /* !_WIN32 */

static int delete_remove(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        osd_show_message("Delete failed: %s", av_err2str(AVERROR(errno)));
        return 0;
    }
    if (!S_ISREG(st.st_mode)) {
        osd_show_message("Won't delete: not a regular file");
        return 0;
    }
    struct stat lst;
    if (lstat(path, &lst) == 0 && S_ISLNK(lst.st_mode)) {
        osd_show_message("Refusing to delete symlink");
        return 0;
    }

    if (remove(path) != 0) {
        osd_show_message("Delete failed: %s", av_err2str(AVERROR(errno)));
        return 0;
    }

    return 1;
}

#endif /* _WIN32 */

int delete_current_file(VideoState **pis, int keep_paused) {
    VideoState *is = *pis;
    const char *reason = delete_current_reason(is);
    if (reason) {
        osd_show_message("Can't delete: %s", reason);
        return 0;
    }

    char *path = av_strdup(is->filename);
    if (!path) {
        return 0;
    }

    char name[512];
    av_strlcpy(name, delete_basename(path), sizeof(name));

    if (!delete_remove(path)) {
        av_free(path);
        return 0;
    }
    av_free(path);

    playlist_remove_current(pis, keep_paused);
    osd_show_message("Deleted %s", name);

    return 1;
}
