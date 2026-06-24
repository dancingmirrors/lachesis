/*
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

#include "lachesis_archive.h"

#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <libavformat/avio.h>
#include <libavutil/avstring.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static int entry_cmp(const void *a, const void *b) {
    const PlaylistEntry *ea = (const PlaylistEntry *)a;
    const PlaylistEntry *eb = (const PlaylistEntry *)b;
    return strcmp(ea->display_path, eb->display_path);
}

int playlist_from_directory(const char *dir_path,
                            PlaylistEntry **out_ptr, int *count) {
    DIR *d = opendir(dir_path);
    if (!d) {
        return -1;
    }

    char **names = NULL;
    int n = 0, cap = 0;

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') {
            continue;
        }

        if (n >= cap) {
            cap = cap ? cap * 2 : 16;
            char **tmp = av_realloc_array(names, cap, sizeof(*names));
            if (!tmp) {
                closedir(d);
                for (int i = 0; i < n; i++) {
                    av_free(names[i]);
                }
                av_free(names);
                return AVERROR(ENOMEM);
            }
            names = tmp;
        }
        char *dup = av_strdup(ent->d_name);
        if (!dup) {
            closedir(d);
            for (int i = 0; i < n; i++) {
                av_free(names[i]);
            }
            av_free(names);
            return AVERROR(ENOMEM);
        }
        names[n++] = dup;
    }
    closedir(d);

    qsort(names, n, sizeof(*names), cmp_str);

    PlaylistEntry *entries = av_calloc(n, sizeof(*entries));
    if (!entries) {
        for (int i = 0; i < n; i++) {
            av_free(names[i]);
        }
        av_free(names);
        return AVERROR(ENOMEM);
    }

    size_t dir_len = strlen(dir_path);
    while (dir_len > 0 && dir_path[dir_len - 1] == '/') {
        dir_len--;
    }

    int out = 0;
    for (int i = 0; i < n; i++) {
        size_t len = dir_len + 1 + strlen(names[i]) + 1;
        char *full = av_malloc(len);
        if (!full) {
            av_free(names[i]);
            continue;
        }
        snprintf(full, len, "%.*s/%s", (int)dir_len, dir_path, names[i]);

        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            av_free(full);
            av_free(names[i]);
            continue;
        }

        entries[out].display_path = full;
        entries[out].archive_path = NULL;
        entries[out].entry_name = NULL;
        av_free(names[i]);
        out++;
    }
    av_free(names);

    *out_ptr = entries;
    *count = out;
    return 0;
}

int is_supported_archive(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) {
        return 0;
    }
    ext++;
    return av_strcasecmp(ext, "zip") == 0 ||
        av_strcasecmp(ext, "7z") == 0 ||
        av_strcasecmp(ext, "rar") == 0 ||
        av_strcasecmp(ext, "cbz") == 0 ||
        av_strcasecmp(ext, "cbr") == 0 ||
        av_strcasecmp(ext, "cb7") == 0;
}

int playlist_from_archive(const char *archive_path,
                          PlaylistEntry **out, int *count) {
    struct archive *arch = archive_read_new();
    archive_read_support_filter_all(arch);
    archive_read_support_format_zip(arch);
    archive_read_support_format_rar(arch);
    archive_read_support_format_rar5(arch);
    archive_read_support_format_7zip(arch);

    int r = archive_read_open_filename(arch, archive_path, 65536);
    if (r != ARCHIVE_OK && r != ARCHIVE_WARN) {
        archive_read_free(arch);
        return -1;
    }

    PlaylistEntry *entries = NULL;
    int n = 0, cap = 0;

    struct archive_entry *entry;
    while ((r = archive_read_next_header(arch, &entry)) == ARCHIVE_OK ||
           r == ARCHIVE_WARN) {
        if (archive_entry_filetype(entry) != AE_IFREG) {
            continue;
        }
        const char *name = archive_entry_pathname_utf8(entry);
        if (!name) {
            name = archive_entry_pathname(entry);
        }
        if (!name) {
            continue;
        }

        if (n >= cap) {
            cap = cap ? cap * 2 : 16;
            PlaylistEntry *tmp = av_realloc_array(entries, cap, sizeof(*tmp));
            if (!tmp) {
                goto oom;
            }
            entries = tmp;
        }

        char display[1024];
        snprintf(display, sizeof(display), "%s|%s", archive_path, name);

        entries[n].display_path = av_strdup(display);
        entries[n].archive_path = av_strdup(archive_path);
        entries[n].entry_name = av_strdup(name);
        if (!entries[n].display_path || !entries[n].archive_path ||
            !entries[n].entry_name) {
            av_free(entries[n].display_path);
            av_free(entries[n].archive_path);
            av_free(entries[n].entry_name);
            goto oom;
        }
        n++;
        archive_read_data_skip(arch);
    }

    archive_read_free(arch);

    qsort(entries, n, sizeof(*entries), entry_cmp);

    *out = entries;
    *count = n;
    return 0;

oom:
    for (int i = 0; i < n; i++) {
        av_free(entries[i].display_path);
        av_free(entries[i].archive_path);
        av_free(entries[i].entry_name);
    }
    av_free(entries);
    archive_read_free(arch);
    return AVERROR(ENOMEM);
}

#define ARCHIVE_IO_BUFSIZE 65536

typedef struct {
    char *archive_path;
    char *entry_name;
    struct archive *arch;
    int64_t size;
    int64_t pos;
} ArchiveIO;

static int archive_io_open(ArchiveIO *io) {
    struct archive *arch = archive_read_new();
    archive_read_support_filter_all(arch);
    archive_read_support_format_zip(arch);
    archive_read_support_format_rar(arch);
    archive_read_support_format_rar5(arch);
    archive_read_support_format_7zip(arch);

    int r = archive_read_open_filename(arch, io->archive_path, ARCHIVE_IO_BUFSIZE);
    if (r != ARCHIVE_OK && r != ARCHIVE_WARN) {
        fprintf(stderr, "Cannot open archive '%s': %s!\n", io->archive_path, archive_error_string(arch));
        archive_read_free(arch);
        return -1;
    }

    struct archive_entry *entry;
    while ((r = archive_read_next_header(arch, &entry)) == ARCHIVE_OK ||
           r == ARCHIVE_WARN) {
        if (archive_entry_filetype(entry) != AE_IFREG) {
            continue;
        }
        const char *name = archive_entry_pathname_utf8(entry);
        if (!name) {
            name = archive_entry_pathname(entry);
        }
        if (name && strcmp(name, io->entry_name) == 0) {
            io->arch = arch;
            if (archive_entry_size_is_set(entry)) {
                io->size = archive_entry_size(entry);
            }
            return 0;
        }
        archive_read_data_skip(arch);
    }

    archive_read_free(arch);

    return -1;
}

static void archive_io_close(ArchiveIO *io) {
    if (io->arch) {
        archive_read_free(io->arch);
        io->arch = NULL;
    }
}

static int archive_read_packet(void *opaque, uint8_t *buf, int buf_size) {
    ArchiveIO *io = opaque;
    if (!io->arch) {
        return AVERROR_EOF;
    }

    la_ssize_t r = archive_read_data(io->arch, buf, buf_size);
    if (r == 0) {
        return AVERROR_EOF;
    }
    if (r < 0) {
        return AVERROR(EIO);
    }
    io->pos += r;
    return (int)r;
}

static int64_t archive_seek(void *opaque, int64_t offset, int whence) {
    ArchiveIO *io = opaque;

    if (whence == AVSEEK_SIZE) {
        return io->size;
    }

    int64_t target;
    switch (whence) {
    case SEEK_SET:
        target = offset;
        break;
    case SEEK_CUR:
        target = io->pos + offset;
        break;
    case SEEK_END:
        if (io->size < 0) {
            return -1;
        }
        target = io->size + offset;
        break;
    default:
        return -1;
    }

    if (target < 0) {
        return -1;
    }

    if (target < io->pos) {
        if (archive_seek_data(io->arch, target, SEEK_SET) >= 0) {
            io->pos = target;
            return io->pos;
        }
        archive_io_close(io);
        io->pos = 0;
        if (archive_io_open(io) < 0) {
            return -1;
        }
    }

    /* Seek forward by reading and discarding bytes. */
    if (target > io->pos) {
        uint8_t discard[4096];
        while (io->pos < target) {
            int want = (int)FFMIN((int64_t)sizeof(discard), target - io->pos);
            la_ssize_t r = archive_read_data(io->arch, discard, want);
            if (r <= 0) {
                return -1;
            }
            io->pos += r;
        }
    }

    return io->pos;
}

