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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavutil/attributes.h>
#include <libavutil/avstring.h>
#include <libavutil/mem.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>

#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
/* clang-format off */
#include <windows.h>
#include <sddl.h>
/* clang-format on */
#else
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "lachesis_alloc.h"
#include "lachesis_internal.h"
#include "lachesis_keys.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_osd.h"
#include "lachesis_playlist.h"
#include "lachesis_single.h"

/* Bumped only if the wire format stops being backwards compatible. */
#define SINGLE_MAGIC "LACHESIS-SINGLE-1"

#define SINGLE_VERB_PLAY "play"
#define SINGLE_VERB_QUEUE "queue"

#define SINGLE_FIELD_TOKEN "activation="

#define SINGLE_ACK_OK 1

#define SINGLE_PAYLOAD_MAX (8u * 1024u * 1024u)
#define SINGLE_PATHS_MAX 4096
#define SINGLE_QUEUE_MAX 64

#if defined(_WIN32)
#define SINGLE_ENDPOINT_MAX 256
#else
#define SINGLE_ENDPOINT_MAX (sizeof(((struct sockaddr_un *)0)->sun_path))
#endif

#define SINGLE_CWD_MAX 4096

#define SINGLE_CLIENT_TIMEOUT_MS 3000
#define SINGLE_SERVER_TIMEOUT_MS 1000

#define SINGLE_STOP_TIMEOUT_NS UINT64_C(1000000000)
#define SINGLE_STOP_WAKE_NS UINT64_C(10000000)
#if defined(_WIN32)
#define SINGLE_STOP_POLL_NS UINT64_C(1000000)
#else
#define SINGLE_STOP_POLL_NS UINT64_C(250000)
#endif

#define SINGLE_ACCEPT_FAILS 20

const char *const single_modes[] = {"yes", "no", "queue", NULL};

int single_mode = SINGLE_REPLACE;

typedef struct SingleRequest {
    struct SingleRequest *next;
    int mode;
    char *blob;
    const char *token;
    char **paths;
    int n_paths;
} SingleRequest;

static SDL_Mutex *single_mutex;
static SingleRequest *single_head;
static SingleRequest *single_tail;
static int single_depth;
static SDL_AtomicInt single_stop;
static SDL_AtomicInt single_done;
static SDL_Thread *single_thread;
static int single_listening;
static int single_event_posted;

#if defined(_WIN32)
static wchar_t *single_utf8_to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;

    if (n <= 0) {
        return NULL;
    }
    if (!(w = av_malloc((size_t)n * sizeof(*w)))) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        av_free(w);
        return NULL;
    }

    return w;
}

static char *single_wide_to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;

    if (n <= 0) {
        return NULL;
    }
    if (!(s = av_malloc((size_t)n))) {
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
        av_free(s);
        return NULL;
    }

    return s;
}

static char *single_abs_native(const char *path) {
    wchar_t *wide = single_utf8_to_wide(path);
    wchar_t *full = NULL;
    char *out = NULL;
    DWORD n;

    if (!wide) {
        return NULL;
    }
    n = GetFullPathNameW(wide, 0, NULL, NULL);
    if (n && (full = av_malloc((size_t)n * sizeof(*full)))) {
        if (GetFullPathNameW(wide, n, full, NULL)) {
            out = single_wide_to_utf8(full);
        }
    }
    av_free(full);
    av_free(wide);

    return out;
}
#else
static char *single_abs_native(const char *path) {
    char cwd[SINGLE_CWD_MAX];

    if (path[0] == '/') {
        return av_strdup(path);
    }
    if (!getcwd(cwd, sizeof(cwd))) {
        return NULL;
    }

    return av_asprintf("%s%s%s", cwd, cwd[1] ? "/" : "", path);
}
#endif

static char *single_abs_path(const char *path) {
    const char *proto;
    char *abs, *out;

    if (!strncmp(path, "file:", 5)) {
        if (!(abs = single_abs_native(path + 5))) {
            return NULL;
        }
        out = av_asprintf("file:%s", abs);
        av_free(abs);

        return out;
    }
    proto = avio_find_protocol_name(path);
    if (!proto || strcmp(proto, "file")) {
        return av_strdup(path);
    }

    return single_abs_native(path);
}

