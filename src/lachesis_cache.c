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

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include <libavutil/dict.h>
#include <libavutil/macros.h>
#include <libavutil/mem.h>

#include "lachesis_alloc.h"
#include "lachesis_cache.h"
#include "lachesis_log.h"

#ifdef _WIN32
#include <direct.h>
#include <sys/utime.h>
#include <windows.h>
#define LACHESIS_GETPID() GetCurrentProcessId()
#define LACHESIS_PATH_SEP "\\"
#define lachesis_utime _utime
#else
#include <dirent.h>
#include <unistd.h>
#include <utime.h>
#define LACHESIS_GETPID() getpid()
#define LACHESIS_PATH_SEP "/"
#define lachesis_utime utime
#endif

#define LACHESIS_SHADER_CACHE_LIMIT (64u << 20)

static int lachesis_mkdir(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static void lachesis_mkdir_p(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p == '/'
#ifdef _WIN32
            || *p == '\\'
#endif
        ) {
            char c = *p;
            *p = '\0';
            lachesis_mkdir(path);
            *p = c;
        }
    }
    lachesis_mkdir(path);
}

static int resolve_cache_dir(const AVDictionary *opt, char *buf, size_t size) {
    const AVDictionaryEntry *entry = av_dict_get(opt, "cache_dir", NULL, 0);
    const char *base;

    if (entry && entry->value && entry->value[0]) {
        snprintf(buf, size, "%s", entry->value);
        return 0;
    }
#ifdef _WIN32
    base = getenv("LOCALAPPDATA");
    if (base && base[0]) {
        snprintf(buf, size, "%s\\lachesis", base);
        return 0;
    }
#elif defined(__APPLE__)
    base = getenv("HOME");
    if (base && base[0]) {
        snprintf(buf, size, "%s/Library/Caches/lachesis", base);
        return 0;
    }
#else
    base = getenv("XDG_CACHE_HOME");
    if (base && base[0]) {
        snprintf(buf, size, "%s/lachesis", base);
        return 0;
    }
    base = getenv("HOME");
    if (base && base[0]) {
        snprintf(buf, size, "%s/.cache/lachesis", base);
        return 0;
    }
#endif
    return -1;
}

#define CACHE_KEY_DIGITS 16
#define CACHE_PATH_MAX (4096 + 128)
#define CACHE_TEMP_AGE (24 * 60 * 60)

#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

#define CACHE_FILE_HEAD 40
#define CACHE_COUNT_OFF 12
#define CACHE_KEY_OFF 16
#define CACHE_SIZE_OFF 24
#define CACHE_LOAD_PAD 4

static atomic_uint cache_write_seq;

static void cache_keep(void *data) {
    (void)data;
}

static uint32_t cache_u32(const uint8_t *at) {
    uint32_t v;

    memcpy(&v, at, sizeof(v));

    return v;
}

static uint64_t cache_u64(const uint8_t *at) {
    uint64_t v;

    memcpy(&v, at, sizeof(v));

    return v;
}

static pl_cache_obj cache_unpack(const uint8_t *blob, size_t size, uint64_t key) {
    pl_cache_obj obj = {.key = key};
    uint64_t payload;
    pl_cache one;

    if (size < CACHE_FILE_HEAD) {
        return obj;
    }
    payload = cache_u64(blob + CACHE_SIZE_OFF);
    if (cache_u32(blob + CACHE_COUNT_OFF) != 1 ||
        cache_u64(blob + CACHE_KEY_OFF) != key || !payload ||
        payload > size - CACHE_FILE_HEAD ||
        payload > LACHESIS_SHADER_CACHE_LIMIT) {
        return obj;
    }
    one = pl_cache_create(&pl_cache_default_params);
    if (pl_cache_load(one, blob, size + CACHE_LOAD_PAD) == 1) {
        pl_cache_get(one, &obj);
    }
    pl_cache_destroy(&one);

    return obj;
}

static void lachesis_cache_set_dir(void *priv, pl_cache_obj obj) {
    pl_cache_obj copy = {.key = obj.key,
                         .data = obj.data,
                         .size = obj.size,
                         .free = cache_keep};
    const char *prefix = priv;
    char tmp[CACHE_PATH_MAX + 64];
    char path[CACHE_PATH_MAX];
    pl_cache one;
    FILE *f;
    int ok;

    if (!prefix || !prefix[0]) {
        return;
    }
    snprintf(path, sizeof(path), "%s%016" PRIx64, prefix, obj.key);
    if (!obj.size) {
        remove(path);
        return;
    }
    if ((f = fopen(path, "rb"))) {
        fclose(f);
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.%" PRIuMAX ".%x.tmp", path,
             (uintmax_t)LACHESIS_GETPID(),
             atomic_fetch_add_explicit(&cache_write_seq, 1u,
                                       memory_order_relaxed));
    if (!(f = fopen(tmp, "wb"))) {
        return;
    }
    one = pl_cache_create(&pl_cache_default_params);
    pl_cache_set(one, &copy);
    ok = pl_cache_save_file(one, f) == 1 && !ferror(f);
    pl_cache_destroy(&one);
    if (fclose(f)) {
        ok = 0;
    }
    if (ok) {
#if defined(_WIN32)
        ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
        ok = rename(tmp, path) == 0;
#endif
    }
    if (!ok) {
        remove(tmp);
    }
}

