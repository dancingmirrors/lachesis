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

#ifndef LACHESIS_ARCHIVE_H
#define LACHESIS_ARCHIVE_H

#include <libavformat/avio.h>

typedef struct PlaylistEntry {
    char *display_path;
    char *archive_path;
    char *entry_name;
} PlaylistEntry;

int playlist_from_directory(const char *dir_path,
                            PlaylistEntry **out_ptr, int *count);

int playlist_from_archive(const char *archive_path,
                          PlaylistEntry **out, int *count);

int is_supported_archive(const char *path);

AVIOContext *archive_entry_open_avio(const char *archive_path,
                                     const char *entry_name);

void archive_entry_close_avio(AVIOContext *avio);

#endif // LACHESIS_ARCHIVE_H
