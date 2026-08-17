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

extern int playlist_size;
extern int playlist_pos;
extern int playlist_nav_dir;

const PlaylistEntry *playlist_get(int pos);

int playlist_add_input(const char *path);

int playlist_path_is_usable(const char *path);

void playlist_add_directory(const char *dir_path);
void playlist_report_filtered(void);
void playlist_remove_at(int pos);
void playlist_clear(void);
void playlist_shuffle(void);
void playlist_reverse(void);

int playlist_entry_is_reachable(int pos);

void playlist_drop_character_devices(int format_forced);

void playlist_warn_unsafe_disabled(const char *path, int open_failed);

const char *playlist_protocol_whitelist(const char *url);

#endif /* LACHESIS_PLAYLIST_H */
