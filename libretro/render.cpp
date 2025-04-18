/*
 *  Copyright (C) 2002-2013  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <sys/types.h>
#include <assert.h>
#include <math.h>
#include <cassert>
#include <string>
#include <string_view> // C++17
#include <algorithm> // For std::fill, std::copy
#include <cstdio> // For fprintf

#include "dosbox.h"
#include "video.h"
#include "render.h"
#include "setup.h"
#include "control.h"
#include "mapper.h"
#include "cross.h"
#include "hardware.h"
#include "support.h"

#include "render_scalers.h"

Render_t render;
ScalerLineHandler_t RENDER_DrawLine;

static void RENDER_CallBack(GFX_CallBackFunctions_t function);

inline void Check_Palette() {
    fprintf(stderr, "[RENDER] Checking palette, mode=%d\n", render.scale.inMode);
    if (render.scale.inMode != scalerMode8) {
        render.pal.changed = false;
        render.pal.first = 256;
        render.pal.last = 0;
        return;
    }

    if (render.pal.changed) {
        std::fill(std::begin(render.pal.modified), std::end(render.pal.modified), 0); // C++17
        render.pal.changed = false;
    }
    if (render.pal.first > render.pal.last) {
        return;
    }

    for (Bitu i = render.pal.first; i <= render.pal.last; ++i) {
        Bit8u r = render.pal.rgb[i].red;
        Bit8u g = render.pal.rgb[i].green;
        Bit8u b = render.pal.rgb[i].blue;
        Bit32u newPal = GFX_GetRGB(r, g, b);
        if (newPal != render.pal.lut.b32[i]) {
            render.pal.changed = true;
            render.pal.modified[i] = 1;
            render.pal.lut.b32[i] = newPal;
        }
    }
    render.pal.first = 256;
    render.pal.last = 0;
}

void RENDER_SetPal(Bit8u entry, Bit8u red, Bit8u green, Bit8u blue) {
    fprintf(stderr, "[RENDER] Setting palette entry=%u, r=%u, g=%u, b=%u\n", entry, red, green, blue);
    render.pal.rgb[entry].red = red;
    render.pal.rgb[entry].green = green;
    render.pal.rgb[entry].blue = blue;
    if (render.pal.first > entry) render.pal.first = entry;
    if (render.pal.last < entry) render.pal.last = entry;
}

static void RENDER_EmptyLineHandler(const void* /*src*/) {
    fprintf(stderr, "[RENDER] Empty line handler called\n");
}

static void RENDER_StartLineHandler(const void* s) {
    fprintf(stderr, "[RENDER] Start line handler, src=%p, inLine=%lu\n", s, render.scale.inLine);
    if (s) {
        const Bitu* src = static_cast<const Bitu*>(s);
        Bit8u* cache = render.scale.cacheRead; // cacheRead is Bit8u*
        for (Bits x = render.src.start; x > 0; --x) {
            Bitu srcVal = *src++;
            Bitu cacheVal = *reinterpret_cast<Bitu*>(cache);
            if (GCC_UNLIKELY(srcVal != cacheVal)) {
                fprintf(stderr, "[RENDER] Cache mismatch, starting update\n");
                if (!GFX_StartUpdate(render.scale.outWrite, render.scale.outPitch)) {
                    fprintf(stderr, "[RENDER] GFX_StartUpdate failed\n");
                    RENDER_DrawLine = RENDER_EmptyLineHandler;
                    return;
                }
                render.scale.outWrite += render.scale.outPitch * Scaler_ChangedLines[0];
                RENDER_DrawLine = render.scale.lineHandler;
                RENDER_DrawLine(s);
                return;
            }
            cache += sizeof(Bitu);
        }
    }
    render.scale.cacheRead += render.scale.cachePitch;
    Scaler_ChangedLines[0] += Scaler_Aspect[render.scale.inLine];
    render.scale.inLine++;
    render.scale.outLine++;
}

