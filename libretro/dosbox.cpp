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
 *
 *  Wengier: LFN support
 */

#include <libretro.h>
#include <retro_timers.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <cstdio>
#include "dosbox.h"
#include "setup.h"
#include "callback.h"
#include "control.h"
#include "cpu.h"
#include "cross.h"
#include "ints/int10.h"
#include "logging.h"
#include "render.h"
#include "menu.h"
#include "mapper.h"
#include "pic.h"

// Timing functions for libretro
static Bit32u GetTicks(void) {
    Bit32u ticks = (Bit32u)((clock() * 1000) / CLOCKS_PER_SEC);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[TIMER] GetTicks: %u ms\n", ticks);
    else
        fprintf(stdout, "[TIMER] GetTicks: %u ms\n", ticks);
    return ticks;
}

static void TIMER_AddTick(void) {
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[TIMER] Adding tick event\n");
    else
        fprintf(stdout, "[TIMER] Adding tick event\n");
    PIC_AddEvent(NULL, 0);
}

extern retro_log_printf_t log_cb;
extern MachineType machine;
extern SVGACards svgaCard;

Config * control;

/* The whole load of startups for all the subfunctions */
void MSG_Init(Section_prop *);
void LOG_StartUp(void);
void MEM_Init(Section *);
void PAGING_Init(Section *);
void IO_Init(Section *);
void CALLBACK_Init(Section*);
void PROGRAMS_Init(Section*);
void RENDER_Init(Section*);
void VGA_Init(Section*);

void DOS_Init(Section*);

void CPU_Init(Section*);

#if C_FPU
void FPU_Init(Section*);
#endif

void DMA_Init(Section*);

void MIXER_Init(Section*);
void MIDI_Init(Section*);
void HARDWARE_Init(Section*);

#if defined(PCI_FUNCTIONALITY_ENABLED)
void PCI_Init(Section*);
#endif

void KEYBOARD_Init(Section*);
void JOYSTICK_Init(Section*);
void MOUSE_Init(Section*);
void SBLASTER_Init(Section*);
void GUS_Init(Section*);
void MPU401_Init(Section*);
void PCSPEAKER_Init(Section*);
void TANDYSOUND_Init(Section*);
void DISNEY_Init(Section*);
void SERIAL_Init(Section*); 

#if C_IPX
void IPX_Init(Section*);
#endif

void SID_Init(Section* sec);

void PIC_Init(Section*);
void TIMER_Init(Section*);
void BIOS_Init(Section*);
void DEBUG_Init(Section*);
void CMOS_Init(Section*);

void MSCDEX_Init(Section*);
void DRIVES_Init(Section*);
void CDROM_Image_Init(Section*);

/* Dos Internal mostly */
void EMS_Init(Section*);
void XMS_Init(Section*);

void DOS_KeyboardLayout_Init(Section*);

void AUTOEXEC_Init(Section*);
void SHELL_Init(void);

void INT10_Init(Section*);

static LoopHandler * loop;

bool SDLNetInited;

static Bit32u ticksRemain;
static Bit32u ticksLast;
static Bit32u ticksAdded;
Bit32s ticksDone;
Bit32u ticksScheduled;
bool ticksLocked;

