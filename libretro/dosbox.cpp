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
    char log_msg[256];
    Bit32u ticks = (Bit32u)((clock() * 1000) / CLOCKS_PER_SEC);
    snprintf(log_msg, sizeof(log_msg), "[TIMER] GetTicks: %u ms", ticks);
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    return ticks;
}

static void TIMER_AddTick(void) {
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "[TIMER] Adding tick event");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
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
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "[LOOP] Entering Normal_Loop");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    Bits ret;
    while (1) {
        if (PIC_RunQueue()) {
            ret = (*cpudecoder)();
            if (GCC_UNLIKELY(ret<0)) {
                snprintf(log_msg, sizeof(log_msg), "[LOOP] CPU decoder returned %ld, exiting", ret);
                if (log_cb) log_cb(RETRO_LOG_WARN, log_msg); else printf("%s\n", log_msg);
                return 1;
            }
            if (ret>0) {
                if (GCC_UNLIKELY(ret >= CB_MAX)) {
                    snprintf(log_msg, sizeof(log_msg), "[LOOP] Invalid callback index %ld", ret);
                    if (log_cb) log_cb(RETRO_LOG_ERROR, log_msg); else printf("%s\n", log_msg);
                    return 0;
                }
                Bitu blah = (*CallBack_Handlers[ret])();
                if (GCC_UNLIKELY(blah)) {
                    snprintf(log_msg, sizeof(log_msg), "[LOOP] Callback returned %lu, exiting", blah);
                    if (log_cb) log_cb(RETRO_LOG_WARN, log_msg); else printf("%s\n", log_msg);
                    return blah;
                }
            }
#if C_DEBUG
            if (DEBUG_ExitLoop()) {
                snprintf(log_msg, sizeof(log_msg), "[LOOP] Debug exit loop triggered");
                if (log_cb) log_cb(RETRO_LOG_WARN, log_msg); else printf("%s\n", log_msg);
                return 0;
            }
#endif
        } else {
            GFX_Events();
            if (ticksRemain>0) {
                TIMER_AddTick();
                ticksRemain--;
            } else goto increaseticks;
        }
    }