static void RENDER_FinishLineHandler(const void* s) {
    fprintf(stderr, "[RENDER] Finish line handler, src=%p\n", s);
    if (s) {
        const Bitu* src = static_cast<const Bitu*>(s);
        Bit8u* cache = render.scale.cacheRead;
        if (render.src.start > 0) {
            std::copy(reinterpret_cast<const Bit8u*>(src),
                      reinterpret_cast<const Bit8u*>(src) + render.src.start * sizeof(Bitu),
                      cache); // C++17
        }
    }
    render.scale.cacheRead += render.scale.cachePitch;
}

static void RENDER_ClearCacheHandler(const void* src) {
    fprintf(stderr, "[RENDER] Clear cache handler, src=%p\n", src);
    Bitu width = render.scale.cachePitch / 4;
    Bit32u* srcLine = static_cast<Bit32u*>(const_cast<void*>(src));
    Bit8u* cache = render.scale.cacheRead;
    Bit32u* cacheLine = reinterpret_cast<Bit32u*>(cache);
    for (Bitu x = 0; x < width; ++x) {
        cacheLine[x] = ~srcLine[x];
    }
    render.scale.lineHandler(src);
}

bool RENDER_StartUpdate() {
    fprintf(stderr, "[RENDER] Starting update, updating=%d, active=%d, frameskip=%lu/%lu\n",
            render.updating, render.active, render.frameskip.count, render.frameskip.max);
    if (render.updating || !render.active) {
        return false;
    }
    if (render.frameskip.count < render.frameskip.max) {
        render.frameskip.count++;
        return false;
    }
    render.frameskip.count = 0;
    Check_Palette();
    render.scale.inLine = 0;
    render.scale.outLine = 0;
    render.scale.cacheRead = (Bit8u*)&scalerSourceCache; // Cast to Bit8u*
    render.scale.outWrite = nullptr;
    render.scale.outPitch = 0;
    Scaler_ChangedLines[0] = 0;
    Scaler_ChangedLineIndex = 0;
    if (render.scale.clearCache) {
        if (!GFX_StartUpdate(render.scale.outWrite, render.scale.outPitch)) {
            fprintf(stderr, "[RENDER] GFX_StartUpdate failed for clear cache\n");
            return false;
        }
        fprintf(stderr, "[RENDER] Clear cache: outWrite=%p, outPitch=%lu\n", render.scale.outWrite, render.scale.outPitch);
        render.fullFrame = true;
        render.scale.clearCache = false;
        RENDER_DrawLine = RENDER_ClearCacheHandler;
    } else {
        if (render.pal.changed) {
            if (!GFX_StartUpdate(render.scale.outWrite, render.scale.outPitch)) {
                fprintf(stderr, "[RENDER] GFX_StartUpdate failed for palette update\n");
                return false;
            }
            fprintf(stderr, "[RENDER] Palette update: outWrite=%p, outPitch=%lu\n", render.scale.outWrite, render.scale.outPitch);
            RENDER_DrawLine = render.scale.linePalHandler;
            render.fullFrame = true;
        } else {
            RENDER_DrawLine = RENDER_StartLineHandler;
            render.fullFrame = false;
        }
    }
    render.updating = true;
    return true;
}

static void RENDER_Halt() {
    fprintf(stderr, "[RENDER] Halting renderer\n");
    RENDER_DrawLine = RENDER_EmptyLineHandler;
    GFX_EndUpdate(nullptr);
    render.updating = false;
    render.active = false;
}

void RENDER_EndUpdate(bool abort) {
    fprintf(stderr, "[RENDER] Ending update, abort=%d, outWrite=%p\n", abort, render.scale.outWrite);
    if (!render.updating) {
        return;
    }
    RENDER_DrawLine = RENDER_EmptyLineHandler;
    if (render.scale.outWrite) {
        GFX_EndUpdate(abort ? nullptr : Scaler_ChangedLines);
        render.frameskip.hadSkip[render.frameskip.index] = 0;
    } else {
        render.frameskip.hadSkip[render.frameskip.index] = 1;
    }
    render.frameskip.index = (render.frameskip.index + 1) & (RENDER_SKIP_CACHE - 1);
    render.updating = false;
}

