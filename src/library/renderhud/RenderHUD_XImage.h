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

#ifndef LIBTAS_RENDERHUD_XIMAGE_H_INCL
#define LIBTAS_RENDERHUD_XIMAGE_H_INCL

#include "RenderHUD_Base_Linux.h"

#include <X11/Xlib.h>
#include <vector>

namespace libtas {

/**
 * @class RenderHUD_XImage
 * @brief Software HUD backend for games presenting XImages.
 *
 * Rasterizes the ImGui draw data on the CPU into a copy of the game image,
 * which is then presented instead of the game image.
 */
class RenderHUD_XImage : public RenderHUD_Base_Linux
{
    public:
        ~RenderHUD_XImage();

        /**
         * @brief Sets the game image that the HUD is drawn on.
         *
         * @param[in] image XImage presented by the game this frame
         */
        void setImage(const XImage* image);

        void newFrame();
        void render();

        /**
         * @brief Returns the game image with the HUD drawn on it.
         *
         * @return the image to present, or nullptr if the HUD was not drawn
         *         this frame
         */
        XImage* renderedImage();

    private:
        const XImage* gameImage = nullptr;

        /* Copy of the game image with the HUD drawn on it */
        XImage hudImage;
        std::vector<char> hudPixels;
        bool rendered = false;
};
}

#endif
