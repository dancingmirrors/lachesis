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

#include <windows.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static wchar_t *dup_wide(const wchar_t *str) {
    size_t size = (wcslen(str) + 1) * sizeof(*str);
    wchar_t *copy = malloc(size);

    if (copy) {
        memcpy(copy, str, size);
    }

    return copy;
}

static HANDLE std_handle(DWORD id) {
    HANDLE handle = GetStdHandle(id);

    return handle == INVALID_HANDLE_VALUE ? NULL : handle;
}

static void write_error(const wchar_t *text) {
    HANDLE handle = std_handle(STD_ERROR_HANDLE);
    DWORD mode, written;
    char *utf8;
    int size;

    if (!handle) {
        return;
    }
    if (GetConsoleMode(handle, &mode)) {
        WriteConsoleW(handle, text, (DWORD)wcslen(text), &written, NULL);
        return;
    }

    size = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (size <= 1 || !(utf8 = malloc((size_t)size))) {
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, size, NULL, NULL);
    WriteFile(handle, utf8, (DWORD)size - 1, &written, NULL);
    free(utf8);
}

static void report_last_error(const wchar_t *path) {
    DWORD error = GetLastError();
    wchar_t *message = NULL;

    write_error(L"lachesis: cannot run ");
    write_error(path);
    write_error(L": ");
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, error, 0, (wchar_t *)&message, 0, NULL) &&
        message) {
        write_error(message);
        LocalFree(message);
    } else {
        write_error(L"unknown error\r\n");
    }
}

static wchar_t *module_path(void) {
    DWORD size = MAX_PATH;

    for (;;) {
        wchar_t *path = malloc((size_t)size * sizeof(*path));
        DWORD len;

        if (!path) {
            return NULL;
        }
        len = GetModuleFileNameW(NULL, path, size);
        if (len > 0 && len < size) {
            return path;
        }
        free(path);
        if (len == 0 || size >= 0x10000) {
            return NULL;
        }
        size *= 2;
    }
}

static wchar_t *player_path(const wchar_t *path) {
    wchar_t *player;
    size_t len, base, stem;

    len = wcslen(path);
    base = len;
    while (base > 0 && path[base - 1] != L'\\' && path[base - 1] != L'/') {
        base--;
    }

    stem = len;
    for (size_t i = len; i > base + 1; i--) {
        if (path[i - 1] == L'.') {
            stem = i - 1;
            break;
        }
    }

    player = malloc((stem + sizeof(L".exe") / sizeof(wchar_t)) * sizeof(*player));
    if (player) {
        memcpy(player, path, stem * sizeof(*player));
        memcpy(player + stem, L".exe", sizeof(L".exe"));
    }

    return player;
}

static BOOL WINAPI console_ctrl_handler(DWORD type) {
    (void)type;

    return TRUE;
}

int main(void) {
    wchar_t *self = module_path();
    wchar_t *player = self ? player_path(self) : NULL;
    wchar_t *command_line = dup_wide(GetCommandLineW());
    PROCESS_INFORMATION process;
    STARTUPINFOW startup;
    DWORD status = 1;

    if (!self || !player || !command_line) {
        write_error(L"lachesis: out of memory\r\n");
        goto done;
    }
    if (!lstrcmpiW(self, player)) {
        write_error(L"lachesis: this launcher has to keep the .com extension\r\n");
        goto done;
    }

    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = std_handle(STD_INPUT_HANDLE);
    startup.hStdOutput = std_handle(STD_OUTPUT_HANDLE);
    startup.hStdError = std_handle(STD_ERROR_HANDLE);

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    if (!CreateProcessW(player, command_line, NULL, NULL, TRUE, 0, NULL, NULL,
                        &startup, &process)) {
        report_last_error(player);
        goto done;
    }

    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    if (!GetExitCodeProcess(process.hProcess, &status)) {
        status = 1;
    }
    CloseHandle(process.hProcess);

done:
    free(self);
    free(player);
    free(command_line);

    return (int)status;
}