static int single_path_is_local_stream(const char *path) {
    return !strcmp(path, "-") || !strncmp(path, "fd:", 3) ||
        !strncmp(path, "pipe:", 5);
}

#if !defined(_WIN32)
/* Keeps concurrent graphical sessions from stealing each other's files. */
static void single_session_key(char *buf, size_t size) {
    const char *display = getenv("WAYLAND_DISPLAY");
    uint32_t hash = 2166136261u;

    if (!display || !display[0]) {
        display = getenv("DISPLAY");
    }
    if (!display || !display[0]) {
        display = "none";
    }
    for (const char *p = display; *p; p++) {
        hash = (hash ^ (unsigned char)*p) * 16777619u;
    }
    snprintf(buf, size, "%08x", hash);
}
#endif

#if defined(_WIN32)
static int single_endpoint(char *buf, size_t size) {
    DWORD session = 0;
    int n;

    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
        buf[0] = '\0';
        return 0;
    }
    n = snprintf(buf, size, "\\\\.\\pipe\\lachesis-single-%lu",
                 (unsigned long)session);

    return n > 0 && (size_t)n < size;
}
#else
static int single_base_is_safe(const char *base) {
    struct stat st;

    if (lstat(base, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return 0;
    }

    return !(st.st_mode & S_IWOTH) || (st.st_mode & S_ISVTX);
}

static int single_private_dir(const char *path) {
    struct stat st;

    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
        return 0;
    }
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return 0;
    }

    return st.st_uid == getuid() && !(st.st_mode & (S_IRWXG | S_IRWXO));
}

static int single_endpoint(char *buf, size_t size) {
    const char *bases[3];
    const char *env;
    char key[16];
    int n_bases = 0;

    single_session_key(key, sizeof(key));
    if ((env = getenv("XDG_RUNTIME_DIR")) && env[0] == '/') {
        bases[n_bases++] = env;
    }
    if ((env = getenv("TMPDIR")) && env[0] == '/') {
        bases[n_bases++] = env;
    }
    bases[n_bases++] = "/tmp";

    for (int i = 0; i < n_bases; i++) {
        char dir[SINGLE_ENDPOINT_MAX];
        int n = snprintf(dir, sizeof(dir), "%s/lachesis-%lu", bases[i],
                         (unsigned long)getuid());

        if (n <= 0 || (size_t)n >= sizeof(dir)) {
            continue;
        }
        n = snprintf(buf, size, "%s/single-%s", dir, key);
        if (n <= 0 || (size_t)n >= size || !single_base_is_safe(bases[i]) ||
            !single_private_dir(dir)) {
            continue;
        }

        return 1;
    }
    buf[0] = '\0';

    return 0;
}
#endif

static void single_request_free(SingleRequest *req) {
    if (!req) {
        return;
    }
    av_free(req->paths);
    av_free(req->blob);
    av_free(req);
}

static char *single_next_token(char *blob, size_t len, size_t *pos) {
    char *tok;

    if (*pos >= len) {
        return NULL;
    }
    tok = blob + *pos;
    while (*pos < len && blob[*pos]) {
        (*pos)++;
    }
    if (*pos >= len) {
        return NULL;
    }
    (*pos)++;

    return tok;
}

