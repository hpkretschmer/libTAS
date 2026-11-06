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
 * from the page, depending on timing. To be deterministic, the page signals
 * each of its frames by calling readlink() on the path below (in NW.js:
 * `fs.readlinkSync()`), and presents only end a frame when the page signaled
 * a new frame since the last frame boundary. Setting the environment variable
 * LIBTAS_PAGE_FRAMES=1 enables this from the start, otherwise it is enabled
 * by the first signal. libTAS injects the script that does this into NW.js
 * games (see PageScript.h).
 *
 * The signal returns the inputs of that frame as text:
 * "<pointer x> <pointer y> <pointer button mask> <wheel> r<num>/<den> [s]
 * <keysym>...", with the button mask using SingleInput::POINTER_B* bits,
 * the framerate of the frame as num/den, "s" if the frame skips its draw
 * (see below), and the keysyms in hex.
 * The page can apply these inputs itself at the start of its frame, instead
 * of receiving input events from the browser at an undefined time, and
 * advance its clock by one frame duration. */
#define LIBTAS_PAGE_FRAME_PATH "/libtas/page_frame"

/* When libTAS skips drawing a frame (fast-forward), it adds " s" after the
 * framerate, and the page may then not draw on screen.
 *
 * The page can signal the end of each frame by calling access() on one of
 * the paths below, depending on if it drew on screen in the frame (or would
 * have, if the frame skips its draw). Then the frame ends after that signal:
 * at the next present if it drew, otherwise without waiting for a present,
 * and frames that did not draw are non-draw frames. Pages that never signal
 * the end of a frame keep ending each frame with a present. */
#define LIBTAS_PAGE_FRAME_END_PATH "/libtas/page_frame_end"
#define LIBTAS_PAGE_FRAME_END_NODRAW_PATH "/libtas/page_frame_end_nodraw"

namespace libtas {

namespace PageFrame
{
/* Called when the page signals a new frame. Waits until the frame boundary
 * of the previous page frame is done, so that each frame boundary contains
 * one page frame, and the new page frame runs with the inputs of this frame
 * boundary. Writes the inputs of the new page frame into buf as described
 * above. Returns the length written, without null terminator. */
int signalWithInputs(char* buf, int size);

/* Called when the page ends its frame, with if it drew on screen. From then
 * on, frames only end after the page ended them. */
void endFrame(bool drew);

/* Called by the main thread when it waits. Ends the page frame that the page
 * ended if it does not wait for a present (it skips its draw or did not
 * draw), and returns if it did. Frame boundaries stay on the main thread,
 * which presents the other frames. */
bool endRequestedFrame();

/* Returns if a present should end a frame, and consumes the page frame */
bool shouldEndFrame();

/* Called when the frame boundary of a present that ended a frame is done */
void frameEnded();

/* Returns if the game signals its frames, so that only presents of a page
 * frame end a frame */
bool isEnabled();
}

}

#endif
