/*
 *  Copyright (C) 2002-2015  The DOSBox Team
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


#include <string.h>
#include <math.h>
#include "dosbox.h"
#include "video.h"
#include "render.h"
#include "render_scalers.h"
#include "vga.h"
#include "pic.h"

//#undef C_DEBUG
//#define C_DEBUG 1
//#define LOG(X,Y) LOG_MSG

#define VGA_PARTS 4

typedef Bit8u * (* VGA_Line_Handler)(Bitu vidstart, Bitu line);

static VGA_Line_Handler VGA_DrawLine;
static Bit8u TempLine[SCALER_MAXWIDTH * 4];

static Bit8u * VGA_Draw_1BPP_Line(Bitu vidstart, Bitu line) {
    const Bit8u *base = vga.tandy.draw_base + ((line & vga.tandy.line_mask) << vga.tandy.line_shift);
    Bit32u *draw = (Bit32u *)TempLine;
    Bitu mask = 8 * 1024 - 1;
    for (Bitu x = vga.draw.blocks; x > 0; x--, vidstart++) {
        Bitu val = base[vidstart & mask];
        draw[0] = CGA_2_Table[val >> 4];
        draw[1] = CGA_2_Table[val & 0xf];
        draw += 2;
    }
    return TempLine;
}

static Bit8u * VGA_Draw_2BPP_Line(Bitu vidstart, Bitu line) {
    const Bit8u *base = vga.tandy.draw_base + ((line & vga.tandy.line_mask) << vga.tandy.line_shift);
    Bit32u *draw = (Bit32u *)TempLine;
    Bitu mask = vga.tandy.addr_mask;
    for (Bitu x = vga.draw.blocks; x > 0; x--) {
        *draw++ = CGA_4_Table[base[vidstart++ & mask]];
    }
    return TempLine;
}

static Bit8u * VGA_Draw_2BPPHiRes_Line(Bitu vidstart, Bitu line) {
    const Bit8u *base = vga.tandy.draw_base + ((line & vga.tandy.line_mask) << vga.tandy.line_shift);
    Bit32u *draw = (Bit32u *)TempLine;
    Bitu mask = vga.tandy.addr_mask;
    for (Bitu x = vga.draw.blocks; x > 0; x--) {
        Bitu val1 = base[vidstart++ & mask];
        Bitu val2 = base[vidstart++ & mask];
        draw[0] = CGA_4_HiRes_Table[(val1 >> 4) | (val2 & 0xf0)];
        draw[1] = CGA_4_HiRes_Table[(val1 & 0x0f) | ((val2 & 0x0f) << 4)];
        draw += 2;
    }
    return TempLine;
}

static Bitu temp[643]={0};

static Bit8u * VGA_Draw_CGA16_Line(Bitu vidstart, Bitu line) {
    const Bit8u *base = vga.tandy.draw_base + ((line & vga.tandy.line_mask) << vga.tandy.line_shift);
    Bit32u *draw = (Bit32u *)TempLine;
    Bitu mask = 8 * 1024 - 1;
    Bitu temp[643] = {0};
    temp[1] = (base[vidstart & mask] >> 6) & 3;
    for (Bitu x = 2; x < 640; x += 2) {
        temp[x] = temp[x - 1] & 0xf;
        temp[x + 1] = (temp[x] << 2) | ((base[(vidstart + (x >> 3)) & mask] >> (6 - (x & 6))) & 3);
    }
    temp[640] = temp[639] & 0xf;
    temp[641] = temp[640] << 2;
    temp[642] = temp[641] & 0xf;

    for (Bitu x = 0, i = 2; x < vga.draw.blocks; x++) {
        draw[0] = 0xc0708030 | temp[i] | (temp[i + 1] << 8) | (temp[i + 2] << 16) | (temp[i + 3] << 24);
        draw[1] = 0xc0708030 | temp[i + 4] | (temp[i + 5] << 8) | (temp[i + 6] << 16) | (temp[i + 7] << 24);
        draw += 2;
        i += 8;
    }
    return TempLine;
}

static Bit8u * VGA_Draw_4BPP_Line(Bitu vidstart, Bitu line) {
    const Bit8u *base = vga.tandy.draw_base + ((line & vga.tandy.line_mask) << vga.tandy.line_shift);
    Bit8u *draw = TempLine;
    Bitu mask = vga.tandy.addr_mask;
    for (Bitu x = vga.draw.blocks * 2; x > 0; x--) {
        Bit8u byte = base[vidstart++ & mask];
        *draw++ = vga.attr.palette[byte >> 4];
        *draw++ = vga.attr.palette[byte & 0x0f];
    }
    return TempLine;
}

static Bit8u * VGA_Draw_4BPP_Line_Double(Bitu vidstart, Bitu line) {
    const Bit8u *base = vga.tandy.draw_base + ((line & vga.tandy.line_mask) << vga.tandy.line_shift);
    Bit8u *draw = TempLine;
    Bitu mask = vga.tandy.addr_mask;
    for (Bitu x = vga.draw.blocks; x > 0; x--) {
        Bit8u byte = base[vidstart++ & mask];
        Bit8u data = vga.attr.palette[byte >> 4];
        draw[0] = draw[1] = data;
        data = vga.attr.palette[byte & 0x0f];
        draw[2] = draw[3] = data;
        draw += 4;
    }
    return TempLine;
}

#ifdef VGA_KEEP_CHANGES
static Bit8u * VGA_Draw_Changes_Line(Bitu vidstart, Bitu line) {
    Bitu checkMask = vga.changes.checkMask;
    Bit8u *map = vga.changes.map;
    Bitu start = vidstart >> VGA_CHANGE_SHIFT;
    Bitu end = (vidstart + vga.draw.line_length) >> VGA_CHANGE_SHIFT;
    Bitu offset = vidstart & vga.draw.linear_mask;
    Bit8u *ret = vga.draw.linear_base + offset;
    while (start <= end) {
        if (map[start++] & checkMask) {
            if (vga.draw.linear_mask - offset < vga.draw.line_length)
                memcpy(vga.draw.linear_base + vga.draw.linear_mask + 1, vga.draw.linear_base, vga.draw.line_length);
#if !defined(C_UNALIGNED_MEMORY)
            if (((Bitu)ret) & (sizeof(Bitu) - 1)) {
                memcpy(TempLine, ret, vga.draw.line_length);
                return TempLine;
            }
#endif
            return ret;
        }
    }
    return 0;
}
#endif

static Bit8u * VGA_Draw_Linear_Line(Bitu vidstart, Bitu /*line*/) {
    Bitu offset = vidstart & vga.draw.linear_mask;
    Bit8u *ret = vga.draw.linear_base + offset;
    if (GCC_UNLIKELY((vga.draw.line_length + offset) & ~vga.draw.linear_mask)) {
        Bitu end = (offset + vga.draw.line_length) & vga.draw.linear_mask;
        Bitu wrapped_len = end & 0xFFF;
        Bitu unwrapped_len = vga.draw.line_length - wrapped_len;
        memcpy(TempLine, ret, unwrapped_len);
        memcpy(TempLine + unwrapped_len, vga.draw.linear_base, wrapped_len);
        ret = TempLine;
    }
#if !defined(C_UNALIGNED_MEMORY)
    if (GCC_UNLIKELY(((Bitu)ret) & (sizeof(Bitu) - 1))) {
        memcpy(TempLine, ret, vga.draw.line_length);
        return TempLine;
    }
#endif
    return ret;
}