static Bitu Normal_Loop(void) {
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[LOOP] Entering Normal_Loop\n");
    else
        fprintf(stdout, "[LOOP] Entering Normal_Loop\n");

    Bits ret;
    while (1) {
        if (PIC_RunQueue()) {
            ret = (*cpudecoder)();
            if (GCC_UNLIKELY(ret < 0)) {
                if (log_cb)
                    log_cb(RETRO_LOG_WARN, "[LOOP] CPU decoder returned %ld, exiting\n", ret);
                else
                    fprintf(stdout, "[LOOP] CPU decoder returned %ld, exiting\n", ret);
                return 1;
            }
            if (ret > 0) {
                if (GCC_UNLIKELY(ret >= CB_MAX)) {
                    if (log_cb)
                        log_cb(RETRO_LOG_ERROR, "[LOOP] Invalid callback index %ld\n", ret);
                    else
                        fprintf(stdout, "[LOOP] Invalid callback index %ld\n", ret);
                    return 0;
                }
                Bitu blah = (*CallBack_Handlers[ret])();
                if (GCC_UNLIKELY(blah)) {
                    if (log_cb)
                        log_cb(RETRO_LOG_WARN, "[LOOP] Callback returned %lu, exiting\n", blah);
                    else
                        fprintf(stdout, "[LOOP] Callback returned %lu, exiting\n", blah);
                    return blah;
                }
            }
#if C_DEBUG
            if (DEBUG_ExitLoop()) {
                if (log_cb)
                    log_cb(RETRO_LOG_WARN, "[LOOP] Debug exit loop triggered\n");
                else
                    fprintf(stdout, "[LOOP] Debug exit loop triggered\n");
                return 0;
            }
#endif
        } else {
            GFX_Events();
            if (ticksRemain > 0) {
                TIMER_AddTick();
                ticksRemain--;
            } else
                goto increaseticks;
        }
    }
increaseticks:
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[LOOP] Adjusting ticks, locked=%d\n", ticksLocked);
    else
        fprintf(stdout, "[LOOP] Adjusting ticks, locked=%d\n", ticksLocked);

    if (GCC_UNLIKELY(ticksLocked)) {
        ticksRemain = 5;
        ticksLast = GetTicks();
        ticksAdded = 0;
        ticksDone = 0;
        ticksScheduled = 0;
    } else {
        Bit32u ticksNew;
        ticksNew = GetTicks();
        ticksScheduled += ticksAdded;
        if (ticksNew > ticksLast) {
            ticksRemain = ticksNew - ticksLast;
            ticksLast = ticksNew;
            ticksDone += ticksRemain;
            if (ticksRemain > 20) {
                ticksRemain = 20;
            }
            ticksAdded = ticksRemain;
            if (CPU_CycleAutoAdjust && !CPU_SkipCycleAutoAdjust) {
                if (ticksScheduled >= 250 || ticksDone >= 250 || (ticksAdded > 15 && ticksScheduled >= 5)) {
                    if (ticksDone < 1)
                        ticksDone = 1;
                    Bit32s ratio = (ticksScheduled * (CPU_CyclePercUsed * 90 * 1024 / 100 / 100)) / ticksDone;
                    Bit32s new_cmax = CPU_CycleMax;
                    Bit64s cproc = (Bit64s)CPU_CycleMax * (Bit64s)ticksScheduled;
                    if (cproc > 0) {
                        double ratioremoved = (double)CPU_IODelayRemoved / (double)cproc;
                        if (ratioremoved < 1.0) {
                            ratio = (Bit32s)((double)ratio * (1 - ratioremoved));
                            if (ticksScheduled >= 250 && ticksDone < 10 && ratio > 20480)
                                ratio = 20480;
                            Bit64s cmax_scaled = (Bit64s)CPU_CycleMax * (Bit64s)ratio;
                            new_cmax = (Bit32s)(1 + (CPU_CycleMax >> 1) + cmax_scaled / (Bit64s)2048);
                        }
                    }
                    if (new_cmax < CPU_CYCLES_LOWER_LIMIT)
                        new_cmax = CPU_CYCLES_LOWER_LIMIT;
                    if (ratio > 10) {
                        if ((ratio > 120) || (ticksDone < 700)) {
                            CPU_CycleMax = new_cmax;
                            if (CPU_CycleLimit > 0) {
                                if (CPU_CycleMax > CPU_CycleLimit)
                                    CPU_CycleMax = CPU_CycleLimit;
                            }
                        }
                    }
                    CPU_IODelayRemoved = 0;
                    ticksDone = 0;
                    ticksScheduled = 0;
                } else if (ticksAdded > 15) {
                    CPU_CycleMax /= 3;
                    if (CPU_CycleMax < CPU_CYCLES_LOWER_LIMIT)
                        CPU_CycleMax = CPU_CYCLES_LOWER_LIMIT;
                }
            }
        } else {
            ticksAdded = 0;
            retro_sleep(1);
            ticksDone -= GetTicks() - ticksNew;
            if (ticksDone < 0)
                ticksDone = 0;
        }
    }
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[LOOP] Normal_Loop completed, ticksRemain=%d\n", ticksRemain);
    else
        fprintf(stdout, "[LOOP] Normal_Loop completed, ticksRemain=%d\n", ticksRemain);
    return 0;
}

void DOSBOX_SetLoop(LoopHandler * handler) {
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX] Setting loop handler: %p\n", handler);
    else
        fprintf(stdout, "[DOSBOX] Setting loop handler: %p\n", handler);
    loop = handler;
}