static SingleRequest *single_parse(char *blob, size_t len) {
    SingleRequest *req;
    size_t pos = 0;
    char *magic, *verb, *path;
    int cap = 0;

    magic = single_next_token(blob, len, &pos);
    verb = single_next_token(blob, len, &pos);
    if (!magic || !verb || strcmp(magic, SINGLE_MAGIC)) {
        av_free(blob);
        return NULL;
    }
    if (!(req = av_mallocz(sizeof(*req)))) {
        av_free(blob);
        return NULL;
    }
    req->blob = blob;
    if (!strcmp(verb, SINGLE_VERB_PLAY)) {
        req->mode = SINGLE_REPLACE;
    } else if (!strcmp(verb, SINGLE_VERB_QUEUE)) {
        req->mode = SINGLE_QUEUE;
    } else {
        /* Guessing here would mean guessing at whether to wipe the playlist. */
        single_request_free(req);
        return NULL;
    }

    while ((path = single_next_token(blob, len, &pos))) {
        if (!path[0]) {
            continue;
        }
        if (!req->n_paths && !req->token &&
            !strncmp(path, SINGLE_FIELD_TOKEN, strlen(SINGLE_FIELD_TOKEN))) {
            req->token = path + strlen(SINGLE_FIELD_TOKEN);
            continue;
        }
        if (req->n_paths >= SINGLE_PATHS_MAX) {
            break;
        }
        if (req->n_paths >= cap) {
            int new_cap = cap ? cap * 2 : 8;
            char **tmp = av_realloc_array(req->paths, new_cap, sizeof(*tmp));

            if (!tmp) {
                single_request_free(req);
                return NULL;
            }
            req->paths = tmp;
            cap = new_cap;
        }
        req->paths[req->n_paths++] = path;
    }
    if (!req->n_paths) {
        single_request_free(req);
        return NULL;
    }

    return req;
}

static char *single_token_field(void) {
    const char *token = getenv("XDG_ACTIVATION_TOKEN");

    if (!token || !token[0]) {
        return NULL;
    }

    return av_asprintf("%s%s", SINGLE_FIELD_TOKEN, token);
}

static char *single_build(char **paths, int n_paths, int mode, size_t *out_len) {
    const char *verb = mode == SINGLE_QUEUE ? SINGLE_VERB_QUEUE : SINGLE_VERB_PLAY;
    char **abs = av_calloc((size_t)n_paths, sizeof(*abs));
    char *token = mode == SINGLE_QUEUE ? NULL : single_token_field();
    size_t len = strlen(SINGLE_MAGIC) + 1 + strlen(verb) + 1;
    char *blob = NULL;
    size_t pos = 0;
    int n_abs = 0;

    if (!abs) {
        av_free(token);
        return NULL;
    }
    if (token) {
        len += strlen(token) + 1;
    }
    for (int i = 0; i < n_paths; i++) {
        if (!playlist_path_is_usable(paths[i])) {
            continue;
        }
        if (!(abs[n_abs] = single_abs_path(paths[i]))) {
            goto done;
        }
        len += strlen(abs[n_abs]) + 1;
        n_abs++;
    }
    if (!n_abs) {
        goto done;
    }
    if (len > SINGLE_PAYLOAD_MAX || !(blob = av_malloc(len))) {
        blob = NULL;
        goto done;
    }

    memcpy(blob + pos, SINGLE_MAGIC, strlen(SINGLE_MAGIC) + 1);
    pos += strlen(SINGLE_MAGIC) + 1;
    memcpy(blob + pos, verb, strlen(verb) + 1);
    pos += strlen(verb) + 1;
    if (token) {
        memcpy(blob + pos, token, strlen(token) + 1);
        pos += strlen(token) + 1;
    }
    for (int i = 0; i < n_abs; i++) {
        memcpy(blob + pos, abs[i], strlen(abs[i]) + 1);
        pos += strlen(abs[i]) + 1;
    }
    *out_len = pos;

done:
    for (int i = 0; i < n_abs; i++) {
        av_free(abs[i]);
    }
    av_free(abs);
    av_free(token);

    return blob;
}

static int single_enqueue(SingleRequest *req) {
    SDL_LockMutex(single_mutex);
    if (SDL_GetAtomicInt(&single_stop) || single_depth >= SINGLE_QUEUE_MAX) {
        SDL_UnlockMutex(single_mutex);
        return 0;
    }
    single_depth++;
    if (single_tail) {
        single_tail->next = req;
    } else {
        single_head = req;
    }
    single_tail = req;
    SDL_UnlockMutex(single_mutex);

    return 1;
}

