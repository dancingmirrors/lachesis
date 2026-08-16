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

#ifndef LACHESIS_ALLOC_H
#define LACHESIS_ALLOC_H

#include <stddef.h>

#include <libavutil/attributes.h>
#include <libavutil/avstring.h>
#include <libavutil/mem.h>

void alloc_track_init(void);
void alloc_track_report(void);

void alloc_track_complete(void);

/* Safe to call when tracking is off, on a NULL pointer, or on a pointer that
 * was never tracked. */
void alloc_track_disown(const void *ptr);

void *alloc_wrap_malloc(size_t size, const char *loc);
void *alloc_wrap_mallocz(size_t size, const char *loc);
void *alloc_wrap_malloc_array(size_t nmemb, size_t size, const char *loc);
void *alloc_wrap_calloc(size_t nmemb, size_t size, const char *loc);
void *alloc_wrap_realloc(void *ptr, size_t size, const char *loc);
void *alloc_wrap_realloc_f(void *ptr, size_t nelem, size_t elsize, const char *loc);
void *alloc_wrap_realloc_array(void *ptr, size_t nmemb, size_t size, const char *loc);

av_warn_unused_result int alloc_wrap_reallocp(void *ptr, size_t size, const char *loc);
av_warn_unused_result int alloc_wrap_reallocp_array(void *ptr, size_t nmemb, size_t size, const char *loc);

void *alloc_wrap_fast_realloc(void *ptr, unsigned int *size, size_t min_size, const char *loc);
void alloc_wrap_fast_malloc(void *ptr, unsigned int *size, size_t min_size, const char *loc);
void alloc_wrap_fast_mallocz(void *ptr, unsigned int *size, size_t min_size, const char *loc);

char *alloc_wrap_strdup(const char *s, const char *loc);
char *alloc_wrap_strndup(const char *s, size_t len, const char *loc);

void *alloc_wrap_memdup(const void *p, size_t size, const char *loc);

char *alloc_wrap_string(char *s, const char *loc);

void alloc_wrap_free(void *ptr);
void alloc_wrap_freep(void *ptr);

#define LACHESIS_ALLOC_STR_(x) #x
#define LACHESIS_ALLOC_STR(x) LACHESIS_ALLOC_STR_(x)
#define LACHESIS_ALLOC_LOC __FILE__ ":" LACHESIS_ALLOC_STR(__LINE__)

#ifndef LACHESIS_ALLOC_INTERNAL

#define av_malloc(size) alloc_wrap_malloc((size), LACHESIS_ALLOC_LOC)
#define av_mallocz(size) alloc_wrap_mallocz((size), LACHESIS_ALLOC_LOC)
#define av_malloc_array(nmemb, size) alloc_wrap_malloc_array((nmemb), (size), LACHESIS_ALLOC_LOC)
#define av_calloc(nmemb, size) alloc_wrap_calloc((nmemb), (size), LACHESIS_ALLOC_LOC)
#define av_realloc(ptr, size) alloc_wrap_realloc((ptr), (size), LACHESIS_ALLOC_LOC)
#define av_realloc_f(ptr, nelem, elsize) alloc_wrap_realloc_f((ptr), (nelem), (elsize), LACHESIS_ALLOC_LOC)
#define av_realloc_array(ptr, nmemb, size) alloc_wrap_realloc_array((ptr), (nmemb), (size), LACHESIS_ALLOC_LOC)
#define av_reallocp(ptr, size) alloc_wrap_reallocp((ptr), (size), LACHESIS_ALLOC_LOC)
#define av_reallocp_array(ptr, nmemb, size) alloc_wrap_reallocp_array((ptr), (nmemb), (size), LACHESIS_ALLOC_LOC)
#define av_fast_realloc(ptr, size, min_size) alloc_wrap_fast_realloc((ptr), (size), (min_size), LACHESIS_ALLOC_LOC)
#define av_fast_malloc(ptr, size, min_size) alloc_wrap_fast_malloc((ptr), (size), (min_size), LACHESIS_ALLOC_LOC)
#define av_fast_mallocz(ptr, size, min_size) alloc_wrap_fast_mallocz((ptr), (size), (min_size), LACHESIS_ALLOC_LOC)
#define av_strdup(s) alloc_wrap_strdup((s), LACHESIS_ALLOC_LOC)
#define av_strndup(s, len) alloc_wrap_strndup((s), (len), LACHESIS_ALLOC_LOC)
#define av_memdup(p, size) alloc_wrap_memdup((p), (size), LACHESIS_ALLOC_LOC)
#define av_asprintf(...) alloc_wrap_string(av_asprintf(__VA_ARGS__), LACHESIS_ALLOC_LOC)
#define av_free(ptr) alloc_wrap_free(ptr)
#define av_freep(ptr) alloc_wrap_freep(ptr)

#endif /* LACHESIS_ALLOC_INTERNAL */

#endif /* LACHESIS_ALLOC_H */
