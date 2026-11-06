/*
    Copyright 2015-2026 Clément Gallet <clement.gallet@ens-lyon.org>

    This file is part of libTAS.

    libTAS is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    libTAS is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with libTAS.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBTAS_PAGESCRIPT_H_INCLUDED
#define LIBTAS_PAGESCRIPT_H_INCLUDED

/* Browser-based games need a script in the page to signal their frames and
 * get their inputs (see PageFrame.h). libTAS injects that script,
 * web/libtas_web.js, into NW.js games, so that the game's files stay
 * unchanged: when page frames are enabled with LIBTAS_PAGE_FRAMES=1, the
 * manifest package.json of the NW.js app that the game reads gets
 * "inject_js_start" with the path below, and libTAS gives the script when the
 * game reads that path. */
#define LIBTAS_PAGE_SCRIPT_PATH "/libtas/libtas_web.js"

namespace libtas {

namespace PageScript
{
/* Returned by open() if the file is not one that libTAS gives */
static const int NOT_HANDLED = -2;

/* If `file` is the manifest of the NW.js app or the injected script, returns
 * a file descriptor of a read-only copy in memory with the content that
 * libTAS gives, or -1 on error. Otherwise returns NOT_HANDLED. */
int open(const char* file, int oflag);
}

}

#endif