static SingleRequest *single_dequeue(void) {
    SingleRequest *req;

    SDL_LockMutex(single_mutex);
    if ((req = single_head)) {
        single_depth--;
        single_head = req->next;
        if (!single_head) {
            single_tail = NULL;
        }
        req->next = NULL;
    }
    SDL_UnlockMutex(single_mutex);

    return req;
}

static void single_put_u32(uint8_t *buf, uint32_t v) {
    buf[0] = (uint8_t)(v & 0xff);
    buf[1] = (uint8_t)((v >> 8) & 0xff);
    buf[2] = (uint8_t)((v >> 16) & 0xff);
    buf[3] = (uint8_t)((v >> 24) & 0xff);
}

static uint32_t single_get_u32(const uint8_t *buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
        ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

#if defined(_WIN32)

static HANDLE single_pipe = INVALID_HANDLE_VALUE;
static char single_pipe_name[SINGLE_ENDPOINT_MAX];

static int single_io_read(HANDLE h, void *buf, size_t len, int timeout_ms) {
    uint8_t *p = buf;
    int waited = 0;

    while (len) {
        DWORD avail = 0, got = 0;

        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) {
            return 0;
        }
        if (!avail) {
            if (waited >= timeout_ms) {
                return 0;
            }
            SDL_Delay(10);
            waited += 10;
            continue;
        }
        if ((size_t)avail > len) {
            avail = (DWORD)len;
        }
        if (!ReadFile(h, p, avail, &got, NULL) || !got) {
            return 0;
        }
        p += got;
        len -= got;
        waited = 0;
    }

    return 1;
}

static int single_io_write(HANDLE h, const void *buf, size_t len) {
    const uint8_t *p = buf;

    while (len) {
        DWORD put = 0;

        if (!WriteFile(h, p, (DWORD)len, &put, NULL) || !put) {
            return 0;
        }
        p += put;
        len -= put;
    }

    return 1;
}

typedef union SingleTokenUser {
    TOKEN_USER user;
    char raw[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
} SingleTokenUser;

static PSECURITY_DESCRIPTOR single_pipe_descriptor(void) {
    PSECURITY_DESCRIPTOR sd = NULL;
    HANDLE token = NULL;
    SingleTokenUser user;
    char sddl[128];
    char *sid = NULL;
    DWORD len = 0;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return NULL;
    }
    if (GetTokenInformation(token, TokenUser, &user, sizeof(user), &len) &&
        ConvertSidToStringSidA(user.user.User.Sid, &sid)) {
        if (snprintf(sddl, sizeof(sddl), "D:P(A;;GA;;;%s)", sid) > 0) {
            ConvertStringSecurityDescriptorToSecurityDescriptorA(
                sddl, SDDL_REVISION_1, &sd, NULL);
        }
        LocalFree(sid);
    }
    CloseHandle(token);

    return sd;
}

static HANDLE single_pipe_instance(const char *name, int first) {
    SECURITY_ATTRIBUTES sa;
    PSECURITY_DESCRIPTOR sd = single_pipe_descriptor();
    DWORD mode = PIPE_ACCESS_DUPLEX;
    HANDLE h;

    if (!sd) {
        return INVALID_HANDLE_VALUE;
    }
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    if (first) {
        mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
    }
    h = CreateNamedPipeA(name, mode,
                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                             PIPE_REJECT_REMOTE_CLIENTS,
                         PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, &sa);
    LocalFree(sd);

    return h;
}

static int single_server_open(void) {
    if (!single_endpoint(single_pipe_name, sizeof(single_pipe_name))) {
        return 0;
    }
    single_pipe = single_pipe_instance(single_pipe_name, 1);

    return single_pipe != INVALID_HANDLE_VALUE;
}

static int single_server_accept(HANDLE *out) {
    for (int i = 0; i < 2; i++) {
        if (single_pipe == INVALID_HANDLE_VALUE ||
            SDL_GetAtomicInt(&single_stop)) {
            return 0;
        }
        if (ConnectNamedPipe(single_pipe, NULL)) {
            break;
        }
        if (GetLastError() == ERROR_PIPE_CONNECTED) {
            break;
        }
        if (GetLastError() != ERROR_NO_DATA) {
            return 0;
        }
        DisconnectNamedPipe(single_pipe);
        if (i) {
            return 0;
        }
    }
    *out = single_pipe;

    return 1;
}