void DOSBOX_SetNormalLoop() {
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX] Setting normal loop\n");
    else
        fprintf(stdout, "[DOSBOX] Setting normal loop\n");
    loop = Normal_Loop;
}

void DOSBOX_RunMachine(void) {
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX] Running machine\n");
    else
        fprintf(stdout, "[DOSBOX] Running machine\n");

    Bitu ret;
    do {
        ret = (*loop)();
        if (log_cb)
            log_cb(RETRO_LOG_INFO, "[DOSBOX] Loop iteration, ret=%lu\n", ret);
        else
            fprintf(stdout, "[DOSBOX] Loop iteration, ret=%lu\n", ret);
    } while (!ret);

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX] Machine run completed\n");
    else
        fprintf(stdout, "[DOSBOX] Machine run completed\n");
}

static void DOSBOX_UnlockSpeed(bool pressed) {
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX] UnlockSpeed: pressed=%d\n", pressed);
    else
        fprintf(stdout, "[DOSBOX] UnlockSpeed: pressed=%d\n", pressed);

    static bool autoadjust = false;
    if (pressed) {
        LOG_MSG("Fast Forward ON");
        ticksLocked = true;
        if (CPU_CycleAutoAdjust) {
            autoadjust = true;
            CPU_CycleAutoAdjust = false;
            CPU_CycleMax /= 3;
            if (CPU_CycleMax < 1000)
                CPU_CycleMax = 1000;
        }
    } else {
        LOG_MSG("Fast Forward OFF");
        ticksLocked = false;
        if (autoadjust) {
            autoadjust = false;
            CPU_CycleAutoAdjust = true;
        }
    }
}

static void DOSBOX_RealInit(Section * sec) {
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX] Entering RealInit\n");
    else
        fprintf(stdout, "[DOSBOX] Entering RealInit\n");

    Section_prop * section = static_cast<Section_prop *>(sec);
    ticksRemain = 0;
    ticksLast = GetTicks();
    ticksLocked = true;
    DOSBOX_SetLoop(&Normal_Loop);
    MSG_Init(section);

    MAPPER_AddHandler(DOSBOX_UnlockSpeed, MK_f12, MMOD2, "speedlock", "Speedlock");
    std::string cmd_machine;
    if (control->cmdline->FindString("-machine", cmd_machine, true)) {
        section->HandleInputline(std::string("machine=") + cmd_machine);
        if (log_cb)
            log_cb(RETRO_LOG_INFO, "[DOSBOX] Command-line machine: %s\n", cmd_machine.c_str());
        else
            fprintf(stdout, "[DOSBOX] Command-line machine: %s\n", cmd_machine.c_str());
    }

    std::string mtype(section->Get_string("machine"));
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX] Machine type: %s\n", mtype.c_str());
    else
        fprintf(stdout, "[DOSBOX] Machine type: %s\n", mtype.c_str());

    int10.vesa_nolfb = false;
    int10.vesa_oldvbe = false;
    if (mtype == "cga") {
        machine = MCH_CGA;
    } else if (mtype == "tandy") {
        machine = MCH_TANDY;
    } else if (mtype == "pcjr") {
        machine = MCH_PCJR;
    } else if (mtype == "hercules") {
        machine = MCH_HERC;
    } else if (mtype == "ega") {
        machine = MCH_EGA;
    } else if (mtype == "svga_s3") {
        svgaCard = SVGA_S3Trio;
    } else if (mtype == "vesa_nolfb") {
        svgaCard = SVGA_S3Trio;
        int10.vesa_nolfb = true;
    } else if (mtype == "vesa_oldvbe") {
        svgaCard = SVGA_S3Trio;
        int10.vesa_oldvbe = true;
    } else if (mtype == "svga_et4000") {
        svgaCard = SVGA_TsengET4K;
    } else if (mtype == "svga_et3000") {
        svgaCard = SVGA_TsengET3K;
    } else if (mtype == "svga_paradise") {
        svgaCard = SVGA_ParadisePVGA1A;
    } else if (mtype == "vgaonly") {
        svgaCard = SVGA_None;
    } else {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "[DOSBOX] Unknown machine type: %s\n", mtype.c_str());
        else
            fprintf(stdout, "[DOSBOX] Unknown machine type: %s\n", mtype.c_str());
        E_Exit("DOSBOX:Unknown machine type %s", mtype.c_str());
    }

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX] RealInit completed, machine=%d, svgaCard=%d\n", machine, svgaCard);
    else
        fprintf(stdout, "[DOSBOX] RealInit completed, machine=%d, svgaCard=%d\n", machine, svgaCard);
}

