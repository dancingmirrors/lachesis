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

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <conio.h>
#include <io.h>
#else
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <SDL3/SDL.h>

#include "lachesis_options.h"
#include "lachesis_terminal.h"

static int terminal_active = 0;

#if !defined(_WIN32)
static struct termios saved_termios;
static int saved_termios_valid = 0;

static void terminal_restore(void) {
    if (saved_termios_valid) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
        saved_termios_valid = 0;
    }
}
#endif

static void request_quit(void) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&event);
}

static int is_quit_key(int c) {
    return c == 'q';
}

void terminal_input_init(void) {
    if (terminal_quit_disable) {
        return;
    }
#if defined(_WIN32)
    if (!_isatty(_fileno(stdin))) {
        return;
    }
    terminal_active = 1;
#else
    if (!isatty(STDIN_FILENO)) {
        return;
    }
    struct termios tio;
    if (tcgetattr(STDIN_FILENO, &tio) != 0) {
        return;
    }
    saved_termios = tio;
    saved_termios_valid = 1;
    tio.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &tio) != 0) {
        saved_termios_valid = 0;
        return;
    }
    atexit(terminal_restore);
    terminal_active = 1;
#endif
}

void terminal_input_poll(void) {
    if (!terminal_active) {
        return;
    }
#if defined(_WIN32)
    while (_kbhit()) {
        if (is_quit_key(_getch())) {
            request_quit();
            return;
        }
    }
#else
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    for (;;) {
        if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) {
            return;
        }
        char buf[32];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) {
            return;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (is_quit_key((unsigned char)buf[i])) {
                request_quit();
                return;
            }
        }
    }
#endif
}