static Bitu MakeAspectTable(Bitu skip, Bitu height, double scaley, Bitu miny) {
    fprintf(stderr, "[RENDER] Making aspect table: skip=%lu, height=%lu, scaley=%f, miny=%lu\n", skip, height, scaley, miny);
    double lines = 0;
    Bitu linesadded = 0;
    std::fill(Scaler_Aspect, Scaler_Aspect + skip, 0); // C++17
    height += skip;
    for (Bitu i = skip; i < height; ++i) {
        lines += scaley;
        if (lines >= miny) {
            Bitu templines = static_cast<Bitu>(lines);
            lines -= templines;
            linesadded += templines;
            Scaler_Aspect[i] = templines;
        } else {
            Scaler_Aspect[i] = 0;
        }
    }
    return linesadded;
}

static void RENDER_Reset() {
    fprintf(stderr, "[RENDER] Resetting renderer: width=%lu, height=%lu, bpp=%lu, dblw=%d, dblh=%d\n",
            render.src.width, render.src.height, render.src.bpp, render.src.dblw, render.src.dblh);
    Bitu width = render.src.width;
    Bitu height = render.src.height;
    bool dblw = render.src.dblw;
    bool dblh = render.src.dblh;

    double gfx_scalew = 1.0;
    double gfx_scaleh = 1.0;
    if (render.aspect) {
        if (render.src.ratio > 1.0) {
            gfx_scaleh = render.src.ratio;
        } else {
            gfx_scalew = 1.0 / render.src.ratio;
        }
    }

    Bitu gfx_flags, xscale, yscale;
    ScalerSimpleBlock_t* simpleBlock = &ScaleNormal1x;
    ScalerComplexBlock_t* complexBlock = nullptr;
    if ((dblh && dblw) || (render.scale.forced && !dblh && !dblw)) {
        simpleBlock = &ScaleNormal1x;
    } else if (dblw) {
        simpleBlock = &ScaleNormalDw;
    } else if (dblh) {
        simpleBlock = &ScaleNormalDh;
    } else {
        simpleBlock = &ScaleNormal1x;
    }

    gfx_flags = simpleBlock->gfxFlags;
    xscale = simpleBlock->xscale;
    yscale = simpleBlock->yscale;

    switch (render.src.bpp) {
    case 8:
        render.src.start = (render.src.width * 1) / sizeof(Bitu);
        gfx_flags |= (gfx_flags & GFX_CAN_8) ? GFX_LOVE_8 : GFX_LOVE_32;
        break;
    case 15:
        render.src.start = (render.src.width * 2) / sizeof(Bitu);
        gfx_flags |= GFX_LOVE_15 | GFX_RGBONLY;
        gfx_flags &= ~GFX_CAN_8;
        break;
    case 16:
        render.src.start = (render.src.width * 2) / sizeof(Bitu);
        gfx_flags |= GFX_LOVE_16 | GFX_RGBONLY;
        gfx_flags &= ~GFX_CAN_8;
        break;
    case 32:
        render.src.start = (render.src.width * 4) / sizeof(Bitu);
        gfx_flags |= GFX_LOVE_32 | GFX_RGBONLY;
        gfx_flags &= ~GFX_CAN_8;
        break;
    default:
        fprintf(stderr, "[RENDER] Invalid bpp=%lu\n", render.src.bpp);
        E_Exit("RENDER:Wrong source bpp %lu", static_cast<unsigned long>(render.src.bpp));
    }

    gfx_flags = GFX_GetBestMode(gfx_flags);
    if (!gfx_flags) {
        fprintf(stderr, "[RENDER] Failed to get graphics mode\n");
        E_Exit("Failed to create a rendering output");
    }

    width *= xscale;
    Bitu skip = complexBlock ? 1 : 0;
    if (gfx_flags & GFX_SCALING) {
        height = MakeAspectTable(skip, render.src.height, yscale, yscale);
    } else if ((gfx_flags & GFX_CAN_RANDOM) && gfx_scaleh > 1) {
        gfx_scaleh *= yscale;
        height = MakeAspectTable(skip, render.src.height, gfx_scaleh, yscale);
    } else {
        gfx_flags &= ~GFX_CAN_RANDOM;
        height = MakeAspectTable(skip, render.src.height, yscale, yscale);
    }

    gfx_flags = GFX_SetSize(width, height, gfx_flags, gfx_scalew, gfx_scaleh, &RENDER_CallBack);
    if (!(gfx_flags & GFX_CAN_32)) {
        fprintf(stderr, "[RENDER] Failed to set 32-bit mode\n");
        E_Exit("Failed to create a rendering output");
    }
    render.scale.outMode = scalerMode32;

    ScalerLineBlock_t* lineBlock = (gfx_flags & GFX_HARDWARE) ? &simpleBlock->Linear : &simpleBlock->Random;
    switch (render.src.bpp) {
    case 8:
        render.scale.lineHandler = (*lineBlock)[0][render.scale.outMode];
        render.scale.linePalHandler = (*lineBlock)[4][render.scale.outMode];
        render.scale.inMode = scalerMode8;
        render.scale.cachePitch = render.src.width * 1;
        break;
    case 15:
        render.scale.lineHandler = (*lineBlock)[1][render.scale.outMode];
        render.scale.linePalHandler = nullptr;
        render.scale.inMode = scalerMode15;
        render.scale.cachePitch = render.src.width * 2;
        break;
    case 16:
        render.scale.lineHandler = (*lineBlock)[2][render.scale.outMode];
        render.scale.linePalHandler = nullptr;
        render.scale.inMode = scalerMode16;
        render.scale.cachePitch = render.src.width * 2;
        break;
    case 32:
        render.scale.lineHandler = (*lineBlock)[3][render.scale.outMode];
        render.scale.linePalHandler = nullptr;
        render.scale.inMode = scalerMode32;
        render.scale.cachePitch = render.src.width * 4;
        break;
    default:
        fprintf(stderr, "[RENDER] Invalid bpp=%lu\n", render.src.bpp);
        E_Exit("RENDER:Wrong source bpp %lu", static_cast<unsigned long>(render.src.bpp));
    }

    render.scale.blocks = render.src.width / SCALER_BLOCKSIZE;
    render.scale.lastBlock = render.src.width % SCALER_BLOCKSIZE;
    render.scale.inHeight = render.src.height;
    render.pal.first = 0;
    render.pal.last = 255;
    render.pal.changed = false;
    std::fill(std::begin(render.pal.modified), std::end(render.pal.modified), 0); // C++17
    RENDER_DrawLine = RENDER_FinishLineHandler;
    render.scale.outWrite = nullptr;
    render.scale.clearCache = true;
    render.active = true;
    fprintf(stderr, "[RENDER] Reset complete: outMode=%d, blocks=%lu, lastBlock=%lu\n",
            render.scale.outMode, render.scale.blocks, render.scale.lastBlock);
}

