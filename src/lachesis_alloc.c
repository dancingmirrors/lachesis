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

#define LACHESIS_ALLOC_INTERNAL

#include "lachesis_alloc.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include <libavutil/macros.h>

#define ALLOC_TABLE_MIN 1024

#define ALLOC_TOMBSTONE ((const void *)(uintptr_t)1)

struct alloc_entry {
    const void *ptr;
    const char *loc;
    size_t size;
};

struct alloc_site {
    const char *loc;
    uint64_t bytes;
    uint64_t count;
};

static SDL_Mutex *alloc_mutex;
static struct alloc_entry *alloc_table;
static size_t alloc_cap;
static size_t alloc_live;
static size_t alloc_dead;
static uint64_t alloc_live_bytes;
static uint64_t alloc_peak_bytes;
static uint64_t alloc_total;
static int alloc_enabled;
static int alloc_reported;
static int alloc_lost;
static int alloc_completed;
static int alloc_aborted;

static size_t alloc_hash(const void *ptr) {
    uint64_t x = (uint64_t)(uintptr_t)ptr;

    x >>= 4;
    x *= UINT64_C(0x9e3779b97f4a7c15);
    x ^= x >> 29;

    return (size_t)x;
}

static size_t alloc_mul(size_t a, size_t b) {
    if (a && b > SIZE_MAX / a) {
        return SIZE_MAX;
    }

    return a * b;
}

static struct alloc_entry *alloc_find(const void *ptr) {
    size_t mask = alloc_cap - 1;
    size_t i = alloc_hash(ptr) & mask;

    for (size_t probe = 0; probe < alloc_cap; probe++) {
        struct alloc_entry *e = &alloc_table[i];

        if (!e->ptr) {
            return NULL;
        }
        if (e->ptr == ptr) {
            return e;
        }
        i = (i + 1) & mask;
    }

    return NULL;
}

static struct alloc_entry *alloc_slot(const void *ptr) {
    size_t mask = alloc_cap - 1;
    size_t i = alloc_hash(ptr) & mask;
    struct alloc_entry *reuse = NULL;

    for (size_t probe = 0; probe < alloc_cap; probe++) {
        struct alloc_entry *e = &alloc_table[i];

        if (!e->ptr) {
            return reuse ? reuse : e;
        }
        if (e->ptr == ALLOC_TOMBSTONE) {
            if (!reuse) {
                reuse = e;
            }
        } else if (e->ptr == ptr) {
            return e;
        }
        i = (i + 1) & mask;
    }

    return reuse;
}

static int alloc_reserve(void) {
    struct alloc_entry *fresh, *stale;
    size_t cap, need, stale_cap;

    if (alloc_cap && (alloc_live + alloc_dead + 1) * 4 <= alloc_cap * 3) {
        return 1;
    }

    need = (alloc_live + 1) * 2;
    cap = ALLOC_TABLE_MIN;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            return 0;
        }
        cap *= 2;
    }

    fresh = calloc(cap, sizeof(*fresh));
    if (!fresh) {
        return 0;
    }

    stale = alloc_table;
    stale_cap = alloc_cap;
    alloc_table = fresh;
    alloc_cap = cap;
    alloc_dead = 0;

    for (size_t i = 0; i < stale_cap; i++) {
        if (stale[i].ptr && stale[i].ptr != ALLOC_TOMBSTONE) {
            struct alloc_entry *e = alloc_slot(stale[i].ptr);

            if (e) {
                *e = stale[i];
            } else {
                alloc_live--;
                alloc_live_bytes -= stale[i].size;
                alloc_lost = 1;
            }
        }
    }
    free(stale);

    return 1;
}

static void alloc_insert(const void *ptr, size_t size, const char *loc, int fresh) {
    struct alloc_entry *e;

    if (!alloc_enabled || !ptr) {
        return;
    }

    SDL_LockMutex(alloc_mutex);

    if (!alloc_reserve()) {
        alloc_lost = 1;
        SDL_UnlockMutex(alloc_mutex);
        return;
    }

    e = alloc_slot(ptr);
    if (!e) {
        alloc_lost = 1;
        SDL_UnlockMutex(alloc_mutex);
        return;
    }

    if (e->ptr == ptr) {
        alloc_live_bytes -= e->size;
    } else {
        if (e->ptr == ALLOC_TOMBSTONE) {
            alloc_dead--;
        }
        alloc_live++;
    }

    e->ptr = ptr;
    e->size = size;
    e->loc = loc;

    alloc_live_bytes += size;
    if (fresh) {
        alloc_total++;
    }
    if (alloc_live_bytes > alloc_peak_bytes) {
        alloc_peak_bytes = alloc_live_bytes;
    }

    SDL_UnlockMutex(alloc_mutex);
}

