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

#if !defined(IsReparseTagNameSurrogate)
#define IsReparseTagNameSurrogate(tag) (((tag) & 0x20000000) != 0)
#endif

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

static int delete_path_is_extended(const wchar_t *w) {
    return w[0] == L'\\' && (w[1] == L'\\' || w[1] == L'?') && w[2] == L'?' &&
        w[3] == L'\\';
}

static wchar_t *delete_extended_path(wchar_t *w) {
    if (delete_path_is_extended(w) ||
        (w[0] == L'\\' && w[1] == L'\\' && w[2] == L'.' && w[3] == L'\\')) {
        return w;
    }

    DWORD n = GetFullPathNameW(w, 0, NULL, NULL);
    if (n == 0) {
        return w;
    }
    wchar_t *full = av_malloc_array((size_t)n, sizeof(*full));
    if (!full) {
        return w;
    }
    if (GetFullPathNameW(w, n, full, NULL) == 0) {
        av_free(full);
        return w;
    }

    size_t len = wcslen(full);
    if (len < MAX_PATH || delete_path_is_extended(full)) {
        av_free(w);
        return full;
    }

    /* UNC paths swap their leading "\\" for the "\\?\UNC\" prefix. */
    const wchar_t *prefix = (full[0] == L'\\' && full[1] == L'\\')
        ? L"\\\\?\\UNC\\"
        : L"\\\\?\\";
    const wchar_t *tail = (full[0] == L'\\' && full[1] == L'\\') ? full + 2 : full;
    wchar_t *ext = av_malloc_array(wcslen(prefix) + wcslen(tail) + 1, sizeof(*ext));
    if (!ext) {
        av_free(w);
        return full;
    }
    wcscpy(ext, prefix);
    wcscat(ext, tail);
    av_free(full);
    av_free(w);

    return ext;
}

static wchar_t *delete_wpath(const char *path) {
    wchar_t *w = delete_utf8_to_wchar(path);

    return w ? delete_extended_path(w) : NULL;
}

static void delete_win_fail(DWORD code, char *err, size_t errsz) {
    char *msg = NULL;
    DWORD len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                               (LPSTR)&msg, 0, NULL);

    av_strlcpy(err, "Delete failed: ", errsz);
    if (len && msg) {
        while (len > 0 && (msg[len - 1] == '\r' || msg[len - 1] == '\n' || msg[len - 1] == '.' || msg[len - 1] == ' ')) {
            msg[--len] = '\0';
        }
        av_strlcat(err, msg, errsz);
    } else {
        av_strlcatf(err, errsz, "error %lu", (unsigned long)code);
    }
    if (msg) {
        LocalFree(msg);
    }
}

static int delete_is_symlink(const wchar_t *wpath) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpath, &fd);

    if (h == INVALID_HANDLE_VALUE) {
        return 1;
    }
    FindClose(h);

    return (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        IsReparseTagNameSurrogate(fd.dwReserved0);
}

static int delete_check(const char *path, char *err, size_t errsz) {
    wchar_t *wpath = delete_wpath(path);
    if (!wpath) {
        av_strlcpy(err, "Delete failed: bad path encoding", errsz);
        return 0;
    }

    int ok = 0;
    DWORD attr = GetFileAttributesW(wpath);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        delete_win_fail(GetLastError(), err, errsz);
    } else if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        av_strlcpy(err, "Won't delete: not a regular file", errsz);
    } else if ((attr & FILE_ATTRIBUTE_REPARSE_POINT) && delete_is_symlink(wpath)) {
        av_strlcpy(err, "Refusing to delete symlink", errsz);
    } else {
        ok = 1;
    }
    av_free(wpath);

    return ok;
}

static int delete_unlink(const char *path, char *err, size_t errsz) {
    wchar_t *wpath = delete_wpath(path);
    if (!wpath) {
        av_strlcpy(err, "Delete failed: bad path encoding", errsz);
        return 0;
    }

    int ok = 1;
    if (!DeleteFileW(wpath)) {
        delete_win_fail(GetLastError(), err, errsz);
        ok = 0;
    }
    av_free(wpath);

    return ok;
}

#else /* !_WIN32 */

static int delete_check(const char *path, char *err, size_t errsz) {
    struct stat st;
    if (stat(path, &st) != 0) {
        snprintf(err, errsz, "Delete failed: %s", av_err2str(AVERROR(errno)));
        return 0;
    }
    if (!S_ISREG(st.st_mode)) {
        av_strlcpy(err, "Won't delete: not a regular file", errsz);
        return 0;
    }
    struct stat lst;
    if (lstat(path, &lst) == 0 && S_ISLNK(lst.st_mode)) {
        av_strlcpy(err, "Refusing to delete symlink", errsz);
        return 0;
    }

    return 1;
}

static int delete_unlink(const char *path, char *err, size_t errsz) {
    if (remove(path) != 0) {
        snprintf(err, errsz, "Delete failed: %s", av_err2str(AVERROR(errno)));
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

    char err[256];
    if (!delete_check(path, err, sizeof(err))) {
        osd_show_message("%s", err);
        av_free(path);
        return 0;
    }

    double resume_at;
    int released = playlist_close_current(pis, &resume_at);

    if (!delete_unlink(path, err, sizeof(err))) {
        playlist_reopen_current(pis, keep_paused, resume_at);
        if (released) {
            osd_show_message("%s", err);
        } else {
            osd_show_message("Delete failed: still reading %s", name);
        }
        av_free(path);
        return 0;
    }
    av_free(path);

    playlist_drop_current(pis, keep_paused);
    osd_show_message("Deleted %s", name);

    return 1;
}