static Bit8u * VGA_Draw_Xlat16_Linear_Line(Bitu vidstart, Bitu /*line*/) {
    Bitu offset = vidstart & vga.draw.linear_mask;
    Bit8u *src = vga.draw.linear_base + offset;
    Bit16u *temps = (Bit16u *)TempLine;
    if (GCC_UNLIKELY((vga.draw.line_length + offset) & ~vga.draw.linear_mask)) {
        Bitu end = (offset + vga.draw.line_length) & vga.draw.linear_mask;
        Bitu wrapped_len = end & 0xFFF;
        Bitu unwrapped_len = vga.draw.line_length - wrapped_len;
        for (Bitu i = 0; i < unwrapped_len; i++) temps[i] = vga.dac.xlat16[src[i]];
        for (Bitu i = 0; i < wrapped_len; i++) temps[i + unwrapped_len] = vga.dac.xlat16[vga.draw.linear_base[i]];
    } else {
        for (Bitu i = 0; i < vga.draw.line_length; i++) temps[i] = vga.dac.xlat16[src[i]];
    }
    return TempLine;
}

//Test version, might as well keep it
/* static Bit8u * VGA_Draw_Chain_Line(Bitu vidstart, Bitu line) {
	Bitu i = 0;
	for ( i = 0; i < vga.draw.width;i++ ) {
		Bitu addr = vidstart + i;
		TempLine[i] = vga.mem.linear[((addr&~3)<<2)+(addr&3)];
	}
	return TempLine;
} */

static Bit8u * VGA_Draw_VGA_Line_HWMouse(Bitu vidstart, Bitu /*line*/) {
    if (!svga.hardware_cursor_active || !svga.hardware_cursor_active())
        return vga.mem.linear + vidstart;

    Bitu lineat = (vidstart - (vga.config.real_start << 2)) / vga.draw.width;
    if ((vga.s3.hgc.posx >= vga.draw.width) || (lineat < vga.s3.hgc.originy) || 
        (lineat > (vga.s3.hgc.originy + (63U - vga.s3.hgc.posy))))
        return vga.mem.linear + vidstart;

    memcpy(TempLine, vga.mem.linear + vidstart, vga.draw.width);
    Bitu sourceStartBit = ((lineat - vga.s3.hgc.originy) + vga.s3.hgc.posy) * 64 + vga.s3.hgc.posx;
    Bitu cursorMemStart = ((sourceStartBit >> 2) & ~1) + (((Bit32u)vga.s3.hgc.startaddr) << 10);
    Bitu cursorStartBit = sourceStartBit & 0x7;
    if (cursorMemStart & 0x2) cursorMemStart--;
    Bitu cursorMemEnd = cursorMemStart + ((64 - vga.s3.hgc.posx) >> 2);
    Bit8u *xat = TempLine + vga.s3.hgc.originx;
    for (Bitu m = cursorMemStart; m < cursorMemEnd; m += (m & 1) ? 3 : 1) {
        Bit8u bitsA = vga.mem.linear[m];
        Bit8u bitsB = vga.mem.linear[m + 2];
        Bit8u bit = 0x80 >> cursorStartBit;
        cursorStartBit = 0;
        while (bit) {
            if (bitsA & bit) {
                if (bitsB & bit) *xat ^= 0xFF;
            } else if (bitsB & bit) {
                *xat = vga.s3.hgc.forestack[0];
            } else {
                *xat = vga.s3.hgc.backstack[0];
            }
            xat++;
            bit >>= 1;
        }
    }
    return TempLine;
}

static Bit8u * VGA_Draw_LIN16_Line_HWMouse(Bitu vidstart, Bitu /*line*/) {
    if (!svga.hardware_cursor_active || !svga.hardware_cursor_active())
        return vga.mem.linear + vidstart;

    Bitu lineat = ((vidstart - (vga.config.real_start << 2)) >> 1) / vga.draw.width;
    if ((vga.s3.hgc.posx >= vga.draw.width) || (lineat < vga.s3.hgc.originy) || 
        (lineat > (vga.s3.hgc.originy + (63U - vga.s3.hgc.posy))))
        return vga.mem.linear + vidstart;

    memcpy(TempLine, vga.mem.linear + vidstart, vga.draw.width * 2);
    Bitu sourceStartBit = ((lineat - vga.s3.hgc.originy) + vga.s3.hgc.posy) * 64 + vga.s3.hgc.posx;
    Bitu cursorMemStart = ((sourceStartBit >> 2) & ~1) + (((Bit32u)vga.s3.hgc.startaddr) << 10);
    Bitu cursorStartBit = sourceStartBit & 0x7;
    if (cursorMemStart & 0x2) cursorMemStart--;
    Bitu cursorMemEnd = cursorMemStart + ((64 - vga.s3.hgc.posx) >> 2);
    Bit16u *xat = ((Bit16u *)TempLine) + vga.s3.hgc.originx;
    for (Bitu m = cursorMemStart; m < cursorMemEnd; m += (m & 1) ? 3 : 1) {
        Bit8u bitsA = vga.mem.linear[m];
        Bit8u bitsB = vga.mem.linear[m + 2];
        Bit8u bit = 0x80 >> cursorStartBit;
        cursorStartBit = 0;
        while (bit) {
            if (bitsA & bit) {
                if (bitsB & bit) *xat ^= ~0U;
            } else if (bitsB & bit) {
                *xat = *(Bit16u *)vga.s3.hgc.forestack;
            } else {
                *xat = *(Bit16u *)vga.s3.hgc.backstack;
            }
            xat++;
            bit >>= 1;
        }
    }
    return TempLine;
}

static Bit8u * VGA_Draw_LIN32_Line_HWMouse(Bitu vidstart, Bitu /*line*/) {
    if (!svga.hardware_cursor_active || !svga.hardware_cursor_active())
        return vga.mem.linear + vidstart;

    Bitu lineat = ((vidstart - (vga.config.real_start << 2)) >> 2) / vga.draw.width;
    if ((vga.s3.hgc.posx >= vga.draw.width) || (lineat < vga.s3.hgc.originy) || 
        (lineat > (vga.s3.hgc.originy + (63U - vga.s3.hgc.posy))))
        return vga.mem.linear + vidstart;

    memcpy(TempLine, vga.mem.linear + vidstart, vga.draw.width * 4);
    Bitu sourceStartBit = ((lineat - vga.s3.hgc.originy) + vga.s3.hgc.posy) * 64 + vga.s3.hgc.posx;
    Bitu cursorMemStart = ((sourceStartBit >> 2) & ~1) + (((Bit32u)vga.s3.hgc.startaddr) << 10);
    Bitu cursorStartBit = sourceStartBit & 0x7;
    if (cursorMemStart & 0x2) cursorMemStart--;
    Bitu cursorMemEnd = cursorMemStart + ((64 - vga.s3.hgc.posx) >> 2);
    Bit32u *xat = ((Bit32u *)TempLine) + vga.s3.hgc.originx;
    for (Bitu m = cursorMemStart; m < cursorMemEnd; m += (m & 1) ? 3 : 1) {
        Bit8u bitsA = vga.mem.linear[m];
        Bit8u bitsB = vga.mem.linear[m + 2];
        Bit8u bit = 0x80 >> cursorStartBit;
        cursorStartBit = 0;
        while (bit) {
            if (bitsA & bit) {
                if (bitsB & bit) *xat ^= ~0U;
            } else if (bitsB & bit) {
                *xat = *(Bit32u *)vga.s3.hgc.forestack;
            } else {
                *xat = *(Bit32u *)vga.s3.hgc.backstack;
            }
            xat++;
            bit >>= 1;
        }
    }
    return TempLine;
}

