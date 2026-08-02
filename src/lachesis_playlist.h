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

#ifndef LACHESIS_PLAYLIST_H
#define LACHESIS_PLAYLIST_H

typedef struct PlaylistEntry {
    char *display_path;
    char *archive_path;
    char *entry_name;
    int from_playlist;
} PlaylistEntry;

int is_supported_playlist(const char *path);

int playlist_from_unsafe(const char *unsafe_path, PlaylistEntry **out, int *count);

int playlist_from_archive_unsafe(const char *archive_path, const char *entry_name,
                                 const PlaylistEntry *members, int nb_members,
                                 PlaylistEntry **out, int *count);

const char *playlist_protocol_whitelist(const char *url);

#endif /* LACHESIS_PLAYLIST_H */
