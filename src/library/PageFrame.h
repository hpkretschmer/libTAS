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

#ifndef LIBTAS_PAGEFRAME_H_INCLUDED
#define LIBTAS_PAGEFRAME_H_INCLUDED

/* Browser-based games (e.g. NW.js) present more images than they produce
 * game frames, because the browser compositor can present without a new frame
 * from the page, depending on timing. To be deterministic, such a game can
 * signal each of its frames by calling access() on the path below (in NW.js:
 * `fs.accessSync()`), and presents only end a frame when the page signaled
 * a new frame since the last frame boundary. Setting the environment variable
 * LIBTAS_PAGE_FRAMES=1 enables this from the start, otherwise it is enabled
 * by the first signal. */
#define LIBTAS_PAGE_FRAME_PATH "/libtas/page_frame"

namespace libtas {

namespace PageFrame
{
/* Called when the page signals a new frame. Waits until the previous page
 * frame was presented, so that each frame boundary contains one page frame. */
void signal();

/* Returns if a present should end a frame, and consumes the page frame */
bool shouldEndFrame();

/* Returns if the game signals its frames, so that only presents of a page
 * frame end a frame */
bool isEnabled();
}

}

#endif