static void alloc_remember(const void *ptr, size_t size, const char *loc) {
    alloc_insert(ptr, size, loc, 1);
}

static struct alloc_entry alloc_release(const void *ptr) {
    struct alloc_entry old = {0};
    struct alloc_entry *e;

    if (!alloc_enabled || !ptr) {
        return old;
    }

    SDL_LockMutex(alloc_mutex);

    e = alloc_cap ? alloc_find(ptr) : NULL;
    if (e) {
        old = *e;
        alloc_live_bytes -= e->size;
        alloc_live--;
        alloc_dead++;
        e->ptr = ALLOC_TOMBSTONE;
        e->size = 0;
        e->loc = NULL;
    }

    SDL_UnlockMutex(alloc_mutex);

    return old;
}

static void alloc_return(const struct alloc_entry *old) {
    alloc_insert(old->ptr, old->size, old->loc, 0);
}

static void alloc_forget(const void *ptr) {
    alloc_release(ptr);
}

void alloc_track_disown(const void *ptr) {
    alloc_forget(ptr);
}

static int alloc_env_off(const char *env) {
    static const char *const no[] = {"0", "no", "false", "off", "disable", "disabled"};

    if (!env || !*env) {
        return 1;
    }
    for (size_t i = 0; i < FF_ARRAY_ELEMS(no); i++) {
        if (!av_strcasecmp(env, no[i])) {
            return 1;
        }
    }

    return 0;
}

void alloc_track_init(void) {
    if (alloc_env_off(getenv("LACHESIS_LEAK_REPORT"))) {
        return;
    }

    alloc_mutex = SDL_CreateMutex();
    if (!alloc_mutex) {
        return;
    }

    if (atexit(alloc_track_report) != 0) {
        SDL_DestroyMutex(alloc_mutex);
        alloc_mutex = NULL;
        return;
    }

    alloc_enabled = 1;
}

void alloc_track_complete(void) {
    alloc_completed = 1;
}

void alloc_track_abort(void) {
    alloc_aborted = 1;
}

static const char *alloc_trim(const char *loc) {
    const char *slash;

    if (!loc) {
        return "<unknown>";
    }
    slash = strrchr(loc, '/');
    if (slash) {
        loc = slash + 1;
    }
    slash = strrchr(loc, '\\');
    if (slash) {
        loc = slash + 1;
    }

    return loc;
}

static void alloc_human(char *buf, size_t bufsize, uint64_t bytes) {
    static const char *const units[] = {"KiB", "MiB", "GiB", "TiB"};
    double value = (double)bytes;
    size_t unit = 0;

    if (bytes < 1024) {
        snprintf(buf, bufsize, "%" PRIu64 " B", bytes);
        return;
    }
    value /= 1024.0;
    while (value >= 1024.0 && unit + 1 < FF_ARRAY_ELEMS(units)) {
        value /= 1024.0;
        unit++;
    }
    snprintf(buf, bufsize, "%.1f %s", value, units[unit]);
}

static int alloc_cmp_site(const void *a, const void *b) {
    const struct alloc_site *x = a, *y = b;

    if (x->bytes != y->bytes) {
        return x->bytes > y->bytes ? -1 : 1;
    }
    if (x->count != y->count) {
        return x->count > y->count ? -1 : 1;
    }

    return strcmp(alloc_trim(x->loc), alloc_trim(y->loc));
}

static int alloc_cmp_block(const void *a, const void *b) {
    const struct alloc_entry *x = a, *y = b;
    int order = strcmp(alloc_trim(x->loc), alloc_trim(y->loc));

    if (order) {
        return order;
    }
    if (x->ptr != y->ptr) {
        return (uintptr_t)x->ptr > (uintptr_t)y->ptr ? 1 : -1;
    }

    return 0;
}

static struct alloc_entry *alloc_snapshot(size_t *count) {
    struct alloc_entry *blocks;
    size_t n = 0;

    if (!alloc_live) {
        return NULL;
    }
    blocks = calloc(alloc_live, sizeof(*blocks));
    if (!blocks) {
        return NULL;
    }
    for (size_t i = 0; i < alloc_cap && n < alloc_live; i++) {
        if (alloc_table[i].ptr && alloc_table[i].ptr != ALLOC_TOMBSTONE) {
            blocks[n++] = alloc_table[i];
        }
    }
    *count = n;