#define CACHE_TOUCH_AGE (24 * 60 * 60)

static void cache_touch(const char *path) {
    struct stat st;

    if (stat(path, &st) ||
        (int64_t)time(NULL) - (int64_t)st.st_mtime < CACHE_TOUCH_AGE) {
        return;
    }
    lachesis_utime(path, NULL);
}

static pl_cache_obj lachesis_cache_get_dir(void *priv, uint64_t key) {
    pl_cache_obj obj = {.key = key};
    const char *prefix = priv;
    char path[CACHE_PATH_MAX];
    uint8_t *blob;
    long size;
    FILE *f;

    if (!prefix || !prefix[0]) {
        return (pl_cache_obj){0};
    }
    snprintf(path, sizeof(path), "%s%016" PRIx64, prefix, key);
    if (!(f = fopen(path, "rb"))) {
        return (pl_cache_obj){0};
    }
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return (pl_cache_obj){0};
    }
    if ((uint64_t)size >
        LACHESIS_SHADER_CACHE_LIMIT + CACHE_FILE_HEAD + CACHE_LOAD_PAD) {
        fclose(f);
        remove(path);
        return (pl_cache_obj){0};
    }
    if (!(blob = av_malloc((size_t)size + CACHE_LOAD_PAD))) {
        fclose(f);
        return (pl_cache_obj){0};
    }
    if (fread(blob, 1, (size_t)size, f) == (size_t)size) {
        memset(blob + size, 0, CACHE_LOAD_PAD);
        obj = cache_unpack(blob, (size_t)size, key);
    }
    av_free(blob);
    fclose(f);
    if (!obj.size) {
        remove(path);
    } else {
        cache_touch(path);
    }

    return obj;
}

struct CacheEntry {
    char *path;
    uint64_t size;
    int64_t used;
};

static int cache_entry_cmp(const void *a, const void *b) {
    const struct CacheEntry *x = a, *y = b;

    if (x->used != y->used) {
        return x->used < y->used ? 1 : -1;
    }

    return 0;
}

#define CACHE_NAME_OBJECT 1
#define CACHE_NAME_TEMP 2

