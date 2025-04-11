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
#include <stddef.h>

#include "dosbox.h"
#include "callback.h"
#include "regs.h"
#include "mem.h"
#include "inout.h"
#include "int10.h"
#include "dos_inc.h"

#define VESA_SUCCESS          0x00
#define VESA_FAIL             0x01
#define VESA_HW_UNSUPPORTED   0x02
#define VESA_MODE_UNSUPPORTED 0x03
// internal definition to pass to the caller
#define VESA_UNIMPLEMENTED    0xFF

static struct {
	Bitu setwindow;
	Bitu pmStart;
	Bitu pmWindow;
	Bitu pmPalette;
} callback;

static char string_oem[]="S3 Incorporated. Trio64";
static char string_vendorname[]="DOSBox Development Team";
static char string_productname[]="DOSBox - The DOS Emulator";
static char string_productrev[]="DOSBox " VERSION;

#ifdef _MSC_VER
#pragma pack (1)
#endif
struct MODE_INFO{
	Bit16u ModeAttributes;
	Bit8u WinAAttributes;
	Bit8u WinBAttributes;
	Bit16u WinGranularity;
	Bit16u WinSize;
	Bit16u WinASegment;
	Bit16u WinBSegment;
	Bit32u WinFuncPtr;
	Bit16u BytesPerScanLine;
	Bit16u XResolution;
	Bit16u YResolution;
	Bit8u XCharSize;
	Bit8u YCharSize;
	Bit8u NumberOfPlanes;
	Bit8u BitsPerPixel;
	Bit8u NumberOfBanks;
	Bit8u MemoryModel;
	Bit8u BankSize;
	Bit8u NumberOfImagePages;
	Bit8u Reserved_page;
	Bit8u RedMaskSize;
	Bit8u RedMaskPos;
	Bit8u GreenMaskSize;
	Bit8u GreenMaskPos;
	Bit8u BlueMaskSize;
	Bit8u BlueMaskPos;
	Bit8u ReservedMaskSize;
	Bit8u ReservedMaskPos;
	Bit8u DirectColorModeInfo;
	Bit32u PhysBasePtr;
	Bit32u OffScreenMemOffset;
	Bit16u OffScreenMemSize;
	Bit8u Reserved[206];
} GCC_ATTRIBUTE(packed);
#ifdef _MSC_VER
#pragma pack()
#endif

Bit8u VESA_GetSVGAInformation(Bit16u seg, Bit16u off) {
    PhysPt buffer = PhysMake(seg, off);
    Bit16u vbe2_pos = 256 + off;
    bool vbe2 = ((mem_readd(buffer) | 0x20202020) == 0x56424532) && !int10.vesa_oldvbe;  // Fast ID check with case normalization

    // Zero buffer inline based on VBE version
    PhysPt end = buffer + (vbe2 ? 0x200 : 0x100);
    for (PhysPt p = buffer; p < end; p++) mem_writeb(p, 0);

    // Precomputed constants
    static const char vesa_id[] = "VESA";
    Bit16u vesa_version = int10.vesa_oldvbe ? 0x102 : 0x200;
    Bit16u mem_size = static_cast<Bit16u>(vga.vmemsize / (64 * 1024));

    // Write common data directly
    MEM_BlockWrite(buffer, vesa_id, 4);
    mem_writew(buffer + 0x04, vesa_version);
    mem_writed(buffer + 0x0a, 0x0);              // Capabilities and flags
    mem_writed(buffer + 0x0e, int10.rom.vesa_modes);
    mem_writew(buffer + 0x12, mem_size);

    // VBE2-specific data
    if (vbe2) {
        RealPt base = RealMake(seg, vbe2_pos);
        mem_writed(buffer + 0x06, base);
        MEM_BlockWrite(PhysMake(seg, vbe2_pos), string_oem, sizeof(string_oem) - 1);
        vbe2_pos += sizeof(string_oem) - 1;

        mem_writew(buffer + 0x14, 0x200);
        mem_writed(buffer + 0x16, RealMake(seg, vbe2_pos));
        MEM_BlockWrite(PhysMake(seg, vbe2_pos), string_vendorname, sizeof(string_vendorname) - 1);
        vbe2_pos += sizeof(string_vendorname) - 1;

        mem_writed(buffer + 0x1a, RealMake(seg, vbe2_pos));
        MEM_BlockWrite(PhysMake(seg, vbe2_pos), string_productname, sizeof(string_productname) - 1);
        vbe2_pos += sizeof(string_productname) - 1;

        mem_writed(buffer + 0x1e, RealMake(seg, vbe2_pos));
        MEM_BlockWrite(PhysMake(seg, vbe2_pos), string_productrev, sizeof(string_productrev) - 1);
    } else {
        mem_writed(buffer + 0x06, int10.rom.oemstring);
    }

    return VESA_SUCCESS;
}

