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
#include "GlobalState.h"
#include "inputs/inputs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

/* True if the page signaled a frame that was not presented yet */
static bool pending = false;

/* True from the present ending a frame until its frame boundary is done */
static bool presenting = false;

/* Wait until the frame boundary of the previous page frame is done */
static void waitForPreviousFrame(std::unique_lock<std::mutex>& lock)
{
    if (!enabled) {
        LOG(LL_INFO, LCF_WINDOW, "Page signaled a frame, presents now only end a frame after a page frame");
        enabled = true;
    }

    /* Wait for the previous page frame to be presented, so that a frame
     * boundary never contains two page frames */
    for (int s = 0; pending && (s < 10); s++) {
        if (!presented.wait_for(lock, std::chrono::seconds(1), []{return !pending;}))
            LOG(LL_WARN, LCF_WINDOW, "Page frame still not presented after %d s", s+1);
    }

    /* Then wait for its frame boundary to be done, which takes as long as
     * the game is paused, so that the new page frame does not run before the
     * inputs of the frame boundary are set */
    presented.wait(lock, []{return !presenting;});
}

void PageFrame::signal()
{
    /* Our own waits must not be handled as game waits */
    GlobalNative gn;
    std::unique_lock<std::mutex> lock(mutex);

    waitForPreviousFrame(lock);
    pending = true;
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
    for (uint32_t key : ai.keyboard) {
        if (!key || (len >= size))
            break;
        len += snprintf(buf + len, size - len, " %x", key);
    }

    pending = true;
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

    pending = false;
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
