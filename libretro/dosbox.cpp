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
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "dosbox.h"
#include "debug.h"
#include "cpu.h"
#include "video.h"
#include "pic.h"
#include "callback.h"
#include "inout.h"
#include "mixer.h"
#include "timer.h"
#include "dos_inc.h"
#include "setup.h"
#include "control.h"
#include "cross.h"
#include "programs.h"
#include "support.h"
#include "mapper.h"
#include "ints/int10.h"
#include "render.h"
#include "pci_bus.h"
#include "libretro.h"

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
    printf("[DOSBOX-LOOP] Entering Normal_Loop\n");

    Bits ret;
    while (1) {
        if (PIC_RunQueue()) {
            if (!cpudecoder) {
                printf("[DOSBOX-LOOP] Error: cpudecoder is null\n");
                return 1;
            }
            ret = (*cpudecoder)();
            if (GCC_UNLIKELY(ret < 0)) {
                printf("[DOSBOX-LOOP] CPU decoder returned %ld, exiting\n", ret);
                return 1;
            }
            if (ret > 0) {
                if (GCC_UNLIKELY(ret >= CB_MAX || !CallBack_Handlers[ret])) {
                    printf("[DOSBOX-LOOP] Invalid or null callback index %ld\n", ret);
                    return 0;
                }
                Bitu blah = (*CallBack_Handlers[ret])();
                if (GCC_UNLIKELY(blah)) {
                    printf("[DOSBOX-LOOP] Callback returned %lu, exiting\n", blah);
                    return blah;
                }
            }
#if C_DEBUG
            if (DEBUG_ExitLoop()) {
                printf("[DOSBOX-LOOP] Debug exit loop triggered\n");
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
    printf("[DOSBOX-LOOP] Adjusting ticks, locked=%d\n", ticksLocked);

    if (GCC_UNLIKELY(ticksLocked)) {
        ticksRemain = 5;
        ticksLast = GetTicks();
        ticksAdded = 0;
        ticksDone = 0;
        ticksScheduled = 0;
    } else {
        Bit32u ticksNew = GetTicks();
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
    printf("[DOSBOX-LOOP] Normal_Loop completed, ticksRemain=%d\n", ticksRemain);
    return 0;
}

void DOSBOX_SetLoop(LoopHandler * handler) {
    printf("[DOSBOX] Setting loop handler: %p\n", handler);
    loop = handler;
}

void DOSBOX_SetNormalLoop() {
    printf("[DOSBOX] Setting normal loop\n");
    loop = Normal_Loop;
}

void DOSBOX_RunMachine(void) {
    printf("[DOSBOX] Running machine\n");

    if (!loop) {
        printf("[DOSBOX] Error: loop handler is null\n");
        return;
    }

    Bitu ret;
    do {
        ret = (*loop)();
        printf("[DOSBOX] Loop iteration, ret=%lu\n", ret);
    } while (!ret);

    printf("[DOSBOX] Machine run completed\n");
}

static void DOSBOX_UnlockSpeed(bool pressed) {
    printf("[DOSBOX] UnlockSpeed: pressed=%d\n", pressed);

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
    printf("[DOSBOX] Entering RealInit\n");

    Section_prop * section = static_cast<Section_prop *>(sec);
    ticksRemain = 0;
    ticksLast = GetTicks();
    ticksLocked = false;
    DOSBOX_SetLoop(&Normal_Loop);
    MSG_Init(section);
    printf("[DOSBOX] MSG_Init completed\n");

    MAPPER_AddHandler(DOSBOX_UnlockSpeed, MK_f12, MMOD2, "speedlock", "Speedlock");
    printf("[DOSBOX] Mapper handler added for UnlockSpeed\n");

    std::string cmd_machine;
    if (control->cmdline->FindString("-machine", cmd_machine, true)) {
        section->HandleInputline(std::string("machine=") + cmd_machine);
        printf("[DOSBOX] Command-line machine: %s\n", cmd_machine.c_str());
    }

    std::string mtype(section->Get_string("machine"));
    printf("[DOSBOX] Machine type: %s\n", mtype.c_str());

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
        printf("[DOSBOX] Unknown machine type: %s\n", mtype.c_str());
        E_Exit("DOSBOX:Unknown machine type %s", mtype.c_str());
    }

    printf("[DOSBOX] RealInit completed, machine=%d, svgaCard=%d\n", machine, svgaCard);
}

void DOSBOX_Init(void) {
    printf("[DOSBOX_INIT] Entering DOSBOX_Init\n");

    Section_prop * secprop;
    Section_line * secline;
    Prop_int* Pint;
    Prop_hex* Phex;
    Prop_string* Pstring;
    Prop_bool* Pbool;
    Prop_multival* Pmulti;
    Prop_multival_remain* Pmulti_remain;

    SDLNetInited = false;
    printf("[DOSBOX_INIT] SDLNetInited set to false\n");

    const char* machines[] = {
        "hercules", "cga", "tandy", "pcjr", "ega",
        "vgaonly", "svga_s3", "svga_et3000", "svga_et4000",
        "svga_paradise", "vesa_nolfb", "vesa_oldvbe", 0 };
    secprop = control->AddSection_prop("dosbox", &DOSBOX_RealInit, true);
    printf("[DOSBOX_INIT] Added dosbox section\n");

    Pstring = secprop->Add_path("language", Property::Changeable::Always, "");
    Pstring->Set_help("Select another language file.");
    Pstring = secprop->Add_string("machine", Property::Changeable::OnlyAtStart, "svga_s3");
    Pstring->Set_values(machines);
    Pstring->Set_help("The type of machine DOSBox tries to emulate.");
    Pstring = secprop->Add_path("captures", Property::Changeable::Always, "capture");
    Pstring->Set_help("Directory where things like wave, midi, screenshot get captured.");
    printf("[DOSBOX_INIT] dosbox section properties added\n");

#if C_DEBUG
    printf("[DOSBOX_INIT] Starting LOG_StartUp\n");
    LOG_StartUp();
    printf("[DOSBOX_INIT] LOG_StartUp completed\n");
#endif

    // Moved CPU_Init earlier to ensure cpudecoder is initialized
    secprop->AddInitFunction(&CPU_Init, true);
    printf("[DOSBOX_INIT] CPU_Init completed\n");

    secprop->AddInitFunction(&CALLBACK_Init, true);
    printf("[DOSBOX_INIT] CALLBACK_Init completed\n");

    secprop->AddInitFunction(&IO_Init, true);
    printf("[DOSBOX_INIT] IO_Init completed\n");
    secprop->AddInitFunction(&PAGING_Init, true);
    printf("[DOSBOX_INIT] PAGING_Init completed\n");
    secprop->AddInitFunction(&MEM_Init, true);
    printf("[DOSBOX_INIT] MEM_Init completed\n");
    secprop->AddInitFunction(&HARDWARE_Init, true);
    printf("[DOSBOX_INIT] HARDWARE_Init completed\n");

    Pint = secprop->Add_int("memsize", Property::Changeable::WhenIdle, 16);
    Pint->SetMinMax(1, 63);
    Pint->Set_help(
        "Amount of memory DOSBox has in megabytes.\n"
        "  This value is best left at its default to avoid problems with some games,\n"
        "  though few games might require a higher value.\n"
        "  There is generally no speed advantage when raising this value.");
    printf("[DOSBOX_INIT] memsize property set to 16 MB\n");

    secprop->AddInitFunction(&PIC_Init, true);
    printf("[DOSBOX_INIT] PIC_Init completed\n");
    secprop->AddInitFunction(&PROGRAMS_Init, true);
    printf("[DOSBOX_INIT] PROGRAMS_Init completed\n");
    secprop->AddInitFunction(&TIMER_Init, true);
    printf("[DOSBOX_INIT] TIMER_Init completed\n");
    secprop->AddInitFunction(&CMOS_Init, true);
    printf("[DOSBOX_INIT] CMOS_Init completed\n");

    secprop = control->AddSection_prop("render", &RENDER_Init, true);
    Pint = secprop->Add_int("frameskip", Property::Changeable::Always, 0);
    Pint->SetMinMax(0, 10);
    Pint->Set_help("How many frames DOSBox skips before drawing one.");
    Pbool = secprop->Add_bool("aspect", Property::Changeable::Always, false);
    Pbool->Set_help("Do aspect correction, if your output method doesn't support scaling this can slow things down!.");
    Pmulti = secprop->Add_multi("scaler", Property::Changeable::Always, " ");
    Pmulti->SetValue("normal2x");
    Pmulti->Set_help("Scaler used to enlarge/enhance low resolution modes. If 'forced' is appended,\n"
                     "then the scaler will be used even if the result might not be desired.");
    Pstring = Pmulti->GetSection()->Add_string("type", Property::Changeable::Always, "normal2x");
    const char *scalers[] = { 
        "none", "normal2x", "normal3x",
#if RENDER_USE_ADVANCED_SCALERS>2
        "advmame2x", "advmame3x", "advinterp2x", "advinterp3x", "hq2x", "hq3x", "2xsai", "super2xsai", "supereagle",
#endif
#if RENDER_USE_ADVANCED_SCALERS>0
        "tv2x", "tv3x", "rgb2x", "rgb3x", "scan2x", "scan3x",
#endif
        0 };
    Pstring->Set_values(scalers);
    const char* force[] = { "", "forced", 0 };
    Pstring = Pmulti->GetSection()->Add_string("force", Property::Changeable::Always, "");
    Pstring->Set_values(force);
    printf("[DOSBOX_INIT] Added render section with RENDER_Init\n");

    secprop = control->AddSection_prop("cpu", &CPU_Init, true);
    const char* cores[] = { "auto",
#if (C_DYNAMIC_X86) || (C_DYNREC)
        "dynamic",
#endif
        "normal", "simple", 0 };
    Pstring = secprop->Add_string("core", Property::Changeable::WhenIdle, "auto");
    Pstring->Set_values(cores);
    Pstring->Set_help("CPU Core used in emulation. auto will switch to dynamic if available and\n"
                      "appropriate.");
    const char* cputype_values[] = { "auto", "386", "386_slow", "486", "486_slow", "pentium_slow", "pentium", "pentium_mmx", "386_prefetch", 0};
    Pstring = secprop->Add_string("cputype", Property::Changeable::Always, "auto");
    Pstring->Set_values(cputype_values);
    Pstring->Set_help("CPU Type used in emulation. auto is the fastest choice.");
    Pmulti_remain = secprop->Add_multiremain("cycles", Property::Changeable::Always, " ");
    Pmulti_remain->Set_help(
        "Amount of instructions DOSBox tries to emulate each millisecond.\n"
        "Setting this value too high results in sound dropouts and lags.\n"
        "Cycles can be set in 3 ways:\n"
        "  'auto'          tries to guess what a game needs.\n"
        "                  It usually works, but can fail for certain games.\n"
        "  'fixed #number' will set a fixed amount of cycles. This is what you usually\n"
        "                  need if 'auto' fails (Example: fixed 4000).\n"
        "  'max'           will allocate as much cycles as your computer is able to\n"
        "                  handle.");
    const char* cyclest[] = { "auto", "fixed", "max", "%u", 0 };
    Pstring = Pmulti_remain->GetSection()->Add_string("type", Property::Changeable::Always, "auto");
    Pmulti_remain->SetValue("auto");
    Pstring->Set_values(cyclest);
    Pstring = Pmulti_remain->GetSection()->Add_string("parameters", Property::Changeable::Always, "");
    Pint = secprop->Add_int("cycleup", Property::Changeable::Always, 10);
    Pint->SetMinMax(1, 1000000);
    Pint->Set_help("Amount of cycles to decrease/increase with keycombos.(CTRL-F11/CTRL-F12)");
    Pint = secprop->Add_int("cycledown", Property::Changeable::Always, 20);
    Pint->SetMinMax(1, 1000000);
    Pint->Set_help("Setting it lower than 100 will be a percentage.");
    printf("[DOSBOX_INIT] Added cpu section with CPU_Init\n");

#if C_FPU
    secprop->AddInitFunction(&FPU_Init, true);
    printf("[DOSBOX_INIT] FPU_Init completed\n");
#endif
    secprop->AddInitFunction(&DMA_Init, true);
    printf("[DOSBOX_INIT] DMA_Init completed\n");
    secprop->AddInitFunction(&VGA_Init, true);
    printf("[DOSBOX_INIT] VGA_Init completed\n");
    secprop->AddInitFunction(&KEYBOARD_Init, true);
    printf("[DOSBOX_INIT] KEYBOARD_Init completed\n");

#if defined(PCI_FUNCTIONALITY_ENABLED)
    secprop = control->AddSection_prop("pci", &PCI_Init, false);
    printf("[DOSBOX_INIT] Added pci section with PCI_Init\n");
#endif

    secprop = control->AddSection_prop("mixer", &MIXER_Init, true);
    Pbool = secprop->Add_bool("nosound", Property::Changeable::OnlyAtStart, false);
    Pbool->Set_help("Enable silent mode, sound is still emulated though.");
    Pint = secprop->Add_int("rate", Property::Changeable::OnlyAtStart, 44100);
    const char *rates[] = { "44100", "48000", "32000", "22050", "16000", "11025", "8000", "49716", 0 };
    Pint->Set_values(rates);
    Pint->Set_help("Mixer sample rate, setting any device's rate higher than this will probably lower their sound quality.");
    const char *blocksizes[] = { "1024", "2048", "4096", "8192", "512", "256", 0 };
    Pint = secprop->Add_int("blocksize", Property::Changeable::OnlyAtStart, 1024);
    Pint->Set_values(blocksizes);
    Pint->Set_help("Mixer block size, larger blocks might help sound stuttering but sound will also be more lagged.");
    Pint = secprop->Add_int("prebuffer", Property::Changeable::OnlyAtStart, 20);
    Pint->SetMinMax(0, 100);
    Pint->Set_help("How many milliseconds of data to keep on top of the blocksize.");
    printf("[DOSBOX_INIT] Added mixer section with MIXER_Init\n");

    secprop = control->AddSection_prop("midi", &MIDI_Init, true);
    secprop->AddInitFunction(&MPU401_Init, true);
    const char* mputypes[] = { "intelligent", "uart", "none", 0 };
    Pstring = secprop->Add_string("mpu401", Property::Changeable::WhenIdle, "intelligent");
    Pstring->Set_values(mputypes);
    Pstring->Set_help("Type of MPU-401 to emulate.");
    const char *devices[] = { "default", "win32", "alsa", "oss", "coreaudio", "coremidi", "mt32", "none", 0 };
    Pstring = secprop->Add_string("mididevice", Property::Changeable::WhenIdle, "default");
    Pstring->Set_values(devices);
    Pstring->Set_help("Device that will receive the MIDI data from MPU-401.");
    Pstring = secprop->Add_string("midiconfig", Property::Changeable::WhenIdle, "");
    Pstring->Set_help("Special configuration options for the device driver. This is usually the id of the device you want to use.\n"
                      "  or in the case of coreaudio, you can specify a soundfont here.\n"
                      "  When using a Roland MT-32 rev. 0 as midi output device, some games may require a delay in order to prevent 'buffer overflow' issues.\n"
                      "  In that case, add 'delaysysex', for example: midiconfig=2 delaysysex\n"
                      "  See the README/Manual for more details.");
    printf("[DOSBOX_INIT] Added midi section with MIDI_Init and MPU401_Init\n");

#if C_DEBUG
    secprop = control->AddSection_prop("debug", &DEBUG_Init, false);
    printf("[DOSBOX_INIT] Added debug section with DEBUG_Init\n");
#endif

    secprop = control->AddSection_prop("sblaster", &SBLASTER_Init, true);
    const char* sbtypes[] = { "sb1", "sb2", "sbpro1", "sbpro2", "sb16", "gb", "none", 0 };
    Pstring = secprop->Add_string("sbtype", Property::Changeable::WhenIdle, "sb16");
    Pstring->Set_values(sbtypes);
    Pstring->Set_help("Type of Soundblaster to emulate. gb is Gameblaster.");
    Phex = secprop->Add_hex("sbbase", Property::Changeable::WhenIdle, 0x220);
    const char *ios[] = { "220", "240", "260", "280", "2a0", "2c0", "2e0", "300", 0 };
    Phex->Set_values(ios);
    Phex->Set_help("The IO address of the soundblaster.");
    Pint = secprop->Add_int("irq", Property::Changeable::WhenIdle, 7);
    const char *irqssb[] = { "7", "5", "3", "9", "10", "11", "12", 0 };
    Pint->Set_values(irqssb);
    Pint->Set_help("The IRQ number of the soundblaster.");
    Pint = secprop->Add_int("dma", Property::Changeable::WhenIdle, 1);
    const char *dmassb[] = { "1", "5", "0", "3", "6", "7", 0 };
    Pint->Set_values(dmassb);
    Pint->Set_help("The DMA number of the soundblaster.");
    Pint = secprop->Add_int("hdma", Property::Changeable::WhenIdle, 5);
    Pint->Set_values(dmassb);
    Pint->Set_help("The High DMA number of the soundblaster.");
    Pbool = secprop->Add_bool("sbmixer", Property::Changeable::WhenIdle, true);
    Pbool->Set_help("Allow the soundblaster mixer to modify the DOSBox mixer.");
    const char* oplmodes[] = { "auto", "cms", "opl2", "dualopl2", "opl3", "opl3gold", "none", 0 };
    Pstring = secprop->Add_string("oplmode", Property::Changeable::WhenIdle, "auto");
    Pstring->Set_values(oplmodes);
    Pstring->Set_help("Type of OPL emulation. On 'auto' the mode is determined by sblaster type. All OPL modes are Adlib-compatible, except for 'cms'.");
    const char* oplemus[] = { "default", "compat", "fast", 0 };
    Pstring = secprop->Add_string("oplemu", Property::Changeable::WhenIdle, "default");
    Pstring->Set_values(oplemus);
    Pstring->Set_help("Provider for the OPL emulation. compat might provide better quality (see oplrate as well).");
    Pint = secprop->Add_int("oplrate", Property::Changeable::WhenIdle, 44100);
    const char *oplrates[] = { "44100", "49716", "48000", "32000", "22050", "16000", "11025", "8000", 0 };
    Pint->Set_values(oplrates);
    Pint->Set_help("Sample rate of OPL music emulation. Use 49716 for highest quality (set the mixer rate accordingly).");
    printf("[DOSBOX_INIT] Added sblaster section with SBLASTER_Init\n");

    secprop = control->AddSection_prop("gus", &GUS_Init, true);
    Pbool = secprop->Add_bool("gus", Property::Changeable::WhenIdle, false);
    Pbool->Set_help("Enable the Gravis Ultrasound emulation.");
    Pint = secprop->Add_int("gusrate", Property::Changeable::WhenIdle, 44100);
    Pint->Set_values(rates);
    Pint->Set_help("Sample rate of Ultrasound emulation.");
    Phex = secprop->Add_hex("gusbase", Property::Changeable::WhenIdle, 0x240);
    const char *iosgus[] = { "240", "220", "260", "280", "2a0", "2c0", "2e0", "300", 0 };
    Phex->Set_values(iosgus);
    Phex->Set_help("The IO base address of the Gravis Ultrasound.");
    Pint = secprop->Add_int("gusirq", Property::Changeable::WhenIdle, 5);
    const char *irqsgus[] = { "5", "3", "7", "9", "10", "11", "12", 0 };
    Pint->Set_values(irqsgus);
    Pint->Set_help("The IRQ number of the Gravis Ultrasound.");
    Pint = secprop->Add_int("gusdma", Property::Changeable::WhenIdle, 3);
    const char *dmasgus[] = { "3", "0", "1", "5", "6", "7", 0 };
    Pint->Set_values(dmasgus);
    Pint->Set_help("The DMA channel of the Gravis Ultrasound.");
    Pstring = secprop->Add_string("ultradir", Property::Changeable::WhenIdle, "C:\\ULTRASND");
    Pstring->Set_help(
        "Path to Ultrasound directory. In this directory\n"
        "there should be a MIDI directory that contains\n"
        "the patch files for GUS playback. Patch sets used\n"
        "with Timidity should work fine.");
    printf("[DOSBOX_INIT] Added gus section with GUS_Init\n");

    secprop = control->AddSection_prop("speaker", &PCSPEAKER_Init, true);
    Pbool = secprop->Add_bool("pcspeaker", Property::Changeable::WhenIdle, true);
    Pbool->Set_help("Enable PC-Speaker emulation.");
    Pint = secprop->Add_int("pcrate", Property::Changeable::WhenIdle, 44100);
    Pint->Set_values(rates);
    Pint->Set_help("Sample rate of the PC-Speaker sound generation.");
    secprop->AddInitFunction(&TANDYSOUND_Init, true);
    const char* tandys[] = { "auto", "on", "off", 0 };
    Pstring = secprop->Add_string("tandy", Property::Changeable::WhenIdle, "auto");
    Pstring->Set_values(tandys);
    Pstring->Set_help("Enable Tandy Sound System emulation. For 'auto', emulation is present only if machine is set to 'tandy'.");
    Pint = secprop->Add_int("tandyrate", Property::Changeable::WhenIdle, 44100);
    Pint->Set_values(rates);
    Pint->Set_help("Sample rate of the Tandy 3-Voice generation.");
    secprop->AddInitFunction(&DISNEY_Init, true);
    Pbool = secprop->Add_bool("disney", Property::Changeable::WhenIdle, true);
    Pbool->Set_help("Enable Disney Sound Source emulation. (Covox Voice Master and Speech Thing compatible).");
    printf("[DOSBOX_INIT] Added speaker section with PCSPEAKER_Init, TANDYSOUND_Init, DISNEY_Init\n");

    secprop = control->AddSection_prop("bios", &BIOS_Init, false);
    secprop->AddInitFunction(&INT10_Init, true);
    printf("[DOSBOX_INIT] Added bios section with BIOS_Init and INT10_Init\n");

    secprop = control->AddSection_prop("joystick", &JOYSTICK_Init, true);
    secprop->AddInitFunction(&MOUSE_Init, true);
    const char* joytypes[] = { "auto", "2axis", "4axis", "4axis_2", "fcs", "ch", "none", 0 };
    Pstring = secprop->Add_string("joysticktype", Property::Changeable::WhenIdle, "auto");
    Pstring->Set_values(joytypes);
    Pstring->Set_help(
        "Type of joystick to emulate: auto (default), none,\n"
        "2axis (supports two joysticks),\n"
        "4axis (supports one joystick, first joystick used),\n"
        "4axis_2 (supports one joystick, second joystick used),\n"
        "fcs (Thrustmaster), ch (CH Flightstick).\n"
        "none disables joystick emulation.\n"
        "auto chooses emulation depending on real joystick(s).\n"
        "(Remember to reset dosbox's mapperfile if you saved it earlier)");
    Pbool = secprop->Add_bool("timed", Property::Changeable::WhenIdle, true);
    Pbool->Set_help("enable timed intervals for axis. Experiment with this option, if your joystick drifts (away).");
    Pbool = secprop->Add_bool("autofire", Property::Changeable::WhenIdle, false);
    Pbool->Set_help("continuously fires as long as you keep the button pressed.");
    Pbool = secprop->Add_bool("swap34", Property::Changeable::WhenIdle, false);
    Pbool->Set_help("swap the 3rd and the 4th axis. can be useful for certain joysticks.");
    Pbool = secprop->Add_bool("buttonwrap", Property::Changeable::WhenIdle, false);
    Pbool->Set_help("enable button wrapping at the number of emulated buttons.");
    printf("[DOSBOX_INIT] Added joystick section with JOYSTICK_Init and MOUSE_Init\n");

    secprop = control->AddSection_prop("serial", &SERIAL_Init, true);
    const char* serials[] = { "dummy", "disabled", "modem", "nullmodem", "directserial", 0 };
    Pmulti_remain = secprop->Add_multiremain("serial1", Property::Changeable::WhenIdle, " ");
    Pstring = Pmulti_remain->GetSection()->Add_string("type", Property::Changeable::WhenIdle, "dummy");
    Pmulti_remain->SetValue("dummy");
    Pstring->Set_values(serials);
    Pstring = Pmulti_remain->GetSection()->Add_string("parameters", Property::Changeable::WhenIdle, "");
    Pmulti_remain->Set_help(
        "set type of device connected to com port.\n"
        "Can be disabled, dummy, modem, nullmodem, directserial.\n"
        "Additional parameters must be in the same line in the form of\n"
        "parameter:value. Parameter for all types is irq (optional).\n"
        "for directserial: realport (required), rxdelay (optional).\n"
        "                 (realport:COM1 realport:ttyS0).\n"
        "for modem: listenport (optional).\n"
        "for nullmodem: server, rxdelay, txdelay, telnet, usedtr,\n"
        "               transparent, port, inhsocket (all optional).\n"
        "Example: serial1=modem listenport:5000");
    Pmulti_remain = secprop->Add_multiremain("serial2", Property::Changeable::WhenIdle, " ");
    Pstring = Pmulti_remain->GetSection()->Add_string("type", Property::Changeable::WhenIdle, "dummy");
    Pmulti_remain->SetValue("dummy");
    Pstring->Set_values(serials);
    Pstring = Pmulti_remain->GetSection()->Add_string("parameters", Property::Changeable::WhenIdle, "");
    Pmulti_remain->Set_help("see serial1");
    Pmulti_remain = secprop->Add_multiremain("serial3", Property::Changeable::WhenIdle, " ");
    Pstring = Pmulti_remain->GetSection()->Add_string("type", Property::Changeable::WhenIdle, "disabled");
    Pmulti_remain->SetValue("disabled");
    Pstring->Set_values(serials);
    Pstring = Pmulti_remain->GetSection()->Add_string("parameters", Property::Changeable::WhenIdle, "");
    Pmulti_remain->Set_help("see serial1");
    Pmulti_remain = secprop->Add_multiremain("serial4", Property::Changeable::WhenIdle, " ");
    Pstring = Pmulti_remain->GetSection()->Add_string("type", Property::Changeable::WhenIdle, "disabled");
    Pmulti_remain->SetValue("disabled");
    Pstring->Set_values(serials);
    Pstring = Pmulti_remain->GetSection()->Add_string("parameters", Property::Changeable::WhenIdle, "");
    Pmulti_remain->Set_help("see serial1");
    printf("[DOSBOX_INIT] Added serial section with SERIAL_Init\n");

    secprop = control->AddSection_prop("dos", &DOS_Init, false);
    secprop->AddInitFunction(&XMS_Init, true);
    Pbool = secprop->Add_bool("xms", Property::Changeable::WhenIdle, true);
    Pbool->Set_help("Enable XMS support.");
    secprop->AddInitFunction(&EMS_Init, true);
    const char* ems_settings[] = { "true", "emsboard", "emm386", "false", 0 };
    Pstring = secprop->Add_string("ems", Property::Changeable::WhenIdle, "true");
    Pstring->Set_values(ems_settings);
    Pstring->Set_help("Enable EMS support. The default (=true) provides the best\n"
                      "compatibility but certain applications may run better with\n"
                      "other choices, or require EMS support to be disabled (=false)\n"
                      "to work at all.");
    Pbool = secprop->Add_bool("umb", Property::Changeable::WhenIdle, true);
    Pbool->Set_help("Enable UMB support.");
    Pstring = secprop->Add_string("ver", Property::Changeable::WhenIdle, "7.10");
    Pstring->Set_help("Set DOS version. The default value is 7.10.");
    const char* lfn_settings[] = { "true", "auto", "false", 0 };
    Pstring = secprop->Add_string("lfn", Property::Changeable::WhenIdle, "auto");
    Pstring->Set_values(lfn_settings);
    Pstring->Set_help("Enable LFN support. The default (=auto) means that LFN support\n"
                      "will be enabled if and only if the major DOS version is set to\n"
                      "at least 7.");
    secprop->AddInitFunction(&DOS_KeyboardLayout_Init, true);
    Pstring = secprop->Add_string("keyboardlayout", Property::Changeable::WhenIdle, "auto");
    Pstring->Set_help("Language code of the keyboard layout (or none).");
    secprop->AddInitFunction(&MSCDEX_Init, true);
    secprop->AddInitFunction(&DRIVES_Init, true);
    secprop->AddInitFunction(&CDROM_Image_Init, true);
    printf("[DOSBOX_INIT] Added dos section with DOS_Init, XMS_Init, EMS_Init, DOS_KeyboardLayout_Init, MSCDEX_Init, DRIVES_Init, CDROM_Image_Init\n");

#if C_IPX
    secprop = control->AddSection_prop("ipx", &IPX_Init, true);
    Pbool = secprop->Add_bool("ipx", Property::Changeable::WhenIdle, false);
    Pbool->Set_help("Enable ipx over UDP/IP emulation.");
    printf("[DOSBOX_INIT] Added ipx section with IPX_Init\n");
#endif

    secline = control->AddSection_line("autoexec", &AUTOEXEC_Init);
    MSG_Add("AUTOEXEC_CONFIGFILE_HELP",
        "Lines in this section will be run at startup.\n"
        "You can put your MOUNT lines here.\n");
    MSG_Add("CONFIGFILE_INTRO",
        "# This is the configuration file for DOSBox %s. (Please use the latest version of DOSBox)\n"
        "# Lines starting with a # are comment lines and are ignored by DOSBox.\n"
        "# They are used to (briefly) document the effect of each option.\n");
    MSG_Add("CONFIG_SUGGESTED_VALUES", "Possible values");
    printf("[DOSBOX_INIT] Added autoexec section with AUTOEXEC_Init\n");

    control->SetStartUp(&SHELL_Init);
    printf("[DOSBOX_INIT] Set SHELL_Init as startup\n");

    printf("[DOSBOX_INIT] DOSBOX_Init completed\n");
}