static const Bit8u* VGA_Text_Memwrap(Bitu vidstart) {
    vidstart &= vga.draw.linear_mask;
    Bitu line_end = 2 * vga.draw.blocks;
    if (GCC_UNLIKELY((vidstart + line_end) > vga.draw.linear_mask)) {
        Bitu break_pos = (vga.draw.linear_mask - vidstart) + 1;
        memcpy(&TempLine[sizeof(TempLine) / 2], vga.tandy.draw_base + vidstart, break_pos);
        memcpy(&TempLine[sizeof(TempLine) / 2 + break_pos], vga.tandy.draw_base, line_end - break_pos);
        return &TempLine[sizeof(TempLine) / 2];
    }
    return vga.tandy.draw_base + vidstart;
}

static Bit32u FontMask[2]={0xffffffff,0x0};
static Bit8u * VGA_TEXT_Draw_Line(Bitu vidstart, Bitu line) {
    Bit32u *draw = (Bit32u *)TempLine;
    const Bit8u *vidmem = VGA_Text_Memwrap(vidstart);
    for (Bitu cx = vga.draw.blocks; cx > 0; cx--) {
        Bitu chr = *vidmem++;
        Bitu col = *vidmem++;
        Bitu font = vga.draw.font_tables[(col >> 3) & 1][chr * 32 + line];
        Bit32u mask1 = TXT_Font_Table[font >> 4] & FontMask[col >> 7];
        Bit32u mask2 = TXT_Font_Table[font & 0xf] & FontMask[col >> 7];
        Bit32u fg = TXT_FG_Table[col & 0xf];
        Bit32u bg = TXT_BG_Table[col >> 4];
        *draw++ = (fg & mask1) | (bg & ~mask1);
        *draw++ = (fg & mask2) | (bg & ~mask2);
    }
    if (vga.draw.cursor.enabled && (vga.draw.cursor.count & 0x8)) {
        Bits font_addr = (vga.draw.cursor.address - vidstart) >> 1;
        if (font_addr >= 0 && font_addr < (Bits)vga.draw.blocks && 
            line >= vga.draw.cursor.sline && line <= vga.draw.cursor.eline) {
            draw = (Bit32u *)(TempLine + font_addr * 8);
            Bit32u att = TXT_FG_Table[vga.tandy.draw_base[vga.draw.cursor.address + 1] & 0xf];
            draw[0] = draw[1] = att;
        }
    }
    return TempLine;
}

static Bit8u * VGA_TEXT_Herc_Draw_Line(Bitu vidstart, Bitu line) {
    Bit32u *draw = (Bit32u *)TempLine;
    const Bit8u *vidmem = VGA_Text_Memwrap(vidstart);
    for (Bitu cx = vga.draw.blocks; cx > 0; cx--) {
        Bitu chr = *vidmem++;
        Bitu attrib = *vidmem++;
        if (!(attrib & 0x77)) {
            *draw++ = 0;
            *draw++ = 0;
        } else {
            Bit32u bg = (attrib & 0x77) == 0x70 ? TXT_BG_Table[0x7] : TXT_BG_Table[0x0];
            Bit32u fg = (attrib & 0x8) ? TXT_FG_Table[0xf] : 
                        ((attrib & 0x77) == 0x70 ? TXT_FG_Table[0x0] : TXT_FG_Table[0x7]);
            Bit32u mask1, mask2;
            if (GCC_UNLIKELY((vga.crtc.underline_location & 0x1f) == line && (attrib & 0x77) == 0x1)) {
                mask1 = mask2 = FontMask[attrib >> 7];
            } else {
                Bitu font = vga.draw.font_tables[0][chr * 32 + line];
                mask1 = TXT_Font_Table[font >> 4] & FontMask[attrib >> 7];
                mask2 = TXT_Font_Table[font & 0xf] & FontMask[attrib >> 7];
            }
            *draw++ = (fg & mask1) | (bg & ~mask1);
            *draw++ = (fg & mask2) | (bg & ~mask2);
        }
    }
    if (vga.draw.cursor.enabled && (vga.draw.cursor.count & 0x8)) {
        Bits font_addr = (vga.draw.cursor.address - vidstart) >> 1;
        if (font_addr >= 0 && font_addr < (Bits)vga.draw.blocks && 
            line >= vga.draw.cursor.sline && line <= vga.draw.cursor.eline) {
            draw = (Bit32u *)(TempLine + font_addr * 8);
            Bit8u attr = vga.tandy.draw_base[vga.draw.cursor.address + 1];
            Bit32u cg = (attr & 0x8) ? TXT_FG_Table[0xf] : 
                        ((attr & 0x77) == 0x70 ? TXT_FG_Table[0x0] : TXT_FG_Table[0x7]);
            draw[0] = draw[1] = cg;
        }
    }
    return TempLine;
}
/*
// combined 8/9-dot wide text mode 8bpp line drawing function
static Bit8u* VGA_TEXT_Draw_Line(Bitu vidstart, Bitu line) {
	// keep it aligned:
	Bit8u* draw = ((Bit8u*)TempLine) + 16 - vga.draw.panning;
	const Bit8u* vidmem = VGA_Text_Memwrap(vidstart); // pointer to chars+attribs
	Bitu blocks = vga.draw.blocks;
	if (vga.draw.panning) blocks++; // if the text is panned part of an 
									// additional character becomes visible
	while (blocks--) { // for each character in the line
		Bitu chr = *vidmem++;
		Bitu attr = *vidmem++;
		// the font pattern
		Bitu font = vga.draw.font_tables[(attr >> 3)&1][(chr<<5)+line];
		
		Bitu background = attr >> 4;
		// if blinking is enabled bit7 is not mapped to attributes
		if (vga.draw.blinking) background &= ~0x8;
		// choose foreground color if blinking not set for this cell or blink on
		Bitu foreground = (vga.draw.blink || (!(attr&0x80)))?
			(attr&0xf):background;
		// underline: all foreground [freevga: 0x77, previous 0x7]
		if (GCC_UNLIKELY(((attr&0x77) == 0x01) &&
			(vga.crtc.underline_location&0x1f)==line))
				background = foreground;
		if (vga.draw.char9dot) {
			font <<=1; // 9 pixels
			// extend to the 9th pixel if needed
			if ((font&0x2) && (vga.attr.mode_control&0x04) &&
				(chr>=0xc0) && (chr<=0xdf)) font |= 1;
			for (Bitu n = 0; n < 9; n++) {
				*draw++ = (font&0x100)? foreground:background;
				font <<= 1;
			}
		} else {
			for (Bitu n = 0; n < 8; n++) {
				*draw++ = (font&0x80)? foreground:background;
				font <<= 1;
			}
		}
	}
	// draw the text mode cursor if needed
	if ((vga.draw.cursor.count&0x8) && (line >= vga.draw.cursor.sline) &&
		(line <= vga.draw.cursor.eline) && vga.draw.cursor.enabled) {
		// the adress of the attribute that makes up the cell the cursor is in
		Bits attr_addr = (vga.draw.cursor.address-vidstart) >> 1;
		if (attr_addr >= 0 && attr_addr < (Bits)vga.draw.blocks) {
			Bitu index = attr_addr * (vga.draw.char9dot? 9:8);
			draw = (Bit8u*)(&TempLine[index]) + 16 - vga.draw.panning;
			
			Bitu foreground = vga.tandy.draw_base[vga.draw.cursor.address+1] & 0xf;
			for (Bitu i = 0; i < 8; i++) {
				*draw++ = foreground;
			}
		}
	}
	return TempLine+16;
}
*/
// combined 8/9-dot wide text mode 16bpp line drawing function
static Bit8u* VGA_TEXT_Xlat16_Draw_Line(Bitu vidstart, Bitu line) {
    Bit16u *draw = ((Bit16u *)TempLine) + 16 - vga.draw.panning;
    const Bit8u *vidmem = VGA_Text_Memwrap(vidstart);
    Bitu blocks = vga.draw.blocks + (vga.draw.panning ? 1 : 0);
    while (blocks--) {
        Bitu chr = *vidmem++;
        Bitu attr = *vidmem++;
        Bitu font = vga.draw.font_tables[(attr >> 3) & 1][(chr << 5) + line];
        Bitu background = attr >> 4 & (vga.draw.blinking ? ~0x8 : 0xF);
        Bitu foreground = (vga.draw.blink || !(attr & 0x80)) ? (attr & 0xf) : background;
        if (GCC_UNLIKELY((attr & 0x77) == 0x01 && (vga.crtc.underline_location & 0x1f) == line))
            background = foreground;
        if (vga.draw.char9dot) {
            font = (font << 1) | (((font & 0x2) && (vga.attr.mode_control & 0x04) && 
                                   chr >= 0xc0 && chr <= 0xdf) ? 1 : 0);
            for (Bitu n = 9; n > 0; n--) {
                *draw++ = vga.dac.xlat16[(font & 0x100) ? foreground : background];
                font <<= 1;
            }
        } else {
            for (Bitu n = 8; n > 0; n--) {
                *draw++ = vga.dac.xlat16[(font & 0x80) ? foreground : background];
                font <<= 1;
            }
        }
    }
    if (vga.draw.cursor.enabled && (vga.draw.cursor.count & 0x8) && 
        line >= vga.draw.cursor.sline && line <= vga.draw.cursor.eline) {
        Bits attr_addr = (vga.draw.cursor.address - vidstart) >> 1;
        if (attr_addr >= 0 && attr_addr < (Bits)vga.draw.blocks) {
            draw = ((Bit16u *)TempLine) + 16 - vga.draw.panning + attr_addr * (vga.draw.char9dot ? 18 : 16);
            Bitu foreground = vga.tandy.draw_base[vga.draw.cursor.address + 1] & 0xf;
            Bit16u color = vga.dac.xlat16[foreground];
            for (Bitu i = 8; i > 0; i--) *draw++ = color;
        }
    }
    return TempLine + 32;
}