static void RENDER_CallBack(GFX_CallBackFunctions_t function) {
    fprintf(stderr, "[RENDER] Callback: function=%d\n", function);
    switch (function) {
    case GFX_CallBackStop:
        RENDER_Halt();
        break;
    case GFX_CallBackRedraw:
        render.scale.clearCache = true;
        break;
    case GFX_CallBackReset:
        GFX_EndUpdate(nullptr);
        RENDER_Reset();
        break;
    default:
        fprintf(stderr, "[RENDER] Unhandled callback function=%d\n", function);
        E_Exit("Unhandled GFX_CallBackReset %d", function);
    }
}

void RENDER_SetSize(Bitu width, Bitu height, Bitu bpp, float fps, double ratio, bool dblw, bool dblh) {
    fprintf(stderr, "[RENDER] Setting size: width=%lu, height=%lu, bpp=%lu, fps=%f, ratio=%f, dblw=%d, dbleq=%d\n",
            width, height, bpp, fps, ratio, dblw, dblh);
    RENDER_Halt();
    if (!width || !height || width > SCALER_MAXWIDTH || height > SCALER_MAXHEIGHT) {
        fprintf(stderr, "[RENDER] Invalid size: width=%lu, height=%lu\n", width, height);
        return;
    }
    if (ratio > 1) {
        double target = height * ratio + 0.025;
        ratio = target / height;
    }
    render.src.width = width;
    render.src.height = height;
    render.src.bpp = bpp;
    render.src.dblw = dblw;
    render.src.dblh = dblh;
    render.src.fps = fps;
    render.src.ratio = ratio;
    RENDER_Reset();
}