void DOSBOX_Init(void) {
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Entering DOSBOX_Init\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Entering DOSBOX_Init\n");

    Section_prop * secprop;
    Section_line * secline;
    Prop_int* Pint;
    Prop_hex* Phex;
    Prop_string* Pstring;
    Prop_bool* Pbool;
    Prop_multival* Pmulti;
    Prop_multival_remain* Pmulti_remain;

    SDLNetInited = false;
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] SDLNetInited set to false\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] SDLNetInited set to false\n");

    const char* machines[] = {
        "hercules", "cga", "tandy", "pcjr", "ega",
        "vgaonly", "svga_s3", "svga_et3000", "svga_et4000",
        "svga_paradise", "vesa_nolfb", "vesa_oldvbe", 0 };
    secprop = control->AddSection_prop("dosbox", &DOSBOX_RealInit);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Added dosbox section\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Added dosbox section\n");

    Pstring = secprop->Add_path("language", Property::Changeable::Always, "");
    Pstring->Set_help("Select another language file.");
    Pstring = secprop->Add_string("machine", Property::Changeable::OnlyAtStart, "svga_s3");
    Pstring->Set_values(machines);
    Pstring->Set_help("The type of machine DOSBox tries to emulate.");
    Pstring = secprop->Add_path("captures", Property::Changeable::Always, "capture");
    Pstring->Set_help("Directory where things like wave, midi, screenshot get captured.");

#if C_DEBUG
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Starting LOG_StartUp\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Starting LOG_StartUp\n");
    LOG_StartUp();
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] LOG_StartUp completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] LOG_StartUp completed\n");
#endif

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing IO\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing IO\n");
    secprop->AddInitFunction(&IO_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] IO_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] IO_Init completed\n");

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing PAGING\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing PAGING\n");
    secprop->AddInitFunction(&PAGING_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] PAGING_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] PAGING_Init completed\n");

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing MEM\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing MEM\n");
    secprop->AddInitFunction(&MEM_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] MEM_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] MEM_Init completed\n");

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing HARDWARE\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing HARDWARE\n");
    secprop->AddInitFunction(&HARDWARE_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] HARDWARE_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] HARDWARE_Init completed\n");

    Pint = secprop->Add_int("memsize", Property::Changeable::WhenIdle, 16);
    Pint->SetMinMax(1, 63);
    Pint->Set_help(
        "Amount of memory DOSBox has in megabytes.\n"
        "  This value is best left at its default to avoid problems with some games,\n"
        "  though few games might require a higher value.\n"
        "  There is generally no speed advantage when raising this value.");

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing CALLBACK\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing CALLBACK\n");
    secprop->AddInitFunction(&CALLBACK_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] CALLBACK_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] CALLBACK_Init completed\n");

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing PIC\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing PIC\n");
    secprop->AddInitFunction(&PIC_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] PIC_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] PIC_Init completed\n");

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing PROGRAMS\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing PROGRAMS\n");
    secprop->AddInitFunction(&PROGRAMS_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] PROGRAMS_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] PROGRAMS_Init completed\n");

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing TIMER\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing TIMER\n");
    secprop->AddInitFunction(&TIMER_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] TIMER_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] TIMER_Init completed\n");

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing CMOS\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing CMOS\n");
    secprop->AddInitFunction(&CMOS_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] CMOS_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] CMOS_Init completed\n");

    secprop = control->AddSection_prop("render", &RENDER_Init, true);
    // ... Render section configuration unchanged ...

    secprop = control->AddSection_prop("cpu", &CPU_Init, true);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing CPU\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing CPU\n");
    secprop->AddInitFunction(&CPU_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] CPU_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] CPU_Init completed\n");

#if C_FPU
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing FPU\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing FPU\n");
    secprop->AddInitFunction(&FPU_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] FPU_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] FPU_Init completed\n");
#endif

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing DMA\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing DMA\n");
    secprop->AddInitFunction(&DMA_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] DMA_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] DMA_Init completed\n");

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing VGA\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing VGA\n");
    secprop->AddInitFunction(&VGA_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] VGA_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] VGA_Init completed\n");

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing KEYBOARD\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing KEYBOARD\n");
    secprop->AddInitFunction(&KEYBOARD_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] KEYBOARD_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] KEYBOARD_Init completed\n");

