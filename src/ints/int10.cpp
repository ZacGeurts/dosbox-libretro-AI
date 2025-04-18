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

#include "dosbox.h"
#include "mem.h"
#include "callback.h"
#include "regs.h"
#include "inout.h"
#include "int10.h"
#include "mouse.h"
#include "setup.h"
#include <cstdio>
#include <cstdint>

using Bit8u = uint8_t;
using Bit16u = uint16_t;
using Bitu = uintptr_t;

Int10Data int10;
static Bitu call_10;
static bool warned_ff = false;

static Bitu INT10_Handler() {
    FILE* log_file = fopen("int10_log.txt", "a");
    if (!log_file) {
        return CBRET_NONE; // Early return if log file cannot be opened
    }
    fprintf(log_file, "INT10_Handler called: AX=0x%04X, BX=0x%04X, DX=0x%04X\n", reg_ax, reg_bx, reg_dx);

    INT10_SetCurMode();
    fprintf(log_file, "Set current video mode\n");

    switch (reg_ah) {
    case 0x00: // Set VideoMode
        fprintf(log_file, "Setting video mode: AL=0x%02X\n", reg_al);
        Mouse_BeforeNewVideoMode(true);
        INT10_SetVideoMode(reg_al);
        Mouse_AfterNewVideoMode(true);
        // Validate BIOS memory post-mode set
        if (IS_TANDY_ARCH) {
            fprintf(log_file, "Tandy/PCjr: Verifying BIOS memory for mode 0x%02X\n", reg_al);
            real_writeb(BIOSMEM_SEG, BIOSMEM_CURRENT_MODE, reg_al);
        }
        fprintf(log_file, "Video mode set\n");
        break;
    case 0x01: // Set TextMode Cursor Shape
        fprintf(log_file, "Setting cursor shape: CH=0x%02X, CL=0x%02X\n", reg_ch, reg_cl);
        INT10_SetCursorShape(reg_ch, reg_cl);
        fprintf(log_file, "Cursor shape set\n");
        break;
    case 0x02: // Set Cursor Pos
        fprintf(log_file, "Setting cursor position: DH=0x%02X, DL=0x%02X, BH=0x%02X\n", reg_dh, reg_dl, reg_bh);
        INT10_SetCursorPos(reg_dh, reg_dl, reg_bh);
        fprintf(log_file, "Cursor position set\n");
        break;
    case 0x03: // Get Cursor Pos and Cursor Shape
        fprintf(log_file, "Getting cursor position and shape: BH=0x%02X\n", reg_bh);
        reg_dl = CURSOR_POS_COL(reg_bh);
        reg_dh = CURSOR_POS_ROW(reg_bh);
        reg_cx = real_readw(BIOSMEM_SEG, BIOSMEM_CURSOR_TYPE);
        fprintf(log_file, "Cursor position: DL=0x%02X, DH=0x%02X, Shape: CX=0x%04X\n", reg_dl, reg_dh, reg_cx);
        break;
    case 0x04: // Read Light Pen Pos
        fprintf(log_file, "Reading light pen position (unsupported)\n");
        reg_ax = 0;
        break;
    case 0x05: // Set Active Page
        fprintf(log_file, "Setting active page: AL=0x%02X\n", reg_al);
        if ((reg_al & 0x80) && IS_TANDY_ARCH) {
            Bit8u crtcpu = real_readb(BIOSMEM_SEG, BIOSMEM_CRTCPU_PAGE);
            fprintf(log_file, "Tandy architecture detected, CRTCPU=0x%02X\n", crtcpu);
            switch (reg_al) {
            case 0x80:
                reg_bh = crtcpu & 7;
                reg_bl = (crtcpu >> 3) & 0x7;
                fprintf(log_file, "Case 0x80: BH=0x%02X, BL=0x%02X\n", reg_bh, reg_bl);
                break;
            case 0x81:
                crtcpu = (crtcpu & 0xc7) | ((reg_bl & 7) << 3);
                fprintf(log_file, "Case 0x81: CRTCPU updated to 0x%02X\n", crtcpu);
                break;
            case 0x82:
                crtcpu = (crtcpu & 0xf8) | (reg_bh & 7);
                fprintf(log_file, "Case 0x82: CRTCPU updated to 0x%02X\n", crtcpu);
                break;
            case 0x83:
                crtcpu = (crtcpu & 0xc0) | (reg_bh & 7) | ((reg_bl & 7) << 3);
                fprintf(log_file, "Case 0x83: CRTCPU updated to 0x%02X\n", crtcpu);
                break;
            }
            if (machine == MCH_PCJR) {
                reg_bh = crtcpu & 7;
                reg_bl = (crtcpu >> 3) & 0x7;
                fprintf(log_file, "PCjr: BH=0x%02X, BL=0x%02X\n", reg_bh, reg_bl);
            }
            IO_WriteB(0x3df, crtcpu);
            real_writeb(BIOSMEM_SEG, BIOSMEM_CRTCPU_PAGE, crtcpu);
            fprintf(log_file, "CRTCPU page set to 0x%02X\n", crtcpu);
        } else {
            INT10_SetActivePage(reg_al);
            fprintf(log_file, "Active page set\n");
        }
        break;
    case 0x06: // Scroll Up
        fprintf(log_file, "Scrolling up: CH=0x%02X, CL=0x%02X, DH=0x%02X, DL=0x%02X, AL=0x%02X, BH=0x%02X\n",
                reg_ch, reg_cl, reg_dh, reg_dl, reg_al, reg_bh);
        INT10_ScrollWindow(reg_ch, reg_cl, reg_dh, reg_dl, -reg_al, reg_bh, 0xFF);
        fprintf(log_file, "Scroll up completed\n");
        break;
    case 0x07: // Scroll Down
        fprintf(log_file, "Scrolling down: CH=0x%02X, CL=0x%02X, DH=0x%02X, DL=0x%02X, AL=0x%02X, BH=0x%02X\n",
                reg_ch, reg_cl, reg_dh, reg_dl, reg_al, reg_bh);
        INT10_ScrollWindow(reg_ch, reg_cl, reg_dh, reg_dl, reg_al, reg_bh, 0xFF);
        fprintf(log_file, "Scroll down completed\n");
        break;
    case 0x08: // Read Character & Attribute at Cursor
        fprintf(log_file, "Reading character and attribute: BH=0x%02X\n", reg_bh);
        INT10_ReadCharAttr(&reg_ax, reg_bh);
        fprintf(log_file, "Character and attribute read: AX=0x%04X\n", reg_ax);
        break;
    case 0x09: // Write Character & Attribute at Cursor CX times
    {
        fprintf(log_file, "Writing character and attribute: AL=0x%02X, BL=0x%02X, BH=0x%02X, CX=0x%04X\n",
                reg_al, reg_bl, reg_bh, reg_cx);
        Bit8u cur_mode = real_readb(BIOSMEM_SEG, BIOSMEM_CURRENT_MODE);
        if (cur_mode == 0x11) {
            INT10_WriteChar(reg_al, (reg_bl & 0x80) | 0x3f, reg_bh, reg_cx, true);
        } else if (cur_mode == 0x13) {
            // Mode 13h (256-color graphics): Treat as character write with default attribute
            INT10_WriteChar(reg_al, reg_bl, reg_bh, reg_cx, false); // Ignore attribute in graphics mode
            fprintf(log_file, "Mode 13h: Character written without attribute\n");
        } else {
            INT10_WriteChar(reg_al, reg_bl, reg_bh, reg_cx, true);
        }
        fprintf(log_file, "Character and attribute written\n");
    }
    break;
    case 0x0A: // Write Character at Cursor CX times
        fprintf(log_file, "Writing character: AL=0x%02X, BL=0x%02X, BH=0x%02X, CX=0x%04X\n",
                reg_al, reg_bl, reg_bh, reg_cx);
        INT10_WriteChar(reg_al, reg_bl, reg_bh, reg_cx, false);
        fprintf(log_file, "Character written\n");
        break;
    case 0x0B: // Set Background/Border Colour & Set Palette
        fprintf(log_file, "Setting background/border or palette: BH=0x%02X, BL=0x%02X\n", reg_bh, reg_bl);
        switch (reg_bh) {
        case 0x00: // Background/Border color
            INT10_SetBackgroundBorder(reg_bl);
            fprintf(log_file, "Background/border color set\n");
            break;
        case 0x01: // Set color Select
        default:
            INT10_SetColorSelect(reg_bl);
            fprintf(log_file, "Color select set\n");
            break;
        }
        break;
    case 0x0C: // Write Graphics Pixel
        fprintf(log_file, "Writing graphics pixel: CX=0x%04X, DX=0x%04X, BH=0x%02X, AL=0x%02X\n",
                reg_cx, reg_dx, reg_bh, reg_al);
        INT10_PutPixel(reg_cx, reg_dx, reg_bh, reg_al);
        fprintf(log_file, "Graphics pixel written\n");
        break;
    case 0x0D: // Read Graphics Pixel
        fprintf(log_file, "Reading graphics pixel: CX=0x%04X, DX=0x%04X, BH=0x%02X\n", reg_cx, reg_dx, reg_bh);
        INT10_GetPixel(reg_cx, reg_dx, reg_bh, &reg_al);
        fprintf(log_file, "Graphics pixel read: AL=0x%02X\n", reg_al);
        break;
    case 0x0E: // Teletype Output
        fprintf(log_file, "Teletype output: AL=0x%02X, BL=0x%02X\n", reg_al, reg_bl);
        INT10_TeletypeOutput(reg_al, reg_bl);
        fprintf(log_file, "Teletype output completed\n");
        break;
    case 0x0F: // Get Videomode
        fprintf(log_file, "Getting video mode\n");
        reg_bh = real_readb(BIOSMEM_SEG, BIOSMEM_CURRENT_PAGE);
        reg_al = real_readb(BIOSMEM_SEG, BIOSMEM_CURRENT_MODE) | (real_readb(BIOSMEM_SEG, BIOSMEM_VIDEO_CTL) & 0x80);
        reg_ah = static_cast<Bit8u>(real_readw(BIOSMEM_SEG, BIOSMEM_NB_COLS));
        fprintf(log_file, "Video mode: BH=0x%02X, AL=0x%02X, AH=0x%02X\n", reg_bh, reg_al, reg_ah);
        break;
    case 0x10: // Palette Functions
        fprintf(log_file, "Palette function: AL=0x%02X\n", reg_al);
        if (!IS_EGAVGA_ARCH && (reg_al > 0x02)) {
            fprintf(log_file, "Palette function not supported for non-EGA/VGA\n");
            break;
        }
        if (!IS_VGA_ARCH && (reg_al > 0x03)) {
            fprintf(log_file, "Palette function not supported for non-VGA\n");
            break;
        }
        switch (reg_al) {
        case 0x00: // Set Single Palette Register
            fprintf(log_file, "Setting single palette register: BL=0x%02X, BH=0x%02X\n", reg_bl, reg_bh);
            INT10_SetSinglePaletteRegister(reg_bl, reg_bh);
            fprintf(log_file, "Single palette register set\n");
            break;
        case 0x01: // Set Border (Overscan) Color
            fprintf(log_file, "Setting overscan border color: BH=0x%02X\n", reg_bh);
            INT10_SetOverscanBorderColor(reg_bh);
            fprintf(log_file, "Overscan border color set\n");
            break;
        case 0x02: // Set All Palette Registers
            fprintf(log_file, "Setting all palette registers: ES:DX=0x%04X:%04X\n", SegValue(es), reg_dx);
            INT10_SetAllPaletteRegisters(SegPhys(es) + reg_dx);
            fprintf(log_file, "All palette registers set\n");
            break;
        case 0x03: // Toggle Intensity/Blinking Bit
            fprintf(log_file, "Toggling intensity/blinking bit: BL=0x%02X\n", reg_bl);
            INT10_ToggleBlinkingBit(reg_bl);
            fprintf(log_file, "Intensity/blinking bit toggled\n");
            break;
        case 0x07: // Get Single Palette Register
            fprintf(log_file, "Getting single palette register: BL=0x%02X\n", reg_bl);
            INT10_GetSinglePaletteRegister(reg_bl, &reg_bh);
            fprintf(log_file, "Single palette register: BH=0x%02X\n", reg_bh);
            break;
        case 0x08: // Read Overscan (Border Color) Register
            fprintf(log_file, "Reading overscan border color\n");
            INT10_GetOverscanBorderColor(&reg_bh);
            fprintf(log_file, "Overscan border color: BH=0x%02X\n", reg_bh);
            break;
        case 0x09: // Read All Palette Registers and Overscan Register
            fprintf(log_file, "Reading all palette registers: ES:DX=0x%04X:%04X\n", SegValue(es), reg_dx);
            INT10_GetAllPaletteRegisters(SegPhys(es) + reg_dx);
            fprintf(log_file, "All palette registers read\n");
            break;
        case 0x10: // Set Individual DAC Register
            fprintf(log_file, "Setting individual DAC register: BL=0x%02X, DH=0x%02X, CH=0x%02X, CL=0x%02X\n",
                    reg_bl, reg_dh, reg_ch, reg_cl);
            INT10_SetSingleDACRegister(reg_bl, reg_dh, reg_ch, reg_cl);
            fprintf(log_file, "Individual DAC register set\n");
            break;
        case 0x12: // Set Block of DAC Registers
            fprintf(log_file, "Setting block of DAC registers: BX=0x%04X, CX=0x%04X, ES:DX=0x%04X:%04X\n",
                    reg_bx, reg_cx, SegValue(es), reg_dx);
            INT10_SetDACBlock(reg_bx, reg_cx, SegPhys(es) + reg_dx);
            fprintf(log_file, "Block of DAC registers set\n");
            break;
        case 0x13: // Select Video DAC Color Page
            fprintf(log_file, "Selecting DAC color page: BL=0x%02X, BH=0x%02X\n", reg_bl, reg_bh);
            INT10_SelectDACPage(reg_bl, reg_bh);
            fprintf(log_file, "DAC color page selected\n");
            break;
        case 0x15: // Get Individual DAC Register
            fprintf(log_file, "Getting individual DAC register: BL=0x%02X\n", reg_bl);
            INT10_GetSingleDACRegister(reg_bl, &reg_dh, &reg_ch, &reg_cl);
            fprintf(log_file, "Individual DAC register: DH=0x%02X, CH=0x%02X, CL=0x%02X\n", reg_dh, reg_ch, reg_cl);
            break;
        case 0x17: // Get Block of DAC Register
            fprintf(log_file, "Getting block of DAC registers: BX=0x%04X, CX=0x%04X, ES:DX=0x%04X:%04X\n",
                    reg_bx, reg_cx, SegValue(es), reg_dx);
            INT10_GetDACBlock(reg_bx, reg_cx, SegPhys(es) + reg_dx);
            fprintf(log_file, "Block of DAC registers read\n");
            break;
        case 0x18: // Set Pel Mask
            fprintf(log_file, "Setting pel mask: BL=0x%02X\n", reg_bl);
            INT10_SetPelMask(reg_bl);
            fprintf(log_file, "Pel mask set\n");
            break;
        case 0x19: // Get Pel Mask
            fprintf(log_file, "Getting pel mask\n");
            INT10_GetPelMask(reg_bl);
            reg_bh = 0;
            fprintf(log_file, "Pel mask: BL=0x%02X\n", reg_bl);
            break;
        case 0x1A: // Get Video DAC Color Page
            fprintf(log_file, "Getting DAC color page\n");
            INT10_GetDACPage(&reg_bl, &reg_bh);
            fprintf(log_file, "DAC color page: BL=0x%02X, BH=0x%02X\n", reg_bl, reg_bh);
            break;
        case 0x1B: // Perform Gray-Scale Summing
            fprintf(log_file, "Performing gray-scale summing: BX=0x%04X, CX=0x%04X\n", reg_bx, reg_cx);
            INT10_PerformGrayScaleSumming(reg_bx, reg_cx);
            fprintf(log_file, "Gray-scale summing performed\n");
            break;
        case 0xF0: // ET4000: Set HiColor Graphics Mode
        case 0xF1: // ET4000: Get DAC Type
        case 0xF2: // ET4000: Check/Set HiColor Mode
            fprintf(log_file, "ET4000-specific palette function called: AL=0x%02X\n", reg_al);
            LOG(LOG_INT10, LOG_ERROR)("Function 10:ET4000 Palette Function %2X not fully implemented", reg_al);
            break;
        default:
            fprintf(log_file, "Unhandled EGA/VGA palette function: AL=0x%02X\n", reg_al);
            LOG(LOG_INT10, LOG_ERROR)("Function 10:Unhandled EGA/VGA Palette Function %2X", reg_al);
            break;
        }
        break;
    case 0x11: // Character Generator Functions
        fprintf(log_file, "Character generator function: AL=0x%02X\n", reg_al);
        if (!IS_EGAVGA_ARCH) {
            fprintf(log_file, "Not supported for non-EGA/VGA\n");
            break;
        }
        if ((reg_al & 0xf0) == 0x10) {
            Mouse_BeforeNewVideoMode(false);
            fprintf(log_file, "Mouse before new video mode\n");
        }
        switch (reg_al) {
        case 0x00: // Load User Font
        case 0x10:
            fprintf(log_file, "Loading user font: ES:BP=0x%04X:%04X, CX=0x%04X, DX=0x%04X, BL=0x%02X, BH=0x%02X\n",
                    SegValue(es), reg_bp, reg_cx, reg_dx, reg_bl, reg_bh);
            INT10_LoadFont(SegPhys(es) + reg_bp, reg_al == 0x10, reg_cx, reg_dx, reg_bl & 0x7f, reg_bh);
            fprintf(log_file, "User font loaded\n");
            break;
        case 0x01: // Load 8x14 Font
        case 0x11:
            fprintf(log_file, "Loading 8x14 font: BL=0x%02X\n", reg_bl);
            INT10_LoadFont(Real2Phys(int10.rom.font_14), reg_al == 0x11, 256, 0, reg_bl & 0x7f, 14);
            fprintf(log_file, "8x14 font loaded\n");
            break;
        case 0x02: // Load 8x8 Font
        case 0x12:
            fprintf(log_file, "Loading 8x8 font: BL=0x%02X\n", reg_bl);
            INT10_LoadFont(Real2Phys(int10.rom.font_8_first), reg_al == 0x12, 256, 0, reg_bl & 0x7f, 8);
            fprintf(log_file, "8x8 font loaded\n");
            break;
        case 0x03: // Set Block Specifier
            fprintf(log_file, "Setting block specifier: BL=0x%02X\n", reg_bl);
            IO_Write(0x3c4, 0x3);
            IO_Write(0x3c5, reg_bl);
            fprintf(log_file, "Block specifier set\n");
            break;
        case 0x04: // Load 8x16 Font
        case 0x14:
            if (!IS_VGA_ARCH) {
                fprintf(log_file, "8x16 font not supported for non-VGA\n");
                break;
            }
            fprintf(log_file, "Loading 8x16 font: BL=0x%02X\n", reg_bl);
            INT10_LoadFont(Real2Phys(int10.rom.font_16), reg_al == 0x14, 256, 0, reg_bl & 0x7f, 16);
            fprintf(log_file, "8x16 font loaded\n");
            break;
        case 0x20: // Set User 8x8 Graphics Characters
            fprintf(log_file, "Setting user 8x8 graphics characters: ES:BP=0x%04X:%04X\n", SegValue(es), reg_bp);
            RealSetVec(0x1f, RealMake(SegValue(es), reg_bp));
            real_writew(BIOSMEM_SEG, BIOSMEM_CHAR_HEIGHT, 8); // Ensure consistency
            fprintf(log_file, "User 8x8 graphics characters set\n");
            break;
        case 0x21: // Set User Graphics Characters
            fprintf(log_file, "Setting user graphics characters: ES:BP=0x%04X:%04X, CX=0x%04X\n",
                    SegValue(es), reg_bp, reg_cx);
            RealSetVec(0x43, RealMake(SegValue(es), reg_bp));
            real_writew(BIOSMEM_SEG, BIOSMEM_CHAR_HEIGHT, reg_cx);
            [[fallthrough]];
        case 0x22: // Rom 8x14 Set
            if (reg_al == 0x22) {
                fprintf(log_file, "Setting ROM 8x14 graphics characters\n");
                RealSetVec(0x43, int10.rom.font_14);
                real_writew(BIOSMEM_SEG, BIOSMEM_CHAR_HEIGHT, 14);
            }
            [[fallthrough]];
        case 0x23: // Rom 8x8 Double Dot Set
            if (reg_al == 0x23) {
                fprintf(log_file, "Setting ROM 8x8 double dot graphics characters\n");
                RealSetVec(0x43, int10.rom.font_8_first);
                real_writew(BIOSMEM_SEG, BIOSMEM_CHAR_HEIGHT, 8);
            }
            [[fallthrough]];
        case 0x24: // Rom 8x16 Set
            if (reg_al == 0x24) {
                if (!IS_VGA_ARCH) {
                    fprintf(log_file, "8x16 font not supported for non-VGA\n");
                    break;
                }
                fprintf(log_file, "Setting ROM 8x16 graphics characters\n");
                RealSetVec(0x43, int10.rom.font_16);
                real_writew(BIOSMEM_SEG, BIOSMEM_CHAR_HEIGHT, 16);
            }
            fprintf(log_file, "Setting number of rows: BL=0x%02X, DL=0x%02X\n", reg_bl, reg_dl);
            switch (reg_bl) {
            case 0x00:
                real_writeb(BIOSMEM_SEG, BIOSMEM_NB_ROWS, reg_dl - 1);
                break;
            case 0x01:
                real_writeb(BIOSMEM_SEG, BIOSMEM_NB_ROWS, 13);
                break;
            case 0x03:
                real_writeb(BIOSMEM_SEG, BIOSMEM_NB_ROWS, 42);
                break;
            case 0x02:
            default:
                real_writeb(BIOSMEM_SEG, BIOSMEM_NB_ROWS, 24);
                break;
            }
            fprintf(log_file, "Number of rows set\n");
            break;
        case 0x30: // Get Font Information
            fprintf(log_file, "Getting font information: BH=0x%02X\n", reg_bh);
            switch (reg_bh) {
            case 0x00: // Interrupt 0x1f Vector
                {
                    RealPt int_1f = RealGetVec(0x1f);
                    SegSet16(es, RealSeg(int_1f));
                    reg_bp = RealOff(int_1f);
                    fprintf(log_file, "Interrupt 0x1f vector: ES:BP=0x%04X:%04X\n", SegValue(es), reg_bp);
                }
                break;
            case 0x01: // Interrupt 0x43 Vector
                {
                    RealPt int_43 = RealGetVec(0x43);
                    SegSet16(es, RealSeg(int_43));
                    reg_bp = RealOff(int_43);
                    fprintf(log_file, "Interrupt 0x43 vector: ES:BP=0x%04X:%04X\n", SegValue(es), reg_bp);
                }
                break;
            case 0x02: // Font 8x14
                SegSet16(es, RealSeg(int10.rom.font_14));
                reg_bp = RealOff(int10.rom.font_14);
                fprintf(log_file, "Font 8x14: ES:BP=0x%04X:%04X\n", SegValue(es), reg_bp);
                break;
            case 0x03: // Font 8x8 First 128
                SegSet16(es, RealSeg(int10.rom.font_8_first));
                reg_bp = RealOff(int10.rom.font_8_first);
                fprintf(log_file, "Font 8x8 first 128: ES:BP=0x%04X:%04X\n", SegValue(es), reg_bp);
                break;
            case 0x04: // Font 8x8 Second 128
                SegSet16(es, RealSeg(int10.rom.font_8_second));
                reg_bp = RealOff(int10.rom.font_8_second);
                fprintf(log_file, "Font 8x8 second 128: ES:BP=0x%04X:%04X\n", SegValue(es), reg_bp);
                break;
            case 0x05: // Alpha Alternate 9x14
                SegSet16(es, RealSeg(int10.rom.font_14_alternate));
                reg_bp = RealOff(int10.rom.font_14_alternate);
                fprintf(log_file, "Alpha alternate 9x14: ES:BP=0x%04X:%04X\n", SegValue(es), reg_bp);
                break;
            case 0x06: // Font 8x16
                if (!IS_VGA_ARCH) {
                    fprintf(log_file, "Font 8x16 not supported for non-VGA\n");
                    break;
                }
                SegSet16(es, RealSeg(int10.rom.font_16));
                reg_bp = RealOff(int10.rom.font_16);
                fprintf(log_file, "Font 8x16: ES:BP=0x%04X:%04X\n", SegValue(es), reg_bp);
                break;
            case 0x07: // Alpha Alternate 9x16
                if (!IS_VGA_ARCH) {
                    fprintf(log_file, "Alpha alternate 9x16 not supported for non-VGA\n");
                    break;
                }
                SegSet16(es, RealSeg(int10.rom.font_16_alternate));
                reg_bp = RealOff(int10.rom.font_16_alternate);
                fprintf(log_file, "Alpha alternate 9x16: ES:BP=0x%04X:%04X\n", SegValue(es), reg_bp);
                break;
            default:
                fprintf(log_file, "Unsupported font request: BH=0x%02X\n", reg_bh);
                LOG(LOG_INT10, LOG_ERROR)("Function 11:30 Request for font %2X", reg_bh);
                break;
            }
            if ((reg_bh <= 7) || (svgaCard == SVGA_TsengET4K)) {
                reg_cx = real_readw(BIOSMEM_SEG, BIOSMEM_CHAR_HEIGHT);
                reg_dl = real_readb(BIOSMEM_SEG, BIOSMEM_NB_ROWS);
                fprintf(log_file, "Font info: CX=0x%04X, DL=0x%02X\n", reg_cx, reg_dl);
            }
            break;
        default:
            fprintf(log_file, "Unsupported character generator call: AL=0x%02X\n", reg_al);
            LOG(LOG_INT10, LOG_ERROR)("Function 11:Unsupported character generator call %2X", reg_al);
            break;
        }
        if ((reg_al & 0xf0) == 0x10) {
            Mouse_AfterNewVideoMode(false);
            fprintf(log_file, "Mouse after new video mode\n");
        }
        break;
    case 0x12: // Alternate Function Select
        fprintf(log_file, "Alternate function select: BL=0x%02X\n", reg_bl);
        if (!IS_EGAVGA_ARCH) {
            fprintf(log_file, "Not supported for non-EGA/VGA\n");
            break;
        }
        switch (reg_bl) {
        case 0x10: // Get EGA Information
            fprintf(log_file, "Getting EGA information\n");
            reg_bh = (real_readw(BIOSMEM_SEG, BIOSMEM_CRTC_ADDRESS) == 0x3B4);
            reg_bl = 3; // 256 kb
            reg_cl = real_readb(BIOSMEM_SEG, BIOSMEM_SWITCHES) & 0x0F;
            reg_ch = real_readb(BIOSMEM_SEG, BIOSMEM_SWITCHES) >> 4;
            fprintf(log_file, "EGA info: BH=0x%02X, BL=0x%02X, CL=0x%02X, CH=0x%02X\n", reg_bh, reg_bl, reg_cl, reg_ch);
            break;
        case 0x20: // Set Alternate Printscreen
            fprintf(log_file, "Setting alternate printscreen (no-op)\n");
            break;
        case 0x30: // Select Vertical Resolution
            if (!IS_VGA_ARCH) {
                fprintf(log_file, "Vertical resolution not supported for non-VGA\n");
                break;
            }
            fprintf(log_file, "Selecting vertical resolution: AL=0x%02X\n", reg_al);
            if (svgaCard != SVGA_None) {
                if (reg_al > 2) {
                    reg_al = 0; // Invalid subfunction
                    fprintf(log_file, "Invalid subfunction\n");
                    break;
                }
            }
            {
                Bit8u modeset_ctl = real_readb(BIOSMEM_SEG, BIOSMEM_MODESET_CTL);
                Bit8u video_switches = real_readb(BIOSMEM_SEG, BIOSMEM_SWITCHES) & 0xf0;
                switch (reg_al) {
                case 0: // 200
                    modeset_ctl &= 0xef;
                    modeset_ctl |= 0x80;
                    video_switches |= 8; // EGA normal/CGA emulation
                    fprintf(log_file, "Set 200-line mode\n");
                    break;
                case 1: // 350
                    modeset_ctl &= 0x6f;
                    video_switches |= 9; // EGA enhanced
                    fprintf(log_file, "Set 350-line mode\n");
                    break;
                case 2: // 400
                    modeset_ctl &= 0x6f;
                    modeset_ctl |= 0x10; // Use 400-line mode at next mode set
                    video_switches |= 9; // EGA enhanced
                    fprintf(log_file, "Set 400-line mode\n");
                    break;
                default:
                    modeset_ctl &= 0xef;
                    video_switches |= 8; // EGA normal/CGA emulation
                    fprintf(log_file, "Set default mode\n");
                    break;
                }
                real_writeb(BIOSMEM_SEG, BIOSMEM_MODESET_CTL, modeset_ctl);
                real_writeb(BIOSMEM_SEG, BIOSMEM_SWITCHES, video_switches);
                reg_al = 0x12; // Success
                fprintf(log_file, "Vertical resolution set: MODESET_CTL=0x%02X, SWITCHES=0x%02X\n", modeset_ctl, video_switches);
            }
            break;
        case 0x31: // Palette Loading on Modeset
            if (!IS_VGA_ARCH) {
                fprintf(log_file, "Palette loading not supported for non-VGA\n");
                break;
            }
            fprintf(log_file, "Setting palette loading: AL=0x%02X\n", reg_al);
            if (svgaCard == SVGA_TsengET4K) reg_al &= 1;
            if (reg_al > 1) {
                reg_al = 0; // Invalid subfunction
                fprintf(log_file, "Invalid subfunction\n");
                break;
            }
            {
                Bit8u temp = real_readb(BIOSMEM_SEG, BIOSMEM_MODESET_CTL) & 0xf7;
                if (reg_al & 1) temp |= 8; // Enable if al=0
                real_writeb(BIOSMEM_SEG, BIOSMEM_MODESET_CTL, temp);
                reg_al = 0x12;
                fprintf(log_file, "Palette loading set: MODESET_CTL=0x%02X\n", temp);
            }
            break;
        case 0x32: // Video Addressing
            if (!IS_VGA_ARCH) {
                fprintf(log_file, "Video addressing not supported for non-VGA\n");
                break;
            }
            fprintf(log_file, "Setting video addressing: AL=0x%02X\n", reg_al);
            LOG(LOG_INT10, LOG_ERROR)("Function 12:Call %2X not handled", reg_bl);
            if (svgaCard == SVGA_TsengET4K) reg_al &= 1;
            if (reg_al > 1) reg_al = 0; // Invalid subfunction
            else reg_al = 0x12; // Fake success
            fprintf(log_file, "Video addressing set: AL=0x%02X\n", reg_al);
            break;
        case 0x33: // Switch Gray-Scale Summing
            if (!IS_VGA_ARCH) {
                fprintf(log_file, "Gray-scale summing not supported for non-VGA\n");
                break;
            }
            fprintf(log_file, "Switching gray-scale summing: AL=0x%02X\n", reg_al);
            if (svgaCard == SVGA_TsengET4K) reg_al &= 1;
            if (reg_al > 1) {
                reg_al = 0;
                fprintf(log_file, "Invalid subfunction\n");
                break;
            }
            {
                Bit8u temp = real_readb(BIOSMEM_SEG, BIOSMEM_MODESET_CTL) & 0xfd;
                if (!(reg_al & 1)) temp |= 2; // Enable if al=0
                real_writeb(BIOSMEM_SEG, BIOSMEM_MODESET_CTL, temp);
                reg_al = 0x12;
                fprintf(log_file, "Gray-scale summing set: MODESET_CTL=0x%02X\n", temp);
            }
            break;
        case 0x34: // Cursor Emulation
            if (!IS_VGA_ARCH) {
                fprintf(log_file, "Cursor emulation not supported for non-VGA\n");
                break;
            }
            fprintf(log_file, "Setting cursor emulation: AL=0x%02X\n", reg_al);
            if (svgaCard == SVGA_TsengET4K) reg_al &= 1;
            if (reg_al > 1) {
                reg_al = 0;
                fprintf(log_file, "Invalid subfunction\n");
                break;
            }
            {
                Bit8u temp = real_readb(BIOSMEM_SEG, BIOSMEM_VIDEO_CTL) & 0xfe;
                real_writeb(BIOSMEM_SEG, BIOSMEM_VIDEO_CTL, temp | reg_al);
                reg_al = 0x12;
                fprintf(log_file, "Cursor emulation set: VIDEO_CTL=0x%02X\n", temp | reg_al);
            }
            break;
        case 0x35:
            if (!IS_VGA_ARCH) {
                fprintf(log_file, "Function not supported for non-VGA\n");
                break;
            }
            fprintf(log_file, "Unhandled function: BL=0x%02X\n", reg_bl);
            LOG(LOG_INT10, LOG_ERROR)("Function 12:Call %2X not handled", reg_bl);
            reg_al = 0x12;
            break;
        case 0x36: // VGA Refresh Control
            if (!IS_VGA_ARCH) {
                fprintf(log_file, "VGA refresh control not supported for non-VGA\n");
                break;
            }
            fprintf(log_file, "Setting VGA refresh control: AL=0x%02X\n", reg_al);
            if ((svgaCard == SVGA_S3Trio) && (reg_al > 1)) {
                reg_al = 0;
                fprintf(log_file, "Invalid subfunction\n");
                break;
            }
            {
                IO_Write(0x3c4, 0x1);
                Bit8u clocking = IO_Read(0x3c5);
                if (reg_al == 0)
                    clocking &= ~0x20;
                else
                    clocking |= 0x20;
                IO_Write(0x3c4, 0x1);
                IO_Write(0x3c5, clocking);
                reg_al = 0x12; // Success
                fprintf(log_file, "VGA refresh control set: Clocking=0x%02X\n", clocking);
            }
            break;
        default:
            fprintf(log_file, "Unhandled function: BL=0x%02X\n", reg_bl);
            LOG(LOG_INT10, LOG_ERROR)("Function 12:Call %2X not handled", reg_bl);
            if (machine != MCH_EGA) reg_al = 0;
            break;
        }
        break;
    case 0x13: // Write String
        fprintf(log_file, "Writing string: DH=0x%02X, DL=0x%02X, AL=0x%02X, BL=0x%02X, ES:BP=0x%04X:%04X, CX=0x%04X, BH=0x%02X\n",
                reg_dh, reg_dl, reg_al, reg_bl, SegValue(es), reg_bp, reg_cx, reg_bh);
        INT10_WriteString(reg_dh, reg_dl, reg_al, reg_bl, SegPhys(es) + reg_bp, reg_cx, reg_bh);
        fprintf(log_file, "String written\n");
        break;
    case 0x14: // Load LCD Character Font (not fully specified)
        fprintf(log_file, "Load LCD Character Font called: AL=0x%02X\n", reg_al);
        LOG(LOG_INT10, LOG_ERROR)("Function 14:Load LCD Character Font %2X not implemented", reg_al);
        break;
    case 0x15: // Return Physical Display Parameters (not fully specified)
        fprintf(log_file, "Return Physical Display Parameters called\n");
        LOG(LOG_INT10, LOG_ERROR)("Function 15:Return Physical Display Parameters not implemented");
        break;
    case 0x1A: // Display Combination
        if (!IS_VGA_ARCH) {
            fprintf(log_file, "Display combination not supported for non-VGA\n");
            break;
        }
        fprintf(log_file, "Display combination: AL=0x%02X\n", reg_al);
        if (reg_al == 0) { // Get DCC
            RealPt vsavept = real_readd(BIOSMEM_SEG, BIOSMEM_VS_POINTER);
            RealPt svstable = real_readd(RealSeg(vsavept), RealOff(vsavept) + 0x10);
            if (svstable) {
                RealPt dcctable = real_readd(RealSeg(svstable), RealOff(svstable) + 0x02);
                Bit8u entries = real_readb(RealSeg(dcctable), RealOff(dcctable) + 0x00);
                Bit8u idx = real_readb(BIOSMEM_SEG, BIOSMEM_DCC_INDEX);
                if (idx < entries) {
                    Bit16u dccentry = real_readw(RealSeg(dcctable), RealOff(dcctable) + 0x04 + idx * 2);
                    if ((dccentry & 0xff) == 0)
                        reg_bx = dccentry >> 8;
                    else
                        reg_bx = dccentry;
                } else {
                    reg_bx = 0xffff;
                }
            } else {
                reg_bx = 0xffff;
            }
            reg_ax = 0x1A;
            fprintf(log_file, "DCC retrieved: BX=0x%04X\n", reg_bx);
        } else if (reg_al == 1) { // Set DCC
            Bit8u newidx = 0xff;
            RealPt vsavept = real_readd(BIOSMEM_SEG, BIOSMEM_VS_POINTER);
            RealPt svstable = real_readd(RealSeg(vsavept), RealOff(vsavept) + 0x10);
            if (svstable) {
                RealPt dcctable = real_readd(RealSeg(svstable), RealOff(svstable) + 0x02);
                Bit8u entries = real_readb(RealSeg(dcctable), RealOff(dcctable) + 0x00);
                if (entries) {
                    Bitu ct;
                    Bit16u swpidx = reg_bh | (reg_bl << 8);
                    for (ct = 0; ct < entries; ct++) {
                        Bit16u dccentry = real_readw(RealSeg(dcctable), RealOff(dcctable) + 0x04 + ct * 2);
                        if ((dccentry == reg_bx) || (dccentry == swpidx)) {
                            newidx = static_cast<Bit8u>(ct);
                            break;
                        }
                    }
                }
            }
            real_writeb(BIOSMEM_SEG, BIOSMEM_DCC_INDEX, newidx);
            reg_ax = 0x1A;
            fprintf(log_file, "DCC set: New index=0x%02X\n", newidx);
        }
        break;
    case 0x1B: // Functionality State Information
        if (!IS_VGA_ARCH) {
            fprintf(log_file, "Functionality state info not supported for non-VGA\n");
            break;
        }
        fprintf(log_file, "Functionality state info: BX=0x%04X\n", reg_bx);
        switch (reg_bx) {
        case 0x0000:
            INT10_GetFuncStateInformation(SegPhys(es) + reg_di);
            reg_al = 0x1B;
            fprintf(log_file, "Functionality state info retrieved\n");
            break;
        default:
            fprintf(log_file, "Unhandled call: BX=0x%04X\n", reg_bx);
            LOG(LOG_INT10, LOG_ERROR)("1B:Unhandled call BX %2X", reg_bx);
            reg_al = 0;
            break;
        }
        break;
    case 0x1C: // Video Save Area
        if (!IS_VGA_ARCH) {
            fprintf(log_file, "Video save area not supported for non-VGA\n");
            break;
        }
        fprintf(log_file, "Video save area: AL=0x%02X\n", reg_al);
        switch (reg_al) {
        case 0: {
            Bitu ret = INT10_VideoState_GetSize(reg_cx);
            if (ret) {
                reg_al = 0x1c;
                reg_bx = static_cast<Bit16u>(ret);
            } else {
                reg_al = 0;
            }
            fprintf(log_file, "Video state size: BX=0x%04X\n", reg_bx);
        } break;
        case 1:
            if (INT10_VideoState_Save(reg_cx, RealMake(SegValue(es), reg_bx)))
                reg_al = 0x1c;
            else
                reg_al = 0;
            fprintf(log_file, "Video state saved: AL=0x%02X\n", reg_al);
            break;
        case 2:
            if (INT10_VideoState_Restore(reg_cx, RealMake(SegValue(es), reg_bx)))
                reg_al = 0x1c;
            else
                reg_al = 0;
            fprintf(log_file, "Video state restored: AL=0x%02X\n", reg_al);
            break;
        default:
            if (svgaCard == SVGA_TsengET4K)
                reg_ax = 0;
            else
                reg_al = 0;
            fprintf(log_file, "Invalid subfunction\n");
            break;
        }
        break;
    case 0x4F: // VESA Calls
        if ((!IS_VGA_ARCH) || (svgaCard != SVGA_S3Trio)) {
            fprintf(log_file, "VESA calls not supported\n");
            break;
        }
        fprintf(log_file, "VESA function: AL=0x%02X\n", reg_al);
        switch (reg_al) {
        case 0x00: // Get SVGA Information
            reg_al = 0x4f;
            reg_ah = VESA_GetSVGAInformation(SegValue(es), reg_di);
            fprintf(log_file, "SVGA information retrieved: AH=0x%02X\n", reg_ah);
            break;
        case 0x01: // Get SVGA Mode Information
            reg_al = 0x4f;
            reg_ah = VESA_GetSVGAModeInformation(reg_cx, SegValue(es), reg_di);
            fprintf(log_file, "SVGA mode information retrieved: AH=0x%02X\n", reg_ah);
            break;
        case 0x02: // Set Videomode
            fprintf(log_file, "Setting SVGA mode: BX=0x%04X\n", reg_bx);
            Mouse_BeforeNewVideoMode(true);
            reg_al = 0x4f;
            reg_ah = VESA_SetSVGAMode(reg_bx);
            Mouse_AfterNewVideoMode(true);
            fprintf(log_file, "SVGA mode set: AH=0x%02X\n", reg_ah);
            break;
        case 0x03: // Get Videomode
            reg_al = 0x4f;
            reg_ah = VESA_GetSVGAMode(reg_bx);
            fprintf(log_file, "SVGA mode retrieved: AH=0x%02X, BX=0x%04X\n", reg_ah, reg_bx);
            break;
        case 0x04: // Save/Restore State
            reg_al = 0x4f;
            fprintf(log_file, "VESA save/restore state: DL=0x%02X\n", reg_dl);
            switch (reg_dl) {
            case 0: {
                Bitu ret = INT10_VideoState_GetSize(reg_cx);
                if (ret) {
                    reg_ah = 0;
                    reg_bx = static_cast<Bit16u>(ret);
                } else {
                    reg_ah = 1;
                }
                fprintf(log_file, "State size: BX=0x%04X, AH=0x%02X\n", reg_bx, reg_ah);
            } break;
            case 1:
                if (INT10_VideoState_Save(reg_cx, RealMake(SegValue(es), reg_bx)))
                    reg_ah = 0;
                else
                    reg_ah = 1;
                fprintf(log_file, "State saved: AH=0x%02X\n", reg_ah);
                break;
            case 0x2:
                if (INT10_VideoState_Restore(reg_cx, RealMake(SegValue(es), reg_bx)))
                    reg_ah = 0;
                else
                    reg_ah = 1;
                fprintf(log_file, "State restored: AH=0x%02X\n", reg_ah);
                break;
            default:
                reg_ah = 1;
                fprintf(log_file, "Invalid subfunction\n");
                break;
            }
            break;
        case 0x05: // Set/Get CPU Window
            fprintf(log_file, "VESA CPU window: BH=0x%02X\n", reg_bh);
            if (reg_bh == 0) { // Set CPU Window
                reg_ah = VESA_SetCPUWindow(reg_bl, reg_dl);
                reg_al = 0x4f;
                fprintf(log_file, "CPU window set: AH=0x%02X\n", reg_ah);
            } else if (reg_bh == 1) { // Get CPU Window
                reg_ah = VESA_GetCPUWindow(reg_bl, reg_dx);
                reg_al = 0x4f;
                fprintf(log_file, "CPU window retrieved: AH=0x%02X, DX=0x%04X\n", reg_ah, reg_dx);
            } else {
                fprintf(log_file, "Unhandled VESA subfunction: BH=0x%02X\n", reg_bh);
                LOG(LOG_INT10, LOG_ERROR)("Unhandled VESA Function %X Subfunction %X", reg_al, reg_bh);
                reg_ah = 0x01;
            }
            break;
        case 0x06: // Scan Line Length
            reg_al = 0x4f;
            reg_ah = VESA_ScanLineLength(reg_bl, reg_cx, reg_bx, reg_cx, reg_dx);
            fprintf(log_file, "Scan line length: AH=0x%02X, BX=0x%04X, CX=0x%04X, DX=0x%04X\n",
                    reg_ah, reg_bx, reg_cx, reg_dx);
            break;
        case 0x07: // Set/Get Display Start
            fprintf(log_file, "VESA display start: BL=0x%02X\n", reg_bl);
            switch (reg_bl) {
            case 0x80: // Set Display Start During Retrace
            case 0x00: // Set Display Start
                reg_al = 0x4f;
                reg_ah = VESA_SetDisplayStart(reg_cx, reg_dx);
                fprintf(log_file, "Display start set: AH=0x%02X\n", reg_ah);
                break;
            case 0x01: // Get Display Start
                reg_al = 0x4f;
                reg_bh = 0x00; // Reserved
                reg_ah = VESA_GetDisplayStart(reg_cx, reg_dx);
                fprintf(log_file, "Display start retrieved: AH=0x%02X, CX=0x%04X, DX=0x%04X\n", reg_ah, reg_cx, reg_dx);
                break;
            default:
                fprintf(log_file, "Unhandled VESA subfunction: BL=0x%02X\n", reg_bl);
                LOG(LOG_INT10, LOG_ERROR)("Unhandled VESA Function %X Subfunction %X", reg_al, reg_bl);
                reg_ah = 0x1;
                break;
            }
            break;
        case 0x09: // Set/Get Palette
            fprintf(log_file, "VESA palette: BL=0x%02X\n", reg_bl);
            switch (reg_bl) {
            case 0x80: // Set Palette During Retrace
            case 0x00: // Set Palette
                reg_ah = VESA_SetPalette(SegPhys(es) + reg_di, reg_dx, reg_cx);
                reg_al = 0x4f;
                fprintf(log_file, "Palette set: AH=0x%02X\n", reg_ah);
                break;
            case 0x01: // Get Palette
                reg_ah = VESA_GetPalette(SegPhys(es) + reg_di, reg_dx, reg_cx);
                reg_al = 0x4f;
                fprintf(log_file, "Palette retrieved: AH=0x%02X\n", reg_ah);
                break;
            default:
                fprintf(log_file, "Unhandled VESA subfunction: BL=0x%02X\n", reg_bl);
                LOG(LOG_INT10, LOG_ERROR)("Unhandled VESA Function %X Subfunction %X", reg_al, reg_bl);
                reg_ah = 0x01;
                break;
            }
            break;
        case 0x0A: // Get Pmode Interface
            fprintf(log_file, "Getting Pmode interface: BL=0x%02X\n", reg_bl);
            if (int10.vesa_oldvbe) {
                reg_ax = 0x014f;
                fprintf(log_file, "Old VBE, returning AX=0x%04X\n", reg_ax);
                break;
            }
            switch (reg_bl) {
            case 0x00:
                reg_edi = RealOff(int10.rom.pmode_interface);
                SegSet16(es, RealSeg(int10.rom.pmode_interface));
                reg_cx = int10.rom.pmode_interface_size;
                reg_ax = 0x004f;
                fprintf(log_file, "Pmode interface: EDI=0x%08X, ES=0x%04X, CX=0x%04X\n", reg_edi, SegValue(es), reg_cx);
                break;
            case 0x01: // Get Code for "Set Window"
                reg_edi = RealOff(int10.rom.pmode_interface) + int10.rom.pmode_interface_window;
                SegSet16(es, RealSeg(int10.rom.pmode_interface));
                reg_cx = 0x10; // Should be enough for callbacks
                reg_ax = 0x004f;
                fprintf(log_file, "Set window code: EDI=0x%08X, ES=0x%04X, CX=0x%04X\n", reg_edi, SegValue(es), reg_cx);
                break;
            case 0x02: // Get Code for "Set Display Start"
                reg_edi = RealOff(int10.rom.pmode_interface) + int10.rom.pmode_interface_start;
                SegSet16(es, RealSeg(int10.rom.pmode_interface));
                reg_cx = 0x10; // Should be enough for callbacks
                reg_ax = 0x004f;
                fprintf(log_file, "Set display start code: EDI=0x%08X, ES=0x%04X, CX=0x%04X\n", reg_edi, SegValue(es), reg_cx);
                break;
            case 0x03: // Get Code for "Set Palette"
                reg_edi = RealOff(int10.rom.pmode_interface) + int10.rom.pmode_interface_palette;
                SegSet16(es, RealSeg(int10.rom.pmode_interface));
                reg_cx = 0x10; // Should be enough for callbacks
                reg_ax = 0x004f;
                fprintf(log_file, "Set palette code: EDI=0x%08X, ES=0x%04X, CX=0x%04X\n", reg_edi, SegValue(es), reg_cx);
                break;
            default:
                reg_ax = 0x014f;
                fprintf(log_file, "Invalid subfunction: AX=0x%04X\n", reg_ax);
                break;
            }
            break;
        default:
            fprintf(log_file, "Unhandled VESA function: AL=0x%02X\n", reg_al);
            LOG(LOG_INT10, LOG_ERROR)("Unhandled VESA Function %X", reg_al);
            reg_al = 0x0;
            break;
        }
        break;
    case 0xF0: // EGA RIL Read Register
        fprintf(log_file, "EGA RIL read register: BL=0x%02X, DX=0x%04X\n", reg_bl, reg_dx);
        INT10_EGA_RIL_ReadRegister(reg_bl, reg_dx);
        fprintf(log_file, "EGA RIL register read\n");
        break;
    case 0xF1: // EGA RIL Write Register
        fprintf(log_file, "EGA RIL write register: BL=0x%02X, BH=0x%02X, DX=0x%04X\n", reg_bl, reg_bh, reg_dx);
        INT10_EGA_RIL_WriteRegister(reg_bl, reg_bh, reg_dx);
        fprintf(log_file, "EGA RIL register written\n");
        break;
    case 0xF2: // EGA RIL Read Register Range
        fprintf(log_file, "EGA RIL read register range: CH=0x%02X, CL=0x%02X, DX=0x%04X, ES:BX=0x%04X:%04X\n",
                reg_ch, reg_cl, reg_dx, SegValue(es), reg_bx);
        INT10_EGA_RIL_ReadRegisterRange(reg_ch, reg_cl, reg_dx, SegPhys(es) + reg_bx);
        fprintf(log_file, "EGA RIL register range read\n");
        break;
    case 0xF3: // EGA RIL Write Register Range
        fprintf(log_file, "EGA RIL write register range: CH=0x%02X, CL=0x%02X, DX=0x%04X, ES:BX=0x%04X:%04X\n",
                reg_ch, reg_cl, reg_dx, SegValue(es), reg_bx);
        INT10_EGA_RIL_WriteRegisterRange(reg_ch, reg_cl, reg_dx, SegPhys(es) + reg_bx);
        fprintf(log_file, "EGA RIL register range written\n");
        break;
    case 0xF4: // EGA RIL Read Register Set
        fprintf(log_file, "EGA RIL read register set: CX=0x%04X, ES:BX=0x%04X:%04X\n", reg_cx, SegValue(es), reg_bx);
        INT10_EGA_RIL_ReadRegisterSet(reg_cx, SegPhys(es) + reg_bx);
        fprintf(log_file, "EGA RIL register set read\n");
        break;
    case 0xF5: // EGA RIL Write Register Set
        fprintf(log_file, "EGA RIL write register set: CX=0x%04X, ES:BX=0x%04X:%04X\n", reg_cx, SegValue(es), reg_bx);
        INT10_EGA_RIL_WriteRegisterSet(reg_cx, SegPhys(es) + reg_bx);
        fprintf(log_file, "EGA RIL register set written\n");
        break;
    case 0xFA: // EGA RIL Get Version Pt
        fprintf(log_file, "EGA RIL getting version pointer\n");
        {
            RealPt pt = INT10_EGA_RIL_GetVersionPt();
            SegSet16(es, RealSeg(pt));
            reg_bx = RealOff(pt);
            fprintf(log_file, "Version pointer: ES:BX=0x%04X:%04X\n", SegValue(es), reg_bx);
        }
        break;
    case 0xFF: // Weird NC Call
        if (!warned_ff) {
            fprintf(log_file, "Weird NC call detected\n");
            LOG(LOG_INT10, LOG_NORMAL)("INT10:FF:Weird NC call");
            warned_ff = true;
        }
        break;
    default:
        fprintf(log_file, "Unsupported function: AX=0x%04X\n", reg_ax);
        LOG(LOG_INT10, LOG_ERROR)("Function %4X not supported", reg_ax);
        break;
    }

    fclose(log_file);
    return CBRET_NONE;
}

