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

#include "PageFrame.h"

#include "logging.h"
#include "global.h" // Global::shared_config, Global::skipping_draw
#include "GlobalState.h"
#include "frame.h"
#include "renderhud/RenderHUD.h"
#include "inputs/inputs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>

namespace libtas {

static std::mutex mutex;
static std::condition_variable presented;

/* Page frame mode can be enabled from the start with this environment
 * variable, so that loading the page before it starts signaling frames does
 * not create frames either */
static bool enabledFromStart()
{
    const char* env = std::getenv("LIBTAS_PAGE_FRAMES");
    return env && (env[0] != '\0') && (strcmp(env, "0") != 0);
}

/* True once the page signaled a frame, so that games which never signal
 * keep ending a frame on each present */
static std::atomic<bool> enabled(enabledFromStart());

/* True if the page signaled a frame that did not end yet */
static bool pending = false;

/* True if that frame skips its draw (fast-forward) */
static bool pendingSkip = false;

/* True once the page signaled the end of a frame, so that frames only end
 * after the page ended them */
static std::atomic<bool> pageEndsFrames(false);

/* True from the present ending a frame until its frame boundary is done */
static bool presenting = false;

/* Wait until the frame boundary of the previous page frame is done */
static void waitForPreviousFrame(std::unique_lock<std::mutex>& lock)
{
    if (!enabled) {
        LOG(LL_INFO, LCF_WINDOW, "Page signaled a frame, presents now only end a frame after a page frame");
        enabled = true;
    }

    /* When the game exits, frames don't end anymore, e.g. the main thread
     * which ends frames that skip their draw waits for the page to exit */
    if (Global::is_exiting)
        return;

    /* Wait for the previous page frame to be presented, so that a frame
     * boundary never contains two page frames */
    for (int s = 0; pending && !Global::is_exiting && (s < 10); s++) {
        if (!presented.wait_for(lock, std::chrono::seconds(1), []{return !pending || Global::is_exiting;}))
            LOG(LL_WARN, LCF_WINDOW, "Page frame still not presented after %d s", s+1);
    }

    /* Then wait for its frame boundary to be done, which takes as long as
     * the game is paused, so that the new page frame does not run before the
     * inputs of the frame boundary are set */
    presented.wait(lock, []{return !presenting;});
}

/* True if the page ended its frame, and the frame boundary did not start
 * yet, and if the page drew on screen in that frame */
static bool endRequested = false;
static bool endDrew = false;

void PageFrame::endFrame(bool drew)
{
    GlobalNative gn;
    std::lock_guard<std::mutex> lock(mutex);
    if (!pageEndsFrames) {
        LOG(LL_INFO, LCF_WINDOW, "Page signals the end of its frames");
        pageEndsFrames = true;
    }
    if (pending) {
        endRequested = true;
        endDrew = drew;
    }
}

bool PageFrame::endRequestedFrame()
{
    bool drew;
    {
        GlobalNative gn;
        std::lock_guard<std::mutex> lock(mutex);

        /* Frames that drew and don't skip their draw end with their present */
        if (!endRequested || (endDrew && !pendingSkip))
            return false;
        drew = endDrew;
        endRequested = false;
        pending = false;
        pendingSkip = false;
        presenting = true;
    }

    /* A frame in which the page did not draw is a non-draw frame. A frame
     * that skips its draw is a draw frame (that skips its draw), without
     * anything to draw. */
    static RenderHUD dummy;
    if (drew)
        frameBoundary([] () {}, dummy);
    else
        frameBoundary(std::function<void()>(), dummy);

    frameEnded();
    return true;
}

/* Set the new page frame as pending */
static void setPending()
{
    pending = true;
    pendingSkip = Global::skipping_draw;
    endRequested = false;
}

void PageFrame::signal()
{
    /* Our own waits must not be handled as game waits */
    GlobalNative gn;
    std::unique_lock<std::mutex> lock(mutex);

    waitForPreviousFrame(lock);
    setPending();
}

int PageFrame::signalWithInputs(char* buf, int size)
{
    GlobalNative gn;
    std::unique_lock<std::mutex> lock(mutex);

    waitForPreviousFrame(lock);

    /* Read the inputs before setting the page frame as pending, because
     * a frame boundary may start right after */
    const AllInputsFlat& ai = Inputs::game_ai;
    int len = snprintf(buf, size, "%d %d %u %d", ai.pointer.x, ai.pointer.y, ai.pointer.mask, ai.pointer.wheel);

    /* The framerate of this frame, as the deterministic timer uses it */
    uint32_t framerate_num = Inputs::ai.misc.framerate_num;
    uint32_t framerate_den = Inputs::ai.misc.framerate_den;
    if (!framerate_num)
        framerate_num = Global::shared_config.initial_framerate_num;
    if (!framerate_den)
        framerate_den = Global::shared_config.initial_framerate_den;
    if (len < size)
        len += snprintf(buf + len, size - len, " r%u/%u", framerate_num, framerate_den);

    /* If this frame skips its draw */
    if (Global::skipping_draw && (len < size))
        len += snprintf(buf + len, size - len, " s");

    for (uint32_t key : ai.keyboard) {
        if (!key || (len >= size))
            break;
        len += snprintf(buf + len, size - len, " %x", key);
    }

    setPending();
    return std::min(len, size);
}

bool PageFrame::shouldEndFrame()
{
    GlobalNative gn;
    std::lock_guard<std::mutex> lock(mutex);

    if (!enabled)
        return true;

    if (!pending)
        return false;

    /* Once the page signals the end of its frames, a present only ends a
     * frame after its end, and only if the page drew in it. The other frames
     * end in endRequestedFrame(). */
    if (pageEndsFrames && (!endRequested || !endDrew))
        return false;

    pending = false;
    pendingSkip = false;
    endRequested = false;
    presenting = true;
    presented.notify_all();
    return true;
}

void PageFrame::frameEnded()
{
    GlobalNative gn;
    std::lock_guard<std::mutex> lock(mutex);

    presenting = false;
    presented.notify_all();
}

bool PageFrame::isEnabled()
{
    return enabled;
}

}