static void archive_io_free_cb(void *opaque) {
    ArchiveIO *io = opaque;
    archive_io_close(io);
    av_free(io->archive_path);
    av_free(io->entry_name);
    av_free(io);
}

AVIOContext *archive_entry_open_avio(const char *archive_path,
                                     const char *entry_name) {
    ArchiveIO *io = av_mallocz(sizeof(*io));
    if (!io) {
        return NULL;
    }

    io->archive_path = av_strdup(archive_path);
    io->entry_name = av_strdup(entry_name);
    io->size = -1;
    io->pos = 0;

    if (!io->archive_path || !io->entry_name) {
        archive_io_free_cb(io);
        return NULL;
    }

    if (archive_io_open(io) < 0) {
        archive_io_free_cb(io);
        return NULL;
    }

    uint8_t *buf = av_malloc(ARCHIVE_IO_BUFSIZE);
    if (!buf) {
        archive_io_free_cb(io);
        return NULL;
    }

    AVIOContext *avio = avio_alloc_context(buf, ARCHIVE_IO_BUFSIZE,
                                           0,
                                           io,
                                           archive_read_packet,
                                           NULL,
                                           archive_seek);
    if (!avio) {
        av_free(buf);
        archive_io_free_cb(io);
        return NULL;
    }

    return avio;
}

void archive_entry_close_avio(AVIOContext *avio) {
    if (!avio) {
        return;
    }
    archive_io_free_cb(avio->opaque);
    avio->opaque = NULL;
    av_freep(&avio->buffer);
    avio_context_free(&avio);
}