Bit8u VESA_GetSVGAModeInformation(Bit16u mode, Bit16u seg, Bit16u off) {
    PhysPt buf = PhysMake(seg, off);
    mode &= 0x3fff;  // Mask early to avoid repeated operations
    if (mode < 0x100) return 0x01;

    // Early SVGA mode check
    if (svga.accepts_mode && !svga.accepts_mode(mode)) return 0x01;

    // Linear search optimized with pointer arithmetic
    const VideoModeBlock* mblock = ModeList_VGA;
    while (mblock->mode != 0xffff && mblock->mode != mode) mblock++;
    if (mblock->mode == 0xffff) return VESA_FAIL;
    if (int10.vesa_oldvbe && mblock->mode >= 0x120) return 0x01;

    // Precompute common values
    Bit8u modeAttributes = 0x1b;  // Default: Color, graphics
    Bitu pageSize;
    MODE_INFO minfo = {0};  // Zero-init only once

    // Switch on mode type with minimal branching
    switch (mblock->type) {
    case M_LIN4:
        pageSize = mblock->sheight * mblock->swidth / 2;
        minfo.BytesPerScanLine = mblock->swidth / 8;
        minfo.NumberOfPlanes = 0x4;
        minfo.BitsPerPixel = 4;
        minfo.MemoryModel = 3;  // EGA planar mode
        break;
    case M_LIN8:
        pageSize = mblock->sheight * mblock->swidth;
        minfo.BytesPerScanLine = mblock->swidth;
        minfo.NumberOfPlanes = 0x1;
        minfo.BitsPerPixel = 8;
        minfo.MemoryModel = 4;  // Packed pixel
        if (!int10.vesa_nolfb) modeAttributes |= 0x80;  // Linear framebuffer
        break;
    case M_LIN15:
        pageSize = mblock->sheight * mblock->swidth * 2;
        minfo.BytesPerScanLine = mblock->swidth * 2;
        minfo.NumberOfPlanes = 0x1;
        minfo.BitsPerPixel = 15;
        minfo.MemoryModel = 6;  // HiColour
        minfo.RedMaskSize = 5;
        minfo.RedMaskPos = 10;
        minfo.GreenMaskSize = 5;
        minfo.GreenMaskPos = 5;
        minfo.BlueMaskSize = 5;
        minfo.BlueMaskPos = 0;
        minfo.ReservedMaskSize = 0x01;
        minfo.ReservedMaskPos = 0x0f;
        if (!int10.vesa_nolfb) modeAttributes |= 0x80;
        break;
    case M_LIN16:
        pageSize = mblock->sheight * mblock->swidth * 2;
        minfo.BytesPerScanLine = mblock->swidth * 2;
        minfo.NumberOfPlanes = 0x1;
        minfo.BitsPerPixel = 16;
        minfo.MemoryModel = 6;
        minfo.RedMaskSize = 5;
        minfo.RedMaskPos = 11;
        minfo.GreenMaskSize = 6;
        minfo.GreenMaskPos = 5;
        minfo.BlueMaskSize = 5;
        minfo.BlueMaskPos = 0;
        if (!int10.vesa_nolfb) modeAttributes |= 0x80;
        break;
    case M_LIN32:
        pageSize = mblock->sheight * mblock->swidth * 4;
        minfo.BytesPerScanLine = mblock->swidth * 4;
        minfo.NumberOfPlanes = 0x1;
        minfo.BitsPerPixel = 32;
        minfo.MemoryModel = 6;
        minfo.RedMaskSize = 8;
        minfo.RedMaskPos = 0x10;
        minfo.GreenMaskSize = 0x8;
        minfo.GreenMaskPos = 0x8;
        minfo.BlueMaskSize = 0x8;
        minfo.BlueMaskPos = 0x0;
        minfo.ReservedMaskSize = 0x8;
        minfo.ReservedMaskPos = 0x18;
        if (!int10.vesa_nolfb) modeAttributes |= 0x80;
        break;
    case M_TEXT:
        pageSize = 0;
        minfo.BytesPerScanLine = mblock->twidth * 2;
        minfo.NumberOfPlanes = 0x4;
        minfo.BitsPerPixel = 4;
        minfo.MemoryModel = 0;  // Text
        modeAttributes = 0x0f;  // Color, text, BIOS output
        break;
    default:
        return VESA_FAIL;
    }

    // Page size adjustment and image pages calculation
    if (pageSize & 0xFFFF) pageSize = (pageSize + 0x10000) & ~0xFFFF;
    minfo.NumberOfImagePages = (pageSize > vga.vmemsize) ? 0 : (vga.vmemsize / pageSize) - 1;
    if (pageSize > vga.vmemsize) modeAttributes &= ~0x1;

    // Set common fields
    minfo.ModeAttributes = modeAttributes;
    minfo.WinAAttributes = 0x7;  // Exists/readable/writable
    minfo.WinFuncPtr = CALLBACK_RealPointer(callback.setwindow);
    minfo.NumberOfBanks = 0x1;
    minfo.Reserved_page = 0x1;
    minfo.XCharSize = mblock->cwidth;
    minfo.YCharSize = mblock->cheight;

    // Conditional settings based on mode type
    if (mblock->type == M_TEXT) {
        minfo.WinGranularity = 32;
        minfo.WinSize = 32;
        minfo.WinASegment = 0xb800;
        minfo.XResolution = mblock->twidth;
        minfo.YResolution = mblock->theight;
    } else {
        minfo.WinGranularity = 64;
        minfo.WinSize = 64;
        minfo.WinASegment = 0xa000;
        minfo.XResolution = mblock->swidth;
        minfo.YResolution = mblock->sheight;
    }

    // Linear framebuffer base
    if (!int10.vesa_nolfb) minfo.PhysBasePtr = S3_LFB_BASE;

    // Write the entire struct at once
    MEM_BlockWrite(buf, &minfo, sizeof(MODE_INFO));
    return VESA_SUCCESS;
}