#ifdef VGA_KEEP_CHANGES
static INLINE void VGA_ChangesEnd(void) {
    if (vga.changes.active) {
        Bitu end = vga.draw.address >> VGA_CHANGE_SHIFT;
        Bit32u total = (4 + end - vga.changes.start) >> 2;
        Bit32u *clear = (Bit32u *)(vga.changes.map + (vga.changes.start & ~3));
        Bit32u clearMask = vga.changes.clearMask;
        while (total--) *clear++ &= clearMask;
    }
}
#endif


static void VGA_ProcessSplit() {
    if (vga.attr.mode_control & 0x20) {
        vga.draw.address = 0;
        vga.draw.panning = 0;
    } else {
        vga.draw.address = vga.draw.byte_panning_shift * vga.draw.bytes_skip;
        if ((vga.mode != M_TEXT) && (machine != MCH_EGA)) vga.draw.address += vga.draw.panning;
    }
    vga.draw.address_line = 0;
}

static Bit8u bg_color_index = 0; // screen-off black index
static void VGA_DrawSingleLine(Bitu /*blah*/) {
    if (GCC_UNLIKELY(vga.attr.disabled)) {
        if (vga.draw.bpp == 8) {
            memset(TempLine, bg_color_index, sizeof(TempLine));
        } else if (vga.draw.bpp == 16) {
            Bit16u *wptr = (Bit16u *)TempLine;
            Bit16u value = vga.dac.xlat16[bg_color_index];
            for (Bitu i = sizeof(TempLine) / 2; i > 0; i--) *wptr++ = value;
        }
        RENDER_DrawLine(TempLine);
    } else {
        RENDER_DrawLine(VGA_DrawLine(vga.draw.address, vga.draw.address_line));
    }
    vga.draw.address_line++;
    if (vga.draw.address_line >= vga.draw.address_line_total) {
        vga.draw.address_line = 0;
        vga.draw.address += vga.draw.address_add;
    }
    vga.draw.lines_done++;
    if (vga.draw.split_line == vga.draw.lines_done) VGA_ProcessSplit();
    if (vga.draw.lines_done < vga.draw.lines_total) {
        PIC_AddEvent(VGA_DrawSingleLine, (float)vga.draw.delay.htotal);
    } else {
        RENDER_EndUpdate(false);
    }
}

static void VGA_DrawEGASingleLine(Bitu /*blah*/) {
    if (GCC_UNLIKELY(vga.attr.disabled)) {
        memset(TempLine, 0, sizeof(TempLine));
        RENDER_DrawLine(TempLine);
    } else {
        Bitu address = vga.draw.address + ((vga.mode != M_TEXT) ? vga.draw.panning : 0);
        RENDER_DrawLine(VGA_DrawLine(address, vga.draw.address_line));
    }
    vga.draw.address_line++;
    if (vga.draw.address_line >= vga.draw.address_line_total) {
        vga.draw.address_line = 0;
        vga.draw.address += vga.draw.address_add;
    }
    vga.draw.lines_done++;
    if (vga.draw.split_line == vga.draw.lines_done) VGA_ProcessSplit();
    if (vga.draw.lines_done < vga.draw.lines_total) {
        PIC_AddEvent(VGA_DrawEGASingleLine, (float)vga.draw.delay.htotal);
    } else {
        RENDER_EndUpdate(false);
    }
}

