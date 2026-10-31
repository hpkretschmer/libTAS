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

#include "xshm.h"
#include "XlibGameWindow.h"

#include "hook.h"
#include "logging.h"
#include "GlobalState.h"
#include "screencapture/ScreenCapture.h"
#include "frame.h"
#include "renderhud/RenderHUD.h"
#include "renderhud/RenderHUD_XImage.h"
#include "PageFrame.h"
#include "DeterministicTimer.h"
#include "global.h"
#include "../shared/sockethelpers.h"
#include "../shared/messages.h"

namespace libtas {

DEFINE_ORIG_POINTER(XShmPutImage)
DEFINE_ORIG_POINTER(XPutImage)

XImage* x11::gameXImage = nullptr;

/* XPutImage callers may pass a temporary XImage structure, so we keep a copy.
 * Its pixel data must stay valid, which is the case for Chromium. */
static XImage gamePutXImage;

OVERRIDE Bool XShmPutImage(
    Display*        dpy,
    Drawable        d,
    GC          gc,
    XImage*     image,
    int         src_x,
    int         src_y,
    int         dst_x,
    int         dst_y,
    unsigned int    src_width,
    unsigned int    src_height,
    Bool        send_event
)
{
    LINK_NAMESPACE_FULLNAME(XShmPutImage, "libXext.so.6");

    if (GlobalState::isNative())
        return orig::XShmPutImage(dpy, d, gc, image, src_x, src_y, dst_x, dst_y, src_width, src_height, send_event);

    LOGTRACE(LCF_WINDOW, "%s called with drawable %d", __func__, d);

    /* Only handle the screen draw if no other rendering API is used */
    if ((Global::game_info.video & GameInfo::VDPAU) || (Global::game_info.video & GameInfo::OPENGL))
        return orig::XShmPutImage(dpy, d, gc, image, src_x, src_y, dst_x, dst_y, src_width, src_height, send_event);

    Global::game_info.video |= GameInfo::XSHM;
    Global::game_info.tosend = true;

    x11::gameXImage = image;

    /* Set the game window to that window */
    XlibGameWindow::promote(d);

    /* The surface can change size independently of the window size, so we
     * must check here everytime. */

    /* Resize the screen capture. Only does something if size has changed */
    ScreenCapture::resize(image->width, image->height);

    /* Start the frame boundary and pass the function to draw */
    static RenderHUD renderHUD;
    frameBoundary([&] () {NATIVECALL(orig::XShmPutImage(dpy, d, gc, image, src_x, src_y, dst_x, dst_y, src_width, src_height, send_event));}, renderHUD);

    return True;
    // return orig::XShmPutImage(dpy, d, gc, image, src_x, src_y, dst_x, dst_y, src_width, src_height, send_event);
}

OVERRIDE int XPutImage(
    Display*        dpy,
    Drawable        d,
    GC          gc,
    XImage*     image,
    int         src_x,
    int         src_y,
    int         dst_x,
    int         dst_y,
    unsigned int    width,
    unsigned int    height
)
{
    LINK_NAMESPACE_GLOBAL(XPutImage);

    if (GlobalState::isNative())
        return orig::XPutImage(dpy, d, gc, image, src_x, src_y, dst_x, dst_y, width, height);

    LOGTRACE(LCF_WINDOW, "%s called with drawable %d", __func__, d);

    /* Only handle draws to a game window (not pixmaps), and only if no other
     * presentation API is used. Software renderers (e.g. Chromium) present
     * each frame with a single XPutImage of the damaged area. */
    const int other_video = GameInfo::SDL1 | GameInfo::SDL2 | GameInfo::SDL3 |
        GameInfo::VDPAU | GameInfo::XSHM | GameInfo::VULKAN;
    if ((Global::game_info.video & other_video) || !XlibGameWindow::isGameWindow(d))
        return orig::XPutImage(dpy, d, gc, image, src_x, src_y, dst_x, dst_y, width, height);

    if (!(Global::game_info.video & GameInfo::XIMAGE)) {
        Global::game_info.video |= GameInfo::XIMAGE;
        Global::game_info.tosend = true;
    }

    gamePutXImage = *image;
    x11::gameXImage = &gamePutXImage;

    XlibGameWindow::promote(d);

    ScreenCapture::resize(image->width, image->height);

    /* For browser-based games signaling their frames, only end a frame if the
     * page produced a new frame */
    if (!PageFrame::shouldEndFrame())
        return orig::XPutImage(dpy, d, gc, image, src_x, src_y, dst_x, dst_y, width, height);

    /* The image only contains the damaged area of the frame, so if a previous
     * draw was skipped (fast-forward), we must draw the whole image. */
    static bool full_redraw = false;
    if (Global::skipping_draw)
        full_redraw = true;

    /* Start the frame boundary and pass the function to draw */
    static RenderHUD_XImage renderHUD;
    renderHUD.setImage(image);
    int ret = 0;
    frameBoundary([&] () {
        /* The HUD is drawn on a copy of the whole image */
        XImage* hud_image = renderHUD.renderedImage();
        if (hud_image) {
            NATIVECALL(ret = orig::XPutImage(dpy, d, gc, hud_image, 0, 0, dst_x - src_x, dst_y - src_y, hud_image->width, hud_image->height));
            /* Remove the HUD from the screen once it is not drawn anymore */
            full_redraw = true;
        }
        else if (full_redraw) {
            NATIVECALL(ret = orig::XPutImage(dpy, d, gc, image, 0, 0, dst_x - src_x, dst_y - src_y, image->width, image->height));
            full_redraw = false;
        }
        else {
            NATIVECALL(ret = orig::XPutImage(dpy, d, gc, image, src_x, src_y, dst_x, dst_y, width, height));
        }
    }, renderHUD);

    return ret;
}

}