increaseticks:
    snprintf(log_msg, sizeof(log_msg), "[LOOP] Adjusting ticks, locked=%d", ticksLocked);
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    if (GCC_UNLIKELY(ticksLocked)) {
        ticksRemain=5;
        ticksLast = GetTicks();
        ticksAdded = 0;
        ticksDone = 0;
        ticksScheduled = 0;
    } else {
        Bit32u ticksNew;
        ticksNew=GetTicks();
        ticksScheduled += ticksAdded;
        if (ticksNew > ticksLast) {
            ticksRemain = ticksNew-ticksLast;
            ticksLast = ticksNew;
            ticksDone += ticksRemain;
            if (ticksRemain > 20) {
                ticksRemain = 20;
            }
            ticksAdded = ticksRemain;
            if (CPU_CycleAutoAdjust && !CPU_SkipCycleAutoAdjust) {
                if (ticksScheduled >= 250 || ticksDone >= 250 || (ticksAdded > 15 && ticksScheduled >= 5)) {
                    if(ticksDone < 1) ticksDone = 1;
                    Bit32s ratio = (ticksScheduled * (CPU_CyclePercUsed*90*1024/100/100)) / ticksDone;
                    Bit32s new_cmax = CPU_CycleMax;
                    Bit64s cproc = (Bit64s)CPU_CycleMax * (Bit64s)ticksScheduled;
                    if (cproc > 0) {
                        double ratioremoved = (double) CPU_IODelayRemoved / (double) cproc;
                        if (ratioremoved < 1.0) {
                            ratio = (Bit32s)((double)ratio * (1 - ratioremoved));
                            if (ticksScheduled >= 250 && ticksDone < 10 && ratio > 20480) 
                                ratio = 20480;
                            Bit64s cmax_scaled = (Bit64s)CPU_CycleMax * (Bit64s)ratio;
                            new_cmax = (Bit32s)(1 + (CPU_CycleMax >> 1) + cmax_scaled / (Bit64s)2048);
                        }
                    }
                    if (new_cmax<CPU_CYCLES_LOWER_LIMIT)
                        new_cmax=CPU_CYCLES_LOWER_LIMIT;
                    if (ratio>10) {
                        if ((ratio>120) || (ticksDone<700)) {
                            CPU_CycleMax = new_cmax;
                            if (CPU_CycleLimit > 0) {
                                if (CPU_CycleMax>CPU_CycleLimit) CPU_CycleMax = CPU_CycleLimit;
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
    snprintf(log_msg, sizeof(log_msg), "[LOOP] Normal_Loop completed, ticksRemain=%d", ticksRemain);
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    return 0;
}

void DOSBOX_SetLoop(LoopHandler * handler) {
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX] Setting loop handler: %p", handler);
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    loop = handler;
}

void DOSBOX_SetNormalLoop() {
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX] Setting normal loop");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    loop = Normal_Loop;
}

void DOSBOX_RunMachine(void) {
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX] Running machine");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    Bitu ret;
    do {
        ret = (*loop)();
        snprintf(log_msg, sizeof(log_msg), "[DOSBOX] Loop iteration, ret=%lu", ret);
        if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    } while (!ret);

    snprintf(log_msg, sizeof(log_msg), "[DOSBOX] Machine run completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
}

static void DOSBOX_UnlockSpeed(bool pressed) {
    char log_msg[256];
    static bool autoadjust = false;
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX] UnlockSpeed: pressed=%d", pressed);
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    if (pressed) {
        LOG_MSG("Fast Forward ON");
        ticksLocked = true;
        if (CPU_CycleAutoAdjust) {
            autoadjust = true;
            CPU_CycleAutoAdjust = false;
            CPU_CycleMax /= 3;
            if (CPU_CycleMax < 1000) CPU_CycleMax = 1000;
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
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX] Entering RealInit");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

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
        snprintf(log_msg, sizeof(log_msg), "[DOSBOX] Command-line machine: %s", cmd_machine.c_str());
        if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    }

    std::string mtype(section->Get_string("machine"));
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX] Machine type: %s", mtype.c_str());
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    int10.vesa_nolfb = false;
    int10.vesa_oldvbe = false;
    if (mtype == "cga") { machine = MCH_CGA; }
    else if (mtype == "tandy") { machine = MCH_TANDY; }
    else if (mtype == "pcjr") { machine = MCH_PCJR; }
    else if (mtype == "hercules") { machine = MCH_HERC; }
    else if (mtype == "ega") { machine = MCH_EGA; }
    else if (mtype == "svga_s3") { svgaCard = SVGA_S3Trio; }
    else if (mtype == "vesa_nolfb") { svgaCard = SVGA_S3Trio; int10.vesa_nolfb = true; }
    else if (mtype == "vesa_oldvbe") { svgaCard = SVGA_S3Trio; int10.vesa_oldvbe = true; }
    else if (mtype == "svga_et4000") { svgaCard = SVGA_TsengET4K; }
    else if (mtype == "svga_et3000") { svgaCard = SVGA_TsengET3K; }
    else if (mtype == "svga_paradise") { svgaCard = SVGA_ParadisePVGA1A; }
    else if (mtype == "vgaonly") { svgaCard = SVGA_None; }
    else {
        snprintf(log_msg, sizeof(log_msg), "[DOSBOX] Unknown machine type: %s", mtype.c_str());
        if (log_cb) log_cb(RETRO_LOG_ERROR, log_msg); else printf("%s\n", log_msg);
        E_Exit("DOSBOX:Unknown machine type %s", mtype.c_str());
    }

    snprintf(log_msg, sizeof(log_msg), "[DOSBOX] RealInit completed, machine=%d, svgaCard=%d", machine, svgaCard);
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
}

void DOSBOX_Init(void) {
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Entering DOSBOX_Init");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    Section_prop * secprop;
    Section_line * secline;
    Prop_int* Pint;
    Prop_hex* Phex;
    Prop_string* Pstring;
    Prop_bool* Pbool;
    Prop_multival* Pmulti;
    Prop_multival_remain* Pmulti_remain;

    SDLNetInited = false;
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] SDLNetInited set to false");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    const char* machines[] = {
        "hercules", "cga", "tandy", "pcjr", "ega",
        "vgaonly", "svga_s3", "svga_et3000", "svga_et4000",
        "svga_paradise", "vesa_nolfb", "vesa_oldvbe", 0 };
    secprop = control->AddSection_prop("dosbox", &DOSBOX_RealInit);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Added dosbox section");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    Pstring = secprop->Add_path("language", Property::Changeable::Always, "");
    Pstring->Set_help("Select another language file.");
    Pstring = secprop->Add_string("machine", Property::Changeable::OnlyAtStart, "svga_s3");
    Pstring->Set_values(machines);
    Pstring->Set_help("The type of machine DOSBox tries to emulate.");
    Pstring = secprop->Add_path("captures", Property::Changeable::Always, "capture");
    Pstring->Set_help("Directory where things like wave, midi, screenshot get captured.");

#if C_DEBUG
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Starting LOG_StartUp");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    LOG_StartUp();
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] LOG_StartUp completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
#endif

    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing IO");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&IO_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] IO_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing PAGING");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&PAGING_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] PAGING_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing MEM");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&MEM_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] MEM_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing HARDWARE");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&HARDWARE_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] HARDWARE_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    Pint = secprop->Add_int("memsize", Property::Changeable::WhenIdle, 16);
    Pint->SetMinMax(1, 63);
    Pint->Set_help(
        "Amount of memory DOSBox has in megabytes.\n"
        "  This value is best left at its default to avoid problems with some games,\n"
        "  though few games might require a higher value.\n"
        "  There is generally no speed advantage when raising this value.");

    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing CALLBACK");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&CALLBACK_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] CALLBACK_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing PIC");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&PIC_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] PIC_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing PROGRAMS");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&PROGRAMS_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] PROGRAMS_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing TIMER");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&TIMER_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] TIMER_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing CMOS");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&CMOS_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] CMOS_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    secprop = control->AddSection_prop("render", &RENDER_Init, true);
    // ... Render section configuration unchanged ...

    secprop = control->AddSection_prop("cpu", &CPU_Init, true);
    // ... CPU section configuration unchanged ...