Bit8u VESA_SetSVGAMode(Bit16u mode) {
	if (INT10_SetVideoMode(mode)) {
		int10.vesa_setmode=mode&0x7fff;
		return VESA_SUCCESS;
	}
	return VESA_FAIL;
}

Bit8u VESA_GetSVGAMode(Bit16u & mode) {
	if (int10.vesa_setmode!=0xffff) mode=int10.vesa_setmode;
	else mode=CurMode->mode;
	return VESA_SUCCESS;
}

Bit8u VESA_SetCPUWindow(Bit8u window,Bit8u address) {
	if (window) return VESA_FAIL;
	if (((Bit32u)(address)*64*1024<vga.vmemsize)) {
		IO_Write(0x3d4,0x6a);
		IO_Write(0x3d5,(Bit8u)address);
		return VESA_SUCCESS;
	} else return VESA_FAIL;
}

Bit8u VESA_GetCPUWindow(Bit8u window,Bit16u & address) {
	if (window) return VESA_FAIL;
	IO_Write(0x3d4,0x6a);
	address=IO_Read(0x3d5);
	return VESA_SUCCESS;
}


Bit8u VESA_SetPalette(PhysPt data,Bitu index,Bitu count) {
//Structure is (vesa 3.0 doc): blue,green,red,alignment
	Bit8u r,g,b;
	if (index>255) return VESA_FAIL;
	if (index+count>256) return VESA_FAIL;
	IO_Write(0x3c8,(Bit8u)index);
	while (count) {
		b = mem_readb(data++);
		g = mem_readb(data++);
		r = mem_readb(data++);
		data++;
		IO_Write(0x3c9,r);
		IO_Write(0x3c9,g);
		IO_Write(0x3c9,b);
		count--;
	}
	return VESA_SUCCESS;
}