static void single_server_done(HANDLE h) {
    FlushFileBuffers(h);
    DisconnectNamedPipe(h);
}

static void single_server_close(void) {
    if (single_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(single_pipe);
        single_pipe = INVALID_HANDLE_VALUE;
    }
}

static void single_server_forget(void) {
    single_pipe_name[0] = '\0';
}

/* Unblocks the listener's ConnectNamedPipe() so that it can see single_stop. */
static void single_server_wake(void) {
    HANDLE h;

    if (!single_pipe_name[0]) {
        return;
    }
    h = CreateFileA(single_pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                    OPEN_EXISTING,
                    SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }
}

static int single_peer_is_self(HANDLE h, ULONG *out_pid) {
    SingleTokenUser theirs, ours;
    HANDLE proc, token = NULL, self = NULL;
    DWORD len = 0;
    ULONG pid = 0;
    int same = 0;

    if (!GetNamedPipeServerProcessId(h, &pid)) {
        return 0;
    }
    *out_pid = pid;
    if (pid == GetCurrentProcessId()) {
        return 1;
    }
    if (!(proc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid)) &&
        !(proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))) {
        return 0;
    }
    if (OpenProcessToken(proc, TOKEN_QUERY, &token) &&
        GetTokenInformation(token, TokenUser, &theirs, sizeof(theirs), &len) &&
        OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &self) &&
        GetTokenInformation(self, TokenUser, &ours, sizeof(ours), &len)) {
        same = EqualSid(theirs.user.User.Sid, ours.user.User.Sid);
    }
    if (token) {
        CloseHandle(token);
    }
    if (self) {
        CloseHandle(self);
    }
    CloseHandle(proc);

    return same;
}

static int single_client_send(const char *payload, size_t len) {
    char name[SINGLE_ENDPOINT_MAX];
    uint8_t hdr[4], ack = 0;
    ULONG pid = 0;
    HANDLE h;
    int ok;

    if (!single_endpoint(name, sizeof(name))) {
        return 0;
    }
    h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                    SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();

        if (err == ERROR_FILE_NOT_FOUND) {
            return 0;
        }
        if (err != ERROR_PIPE_BUSY ||
            !WaitNamedPipeA(name, SINGLE_CLIENT_TIMEOUT_MS)) {
            return -1;
        }
        h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                        OPEN_EXISTING,
                        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
        }
    }
    if (!single_peer_is_self(h, &pid)) {
        CloseHandle(h);
        return -1;
    }
    AllowSetForegroundWindow(pid);
    single_put_u32(hdr, (uint32_t)len);
    ok = single_io_write(h, hdr, sizeof(hdr)) && single_io_write(h, payload, len) &&
        single_io_read(h, &ack, 1, SINGLE_CLIENT_TIMEOUT_MS) &&
        ack == SINGLE_ACK_OK;
    CloseHandle(h);

    return ok ? 1 : -1;
}

#else /* POSIX */

static int single_sock = -1;
static char single_sock_path[SINGLE_ENDPOINT_MAX];

static int single_io_read(int fd, void *buf, size_t len,
                          int timeout_ms av_unused) {
    uint8_t *p = buf;

    while (len) {
        ssize_t got = read(fd, p, len);

        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            return 0;
        }
        p += got;
        len -= (size_t)got;
    }

    return 1;
}

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int single_io_write(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;

    while (len) {
        ssize_t put = send(fd, p, len, MSG_NOSIGNAL);

        if (put < 0 && errno == EINTR) {
            continue;
        }
        if (put <= 0) {
            return 0;
        }
        p += put;
        len -= (size_t)put;
    }

    return 1;
}