static void INT10_Seg40Init() {
    FILE* log_file = fopen("int10_log.txt", "a");
    if (!log_file) return;
    fprintf(log_file, "Initializing segment 40\n");

    real_writeb(BIOSMEM_SEG, BIOSMEM_CHAR_HEIGHT, 16);
    real_writeb(BIOSMEM_SEG, BIOSMEM_VIDEO_CTL, 0x60);
    real_writeb(BIOSMEM_SEG, BIOSMEM_SWITCHES, 0xF9);
    real_writeb(BIOSMEM_SEG, BIOSMEM_MODESET_CTL, 0x51);
    real_writeb(BIOSMEM_SEG, BIOSMEM_CURRENT_MSR, 0x09);
    real_writed(BIOSMEM_SEG, BIOSMEM_VS_POINTER, int10.rom.video_save_pointers);

    fprintf(log_file, "Segment 40 initialized\n");
    fclose(log_file);
}

static void INT10_InitVGA() {
    FILE* log_file = fopen("int10_log.txt", "a");
    if (!log_file) return;
    fprintf(log_file, "Initializing VGA\n");

    if (IS_EGAVGA_ARCH) {
        IO_Write(0x3c2, 0xc3);
        IO_Write(0x3c4, 0x04);
        IO_Write(0x3c5, 0x02);
        if (IS_VGA_ARCH) {
            IO_Write(0x3c8, 0);
            for (Bitu i = 0; i < 3 * 256; i++) IO_Write(0x3c9, 0);
        }
        fprintf(log_file, "VGA initialized\n");
    } else {
        fprintf(log_file, "VGA initialization skipped (not EGA/VGA)\n");
    }
    fclose(log_file);
}

