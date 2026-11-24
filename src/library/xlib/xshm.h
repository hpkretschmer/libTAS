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

#ifndef LIBTAS_XSHM_H_INCL
#define LIBTAS_XSHM_H_INCL

#include "config.h"

#include "hook.h"
#include "../external/X11/XShm.h"

namespace libtas {

namespace x11 {
    /* XImage to render */
    extern XImage* gameXImage;

/* Set after loading a savestate: the window shows a frame from before
 * loading, so the next XPutImage draw must present the whole image */
extern bool fullRedrawRequested;

/* Copy of the XPutImage game image with the HUD drawn on it, between the
 * rendering of the HUD and the draw of the frame (for encoding the OSD) */
extern XImage* hudXImage;
}

OVERRIDE Bool XShmPutImage(
    Display*        /* dpy */,
    Drawable        /* d */,
    GC          /* gc */,
    XImage*     /* image */,
    int         /* src_x */,
    int         /* src_y */,
    int         /* dst_x */,
    int         /* dst_y */,
    unsigned int    /* src_width */,
    unsigned int    /* src_height */,
    Bool        /* send_event */
);

OVERRIDE int XPutImage(
    Display*        /* dpy */,
    Drawable        /* d */,
    GC          /* gc */,
    XImage*     /* image */,
    int         /* src_x */,
    int         /* src_y */,
    int         /* dst_x */,
    int         /* dst_y */,
    unsigned int    /* width */,
    unsigned int    /* height */
);

}

#endif