static void single_set_timeout(int fd, int ms) {
    struct timeval tv;

#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif

    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int single_fill_addr(struct sockaddr_un *addr, const char *path) {
    if (strlen(path) >= sizeof(addr->sun_path)) {
        return 0;
    }
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    snprintf(addr->sun_path, sizeof(addr->sun_path), "%s", path);

    return 1;
}

static int single_bind(const char *path, int *err) {
    struct sockaddr_un addr;
    mode_t old_mask;
    int fd;

    *err = 0;
    if (!single_fill_addr(&addr, path)) {
        *err = ENAMETOOLONG;
        return -1;
    }
    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        *err = errno;
        return -1;
    }
    old_mask = umask(0177);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        *err = errno;
    }
    umask(old_mask);
    if (!*err && listen(fd, 8) != 0) {
        *err = errno;
    }
    if (*err) {
        close(fd);
        return -1;
    }

    return fd;
}

static int single_connect(const char *path, int *err) {
    struct sockaddr_un addr;
    int fd;

    *err = 0;
    if (!single_fill_addr(&addr, path)) {
        *err = ENAMETOOLONG;
        return -1;
    }
    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        *err = errno;
        return -1;
    }
    single_set_timeout(fd, SINGLE_CLIENT_TIMEOUT_MS);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        *err = errno;
        close(fd);
        return -1;
    }

    return fd;
}

static int single_err_says_vacant(int err) {
    return err == ENOENT || err == ECONNREFUSED;
}

static int single_server_open(void) {
    int err;

    if (!single_endpoint(single_sock_path, sizeof(single_sock_path))) {
        return 0;
    }
    single_sock = single_bind(single_sock_path, &err);
    if (single_sock < 0 && err == EADDRINUSE) {
        int probe = single_connect(single_sock_path, &err);

        if (probe >= 0) {
            close(probe);
            return 0;
        }
        if (!single_err_says_vacant(err)) {
            return 0;
        }
        if (unlink(single_sock_path) == 0) {
            single_sock = single_bind(single_sock_path, &err);
        }
    }
    if (single_sock < 0) {
        single_sock_path[0] = '\0';
        return 0;
    }

    return 1;
}

static int single_server_accept(int *out) {
    int fd;

    do {
        fd = accept(single_sock, NULL, NULL);
    } while (fd < 0 && errno == EINTR);

    if (fd < 0) {
        return 0;
    }
    single_set_timeout(fd, SINGLE_SERVER_TIMEOUT_MS);
    *out = fd;

    return 1;
}

static void single_server_done(int fd) {
    close(fd);
}

static void single_server_close(void) {
    if (single_sock >= 0) {
        close(single_sock);
        single_sock = -1;
    }
}

static void single_server_forget(void) {
    if (single_sock_path[0]) {
        unlink(single_sock_path);
        single_sock_path[0] = '\0';
    }
}

static void single_server_wake(void) {
    int fd, err;

    if (!single_sock_path[0]) {
        return;
    }
    if ((fd = single_connect(single_sock_path, &err)) >= 0) {
        close(fd);
    }
}

static int single_client_send(const char *payload, size_t len) {
    char path[SINGLE_ENDPOINT_MAX];
    uint8_t hdr[4], ack = 0;
    int fd, ok, err;

    if (!single_endpoint(path, sizeof(path))) {
        return 0;
    }
    if ((fd = single_connect(path, &err)) < 0) {
        return single_err_says_vacant(err) ? 0 : -1;
    }
    single_put_u32(hdr, (uint32_t)len);
    ok = single_io_write(fd, hdr, sizeof(hdr)) &&
        single_io_write(fd, payload, len) &&
        single_io_read(fd, &ack, 1, SINGLE_CLIENT_TIMEOUT_MS) &&
        ack == SINGLE_ACK_OK;
    close(fd);

    return ok ? 1 : -1;
}

#endif /* POSIX */

#if defined(_WIN32)
typedef HANDLE SingleConnHandle;
#else
typedef int SingleConnHandle;
#endif