extern void GFX_SetTitle(Bit32s cycles, Bits frameskip, bool paused);
static void IncreaseFrameSkip(bool pressed) {
    if (!pressed) return;
    if (render.frameskip.max < 10) render.frameskip.max++;
    fprintf(stderr, "[RENDER] Frame skip increased to %lu\n", render.frameskip.max);
    GFX_SetTitle(-1, render.frameskip.max, false);
}

static void DecreaseFrameSkip(bool pressed) {
    if (!pressed) return;
    if (render.frameskip.max > 0) render.frameskip.max--;
    fprintf(stderr, "[RENDER] Frame skip decreased to %lu\n", render.frameskip.max);
    GFX_SetTitle(-1, render.frameskip.max, false);
}

bool RENDER_Init(Section* sec) {
    fprintf(stderr, "[RENDER] Initializing renderer\n");
    if (!sec) {
        fprintf(stderr, "[RENDER] Error: Null section pointer\n");
        return false;
    }

    Section_prop* section = static_cast<Section_prop*>(sec);
    if (!section) {
        fprintf(stderr, "[RENDER] Error: Invalid section pointer\n");
        return false;
    }

    static bool running = false;
    bool aspect = render.aspect;
    Bitu scalersize = render.scale.size;
    bool scalerforced = render.scale.forced;
    scalerOperation_t scaleOp = render.scale.op;

    render.pal.first = 256;
    render.pal.last = 0;
    render.aspect = section->Get_bool("aspect");
    render.frameskip.max = section->Get_int("frameskip");
    render.frameskip.count = 0;

    std::string scaler = "normal"; // Default scaler
    bool force_scaler = false;

    if (control && control->cmdline) {
        std::string cline; // Changed to std::string
        if (control->cmdline->FindString("-scaler", cline, false)) {
            fprintf(stderr, "[RENDER] Found -scaler %s\n", cline.c_str());
            section->HandleInputline("scaler=" + cline);
        } else if (control->cmdline->FindString("-forcescaler", cline, false)) {
            fprintf(stderr, "[RENDER] Found -forcescaler %s\n", cline.c_str());
            section->HandleInputline("scaler=" + cline + " forced");
            force_scaler = true;
        } else {
            fprintf(stderr, "[RENDER] No scaler specified, using default: %s\n", scaler.c_str());
            section->HandleInputline("scaler=" + scaler);
        }
    } else {
        fprintf(stderr, "[RENDER] No command line, using default scaler: %s\n", scaler.c_str());
        section->HandleInputline("scaler=" + scaler);
    }

    Prop_multival* prop = section->Get_multival("scaler");
    if (!prop) {
        fprintf(stderr, "[RENDER] Failed to get scaler property, using default: %s\n", scaler.c_str());
        render.scale.op = scalerOpNormal;
        render.scale.size = 1;
        render.scale.forced = force_scaler;
    } else {
        scaler = prop->GetSection()->Get_string("type");
        std::string force = prop->GetSection()->Get_string("force"); // Changed to std::string
        render.scale.forced = (force == "forced");
        render.scale.op = scalerOpNormal; // Adjust based on scaler type if needed
        render.scale.size = 1;
        fprintf(stderr, "[RENDER] Scaler set to type=%s, forced=%s\n", scaler.c_str(), force.c_str());
    }

    if (running && render.src.bpp && (render.aspect != aspect || render.scale.op != scaleOp ||
                                      render.scale.size != scalersize || render.scale.forced != scalerforced)) {
        fprintf(stderr, "[RENDER] Reinitializing due to config change\n");
        RENDER_CallBack(GFX_CallBackReset);
    }

    if (!running) render.updating = true;
    running = true;

    MAPPER_AddHandler(DecreaseFrameSkip, MK_f7, MMOD1, "decfskip", "Dec Fskip");
    MAPPER_AddHandler(IncreaseFrameSkip, MK_f8, MMOD1, "incfskip", "Inc Fskip");
    GFX_SetTitle(-1, render.frameskip.max, false);

    fprintf(stderr, "[RENDER] Initialization complete\n");
    return true;
}