Bit8u VESA_GetPalette(PhysPt data,Bitu index,Bitu count) {
	Bit8u r,g,b;
	if (index>255) return VESA_FAIL;
	if (index+count>256) return VESA_FAIL;
	IO_Write(0x3c7,(Bit8u)index);
	while (count) {
		r = IO_Read(0x3c9);
		g = IO_Read(0x3c9);
		b = IO_Read(0x3c9);
		mem_writeb(data++,b);
		mem_writeb(data++,g);
		mem_writeb(data++,r);
		data++;
		count--;
	}
	return VESA_SUCCESS;
}

// maximum offset for the S3 Trio64 is 10 bits
#define S3_MAX_OFFSET 0x3ff

Bit8u VESA_ScanLineLength(Bit8u subcall,Bit16u val, Bit16u & bytes,Bit16u & pixels,Bit16u & lines) {
	// offset register: virtual scanline length
	Bitu pixels_per_offset;
	Bitu bytes_per_offset = 8;
	Bitu vmemsize = vga.vmemsize;
	Bitu new_offset = vga.config.scan_len;
	Bitu screen_height = CurMode->sheight;

	switch (CurMode->type) {
	case M_TEXT:
		vmemsize = 0x8000;      // we have only the 32kB window here
		screen_height = CurMode->theight;
		pixels_per_offset = 16; // two characters each 8 pixels wide
		bytes_per_offset = 4;   // 2 characters + 2 attributes
		break;
	case M_LIN4:
		pixels_per_offset = 16;
		break;
	case M_LIN8:
		pixels_per_offset = 8;
		break;
	case M_LIN15:
	case M_LIN16:
		pixels_per_offset = 4;
		break;
	case M_LIN32:
		pixels_per_offset = 2;
		break;
	default:
		return VESA_MODE_UNSUPPORTED;
	}
	switch (subcall) {
	case 0x00: // set scan length in pixels
		new_offset = val / pixels_per_offset;
		if (val % pixels_per_offset) new_offset++;
		
		if (new_offset > S3_MAX_OFFSET)
			return VESA_HW_UNSUPPORTED; // scanline too long
		vga.config.scan_len = new_offset;
		VGA_CheckScanLength();
		break;

	case 0x01: // get current scanline length
		// implemented at the end of this function
		break;

	case 0x02: // set scan length in bytes
		new_offset = val / bytes_per_offset;
		if (val % bytes_per_offset) new_offset++;
		
		if (new_offset > S3_MAX_OFFSET)
			return VESA_HW_UNSUPPORTED; // scanline too long
		vga.config.scan_len = new_offset;
		VGA_CheckScanLength();
		break;

	case 0x03: // get maximum scan line length
		// the smaller of either the hardware maximum scanline length or
		// the limit to get full y resolution of this mode
		new_offset = S3_MAX_OFFSET;
		if ((new_offset * bytes_per_offset * screen_height) > vmemsize)
			new_offset = vmemsize / (bytes_per_offset * screen_height);
		break;

	default:
		return VESA_UNIMPLEMENTED;
	}

	// set up the return values
	bytes = (Bit16u)(new_offset * bytes_per_offset);
	pixels = (Bit16u)(new_offset * pixels_per_offset);
	if (!bytes)
		// return failure on division by zero
		// some real VESA BIOS implementations may crash here
		return VESA_FAIL;

	lines = (Bit16u)(vmemsize / bytes);
	
	if (CurMode->type==M_TEXT)
		lines *= CurMode->cheight;

	return VESA_SUCCESS;
}