static void single_serve(SingleConnHandle conn) {
    SingleRequest *req;
    uint8_t hdr[4], ack = SINGLE_ACK_OK;
    uint32_t len;
    char *blob;

    if (!single_io_read(conn, hdr, sizeof(hdr), SINGLE_SERVER_TIMEOUT_MS)) {
        return;
    }
    len = single_get_u32(hdr);
    if (!len || len > SINGLE_PAYLOAD_MAX) {
        return;
    }
    if (!(blob = av_malloc((size_t)len + 1))) {
        return;
    }
    if (!single_io_read(conn, blob, len, SINGLE_SERVER_TIMEOUT_MS)) {
        av_free(blob);
        return;
    }
    blob[len] = '\0';

    if (!(req = single_parse(blob, len))) {
        return;
    }
    if (!single_enqueue(req)) {
        single_request_free(req);
        return;
    }

    single_io_write(conn, &ack, 1);
}

static int single_listen_thread(void *arg av_unused) {
    int fails = 0;

    while (!SDL_GetAtomicInt(&single_stop)) {
        SingleConnHandle conn;

        if (!single_server_accept(&conn)) {
            if (SDL_GetAtomicInt(&single_stop)) {
                break;
            }
            /* Idle rather than spin, and give up if it never recovers. */
            if (++fails >= SINGLE_ACCEPT_FAILS) {
                log_warn("Giving up on the single instance endpoint.\n");
                break;
            }
            SDL_Delay(50);
            continue;
        }
        fails = 0;
        if (!SDL_GetAtomicInt(&single_stop)) {
            single_serve(conn);
        }
        single_server_done(conn);
    }
    single_server_close();
    SDL_SetAtomicInt(&single_done, 1);

    return 0;
}

static int single_listen_start(void);

enum SingleRole single_claim(char **paths, int n_paths) {
    const char *given;
    char *payload;
    size_t len = 0;
    int sent;

    if (single_mode == SINGLE_OFF || display_disable || n_paths <= 0) {
        return SINGLE_ROLE_ALONE;
    }

    if ((given = option_first_from_cmdline(options, "single"))) {
        if (option_given_on_cmdline(options, "single")) {
            log_warn("-single does not apply because -%s was given.\n",
                     given);
        }
        return SINGLE_ROLE_ALONE;
    }
    for (int i = 0; i < n_paths; i++) {
        if (single_path_is_local_stream(paths[i])) {
            return SINGLE_ROLE_ALONE;
        }
    }

    if (!(payload = single_build(paths, n_paths, single_mode, &len))) {
        return SINGLE_ROLE_ALONE;
    }

    sent = single_client_send(payload, len);
    if (sent < 0) {
        av_free(payload);
        return SINGLE_ROLE_ALONE;
    }
    if (sent > 0) {
        av_free(payload);
        return SINGLE_ROLE_HANDED_OFF;
    }

    if (single_server_open()) {
        av_free(payload);
        if (!single_listen_start()) {
            return SINGLE_ROLE_ALONE;
        }

        return SINGLE_ROLE_PRIMARY;
    }

    sent = single_client_send(payload, len);
    av_free(payload);

    return sent > 0 ? SINGLE_ROLE_HANDED_OFF : SINGLE_ROLE_ALONE;
}

static int single_listen_start(void) {
    if (!(single_mutex = SDL_CreateMutex())) {
        single_server_close();
        single_server_forget();
        return 0;
    }
    single_thread = SDL_CreateThread(single_listen_thread, "single", NULL);
    if (!single_thread) {
        SDL_DestroyMutex(single_mutex);
        single_mutex = NULL;
        single_server_close();
        single_server_forget();
        return 0;
    }
    single_listening = 1;
    atexit(single_shutdown);

    return 1;
}

void single_poll(void) {
    SDL_Event event;
    int pending;

    if (!single_listening || single_event_posted) {
        return;
    }
    SDL_LockMutex(single_mutex);
    pending = single_head != NULL;
    SDL_UnlockMutex(single_mutex);
    if (!pending) {
        return;
    }

    SDL_zero(event);
    event.type = FF_SINGLE_EVENT;
    if (SDL_PushEvent(&event)) {
        single_event_posted = 1;
    } else {
        SDL_ClearError();
    }
}