    return blocks;
}

static struct alloc_site *alloc_group(const struct alloc_entry *blocks, size_t count, size_t *nsites) {
    struct alloc_site *sites = calloc(count, sizeof(*sites));
    size_t n = 0;

    if (!sites) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        const char *loc = blocks[i].loc;
        size_t s;

        for (s = 0; s < n; s++) {
            if (sites[s].loc == loc ||
                (sites[s].loc && loc && !strcmp(sites[s].loc, loc))) {
                break;
            }
        }
        if (s == n) {
            sites[n++] = (struct alloc_site){.loc = blocks[i].loc};
        }
        sites[s].bytes += blocks[i].size;
        sites[s].count++;
    }
    *nsites = n;

    return sites;
}

void alloc_track_report(void) {
    struct alloc_entry *blocks = NULL;
    struct alloc_site *sites = NULL;
    size_t count = 0, nsites = 0, live;
    uint64_t nallocs;
    char total[32], peak[32];
    int lost, completed;

    if (!alloc_enabled || alloc_aborted) {
        return;
    }

    SDL_LockMutex(alloc_mutex);
    if (alloc_reported) {
        SDL_UnlockMutex(alloc_mutex);
        return;
    }
    alloc_reported = 1;
    live = alloc_live;
    nallocs = alloc_total;
    lost = alloc_lost;
    completed = alloc_completed;
    alloc_human(total, sizeof(total), alloc_live_bytes);
    alloc_human(peak, sizeof(peak), alloc_peak_bytes);
    if (live) {
        blocks = alloc_snapshot(&count);
    }
    SDL_UnlockMutex(alloc_mutex);

    if (live) {
        fprintf(stderr, "LEAK: %zu allocations amounting to %s were never freed "
                        "(%" PRIu64 " allocations, peak %s):\n",
                live, total, nallocs, peak);

        if (!completed) {
            fprintf(stderr, "LEAK: the teardown did not finish "
                            "so this is an overcount.\n");
        }

        if (blocks) {
            sites = alloc_group(blocks, count, &nsites);
        }
        if (sites) {
            qsort(sites, nsites, sizeof(*sites), alloc_cmp_site);
            for (size_t i = 0; i < nsites; i++) {
                char bytes[32];

                alloc_human(bytes, sizeof(bytes), sites[i].bytes);
                fprintf(stderr, "LEAK: %11s in %5" PRIu64 "  %s\n",
                        bytes, sites[i].count, alloc_trim(sites[i].loc));
            }
            free(sites);
        } else {
            fprintf(stderr, "LEAK: out of memory while building the report.\n");
        }

        if (blocks) {
            qsort(blocks, count, sizeof(*blocks), alloc_cmp_block);
            fprintf(stderr, "LEAK: Outstanding blocks:\n");
            for (size_t i = 0; i < count; i++) {
                char bytes[32];

                alloc_human(bytes, sizeof(bytes), blocks[i].size);
                fprintf(stderr, "LEAK: %18p %11s  %s\n",
                        (void *)blocks[i].ptr, bytes, alloc_trim(blocks[i].loc));
            }
        }
        free(blocks);
    }

    if (lost) {
        fprintf(stderr, "LEAK: the tracking table ran out of memory "
                        "so this is an undercount.\n");
    }

    fflush(stderr);
}

void *alloc_wrap_malloc(size_t size, const char *loc) {
    void *ret = av_malloc(size);

    alloc_remember(ret, size, loc);

    return ret;
}

void *alloc_wrap_mallocz(size_t size, const char *loc) {
    void *ret = av_mallocz(size);

    alloc_remember(ret, size, loc);

    return ret;
}

void *alloc_wrap_malloc_array(size_t nmemb, size_t size, const char *loc) {
    void *ret = av_malloc_array(nmemb, size);

    alloc_remember(ret, alloc_mul(nmemb, size), loc);

    return ret;
}

void *alloc_wrap_calloc(size_t nmemb, size_t size, const char *loc) {
    void *ret = av_calloc(nmemb, size);

    alloc_remember(ret, alloc_mul(nmemb, size), loc);

    return ret;
}

void *alloc_wrap_realloc(void *ptr, size_t size, const char *loc) {
    struct alloc_entry old = alloc_release(ptr);
    void *ret = av_realloc(ptr, size);

    if (!ret && size) {
        alloc_return(&old);
        return ret;
    }
    alloc_remember(ret, size, loc);

    return ret;
}