static void SetupTandyBios() {
    static constexpr Bit8u TandyConfig[130] = {
        0x21, 0x42, 0x49, 0x4f, 0x53, 0x20, 0x52, 0x4f, 0x4d, 0x20, 0x76, 0x65, 0x72,
        0x73, 0x69, 0x6f, 0x6e, 0x20, 0x30, 0x32, 0x2e, 0x30, 0x30, 0x2e, 0x30, 0x30,
        0x0d, 0x0a, 0x43, 0x6f, 0x6d, 0x70, 0x61, 0x74, 0x69, 0x62, 0x69, 0x6c, 0x69,
        0x74, 0x79, 0x20, 0x53, 0x6f, 0x66, 0x74, 0x77, 0x61, 0x72, 0x65, 0x0d, 0x0a,
        0x43, 0x6f, 0x70, 0x79, 0x72, 0x69, 0x67, 0x68, 0x74, 0x20, 0x28, 0x43, 0x29,
        0x20, 0x31, 0x39, 0x38, 0x34, 0x2c, 0x31, 0x39, 0x38, 0x35, 0x2c, 0x31, 0x39,
        0x38, 0x36, 0x2c, 0x31, 0x39, 0x38, 0x37, 0x0d, 0x0a, 0x50, 0x68, 0x6f, 0x65,
        0x6e, 0x69, 0x78, 0x20, 0x53, 0x6f, 0x66, 0x74, 0x77, 0x61, 0x72, 0x65, 0x20,
        0x41, 0x73, 0x73, 0x6f, 0x63, 0x69, 0x61, 0x74, 0x65, 0x73, 0x20, 0x4c, 0x74,
        0x64, 0x2e, 0x0d, 0x0a, 0x61, 0x6e, 0x64, 0x20, 0x54, 0x61, 0x6e, 0x64, 0x79
    };

    FILE* log_file = fopen("int10_log.txt", "a");
    if (!log_file) return;
    fprintf(log_file, "Setting up Tandy BIOS\n");

    if (machine == MCH_TANDY) {
        for (Bitu i = 0; i < 130; i++) {
            phys_writeb(0xf0000 + i + 0xc000, TandyConfig[i]);
        }
        fprintf(log_file, "Tandy BIOS set up\n");
    } else {
        fprintf(log_file, "Tandy BIOS setup skipped (not Tandy machine)\n");
    }
    fclose(log_file);
}

void INT10_Init(Section* /*sec*/) {
    // NEW: Guard to prevent multiple initializations
    static bool initialized = false;
    FILE* log_file = fopen("int10_log.txt", "a");
    if (!log_file) return;

    if (initialized) {
        fprintf(log_file, "INT10_Init skipped: Already initialized\n");
        fclose(log_file);
        return;
    }

    fprintf(log_file, "Initializing INT10\n");

    INT10_InitVGA();
    if (IS_TANDY_ARCH) SetupTandyBios();

    call_10 = CALLBACK_Allocate();
    CALLBACK_Setup(call_10, &INT10_Handler, CB_IRET, "Int 10 video");
    RealSetVec(0x10, CALLBACK_RealPointer(call_10));

    INT10_SetupRomMemory();
    INT10_Seg40Init();
    INT10_SetVideoMode(0x3);

    initialized = true; // NEW: Mark as initialized
    fprintf(log_file, "INT10 initialized\n");
    fclose(log_file);
}