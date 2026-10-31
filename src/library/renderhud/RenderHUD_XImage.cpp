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

#include "RenderHUD_XImage.h"

#include "logging.h"
#include "GlobalState.h"

#include "../external/imgui/imgui.h"
#include "../external/imgui/imgui_impl_xlib.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace libtas {

RenderHUD_XImage::~RenderHUD_XImage()
{
    ImGui_ImplXlib_Shutdown();
    ImGui::DestroyContext();
}

void RenderHUD_XImage::setImage(const XImage* image)
{
    gameImage = image;
}

void RenderHUD_XImage::newFrame()
{
    rendered = false;

    if (!ImGui::GetCurrentContext()) {
        if (!gameImage || (gameImage->bits_per_pixel != 32))
            return;

        if (!RenderHUD::init())
            return;

        /* Textures stay in ImGui's CPU memory, where we sample them */
        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = "libtas_ximage";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    }

    RenderHUD::newFrame();
}

/* Position of the lowest bit of a color mask */
static int maskShift(unsigned long mask)
{
    int shift = 0;
    while (mask && !(mask & 1)) {
        mask >>= 1;
        shift++;
    }
    return shift;
}

/* Signed area of the parallelogram formed by (b - a) and (p - a) */
static inline float edge(const ImVec2& a, const ImVec2& b, float px, float py)
{
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}

struct PixelFormat {
    int rshift, gshift, bshift;
};

static void drawTriangle(XImage* img, const PixelFormat& pf, const ImVec4& clip,
    const ImDrawVert& v0, const ImDrawVert& v1, const ImDrawVert& v2, ImTextureData* tex)
{
    float area = edge(v0.pos, v1.pos, v2.pos.x, v2.pos.y);
    if (std::fabs(area) < 1e-6f)
        return;

    int minx = std::max(static_cast<int>(std::floor(std::min({v0.pos.x, v1.pos.x, v2.pos.x}))), static_cast<int>(clip.x));
    int maxx = std::min(static_cast<int>(std::ceil(std::max({v0.pos.x, v1.pos.x, v2.pos.x}))), static_cast<int>(clip.z));
    int miny = std::max(static_cast<int>(std::floor(std::min({v0.pos.y, v1.pos.y, v2.pos.y}))), static_cast<int>(clip.y));
    int maxy = std::min(static_cast<int>(std::ceil(std::max({v0.pos.y, v1.pos.y, v2.pos.y}))), static_cast<int>(clip.w));
    minx = std::max(minx, 0);
    miny = std::max(miny, 0);
    maxx = std::min(maxx, img->width);
    maxy = std::min(maxy, img->height);

    const ImVec4 c0 = ImGui::ColorConvertU32ToFloat4(v0.col);
    const ImVec4 c1 = ImGui::ColorConvertU32ToFloat4(v1.col);
    const ImVec4 c2 = ImGui::ColorConvertU32ToFloat4(v2.col);

    for (int y = miny; y < maxy; y++) {
        uint32_t* row = reinterpret_cast<uint32_t*>(img->data + y * img->bytes_per_line);
        float py = y + 0.5f;
        for (int x = minx; x < maxx; x++) {
            float px = x + 0.5f;

            /* Barycentric coordinates, accepting both triangle orientations */
            float w0 = edge(v1.pos, v2.pos, px, py) / area;
            float w1 = edge(v2.pos, v0.pos, px, py) / area;
            float w2 = edge(v0.pos, v1.pos, px, py) / area;
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;

            float r = w0 * c0.x + w1 * c1.x + w2 * c2.x;
            float g = w0 * c0.y + w1 * c1.y + w2 * c2.y;
            float b = w0 * c0.z + w1 * c1.z + w2 * c2.z;
            float a = w0 * c0.w + w1 * c1.w + w2 * c2.w;

            if (tex && tex->Pixels) {
                float u = w0 * v0.uv.x + w1 * v1.uv.x + w2 * v2.uv.x;
                float v = w0 * v0.uv.y + w1 * v1.uv.y + w2 * v2.uv.y;
                int tx = std::clamp(static_cast<int>(u * tex->Width), 0, tex->Width - 1);
                int ty = std::clamp(static_cast<int>(v * tex->Height), 0, tex->Height - 1);
                const unsigned char* t = static_cast<const unsigned char*>(tex->GetPixelsAt(tx, ty));
                if (tex->Format == ImTextureFormat_Alpha8) {
                    a *= t[0] / 255.0f;
                }
                else {
                    r *= t[0] / 255.0f;
                    g *= t[1] / 255.0f;
                    b *= t[2] / 255.0f;
                    a *= t[3] / 255.0f;
                }
            }

            if (a <= 0)
                continue;

            uint32_t dst = row[x];
            float dr = ((dst >> pf.rshift) & 0xff) / 255.0f;
            float dg = ((dst >> pf.gshift) & 0xff) / 255.0f;
            float db = ((dst >> pf.bshift) & 0xff) / 255.0f;
            uint32_t or_ = static_cast<uint32_t>((r * a + dr * (1 - a)) * 255.0f + 0.5f);
            uint32_t og = static_cast<uint32_t>((g * a + dg * (1 - a)) * 255.0f + 0.5f);
            uint32_t ob = static_cast<uint32_t>((b * a + db * (1 - a)) * 255.0f + 0.5f);
            row[x] = (or_ << pf.rshift) | (og << pf.gshift) | (ob << pf.bshift);
        }
    }
}