static void VGA_DrawPart(Bitu lines) {
    Bit8u *data;
    while (lines--) {
        data = VGA_DrawLine(vga.draw.address, vga.draw.address_line);
        RENDER_DrawLine(data);
        vga.draw.address_line++;
        if (vga.draw.address_line >= vga.draw.address_line_total) {
            vga.draw.address_line = 0;
            vga.draw.address += vga.draw.address_add;
        }
        vga.draw.lines_done++;
        if (vga.draw.split_line == vga.draw.lines_done) {
#ifdef VGA_KEEP_CHANGES
            VGA_ChangesEnd();
#endif
            VGA_ProcessSplit();
#ifdef VGA_KEEP_CHANGES
            vga.changes.start = vga.draw.address >> VGA_CHANGE_SHIFT;
#endif
        }
    }
    if (--vga.draw.parts_left) {
        PIC_AddEvent(VGA_DrawPart, (float)vga.draw.delay.parts,
                     (vga.draw.parts_left != 1) ? vga.draw.parts_lines : (vga.draw.lines_total - vga.draw.lines_done));
    } else {
#ifdef VGA_KEEP_CHANGES
        VGA_ChangesEnd();
#endif
        RENDER_EndUpdate(false);
    }
}

void VGA_SetBlinking(Bitu enabled) {
    Bitu b = enabled ? 0 : 8;
    vga.draw.blinking = enabled;
    vga.attr.mode_control = (vga.attr.mode_control & ~0x08) | (enabled << 3);
    vga.tandy.mode_control = (vga.tandy.mode_control & ~0x20) | (enabled << 5);
    for (Bitu i = 0; i < 8; i++) {
        Bit32u val = b + i;
        TXT_BG_Table[i + 8] = val | (val << 8) | (val << 16) | (val << 24);
    }
}

#ifdef VGA_KEEP_CHANGES
static void INLINE VGA_ChangesStart(void) {
    vga.changes.start = vga.draw.address >> VGA_CHANGE_SHIFT;
    vga.changes.last = vga.changes.start;
    if (vga.changes.lastAddress != vga.draw.address || render.fullFrame) {
        VGA_DrawLine = VGA_Draw_Linear_Line;
        vga.changes.lastAddress = vga.draw.address;
    } else {
        VGA_DrawLine = VGA_Draw_Changes_Line;
    }
    vga.changes.active = true;
    vga.changes.checkMask = vga.changes.writeMask;
    vga.changes.clearMask = ~(0x01010101 << (vga.changes.frame & 7));
    vga.changes.frame++;
    vga.changes.writeMask = 1 << (vga.changes.frame & 7);
}
#endif

static void VGA_VertInterrupt(Bitu /*val*/) {
    if (!vga.draw.vret_triggered && (vga.crtc.vertical_retrace_end & 0x30) == 0x10) {
        vga.draw.vret_triggered = true;
        if (GCC_UNLIKELY(machine == MCH_EGA)) PIC_ActivateIRQ(9);
    }
}

static void VGA_Other_VertInterrupt(Bitu val) {
    if (val) PIC_ActivateIRQ(5);
    else PIC_DeActivateIRQ(5);
}

static void VGA_DisplayStartLatch(Bitu /*val*/) {
    vga.config.real_start = vga.config.display_start & (vga.vmemwrap - 1);
    vga.draw.bytes_skip = vga.config.bytes_skip;
}
 
static void VGA_PanningLatch(Bitu /*val*/) {
    vga.draw.panning = vga.config.pel_panning;
}

static void VGA_VerticalTimer(Bitu /*val*/) {
    vga.draw.delay.framestart = PIC_FullIndex();
    PIC_AddEvent(VGA_VerticalTimer, (float)vga.draw.delay.vtotal);

    switch (machine) {
    case MCH_PCJR:
    case MCH_TANDY:
        PIC_AddEvent(VGA_Other_VertInterrupt, (float)vga.draw.delay.vrstart, 1);
        PIC_AddEvent(VGA_Other_VertInterrupt, (float)vga.draw.delay.vrend, 0);
        // Fall-through
    case MCH_CGA:
    case MCH_HERC:
        VGA_DisplayStartLatch(0);
        break;
    case MCH_VGA:
        PIC_AddEvent(VGA_DisplayStartLatch, (float)vga.draw.delay.vrstart);
        PIC_AddEvent(VGA_PanningLatch, (float)vga.draw.delay.vrend);
        PIC_AddEvent(VGA_VertInterrupt, (float)(vga.draw.delay.vdend + 0.005));
        break;
    case MCH_EGA:
        PIC_AddEvent(VGA_DisplayStartLatch, (float)vga.draw.delay.vrend);
        PIC_AddEvent(VGA_VertInterrupt, (float)(vga.draw.delay.vdend + 0.005));
        break;
    default:
        E_Exit("This new machine needs implementation in VGA_VerticalTimer too.");
    }

    if (vga.draw.vga_override || !RENDER_StartUpdate()) return;

    vga.draw.address_line = vga.config.hlines_skip;
    vga.draw.split_line = IS_EGAVGA_ARCH ? ((vga.config.line_compare + 1) / vga.draw.lines_scaled - vga.draw.vblank_skip) : 0x10000;
    if (svgaCard == SVGA_S3Trio && vga.config.line_compare == 0) vga.draw.split_line = 0;
    if (machine == MCH_EGA) {
        if (vga.draw.doubleheight) vga.draw.split_line *= 2;
        vga.draw.split_line++;
    }

    vga.draw.address = vga.config.real_start;
    vga.draw.byte_panning_shift = 0;

#ifdef VGA_KEEP_CHANGES
    bool startaddr_changed = false;
#endif

    switch (vga.mode) {
    case M_EGA:
        vga.draw.linear_mask = (vga.crtc.mode_control & 0x1) ? 0x1FFFF : 0xFFFF;
        // Fall-through
    case M_LIN4:
        vga.draw.byte_panning_shift = 8;
        vga.draw.address = (vga.draw.address + vga.draw.bytes_skip) * 8 + (machine != MCH_EGA ? vga.draw.panning : 0);
#ifdef VGA_KEEP_CHANGES
        startaddr_changed = true;
#endif
        break;
    case M_VGA:
        vga.draw.linear_base = (vga.config.compatible_chain4 && (vga.crtc.underline_location & 0x40)) ? vga.fastmem : vga.mem.linear;
        vga.draw.linear_mask = (vga.draw.linear_base == vga.fastmem) ? 0xFFFF : vga.vmemwrap - 1;
        // Fall-through
    case M_LIN8:
    case M_LIN15:
    case M_LIN16:
    case M_LIN32:
        vga.draw.byte_panning_shift = 4;
        vga.draw.address = (vga.draw.address + vga.draw.bytes_skip) * 4 + vga.draw.panning;
#ifdef VGA_KEEP_CHANGES
        startaddr_changed = true;
#endif
        break;
    case M_TEXT:
        vga.draw.byte_panning_shift = 2;
        vga.draw.address = (vga.draw.address + vga.draw.bytes_skip) * 2;
        // Fall-through
    case M_TANDY_TEXT:
    case M_HERC_TEXT:
        vga.draw.linear_mask = (machine == MCH_HERC) ? 0xFFF : (IS_EGAVGA_ARCH ? 0x7FFF : 0x3FFF);
        vga.draw.cursor.address = vga.config.cursor_start * 2;
        vga.draw.address *= 2;
        vga.draw.cursor.count++;
        FontMask[1] = (vga.draw.blinking & (vga.draw.cursor.count >> 4)) ? 0 : 0xFFFFFFFF;
        vga.draw.blink = (vga.draw.blinking & (vga.draw.cursor.count >> 4)) || !vga.draw.blinking;
        break;
    case M_HERC_GFX:
    case M_CGA4:
    case M_CGA2:
        vga.draw.address = (vga.draw.address * 2) & 0x1FFF;
        break;
    case M_CGA16:
    case M_TANDY2:
    case M_TANDY4:
    case M_TANDY16:
        vga.draw.address *= 2;
        break;
    }

    if (GCC_UNLIKELY(vga.draw.split_line == 0)) VGA_ProcessSplit();
#ifdef VGA_KEEP_CHANGES
    if (startaddr_changed) VGA_ChangesStart();
#endif

    float draw_skip = vga.draw.vblank_skip ? (float)(vga.draw.delay.htotal * vga.draw.vblank_skip) : 0.0f;
    if (vga.draw.vblank_skip) {
        vga.draw.address += vga.draw.address_add * (vga.draw.vblank_skip / vga.draw.address_line_total);
    }

    switch (vga.draw.mode) {
    case PART:
        if (vga.draw.parts_left) {
            PIC_RemoveEvents(VGA_DrawPart);
            RENDER_EndUpdate(true);
        }
        vga.draw.lines_done = 0;
        vga.draw.parts_left = vga.draw.parts_total;
        PIC_AddEvent(VGA_DrawPart, (float)vga.draw.delay.parts + draw_skip, vga.draw.parts_lines);
        break;
    case DRAWLINE:
    case EGALINE:
        if (vga.draw.lines_done < vga.draw.lines_total) {
            if (vga.draw.mode == EGALINE) PIC_RemoveEvents(VGA_DrawEGASingleLine);
            else PIC_RemoveEvents(VGA_DrawSingleLine);
            RENDER_EndUpdate(true);
        }
        vga.draw.lines_done = 0;
        PIC_AddEvent(vga.draw.mode == EGALINE ? VGA_DrawEGASingleLine : VGA_DrawSingleLine,
                     (float)(vga.draw.delay.htotal / 4.0 + draw_skip));
        break;
    }
}