Bit8u VESA_SetDisplayStart(Bit16u x,Bit16u y) {
	// TODO wait for retrace in case bl==0x80
	Bitu pixels_per_offset;
	Bitu panning_factor = 1;

	switch (CurMode->type) {
	case M_TEXT:
	case M_LIN4:
		pixels_per_offset = 16;
		break;
	case M_LIN8:
		panning_factor = 2; // the panning register ignores bit0 in this mode
		pixels_per_offset = 8;
		break;
	case M_LIN15:
	case M_LIN16:
		panning_factor = 2; // this may be DOSBox specific
		pixels_per_offset = 4;
		break;
	case M_LIN32:
		pixels_per_offset = 2;
		break;
	default:
		return VESA_MODE_UNSUPPORTED;
	}
	// We would have to divide y by the character height for text modes and
	// write the remainder to the CRTC preset row scan register,
	// but VBE2 BIOSes that actually get that far also don't.
	// Only a VBE3 BIOS got it right.
	Bitu virtual_screen_width = vga.config.scan_len * pixels_per_offset;
	Bitu new_start_pixel = virtual_screen_width * y + x;
	Bitu new_crtc_start = new_start_pixel / (pixels_per_offset/2);
	Bitu new_panning = new_start_pixel % (pixels_per_offset/2);
	new_panning *= panning_factor;

	vga.config.display_start = new_crtc_start;
	
	// Setting the panning register is nice as it allows for super smooth
	// scrolling, but if we hit the retrace pulse there may be flicker as
	// panning and display start are latched at different times. 

	IO_Read(0x3da);              // reset attribute flipflop
	IO_Write(0x3c0,0x13 | 0x20); // panning register, screen on
	IO_Write(0x3c0,new_panning);

	return VESA_SUCCESS;
}

Bit8u VESA_GetDisplayStart(Bit16u & x,Bit16u & y) {
	Bitu pixels_per_offset;
	Bitu panning_factor = 1;

	switch (CurMode->type) {
	case M_TEXT:
		pixels_per_offset = 16;
		break;
	case M_LIN4:
		pixels_per_offset = 16;
		break;
	case M_LIN8:
		panning_factor = 2;
		pixels_per_offset = 8;
		break;
	case M_LIN15:
	case M_LIN16:
		panning_factor = 2;
		pixels_per_offset = 4;
		break;
	case M_LIN32:
		pixels_per_offset = 2;
		break;
	default:
		return VESA_MODE_UNSUPPORTED;
	}

	IO_Read(0x3da);              // reset attribute flipflop
	IO_Write(0x3c0,0x13 | 0x20); // panning register, screen on
	Bit8u panning = IO_Read(0x3c1);

	Bitu virtual_screen_width = vga.config.scan_len * pixels_per_offset;
	Bitu start_pixel = vga.config.display_start * (pixels_per_offset/2) 
		+ panning / panning_factor;
	
	y = start_pixel / virtual_screen_width;
	x = start_pixel % virtual_screen_width;
	return VESA_SUCCESS;
}

static Bitu VESA_SetWindow(void) {
	if (reg_bh) reg_ah=VESA_GetCPUWindow(reg_bl,reg_dx);
	else reg_ah=VESA_SetCPUWindow(reg_bl,(Bit8u)reg_dx);
	reg_al=0x4f;
	return 0;
}

static Bitu VESA_PMSetWindow(void) {
	VESA_SetCPUWindow(0,(Bit8u)reg_dx);
	return 0;
}
static Bitu VESA_PMSetPalette(void) {
	VESA_SetPalette(SegPhys(es) +  reg_edi, reg_dx, reg_cx );
	return 0;
}
static Bitu VESA_PMSetStart(void) {
	// This function is from VBE2 and directly sets the VGA
	// display start address.

	// TODO wait for retrace in case bl==0x80
	Bit32u start = (reg_dx << 16) | reg_cx;
	vga.config.display_start = start;
	return 0;
}