#if defined(PCI_FUNCTIONALITY_ENABLED)
    secprop = control->AddSection_prop("pci", &PCI_Init, false);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing PCI\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing PCI\n");
    secprop->AddInitFunction(&PCI_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] PCI_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] PCI_Init completed\n");
#endif

    secprop = control->AddSection_prop("mixer", &MIXER_Init);
    // ... Mixer section configuration unchanged ...

    secprop = control->AddSection_prop("midi", &MIDI_Init, true);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing MIDI\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing MIDI\n");
    secprop->AddInitFunction(&MIDI_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] MIDI_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] MIDI_Init completed\n");

    secprop->AddInitFunction(&MPU401_Init, true);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] MPU401_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] MPU401_Init completed\n");

    secprop = control->AddSection_prop("sblaster", &SBLASTER_Init, true);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing SBLASTER\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing SBLASTER\n");
    secprop->AddInitFunction(&SBLASTER_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] SBLASTER_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] SBLASTER_Init completed\n");

    secprop = control->AddSection_prop("gus", &GUS_Init, true);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing GUS\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing GUS\n");
    secprop->AddInitFunction(&GUS_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] GUS_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] GUS_Init completed\n");

    secprop = control->AddSection_prop("speaker", &PCSPEAKER_Init, true);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing PCSPEAKER\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing PCSPEAKER\n");
    secprop->AddInitFunction(&PCSPEAKER_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] PCSPEAKER_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] PCSPEAKER_Init completed\n");

    secprop->AddInitFunction(&TANDYSOUND_Init, true);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] TANDYSOUND_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] TANDYSOUND_Init completed\n");

    secprop->AddInitFunction(&DISNEY_Init, true);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] DISNEY_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] DISNEY_Init completed\n");

    secprop = control->AddSection_prop("bios", &BIOS_Init, false);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing BIOS\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing BIOS\n");
    secprop->AddInitFunction(&BIOS_Init);
    secprop->AddInitFunction(&INT10_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] BIOS_Init and INT10_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] BIOS_Init and INT10_Init completed\n");

    secprop = control->AddSection_prop("joystick", &JOYSTICK_Init, true);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing JOYSTICK\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing JOYSTICK\n");
    secprop->AddInitFunction(&JOYSTICK_Init);
    secprop->AddInitFunction(&MOUSE_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] JOYSTICK_Init and MOUSE_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] JOYSTICK_Init and MOUSE_Init completed\n");

    secprop = control->AddSection_prop("serial", &SERIAL_Init, true);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing SERIAL\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing SERIAL\n");
    secprop->AddInitFunction(&SERIAL_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] SERIAL_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] SERIAL_Init completed\n");

#if C_IPX
    secprop = control->AddSection_prop("ipx", &IPX_Init, true);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Initializing IPX\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Initializing IPX\n");
    secprop->AddInitFunction(&IPX_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] IPX_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] IPX_Init completed\n");
#endif

    secprop = control->AddSection_prop("dos", &DOS_Init, false);
    secprop->AddInitFunction(&XMS_Init, true);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] XMS_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] XMS_Init completed\n");

    secprop->AddInitFunction(&EMS_Init, true);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] EMS_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] EMS_Init completed\n");

    secprop->AddInitFunction(&DOS_KeyboardLayout_Init, true);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] DOS_KeyboardLayout_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] DOS_KeyboardLayout_Init completed\n");

    secprop->AddInitFunction(&MSCDEX_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] MSCDEX_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] MSCDEX_Init completed\n");

    secprop->AddInitFunction(&DRIVES_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] DRIVES_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] DRIVES_Init completed\n");

    secprop->AddInitFunction(&CDROM_Image_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] CDROM_Image_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] CDROM_Image_Init completed\n");

    secline = control->AddSection_line("autoexec", &AUTOEXEC_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] AUTOEXEC_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] AUTOEXEC_Init completed\n");

    control->SetStartUp(&SHELL_Init);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] Set SHELL_Init as startup\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] Set SHELL_Init as startup\n");

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "[DOSBOX_INIT] DOSBOX_Init completed\n");
    else
        fprintf(stdout, "[DOSBOX_INIT] DOSBOX_Init completed\n");
}