void RenderHUD_XImage::render()
{
    if (!ImGui::GetCurrentContext())
        return;

    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();

    /* Textures are sampled directly from ImGui's own pixel buffers */
    if (draw_data->Textures) {
        for (ImTextureData* tex : *draw_data->Textures) {
            if ((tex->Status == ImTextureStatus_WantCreate) || (tex->Status == ImTextureStatus_WantUpdates)) {
                tex->SetTexID(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(tex)));
                tex->SetStatus(ImTextureStatus_OK);
            }
            else if ((tex->Status == ImTextureStatus_WantDestroy) && (tex->UnusedFrames > 0)) {
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }

    if (!gameImage || (gameImage->bits_per_pixel != 32) || (draw_data->TotalVtxCount == 0))
        return;

    /* Draw on a copy, because the game may only update parts of its image
     * in the next frames */
    hudImage = *gameImage;
    size_t size = static_cast<size_t>(gameImage->bytes_per_line) * gameImage->height;
    hudPixels.resize(size);
    memcpy(hudPixels.data(), gameImage->data, size);
    hudImage.data = hudPixels.data();

    PixelFormat pf = {maskShift(gameImage->red_mask), maskShift(gameImage->green_mask), maskShift(gameImage->blue_mask)};

    for (const ImDrawList* cmd_list : draw_data->CmdLists) {
        const ImDrawVert* vtx = cmd_list->VtxBuffer.Data;
        const ImDrawIdx* idx = cmd_list->IdxBuffer.Data;
        for (const ImDrawCmd& cmd : cmd_list->CmdBuffer) {
            if (cmd.UserCallback)
                continue;

            /* The HUD has a single viewport at (0,0), so ImGui coordinates
             * are image coordinates */
            const ImVec4& clip = cmd.ClipRect;
            ImTextureData* tex = cmd.TexRef._TexData;

            for (unsigned int i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const ImDrawIdx* tri = idx + cmd.IdxOffset + i;
                drawTriangle(&hudImage, pf, clip,
                    vtx[cmd.VtxOffset + tri[0]], vtx[cmd.VtxOffset + tri[1]], vtx[cmd.VtxOffset + tri[2]], tex);
            }
        }
    }

    rendered = true;
}

XImage* RenderHUD_XImage::renderedImage()
{
    return rendered ? &hudImage : nullptr;
}

}