static int single_is_sep(char c) {
#if defined(_WIN32)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static const char *single_display_name(const char *path) {
    const char *base = path;

    if (strstr(path, "://")) {
        return path;
    }
    for (const char *p = path; *p; p++) {
        if (single_is_sep(*p) && p[1]) {
            base = p + 1;
        }
    }

    return base;
}

static int single_is_dir(const char *path) {
#if defined(_WIN32)
    wchar_t *wide = single_utf8_to_wide(path);
    DWORD attr;

    if (!wide) {
        return 0;
    }
    attr = GetFileAttributesW(wide);
    av_free(wide);

    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static void single_add_path(const char *path) {
    if (single_is_dir(path)) {
        playlist_add_directory(path);
        return;
    }
    playlist_add_input(path);
}

static void single_apply(VideoState **pis, const SingleRequest *req) {
    int before = playlist_size;
    int added;

    for (int i = 0; i < req->n_paths; i++) {
        if (single_path_is_local_stream(req->paths[i])) {
            log_warn("IPC error: %s\n",
                     req->paths[i]);
            continue;
        }
        if (!playlist_path_is_usable(req->paths[i])) {
            log_warn("IPC error: %s\n",
                     req->paths[i]);
            continue;
        }
        single_add_path(req->paths[i]);
    }
    playlist_drop_unplayable(before, file_iformat != NULL);
    playlist_report_filtered();
    added = playlist_size - before;
    if (added <= 0) {
        osd_show_message("Nothing to play");
        (*pis)->force_refresh = 1;
        return;
    }

    if (req->mode == SINGLE_QUEUE) {
        const PlaylistEntry *e = playlist_get(playlist_size - 1);

        if (added == 1 && e) {
            osd_show_message("Queued %s", single_display_name(e->display_path));
        } else {
            osd_show_message("Queued %d files", added);
        }
        (*pis)->force_refresh = 1;
        return;
    }

    delete_prompt_cancel(pis);
    playlist_remove_range(0, before);
    playlist_nav_dir = 1;
    window_want_raise(req->token);
    playlist_switch(pis, 0);
    (*pis)->force_refresh = 1;
}

void single_handle_event(VideoState **pis) {
    SingleRequest *req;

    single_event_posted = 0;
    while ((req = single_dequeue())) {
        single_apply(pis, req);
        single_request_free(req);
    }
}

static int single_listen_stop(void) {
    uint64_t deadline = SDL_GetTicksNS() + SINGLE_STOP_TIMEOUT_NS;
    uint64_t next_wake = 0;

    while (!SDL_GetAtomicInt(&single_done)) {
        uint64_t now = SDL_GetTicksNS();

        if (now >= deadline) {
            break;
        }
        if (now >= next_wake) {
            single_server_wake();
            next_wake = now + SINGLE_STOP_WAKE_NS;
        }
        SDL_DelayNS(SINGLE_STOP_POLL_NS);
    }

    return SDL_GetAtomicInt(&single_done);
}

void single_shutdown(void) {
    static int shut;
    SingleRequest *req = NULL;
    int stopped;

    if (shut) {
        return;
    }
    shut = 1;
    single_listening = 0;
    SDL_SetAtomicInt(&single_stop, 1);

    if (!single_thread) {
        single_server_close();
        single_server_forget();
        if (single_mutex) {
            SDL_DestroyMutex(single_mutex);
            single_mutex = NULL;
        }
        return;
    }

    stopped = single_listen_stop();
    if (stopped) {
        SDL_WaitThread(single_thread, NULL);
    } else {
        SDL_DetachThread(single_thread);
    }
    single_thread = NULL;
    single_server_forget();

    SDL_LockMutex(single_mutex);
    req = single_head;
    single_head = NULL;
    single_tail = NULL;
    single_depth = 0;
    SDL_UnlockMutex(single_mutex);

    while (req) {
        SingleRequest *next = req->next;

        single_request_free(req);
        req = next;
    }
    if (stopped) {
        SDL_DestroyMutex(single_mutex);
        single_mutex = NULL;
    }
}