void VGA_CheckScanLength(void) {
    switch (vga.mode) {
    case M_EGA:
    case M_LIN4:
        vga.draw.address_add = vga.config.scan_len * 16;
        break;
    case M_VGA:
    case M_LIN8:
    case M_LIN15:
    case M_LIN16:
    case M_LIN32:
        vga.draw.address_add = vga.config.scan_len * 8;
        break;
    case M_TEXT:
        vga.draw.address_add = vga.config.scan_len * 4;
        break;
    case M_CGA2:
    case M_CGA4:
    case M_CGA16:
        vga.draw.address_add = 80;
        break;
    case M_TANDY2:
        vga.draw.address_add = vga.draw.blocks / 4;
        break;
    case M_TANDY4:
    case M_TANDY16:
    case M_HERC_GFX:
        vga.draw.address_add = vga.draw.blocks;
        break;
    case M_TANDY_TEXT:
    case M_HERC_TEXT:
        vga.draw.address_add = vga.draw.blocks * 2;
        break;
    default:
        vga.draw.address_add = vga.draw.blocks * 8;
        break;
    }
}

void VGA_ActivateHardwareCursor(void) {
    bool hw_active = svga.hardware_cursor_active && svga.hardware_cursor_active();
    VGA_DrawLine = hw_active ? 
        (vga.mode == M_LIN32 ? VGA_Draw_LIN32_Line_HWMouse :
         (vga.mode == M_LIN15 || vga.mode == M_LIN16 ? VGA_Draw_LIN16_Line_HWMouse :
          VGA_Draw_VGA_Line_HWMouse)) : 
        VGA_Draw_Linear_Line;
}