void INT10_SetupVESA(void) {
	/* Put the mode list somewhere in memory */
	Bitu i;
	i=0;
	int10.rom.vesa_modes=RealMake(0xc000,int10.rom.used);
//TODO Maybe add normal vga modes too, but only seems to complicate things
	while (ModeList_VGA[i].mode!=0xffff) {
		bool canuse_mode=false;
		if (!svga.accepts_mode) canuse_mode=true;
		else {
			if (svga.accepts_mode(ModeList_VGA[i].mode)) canuse_mode=true;
		}
		if (ModeList_VGA[i].mode>=0x100 && canuse_mode) {
			if ((!int10.vesa_oldvbe) || (ModeList_VGA[i].mode<0x120)) {
				phys_writew(PhysMake(0xc000,int10.rom.used),ModeList_VGA[i].mode);
				int10.rom.used+=2;
			}
		}
		i++;
	}
	phys_writew(PhysMake(0xc000,int10.rom.used),0xffff);
	int10.rom.used+=2;
	int10.rom.oemstring=RealMake(0xc000,int10.rom.used);
	Bitu len=(Bitu)(strlen(string_oem)+1);
	for (i=0;i<len;i++) {
		phys_writeb(0xc0000+int10.rom.used++,string_oem[i]);
	}
	switch (svgaCard) {
	case SVGA_S3Trio:
		break;
	}
	callback.setwindow=CALLBACK_Allocate();
	callback.pmPalette=CALLBACK_Allocate();
	callback.pmStart=CALLBACK_Allocate();
	CALLBACK_Setup(callback.setwindow,VESA_SetWindow,CB_RETF, "VESA Real Set Window");
	/* Prepare the pmode interface */
	int10.rom.pmode_interface=RealMake(0xc000,int10.rom.used);
	int10.rom.used += 8;		//Skip the byte later used for offsets
	/* PM Set Window call */
	int10.rom.pmode_interface_window = int10.rom.used - RealOff( int10.rom.pmode_interface );
	phys_writew( Real2Phys(int10.rom.pmode_interface) + 0, int10.rom.pmode_interface_window );
	callback.pmWindow=CALLBACK_Allocate();
	int10.rom.used += (Bit16u)CALLBACK_Setup(callback.pmWindow, VESA_PMSetWindow, CB_RETN, PhysMake(0xc000,int10.rom.used), "VESA PM Set Window");
	/* PM Set start call */
	int10.rom.pmode_interface_start = int10.rom.used - RealOff( int10.rom.pmode_interface );
	phys_writew( Real2Phys(int10.rom.pmode_interface) + 2, int10.rom.pmode_interface_start);
	callback.pmStart=CALLBACK_Allocate();
	int10.rom.used += (Bit16u)CALLBACK_Setup(callback.pmStart, VESA_PMSetStart, CB_RETN, PhysMake(0xc000,int10.rom.used), "VESA PM Set Start");
	/* PM Set Palette call */
	int10.rom.pmode_interface_palette = int10.rom.used - RealOff( int10.rom.pmode_interface );
	phys_writew( Real2Phys(int10.rom.pmode_interface) + 4, int10.rom.pmode_interface_palette);
	callback.pmPalette=CALLBACK_Allocate();
	int10.rom.used += (Bit16u)CALLBACK_Setup(callback.pmPalette, VESA_PMSetPalette, CB_RETN, PhysMake(0xc000,int10.rom.used), "VESA PM Set Palette");
	/* Finalize the size and clear the required ports pointer */
	phys_writew( Real2Phys(int10.rom.pmode_interface) + 6, 0);
	int10.rom.pmode_interface_size=int10.rom.used - RealOff( int10.rom.pmode_interface );
}