static int cache_name_matches(const ShaderCache *sc, const char *name) {
    size_t leaf = strlen(sc->leaf);
    size_t rest;

    if (strncmp(name, sc->leaf, leaf)) {
        return 0;
    }
    name += leaf;
    for (int i = 0; i < CACHE_KEY_DIGITS; i++) {
        char c = name[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    name += CACHE_KEY_DIGITS;
    if (!name[0]) {
        return CACHE_NAME_OBJECT;
    }
    rest = strlen(name);

    return name[0] == '.' && rest >= 4 && !strcmp(name + rest - 4, ".tmp")
        ? CACHE_NAME_TEMP
        : 0;
}

static void cache_note(struct CacheEntry **entries, unsigned *num,
                       unsigned *have, const char *dir, const char *name,
                       uint64_t size, int64_t used) {
    struct CacheEntry *entry;
    size_t need;

    if (*num == *have) {
        unsigned grow = *have ? *have * 2 : 64;
        struct CacheEntry *bigger =
            av_realloc_array(*entries, grow, sizeof(**entries));

        if (!bigger) {
            return;
        }
        *entries = bigger;
        *have = grow;
    }
    entry = &(*entries)[*num];
    need = strlen(dir) + strlen(LACHESIS_PATH_SEP) + strlen(name) + 1;
    if (!(entry->path = av_malloc(need))) {
        return;
    }
    snprintf(entry->path, need, "%s" LACHESIS_PATH_SEP "%s", dir, name);
    entry->size = size;
    entry->used = used;
    ++*num;
}

/* Any failure here simply leaves the cache directory as large as it was. */
static void cache_prune(const ShaderCache *sc) {
    struct CacheEntry *entries = NULL;
    unsigned have = 0, num = 0;
    int64_t now = (int64_t)time(NULL);
    uint64_t kept = 0;
    int dropped = 0, swept = 0;

    if (!sc->dir || !sc->prefix) {
        return;
    }

#if defined(_WIN32)
    {
        char pattern[CACHE_PATH_MAX];
        WIN32_FIND_DATAA found;
        HANDLE search;

        snprintf(pattern, sizeof(pattern), "%s*", sc->prefix);
        search = FindFirstFileA(pattern, &found);
        if (search == INVALID_HANDLE_VALUE) {
            return;
        }
        do {
            ULARGE_INTEGER used, wrote, size;
            int kind = cache_name_matches(sc, found.cFileName);
            int64_t stamp;

            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !kind) {
                continue;
            }
            used.LowPart = found.ftLastAccessTime.dwLowDateTime;
            used.HighPart = found.ftLastAccessTime.dwHighDateTime;
            wrote.LowPart = found.ftLastWriteTime.dwLowDateTime;
            wrote.HighPart = found.ftLastWriteTime.dwHighDateTime;
            if (wrote.QuadPart > used.QuadPart) {
                used = wrote;
            }
            size.LowPart = found.nFileSizeLow;
            size.HighPart = found.nFileSizeHigh;
            stamp = (int64_t)(used.QuadPart / 10000000ULL) - 11644473600LL;
            if (kind == CACHE_NAME_TEMP) {
                if (now - stamp > CACHE_TEMP_AGE) {
                    char path[CACHE_PATH_MAX];

                    snprintf(path, sizeof(path), "%s" LACHESIS_PATH_SEP "%s",
                             sc->dir, found.cFileName);
                    remove(path);
                    swept++;
                }
                continue;
            }
            cache_note(&entries, &num, &have, sc->dir, found.cFileName,
                       size.QuadPart, stamp);
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
#else
    {
        struct dirent *ent;
        DIR *dir = opendir(sc->dir);

        if (!dir) {
            return;
        }
        while ((ent = readdir(dir))) {
            char path[CACHE_PATH_MAX];
            struct stat st;
            int kind = cache_name_matches(sc, ent->d_name);

            if (!kind) {
                continue;
            }
            snprintf(path, sizeof(path), "%s" LACHESIS_PATH_SEP "%s",
                     sc->dir, ent->d_name);
            if (stat(path, &st) || !S_ISREG(st.st_mode)) {
                continue;
            }
            if (kind == CACHE_NAME_TEMP) {
                if (now - (int64_t)st.st_mtime > CACHE_TEMP_AGE) {
                    remove(path);
                    swept++;
                }
                continue;
            }
            cache_note(&entries, &num, &have, sc->dir, ent->d_name,
                       (uint64_t)st.st_size,
                       FFMAX((int64_t)st.st_atime, (int64_t)st.st_mtime));
        }
        closedir(dir);
    }
#endif

    if (num) {
        qsort(entries, num, sizeof(*entries), cache_entry_cmp);
        for (unsigned i = 0; i < num; i++) {
            if (kept + entries[i].size > LACHESIS_SHADER_CACHE_LIMIT) {
                remove(entries[i].path);
                dropped++;
            } else {
                kept += entries[i].size;
            }
            av_freep(&entries[i].path);
        }
    }
    av_freep(&entries);

    if (dropped) {
        log_verbose("Dropped %d shader cache object%s to stay within budget.\n",
                    dropped, dropped == 1 ? "" : "s");
    }
    if (swept) {
        log_verbose("Swept %d abandoned shader cache temporar%s.\n", swept,
                    swept == 1 ? "y" : "ies");
    }
}

struct CacheMigration {
    ShaderCache *sc;
    int written;
};

static int cache_migrated(const ShaderCache *sc, pl_cache_obj want) {
    pl_cache_obj have = lachesis_cache_get_dir(sc->prefix, want.key);
    int ok = have.data && have.size == want.size &&
        !memcmp(have.data, want.data, want.size);

    if (have.free) {
        have.free(have.data);
    }

    return ok;
}

static void cache_migrate_obj(void *priv, pl_cache_obj obj) {
    struct CacheMigration *migration = priv;
    ShaderCache *sc = migration->sc;
    pl_cache_obj seed = {.key = obj.key, .data = obj.data, .size = obj.size};
    char path[CACHE_PATH_MAX];

    pl_cache_set(sc->cache, &seed);

    if (cache_migrated(sc, obj)) {
        migration->written++;
        return;
    }
    snprintf(path, sizeof(path), "%s%016" PRIx64, sc->prefix, obj.key);
    remove(path);
    lachesis_cache_set_dir(sc->prefix, obj);
    migration->written += cache_migrated(sc, obj);
}

static int cache_migrate_tries(const char *blob, int bump) {
    char path[CACHE_PATH_MAX + 32];
    int tries = 0;
    FILE *f;

    snprintf(path, sizeof(path), "%s.tries", blob);
    if (bump < 0) {
        remove(path);
        return 0;
    }
    if ((f = fopen(path, "rb"))) {
        if (fscanf(f, "%d", &tries) != 1 || tries < 0) {
            tries = 0;
        }
        fclose(f);
    }
    if (!bump) {
        return tries;
    }
    tries += bump;
    if ((f = fopen(path, "wb"))) {
        fprintf(f, "%d\n", tries);
        fclose(f);
    }

    return tries;
}

static int cache_migrate_retire(const char *blob, const char *spent) {
#if defined(_WIN32)
    if (!MoveFileExA(blob, spent, MOVEFILE_REPLACE_EXISTING)) {
        return 0;
    }
#else
    if (rename(blob, spent)) {
        return 0;
    }
#endif
    cache_migrate_tries(blob, -1);

    return 1;
}

#define CACHE_MIGRATE_TRIES 3

static void cache_migrate(ShaderCache *sc, pl_log log) {
    struct CacheMigration migration = {.sc = sc};
    char spent[CACHE_PATH_MAX + 32];
    char path[CACHE_PATH_MAX];
    int loaded, held = 0, tries;
    pl_cache blob;
    FILE *f;

    snprintf(path, sizeof(path), "%.*s.bin",
             (int)(strlen(sc->prefix) - 1), sc->prefix);
    snprintf(spent, sizeof(spent), "%s.unmigrated", path);

    if (cache_migrate_tries(path, 0) >= CACHE_MIGRATE_TRIES) {
        cache_migrate_retire(path, spent);
        return;
    }
    if (!(f = fopen(path, "rb"))) {
        return;
    }
    blob = pl_cache_create(pl_cache_params(
            .log = log,
            .max_total_size = LACHESIS_SHADER_CACHE_LIMIT));
    if (!blob) {
        fclose(f);
        return;
    }
    loaded = pl_cache_load_file(blob, f);
    if (loaded > 0) {
        held = pl_cache_objects(blob);
        log_verbose("Unpacking %d objects from the old shader cache.\n", held);
        pl_cache_iterate(blob, cache_migrate_obj, &migration);
    }
    pl_cache_destroy(&blob);
    fclose(f);

    if (loaded <= 0 || migration.written == held) {
        remove(path);
        cache_migrate_tries(path, -1);
        return;
    }

    tries = cache_migrate_tries(path, 1);
    if (tries < CACHE_MIGRATE_TRIES) {
        log_warn("Kept the old shader cache because only %d of its %d objects could "
                 "be unpacked.\n",
                 migration.written, held);
        return;
    }
    if (!cache_migrate_retire(path, spent)) {
        log_warn("Could not unpack %d of the old shader cache's %d objects.\n",
                 held - migration.written, held);
        return;
    }
    log_warn("Gave up unpacking the old shader cache after %d attempts. Keeping it "
             "as %s.\n",
             tries, spent);
}

/* Any failure simply leaves rendering uncached. */
void shader_cache_open(ShaderCache *sc, pl_gpu gpu, pl_log log,
                       const char *backend, const AVDictionary *opt) {
    char dir[4096];
    const AVDictionaryEntry *entry = av_dict_get(opt, "cache", NULL, 0);
    int enabled = entry && entry->value ? strtol(entry->value, NULL, 10) : 1;
    size_t need;

    if (!enabled || !gpu) {
        return;
    }
    if (resolve_cache_dir(opt, dir, sizeof(dir)) < 0) {
        return;
    }
    lachesis_mkdir_p(dir);

    snprintf(sc->leaf, sizeof(sc->leaf), "shaders-%s_", backend);
    need = strlen(dir) + strlen(LACHESIS_PATH_SEP) + strlen(sc->leaf) + 1;
    if (!(sc->dir = av_strdup(dir)) ||
        !(sc->prefix = av_malloc(need))) {
        av_freep(&sc->dir);
        return;
    }
    snprintf(sc->prefix, need, "%s" LACHESIS_PATH_SEP "%s", dir,
             sc->leaf);

    sc->cache = pl_cache_create(pl_cache_params(
            .log = log,
            .max_total_size = LACHESIS_SHADER_CACHE_LIMIT,
            .get = lachesis_cache_get_dir,
            .set = lachesis_cache_set_dir,
            .priv = sc->prefix));
    if (!sc->cache) {
        av_freep(&sc->dir);
        av_freep(&sc->prefix);
        return;
    }

    cache_migrate(sc, log);
    cache_prune(sc);

    pl_gpu_set_cache(gpu, sc->cache);
}

void shader_cache_close(ShaderCache *sc, pl_gpu gpu) {
    if (gpu) {
        pl_gpu_set_cache(gpu, NULL);
    }
    pl_cache_destroy(&sc->cache);
    cache_prune(sc);
    av_freep(&sc->dir);
    av_freep(&sc->prefix);
}