void *alloc_wrap_realloc_f(void *ptr, size_t nelem, size_t elsize, const char *loc) {
    void *ret;

    /* This one frees ptr even when it fails. */
    alloc_forget(ptr);
    ret = av_realloc_f(ptr, nelem, elsize);
    alloc_remember(ret, alloc_mul(nelem, elsize), loc);

    return ret;
}

void *alloc_wrap_realloc_array(void *ptr, size_t nmemb, size_t size, const char *loc) {
    struct alloc_entry old = alloc_release(ptr);
    void *ret = av_realloc_array(ptr, nmemb, size);

    if (!ret && nmemb && size) {
        alloc_return(&old);
        return ret;
    }
    alloc_remember(ret, alloc_mul(nmemb, size), loc);

    return ret;
}

void *alloc_wrap_fast_realloc(void *ptr, unsigned int *size, size_t min_size, const char *loc) {
    struct alloc_entry old;
    void *ret;

    if (!alloc_enabled || min_size <= *size) {
        return av_fast_realloc(ptr, size, min_size);
    }
    old = alloc_release(ptr);
    ret = av_fast_realloc(ptr, size, min_size);
    if (!ret) {
        alloc_return(&old);
        return ret;
    }
    alloc_remember(ret, *size, loc);

    return ret;
}

int alloc_wrap_reallocp(void *ptr, size_t size, const char *loc) {
    void *before, *after;
    int ret;

    if (!alloc_enabled) {
        return av_reallocp(ptr, size);
    }
    memcpy(&before, ptr, sizeof(before));
    alloc_forget(before);
    ret = av_reallocp(ptr, size);
    memcpy(&after, ptr, sizeof(after));
    alloc_remember(after, size, loc);

    return ret;
}

int alloc_wrap_reallocp_array(void *ptr, size_t nmemb, size_t size, const char *loc) {
    void *before, *after;
    int ret;

    if (!alloc_enabled) {
        return av_reallocp_array(ptr, nmemb, size);
    }
    memcpy(&before, ptr, sizeof(before));
    alloc_forget(before);
    ret = av_reallocp_array(ptr, nmemb, size);
    memcpy(&after, ptr, sizeof(after));
    alloc_remember(after, alloc_mul(nmemb, size), loc);

    return ret;
}

void alloc_wrap_fast_malloc(void *ptr, unsigned int *size, size_t min_size, const char *loc) {
    void *before, *after;

    if (!alloc_enabled || min_size <= *size) {
        av_fast_malloc(ptr, size, min_size);
        return;
    }
    memcpy(&before, ptr, sizeof(before));
    alloc_forget(before);
    av_fast_malloc(ptr, size, min_size);
    memcpy(&after, ptr, sizeof(after));
    alloc_remember(after, *size, loc);
}

void alloc_wrap_fast_mallocz(void *ptr, unsigned int *size, size_t min_size, const char *loc) {
    void *before, *after;

    if (!alloc_enabled || min_size <= *size) {
        av_fast_mallocz(ptr, size, min_size);
        return;
    }
    memcpy(&before, ptr, sizeof(before));
    alloc_forget(before);
    av_fast_mallocz(ptr, size, min_size);
    memcpy(&after, ptr, sizeof(after));
    alloc_remember(after, *size, loc);
}

char *alloc_wrap_strdup(const char *s, const char *loc) {
    char *ret = av_strdup(s);

    if (ret && alloc_enabled) {
        alloc_remember(ret, strlen(ret) + 1, loc);
    }

    return ret;
}

char *alloc_wrap_strndup(const char *s, size_t len, const char *loc) {
    char *ret = av_strndup(s, len);

    if (ret && alloc_enabled) {
        alloc_remember(ret, strlen(ret) + 1, loc);
    }

    return ret;
}

void *alloc_wrap_memdup(const void *p, size_t size, const char *loc) {
    void *ret = av_memdup(p, size);

    alloc_remember(ret, size, loc);

    return ret;
}

char *alloc_wrap_string(char *s, const char *loc) {
    if (s && alloc_enabled) {
        alloc_remember(s, strlen(s) + 1, loc);
    }

    return s;
}

void alloc_wrap_free(void *ptr) {
    alloc_forget(ptr);
    av_free(ptr);
}

void alloc_wrap_freep(void *ptr) {
    void *val;

    if (alloc_enabled && ptr) {
        memcpy(&val, ptr, sizeof(val));
        alloc_forget(val);
    }
    av_freep(ptr);
}