void VGA_SetupDrawing(Bitu /*val*/) {
    if (vga.mode == M_ERROR) {
        PIC_RemoveEvents(VGA_VerticalTimer);
        PIC_RemoveEvents(VGA_PanningLatch);
        PIC_RemoveEvents(VGA_DisplayStartLatch);
        return;
    }

    vga.draw.mode = (machine == MCH_CGA || machine == MCH_PCJR || machine == MCH_TANDY) ? DRAWLINE :
                    (machine == MCH_EGA) ? EGALINE :
                    (machine == MCH_VGA && svgaCard == SVGA_None) ? DRAWLINE : PART;

    Bitu clock;
    Bitu htotal = 0, hdend = 0, hbstart = 0, hbend = 0, hrstart = 0, hrend = 0;
    Bitu vtotal = 0, vdend = 0, vbstart = 0, vbend = 0, vrstart = 0, vrend = 0;
    Bitu vblank_skip = 0;
    double fps;

    if (IS_EGAVGA_ARCH) {
        htotal = vga.crtc.horizontal_total + 5;
        hdend = vga.crtc.horizontal_display_end + 1;
        hbstart = vga.crtc.start_horizontal_blanking;
        hbend = hbstart + (vga.crtc.end_horizontal_blanking & 0x1F);
        hrstart = vga.crtc.start_horizontal_retrace;
        hrend = hrstart + (vga.crtc.end_horizontal_retrace & 0x1F ? vga.crtc.end_horizontal_retrace & 0x1F : 0x20);

        if (IS_VGA_ARCH) {
            htotal += (vga.s3.ex_hor_overflow & 0x1) << 8;
            hdend += (vga.s3.ex_hor_overflow & 0x2) << 7;
            hbstart += (vga.s3.ex_hor_overflow & 0x4) << 6;
            hbend += (vga.crtc.end_horizontal_retrace & 0x80) >> 2;
            hrstart += (vga.s3.ex_hor_overflow & 0x10) << 4;
        }

        vtotal = vga.crtc.vertical_total + 2 + ((vga.crtc.overflow & 1) << 8);
        vdend = vga.crtc.vertical_display_end + 1 + ((vga.crtc.overflow & 2) << 7);
        vbstart = vga.crtc.start_vertical_blanking + 1 + ((vga.crtc.overflow & 0x08) << 5);
        vrstart = vga.crtc.vertical_retrace_start + ((vga.crtc.overflow & 0x04) << 6);
        vrend = vrstart + (vga.crtc.vertical_retrace_end & 0xF ? vga.crtc.vertical_retrace_end & 0xF : 0x10);
        vbend = vbstart + (vga.crtc.end_vertical_blanking & (IS_VGA_ARCH ? 0x7F : 0x1F) ? vga.crtc.end_vertical_blanking : 0x80);

        if (IS_VGA_ARCH) {
            vtotal += ((vga.crtc.overflow & 0x20) << 4) + ((vga.s3.ex_ver_overflow & 0x1) << 10);
            vdend += ((vga.crtc.overflow & 0x40) << 3) + ((vga.s3.ex_ver_overflow & 0x2) << 9);
            vbstart += ((vga.crtc.maximum_scan_line & 0x20) << 4) + ((vga.s3.ex_ver_overflow & 0x4) << 8);
            vrstart += ((vga.crtc.overflow & 0x80) << 2) + ((vga.s3.ex_ver_overflow & 0x10) << 6);
        }

        clock = svga.get_clock ? svga.get_clock() : (((vga.misc_output >> 2) & 3) ? 
                (machine == MCH_EGA ? 16257000 : 28322000) : (machine == MCH_EGA ? 14318180 : 25175000));
        clock /= (vga.seq.clocking_mode & 1) ? 8 : 9;
        if (vga.seq.clocking_mode & 0x8) htotal *= 2;

        vga.draw.address_line_total = (vga.crtc.maximum_scan_line & 0x1F) + 1;
        vga.draw.double_scan = IS_VGA_ARCH ? (vga.crtc.maximum_scan_line & 0x80) != 0 : (vtotal == 262);
        if (IS_VGA_ARCH && svgaCard == SVGA_None && (vga.mode == M_EGA || vga.mode == M_VGA) && (vga.crtc.maximum_scan_line & 0x80)) {
            vga.draw.address_line_total *= 2;
            vga.draw.double_scan = false;
        }
    } else {
        htotal = vga.other.htotal + 1;
        hdend = vga.other.hdend;
        hbstart = hdend;
        hbend = htotal;
        hrstart = vga.other.hsyncp;
        hrend = hrstart + vga.other.hsyncw;
        vga.draw.address_line_total = vga.other.max_scanline + 1;
        vtotal = vga.draw.address_line_total * (vga.other.vtotal + 1) + vga.other.vadjust;
        vdend = vga.draw.address_line_total * vga.other.vdend;
        vrstart = vga.draw.address_line_total * vga.other.vsyncp;
        vrend = vrstart + 16;
        vbstart = vdend;
        vbend = vtotal;
        vga.draw.double_scan = (vtotal == 262);

        clock = (machine == MCH_HERC) ? ((vga.herc.mode_control & 0x2) ? 16000000 / 16 : 16000000 / 8) :
                ((machine == MCH_CGA || machine == MCH_TANDY || machine == MCH_PCJR) ? 
                 ((vga.tandy.mode_control & 1) ? 14318180 : 14318180 / 2) / 8 : 14318180);
        vga.draw.delay.hdend = hdend * 1000.0 / clock;
    }

    if (!htotal || !vtotal) return;

    fps = (double)clock / (vtotal * htotal);
    vga.draw.delay.htotal = htotal * 1000.0 / clock;
    vga.draw.delay.hblkstart = hbstart * 1000.0 / clock;
    vga.draw.delay.hblkend = hbend * 1000.0 / clock;
    vga.draw.delay.hrstart = hrstart * 1000.0 / clock;
    vga.draw.delay.hrend = hrend * 1000.0 / clock;
    vga.draw.delay.vblkstart = vbstart * vga.draw.delay.htotal;
    vga.draw.delay.vblkend = vbend * vga.draw.delay.htotal;
    vga.draw.delay.vrstart = vrstart * vga.draw.delay.htotal;
    vga.draw.delay.vrend = vrend * vga.draw.delay.htotal;

    vblank_skip = 0;
    if (IS_VGA_ARCH && vbstart < vtotal) {
        if (vbend > vtotal) {
            vblank_skip = vbend & 0x7F;
            if ((vbend & 0x7F) == 1) vblank_skip = 0;
            if (vbstart < vdend) vdend = vbstart;
        } else if (vbstart <= 1) {
            vblank_skip = vbend;
        } else if (vbstart < vdend && vbend >= vdend) {
            vdend = vbstart;
        }
        vdend -= vblank_skip;
    }
    vga.draw.delay.vdend = vdend * vga.draw.delay.htotal;
    vga.draw.vblank_skip = vblank_skip;

    if (machine == MCH_EGA) {
        VGA_ATTR_SetEGAMonitorPalette((vga.misc_output & 1) ? (1.0f / vga.draw.delay.htotal > 19.0f ? EGA : CGA) : MONO);
    }

    vga.draw.parts_total = VGA_PARTS;
    double pwidth = (machine == MCH_EGA) ? 114.0 / htotal : 100.0 / htotal;
    double pheight, target_total = (machine == MCH_EGA) ? 262.0 : 449.0;
    Bitu sync = vga.misc_output >> 6;
    switch (sync) {
    case 0: pheight = (480.0 / 340.0) * (target_total / vtotal); break;
    case 1: pheight = (480.0 / 400.0) * (target_total / vtotal); break;
    case 2: pheight = (480.0 / 350.0) * (target_total / vtotal); break;
    default: 
        target_total = (vga.mode == M_VGA && vtotal == 527) ? 527.0 : 525.0;
        pheight = (480.0 / 480.0) * (target_total / vtotal); 
        break;
    }
    double aspect_ratio = pheight / pwidth;

    vga.draw.delay.parts = vga.draw.delay.vdend / vga.draw.parts_total;
    vga.draw.resizing = false;
    vga.draw.vret_triggered = false;

    if (hbstart < hdend) hdend = hbstart;
    if (!IS_VGA_ARCH && vbstart < vdend) vdend = vbstart;

    Bitu width = hdend;
    Bitu height = vdend;
    bool doubleheight = false;
    bool doublewidth = false;
    Bitu bpp = (vga.mode == M_LIN15) ? 15 : (vga.mode == M_LIN16) ? 16 : (vga.mode == M_LIN32) ? 32 : 8;

    vga.draw.linear_base = vga.mem.linear;
    vga.draw.linear_mask = vga.vmemwrap - 1;

    switch (vga.mode) {
    case M_VGA:
        doublewidth = true;
        width <<= 2;
        bpp = (IS_VGA_ARCH && svgaCard == SVGA_None) ? 16 : 8;
        VGA_DrawLine = (bpp == 16) ? VGA_Draw_Xlat16_Linear_Line : VGA_Draw_Linear_Line;
        break;
    case M_LIN8:
        if (vga.crtc.mode_control & 0x8) width >>= 1;
        else if (svgaCard == SVGA_S3Trio && !(vga.s3.reg_3a & 0x10)) {
            doublewidth = true;
            width >>= 1;
        }
        width <<= 3;
        doublewidth = (vga.crtc.mode_control & 0x8) != 0;
        VGA_ActivateHardwareCursor();
        break;
    case M_LIN32:
        width <<= 3;
        doublewidth = (vga.crtc.mode_control & 0x8) != 0;
        VGA_ActivateHardwareCursor();
        break;
    case M_LIN15:
    case M_LIN16:
        width <<= 2;
        doublewidth = (vga.crtc.mode_control & 0x8) || (svgaCard == SVGA_S3Trio && (vga.s3.pll.cmd & 0x10));
        VGA_ActivateHardwareCursor();
        break;
    case M_LIN4:
    case M_EGA:
        doublewidth = (vga.seq.clocking_mode & 0x8) > 0;
        vga.draw.blocks = width;
        width <<= 3;
        bpp = (vga.mode == M_EGA && IS_VGA_ARCH && svgaCard == SVGA_None) ? 16 : 8;
        VGA_DrawLine = (bpp == 16) ? VGA_Draw_Xlat16_Linear_Line : VGA_Draw_Linear_Line;
        vga.draw.linear_base = vga.fastmem;
        vga.draw.linear_mask = (vga.vmemwrap << 1) - 1;
        break;
    case M_CGA16:
        aspect_ratio = 1.2;
        doubleheight = true;
        vga.draw.blocks = width * 2;
        width <<= 4;
        VGA_DrawLine = VGA_Draw_CGA16_Line;
        break;
    case M_CGA4:
        doublewidth = true;
        vga.draw.blocks = width * 2;
        width <<= 3;
        VGA_DrawLine = VGA_Draw_2BPP_Line;
        break;
    case M_CGA2:
        doubleheight = true;
        vga.draw.blocks = width * 2;
        width <<= 3;
        VGA_DrawLine = VGA_Draw_1BPP_Line;
        break;
    case M_TEXT:
        vga.draw.blocks = width;
        doublewidth = (vga.seq.clocking_mode & 0x8) > 0;
        if (IS_VGA_ARCH && svgaCard == SVGA_None) {
            vga.draw.char9dot = !(vga.seq.clocking_mode & 0x1);
            width *= vga.draw.char9dot ? 9 : 8;
            if (vga.draw.char9dot) aspect_ratio *= 1.125;
            VGA_DrawLine = VGA_TEXT_Xlat16_Draw_Line;
            bpp = 16;
        } else {
            width <<= 3;
            vga.draw.char9dot = false;
            VGA_DrawLine = VGA_TEXT_Draw_Line;
        }
        break;
    case M_HERC_GFX:
        doubleheight = true;
        vga.draw.blocks = width * 2;
        width <<= 4;
        aspect_ratio = ((double)width / (double)height) * (3.0 / 4.0);
        VGA_DrawLine = VGA_Draw_1BPP_Line;
        break;
    case M_TANDY2:
        aspect_ratio = 1.2;
        doubleheight = true;
        doublewidth = (machine == MCH_PCJR) ? !(vga.tandy.gfx_control & 0x8) : !(vga.tandy.mode_control & 0x10);
        vga.draw.blocks = width * (doublewidth ? 4 : 8);
        width = vga.draw.blocks * 2;
        VGA_DrawLine = VGA_Draw_1BPP_Line;
        break;
    case M_TANDY4:
        aspect_ratio = 1.2;
        doubleheight = true;
        doublewidth = (machine == MCH_TANDY) ? !(vga.tandy.mode_control & 0x10) : !(vga.tandy.mode_control & 0x1);
        vga.draw.blocks = width * 2;
        width = vga.draw.blocks * 4;
        VGA_DrawLine = (machine == MCH_TANDY && (vga.tandy.gfx_control & 0x8)) || 
                       (machine == MCH_PCJR && vga.tandy.mode_control == 0x0b) ? 
                       VGA_Draw_2BPPHiRes_Line : VGA_Draw_2BPP_Line;
        break;
    case M_TANDY16:
        aspect_ratio = 1.2;
        doubleheight = true;
        vga.draw.blocks = width * 2;
        if (vga.tandy.mode_control & 0x1) {
            doublewidth = (machine == MCH_TANDY && (vga.tandy.mode_control & 0x10)) ? false : true;
            vga.draw.blocks *= (doublewidth ? 1 : 2);
            width = vga.draw.blocks * 2;
            VGA_DrawLine = VGA_Draw_4BPP_Line;
        } else {
            doublewidth = true;
            width = vga.draw.blocks * 4;
            VGA_DrawLine = VGA_Draw_4BPP_Line_Double;
        }
        break;
    case M_TANDY_TEXT:
        doublewidth = !(vga.tandy.mode_control & 0x1);
        aspect_ratio = 1.2;
        doubleheight = true;
        vga.draw.blocks = width;
        width <<= 3;
        VGA_DrawLine = VGA_TEXT_Draw_Line;
        break;
    case M_HERC_TEXT:
        aspect_ratio = 480.0 / 350.0;
        vga.draw.blocks = width;
        width <<= 3;
        VGA_DrawLine = VGA_TEXT_Herc_Draw_Line;
        break;
    default:
        LOG(LOG_VGA, LOG_ERROR)("Unhandled VGA mode %d while setting up drawing", vga.mode);
        break;
    }

    VGA_CheckScanLength();
    if (vga.draw.double_scan) {
        if (IS_VGA_ARCH) {
            vga.draw.vblank_skip /= 2;
            height /= 2;
        }
        doubleheight = true;
    }

    if (!(IS_VGA_ARCH && svgaCard == SVGA_None && (vga.mode == M_EGA || vga.mode == M_VGA)) && 
        !doubleheight && vga.mode < M_TEXT && !(vga.draw.address_line_total & 1)) {
        vga.draw.address_line_total /= 2;
        doubleheight = true;
        height /= 2;
    }

    vga.draw.lines_total = height;
    vga.draw.parts_lines = vga.draw.lines_total / vga.draw.parts_total;
    vga.draw.line_length = width * ((bpp + 1) / 8);

#ifdef VGA_KEEP_CHANGES
    vga.changes.active = false;
    vga.changes.frame = 0;
    vga.changes.writeMask = 1;
#endif

    if (width >= 640 && height >= 480) {
        aspect_ratio = ((float)width / (float)height) * (3.0 / 4.0);
    }

    bool fps_changed = fabs(vga.draw.delay.vtotal - 1000.0 / fps) > 0.0001;
    if (fps_changed) {
        vga.draw.delay.vtotal = 1000.0 / fps;
        VGA_KillDrawing();
        PIC_RemoveEvents(VGA_Other_VertInterrupt);
        PIC_RemoveEvents(VGA_VerticalTimer);
        PIC_RemoveEvents(VGA_PanningLatch);
        PIC_RemoveEvents(VGA_DisplayStartLatch);
        VGA_VerticalTimer(0);
    }

    if (width != vga.draw.width || height != vga.draw.height || 
        vga.draw.doublewidth != doublewidth || vga.draw.doubleheight != doubleheight || 
        fabs(aspect_ratio - vga.draw.aspect_ratio) > 0.0001 || vga.draw.bpp != bpp || fps_changed) {
        VGA_KillDrawing();
        vga.draw.width = width;
        vga.draw.height = height;
        vga.draw.doublewidth = doublewidth;
        vga.draw.doubleheight = doubleheight;
        vga.draw.aspect_ratio = aspect_ratio;
        vga.draw.bpp = bpp;
        vga.draw.lines_scaled = doubleheight ? 2 : 1;
        if (!vga.draw.vga_override) {
            RENDER_SetSize(width, height, bpp, (float)fps, aspect_ratio, doublewidth, doubleheight);
        }
    }
}

void VGA_KillDrawing(void) {
    PIC_RemoveEvents(VGA_DrawPart);
    PIC_RemoveEvents(VGA_DrawSingleLine);
    PIC_RemoveEvents(VGA_DrawEGASingleLine);
    vga.draw.parts_left = 0;
    vga.draw.lines_done = ~0;
    if (!vga.draw.vga_override) RENDER_EndUpdate(true);
}

void VGA_SetOverride(bool vga_override) {
    if (vga.draw.vga_override != vga_override) {
        if (vga_override) {
            VGA_KillDrawing();
            vga.draw.vga_override = true;
        } else {
            vga.draw.vga_override = false;
            vga.draw.width = 0;
            VGA_SetupDrawing(0);
        }
    }
}