#if C_FPU
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing FPU");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&FPU_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] FPU_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
#endif

    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing DMA");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&DMA_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] DMA_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing VGA");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&VGA_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] VGA_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing KEYBOARD");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&KEYBOARD_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] KEYBOARD_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

#if defined(PCI_FUNCTIONALITY_ENABLED)
    secprop = control->AddSection_prop("pci", &PCI_Init, false);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing PCI");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&PCI_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] PCI_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
#endif

    secprop = control->AddSection_prop("mixer", &MIXER_Init);
    // ... Mixer section configuration unchanged ...

    secprop = control->AddSection_prop("midi", &MIDI_Init, true);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing MIDI");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&MIDI_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] MIDI_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    secprop->AddInitFunction(&MPU401_Init, true);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] MPU401_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    // ... Rest of DOSBOX_Init unchanged (midi, sblaster, gus, speaker, etc.) ...
    // Add logging for remaining init functions
    secprop = control->AddSection_prop("sblaster", &SBLASTER_Init, true);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing SBLASTER");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&SBLASTER_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] SBLASTER_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    secprop = control->AddSection_prop("gus", &GUS_Init, true);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing GUS");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&GUS_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] GUS_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    secprop = control->AddSection_prop("speaker", &PCSPEAKER_Init, true);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing PCSPEAKER");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&PCSPEAKER_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] PCSPEAKER_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    secprop->AddInitFunction(&TANDYSOUND_Init, true);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] TANDYSOUND_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    secprop->AddInitFunction(&DISNEY_Init, true);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] DISNEY_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    secprop = control->AddSection_prop("joystick", &BIOS_Init, false);
    secprop->AddInitFunction(&INT10_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] INT10_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    secprop->AddInitFunction(&MOUSE_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] MOUSE_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    secprop->AddInitFunction(&JOYSTICK_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] JOYSTICK_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    secprop = control->AddSection_prop("serial", &SERIAL_Init, true);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing SERIAL");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&SERIAL_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] SERIAL_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

#if C_IPX
    secprop = control->AddSection_prop("ipx", &IPX_Init, true);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Initializing IPX");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
    secprop->AddInitFunction(&IPX_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] IPX_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
#endif

    secprop = control->AddSection_prop("dos", &DOS_Init, false);
    secprop->AddInitFunction(&XMS_Init, true);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] XMS_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    secprop->AddInitFunction(&EMS_Init, true);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] EMS_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    secprop->AddInitFunction(&DOS_KeyboardLayout_Init, true);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] DOS_KeyboardLayout_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    secprop->AddInitFunction(&MSCDEX_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] MSCDEX_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    secprop->AddInitFunction(&DRIVES_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] DRIVES_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    secprop->AddInitFunction(&CDROM_Image_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] CDROM_Image_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    secline = control->AddSection_line("autoexec", &AUTOEXEC_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] AUTOEXEC_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    control->SetStartUp(&SHELL_Init);
    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] Set SHELL_Init as startup");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);

    snprintf(log_msg, sizeof(log_msg), "[DOSBOX_INIT] DOSBOX_Init completed");
    if (log_cb) log_cb(RETRO_LOG_INFO, log_msg); else printf("%s\n", log_msg);
}