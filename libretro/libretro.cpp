/*
 *  Copyright (C) 2002-2018 - The DOSBox Team
 *  Copyright (C) 2015-2018 - Andrés Suárez
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

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include <libco.h>
#include "libretro.h"
#include "retrodos.h"

#include "setup.h"
#include "dosbox.h"
#include "mapper.h"
#include "joystick.h"
#include "midi.h"
#include "mixer.h"
#include "control.h"
#include "pic.h"
#include "vga.h"
#include "render.h"
#include "ints/int10.h"
#include "shell.h"
#include <cstdio>

#define RETRO_DEVICE_JOYSTICK RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1)

#ifndef PATH_MAX_LENGTH
#define PATH_MAX_LENGTH 4096
#endif

#define CORE_VERSION "0.74"

#ifndef PATH_SEPARATOR
#if defined(WINDOWS_PATH_STYLE) || defined(_WIN32)
#define PATH_SEPARATOR '\\'
#else
#define PATH_SEPARATOR '/'
#endif
#endif

inline constexpr size_t kMaxPorts = 16;

cothread_t mainThread = nullptr;
cothread_t emuThread = nullptr;

Bit32u MIXER_RETRO_GetFrequency();
void MIXER_CallBack(void* userdata, uint8_t* stream, int len);

extern Config* control;
extern Bitu g_memsize;
extern Int10Data int10;
MachineType machine = MCH_VGA;
SVGACards svgaCard = SVGA_S3Trio;

retro_log_printf_t log_cb = nullptr;

int current_port = 0;
bool autofire = false;
std::array<bool, kMaxPorts> gamepad{};
std::array<bool, kMaxPorts> connected{};
bool emulated_mouse = false;
unsigned deadzone = 0;

bool use_core_options = true;
bool adv_core_options = false;

std::string retro_save_directory;
std::string retro_system_directory;
std::string retro_content_directory;
std::string retro_library_name = "DOSBox";

retro_video_refresh_t video_cb = nullptr;
retro_audio_sample_batch_t audio_batch_cb = nullptr;
retro_input_poll_t poll_cb = nullptr;
retro_input_state_t input_cb = nullptr;
retro_environment_t environ_cb = nullptr;

std::string loadPath;
std::string configPath;
bool dosbox_exit = false;
bool frontend_exit = false;
bool is_restarting = false;

extern Bit8u RDOSGFXbuffer[1024 * 768 * 4];
extern Bitu RDOSGFXwidth, RDOSGFXheight, RDOSGFXpitch;
extern unsigned RDOSGFXcolorMode;
extern void* RDOSGFXhaveFrame;
unsigned currentWidth = 0;
unsigned currentHeight = 0;

alignas(16) std::array<uint8_t, 829 * 4> audioData{};
uint32_t samplesPerFrame = 735;

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t /*cb*/) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }

bool update_dosbox_variable(std::string_view section, std::string_view var, std::string_view val) noexcept {
    printf("[LIBRETRO] update_dosbox_variable: section=%s, var=%s, value=%s\n",
           std::string(section).c_str(), std::string(var).c_str(), std::string(val).c_str());
    if (!control) {
        printf("[LIBRETRO] update_dosbox_variable: control is null\n");
        if (log_cb) log_cb(RETRO_LOG_ERROR, "[LIBRETRO] update_dosbox_variable: control is null\n");
        return false;
    }
    if (log_cb) {
        log_cb(RETRO_LOG_INFO, "[LIBRETRO] update_dosbox_variable: section=%s, var=%s, value=%s\n",
               std::string(section).c_str(), std::string(var).c_str(), std::string(val).c_str());
    }
    if (Section* section_ptr = control->GetSection(std::string{section}); section_ptr) {
        if (Section_prop* secprop = dynamic_cast<Section_prop*>(section_ptr)) {
            section_ptr->ExecuteDestroy(false);
            std::string inputline = std::string{var} + '=' + std::string{val};
            bool result = section_ptr->HandleInputline(inputline.c_str());
            section_ptr->ExecuteInit(false);
            printf("[LIBRETRO] update_dosbox_variable: %s %s\n", inputline.c_str(), result ? "success" : "failed");
            if (log_cb) {
                log_cb(RETRO_LOG_INFO, "[LIBRETRO] update_dosbox_variable: %s %s\n",
                       inputline.c_str(), result ? "success" : "failed");
            }
            return result;
        } else {
            printf("[LIBRETRO] update_dosbox_variable: Section %s is not a Section_prop\n",
                   std::string(section).c_str());
            if (log_cb) {
                log_cb(RETRO_LOG_ERROR, "[LIBRETRO] update_dosbox_variable: Section %s is not a Section_prop\n",
                       std::string(section).c_str());
            }
        }
    } else {
        printf("[LIBRETRO] update_dosbox_variable: Section %s not found\n",
               std::string(section).c_str());
        if (log_cb) {
            log_cb(RETRO_LOG_ERROR, "[LIBRETRO] update_dosbox_variable: Section %s not found\n",
                   std::string(section).c_str());
        }
    }
    return false;
}

static const retro_variable vars[] = {
    {"dosbox_use_options", "Enable core-options; true|false"},
    {"dosbox_adv_options", "Enable advanced core-options; false|true"},
    {"dosbox_machine_type",
     "Emulated machine; svga_s3|svga_et3000|svga_et4000|svga_paradise|vesa_nolfb|vesa_oldvbe|hercules|cga|tandy|pcjr|ega|vgaonly"},
    {"dosbox_scaler", "Scaler; none|normal2x|normal3x"},
    {"dosbox_emulated_mouse", "Gamepad emulated mouse; enable|disable"},
    {"dosbox_emulated_mouse_deadzone", "Gamepad emulated deadzone; 5%|10%|15%|20%|25%|30%|0%"},
#if defined(C_DYNREC) || defined(C_DYNAMIC_X86)
    {"dosbox_cpu_core", "CPU core; auto|dynamic|normal|simple"},
#else
    {"dosbox_cpu_core", "CPU core; auto|normal|simple"},
#endif
    {"dosbox_cpu_type", "CPU type; auto|386|386_slow|486|486_slow|pentium_slow|386_prefetch"},
    {"dosbox_cpu_cycles_mode", "CPU cycle mode; fixed"},
    {"dosbox_cpu_cycles_multiplier", "CPU cycle multiplier; 1000|10000|100000|100"},
    {"dosbox_cpu_cycles", "CPU cycles; 1|2|3|4|5|6|7|8|9"},
    {"dosbox_sblaster_type", "Sound Blaster type; sb16|sb1|sb2|sbpro1|sbpro2|gb|none"},
    {"dosbox_pcspeaker", "Enable PC-Speaker; false|true"},
#if defined(C_IPX)
    {"dosbox_ipx", "Enable IPX over UDP; false|true"},
#endif
    {"dosbox_serial1", "Serial Port 1; disabled|dummy|modem|nullmodem|directserial"},
    {"dosbox_serial2", "Serial Port 2; disabled|dummy|modem|nullmodem|directserial"},
    {"dosbox_serial3", "Serial Port 3; disabled|dummy|modem|nullmodem|directserial"},
    {"dosbox_serial4", "Serial Port 4; disabled|dummy|modem|nullmodem|directserial"},
    {nullptr, nullptr},
};

static const retro_variable vars_advanced[] = {
    {"dosbox_use_options", "Enable core-options; true|false"},
    {"dosbox_adv_options", "Enable advanced core-options; false|true"},
    {"dosbox_machine_type",
     "Emulated machine; svga_s3|svga_et3000|svga_et4000|svga_paradise|vesa_nolfb|vesa_oldvbe|hercules|cga|tandy|pcjr|ega|vgaonly"},
    {"dosbox_scaler", "Scaler; none|normal2x|normal3x"},
    {"dosbox_emulated_mouse", "Gamepad emulated mouse; enable|disable"},
    {"dosbox_emulated_mouse_deadzone", "Gamepad emulated deadzone; 5%|10%|15%|20%|25%|30%|0%"},
#if defined(C_DYNREC) || defined(C_DYNAMIC_X86)
    {"dosbox_cpu_core", "CPU core; auto|dynamic|normal|simple"},
#else
    {"dosbox_cpu_core", "CPU core; auto|normal|simple"},
#endif
    {"dosbox_cpu_type", "CPU type; auto|386|386_slow|486|486_slow|pentium_slow|386_prefetch"},
    {"dosbox_cpu_cycles_mode", "CPU cycle mode; fixed"},
    {"dosbox_cpu_cycles_multiplier", "CPU cycle multiplier; 1000|10000|100000|100"},
    {"dosbox_cpu_cycles", "CPU cycles; 1|2|3|4|5|6|7|8|9"},
    {"dosbox_cpu_cycles_multiplier_fine", "CPU fine cycles multiplier; 100|1|10"},
    {"dosbox_cpu_cycles_fine", "CPU fine cycles; 1|2|3|4|5|6|7|9"},
    {"dosbox_sblaster_type", "Sound Blaster type; sb16|sb1|sb2|sbpro1|sbpro2|gb|none"},
    {"dosbox_sblaster_base", "Sound Blaster base address; 220|240|260|280|2a0|2c0|2e0|300"},
    {"dosbox_sblaster_irq", "Sound Blaster IRQ; 5|7|9|10|11|12|3"},
    {"dosbox_sblaster_dma", "Sound Blaster DMA; 1|3|5|6|7|0"},
    {"dosbox_sblaster_hdma", "Sound Blaster High DMA; 7|0|1|3|5|6"},
    {"dosbox_sblaster_opl_mode", "Sound Blaster OPL Mode; auto|cms|opl2|dualopl2|opl3|opl3gold|none"},
    {"dosbox_sblaster_opl_emu", "Sound Blaster OPL Provider; default|compat|fast|mame"},
    {"dosbox_pcspeaker", "Enable PC-Speaker; false|true"},
    {"dosbox_tandy", "Enable Tandy Sound System; auto|on|off"},
    {"dosbox_disney", "Enable Disney Sound Source; false|true"},
#if defined(C_IPX)
    {"dosbox_ipx", "Enable IPX over UDP; false|true"},
#endif
    {"dosbox_serial1", "Serial Port 1; disabled|dummy|modem|nullmodem|directserial"},
    {"dosbox_serial2", "Serial Port 2; disabled|dummy|modem|nullmodem|directserial"},
    {"dosbox_serial3", "Serial Port 3; disabled|dummy|modem|nullmodem|directserial"},
    {"dosbox_serial4", "Serial Port 4; disabled|dummy|modem|nullmodem|directserial"},
    {nullptr, nullptr},
};

void check_variables() noexcept {
    printf("[LIBRETRO] Entering check_variables\n");
    static bool handlers_added = false;
    static unsigned cycles = 0, cycles_fine = 0;
    static unsigned cycles_multiplier = 0, cycles_multiplier_fine = 0;
    static bool update_cycles = false;

    retro_variable var{};

    var.key = "dosbox_use_options";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        use_core_options = std::string_view{var.value} == "true";
        printf("[LIBRETRO] use_core_options=%d\n", use_core_options);
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] use_core_options=%d\n", use_core_options);
    }

    var.key = "dosbox_adv_options";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        bool new_adv = std::string_view{var.value} == "true";
        if (new_adv != adv_core_options) {
            adv_core_options = new_adv;
            environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable*>(adv_core_options ? vars_advanced : vars));
            printf("[LIBRETRO] adv_core_options=%d\n", adv_core_options);
            if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] adv_core_options=%d\n", adv_core_options);
        }
    }

    if (!use_core_options) {
        printf("[LIBRETRO] Core options disabled, skipping variable checks\n");
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Core options disabled, skipping variable checks\n");
        return;
    }

    var.key = "dosbox_machine_type";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        std::string_view machine_type{var.value};
        MachineType new_machine = MCH_VGA;
        SVGACards new_svga = SVGA_S3Trio;
        bool vesa_nolfb = false, vesa_oldvbe = false;

        if (machine_type == "hercules") {
            new_machine = MCH_HERC;
            new_svga = SVGA_None;
        } else if (machine_type == "cga") {
            new_machine = MCH_CGA;
            new_svga = SVGA_None;
        } else if (machine_type == "pcjr") {
            new_machine = MCH_PCJR;
            new_svga = SVGA_None;
        } else if (machine_type == "tandy") {
            new_machine = MCH_TANDY;
            new_svga = SVGA_None;
        } else if (machine_type == "ega") {
            new_machine = MCH_EGA;
            new_svga = SVGA_None;
        } else if (machine_type == "svga_s3") {
            new_svga = SVGA_S3Trio;
        } else if (machine_type == "svga_et4000") {
            new_svga = SVGA_TsengET4K;
        } else if (machine_type == "svga_et3000") {
            new_svga = SVGA_TsengET3K;
        } else if (machine_type == "svga_paradise") {
            new_svga = SVGA_ParadisePVGA1A;
        } else if (machine_type == "vesa_nolfb") {
            new_svga = SVGA_S3Trio;
            vesa_nolfb = true;
        } else if (machine_type == "vesa_oldvbe") {
            new_svga = SVGA_S3Trio;
            vesa_oldvbe = true;
        } else {
            new_svga = SVGA_S3Trio;
        }

        if (machine != new_machine || svgaCard != new_svga || int10.vesa_nolfb != vesa_nolfb || int10.vesa_oldvbe != vesa_oldvbe) {
            machine = new_machine;
            svgaCard = new_svga;
            int10.vesa_nolfb = vesa_nolfb;
            int10.vesa_oldvbe = vesa_oldvbe;
            update_dosbox_variable("dosbox", "machine", machine_type);
            printf("[LIBRETRO] Machine type: %s\n", std::string(machine_type).c_str());
            if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Machine type: %s\n", std::string(machine_type).c_str());
        }
    }

    var.key = "dosbox_emulated_mouse";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        bool new_mouse = std::string_view{var.value} == "enable";
        if (new_mouse != emulated_mouse) {
            emulated_mouse = new_mouse;
            MAPPER_Init();
            printf("[LIBRETRO] emulated_mouse=%d\n", emulated_mouse);
            if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] emulated_mouse=%d\n", emulated_mouse);
        }
    }

    var.key = "dosbox_emulated_mouse_deadzone";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        unsigned new_deadzone = 0;
        std::from_chars(var.value, var.value + ::strlen(var.value), new_deadzone);
        if (new_deadzone != deadzone) {
            deadzone = new_deadzone;
            MAPPER_Init();
            printf("[LIBRETRO] deadzone=%u\n", deadzone);
            if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] deadzone=%u\n", deadzone);
        }
    }

    var.key = "dosbox_cpu_cycles_mode";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        update_cycles = true;
        printf("[LIBRETRO] cpu_cycles_mode=%s\n", var.value);
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] cpu_cycles_mode=%s\n", var.value);
    }

    var.key = "dosbox_cpu_cycles";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        std::from_chars(var.value, var.value + ::strlen(var.value), cycles);
        update_cycles = true;
        printf("[LIBRETRO] cpu_cycles=%u\n", cycles);
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] cpu_cycles=%u\n", cycles);
    }

    var.key = "dosbox_cpu_cycles_multiplier";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        std::from_chars(var.value, var.value + ::strlen(var.value), cycles_multiplier);
        update_cycles = true;
        printf("[LIBRETRO] cpu_cycles_multiplier=%u\n", cycles_multiplier);
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] cpu_cycles_multiplier=%u\n", cycles_multiplier);
    }

    var.key = "dosbox_cpu_cycles_fine";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        std::from_chars(var.value, var.value + ::strlen(var.value), cycles_fine);
        update_cycles = true;
        printf("[LIBRETRO] cpu_cycles_fine=%u\n", cycles_fine);
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] cpu_cycles_fine=%u\n", cycles_fine);
    }

    var.key = "dosbox_cpu_cycles_multiplier_fine";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        std::from_chars(var.value, var.value + ::strlen(var.value), cycles_multiplier_fine);
        update_cycles = true;
        printf("[LIBRETRO] cpu_cycles_multiplier_fine=%u\n", cycles_multiplier_fine);
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] cpu_cycles_multiplier_fine=%u\n", cycles_multiplier_fine);
    }

    var.key = "dosbox_cpu_type";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        update_dosbox_variable("cpu", "cputype", var.value);
    }

    var.key = "dosbox_cpu_core";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        update_dosbox_variable("cpu", "core", var.value);
    }

    var.key = "dosbox_scaler";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        update_dosbox_variable("render", "scaler", var.value);
    }

    if (update_cycles) {
        if (std::string_view{var.value} == "fixed") {
            char s[16];
            auto result = std::to_chars(s, s + sizeof(s), cycles * cycles_multiplier + cycles_fine * cycles_multiplier_fine);
            *result.ptr = '\0';
            update_dosbox_variable("cpu", "cycles", s);
        } else {
            update_dosbox_variable("cpu", "cycles", var.value);
        }
        update_cycles = false;
    }

    var.key = "dosbox_sblaster_type";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        update_dosbox_variable("sblaster", "sbtype", var.value);
    }

    var.key = "dosbox_pcspeaker";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        update_dosbox_variable("speaker", "pcspeaker", var.value);
    }

#if defined(C_IPX)
    var.key = "dosbox_ipx";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        update_dosbox_variable("ipx", "ipx", var.value);
    }
#endif

    for (int i = 1; i <= 4; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "dosbox_serial%d", i);
        var.key = key;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            char prop[16];
            snprintf(prop, sizeof(prop), "serial%d", i);
            update_dosbox_variable("serial", prop, var.value);
            printf("[LIBRETRO] serial%d=%s\n", i, var.value);
            if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] serial%d=%s\n", i, var.value);
        } else {
            char prop[16];
            snprintf(prop, sizeof(prop), "serial%d", i);
            update_dosbox_variable("serial", prop, "disabled");
            printf("[LIBRETRO] serial%d=defaulted to disabled\n", i);
            if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] serial%d=defaulted to disabled\n", i);
        }
    }

    if (adv_core_options) {
        var.key = "dosbox_sblaster_base";
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            update_dosbox_variable("sblaster", "sbbase", var.value);
        }

        var.key = "dosbox_sblaster_irq";
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            update_dosbox_variable("sblaster", "irq", var.value);
        }

        var.key = "dosbox_sblaster_dma";
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            update_dosbox_variable("sblaster", "dma", var.value);
        }

        var.key = "dosbox_sblaster_hdma";
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            update_dosbox_variable("sblaster", "hdma", var.value);
        }

        var.key = "dosbox_sblaster_opl_mode";
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            update_dosbox_variable("sblaster", "oplmode", var.value);
        }

        var.key = "dosbox_sblaster_opl_emu";
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            update_dosbox_variable("sblaster", "oplemu", var.value);
        }

        var.key = "dosbox_tandy";
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            update_dosbox_variable("speaker", "tandy", var.value);
        }

        var.key = "dosbox_disney";
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            update_dosbox_variable("speaker", "disney", var.value);
        }
    }

    if (!handlers_added) {
        printf("[LIBRETRO] No mapper handlers defined, skipping registration\n");
        if (log_cb) log_cb(RETRO_LOG_WARN, "[LIBRETRO] No mapper handlers defined, skipping registration\n");
        handlers_added = true;
    }
    printf("[LIBRETRO] Exiting check_variables\n");
}

void leave_thread(Bitu /*unused*/) noexcept {
    MIXER_CallBack(nullptr, audioData.data(), samplesPerFrame * 4);
    co_switch(mainThread);
    PIC_AddEvent(leave_thread, 1000.0f / 60.0f, 0);
}

void start_dosbox() {
    printf("[LIBRETRO] Entering start_dosbox\n");
    if (log_cb) log_cb(RETRO_LOG_DEBUG, "[LIBRETRO] Entering start_dosbox\n");

    if (control) {
        printf("[LIBRETRO] Config already initialized, resetting\n");
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Config already initialized, resetting\n");
        delete control;
        control = nullptr;
    }

    std::array<const char*, 2> argv = {"dosbox", loadPath.empty() ? nullptr : loadPath.c_str()};
    CommandLine com_line(loadPath.empty() ? 1 : 2, argv.data());
    control = new Config(&com_line);
    if (!control) {
        printf("[LIBRETRO] Failed to create Config\n");
        if (log_cb) log_cb(RETRO_LOG_ERROR, "[LIBRETRO] Failed to create Config\n");
        return;
    }
    printf("[LIBRETRO] CommandLine initialized, argc=%d\n", com_line.GetCount());
    if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] CommandLine initialized, argc=%d\n", com_line.GetCount());

    if (!configPath.empty()) {
        printf("[LIBRETRO] Parsing config file: %s\n", configPath.c_str());
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Parsing config file: %s\n", configPath.c_str());
        control->ParseConfigFile(configPath.c_str());
    }

    check_variables();
    if (!is_restarting) {
        printf("[LIBRETRO] Initializing DOSBox subsystems\n");
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Initializing DOSBox subsystems\n");
        DOSBOX_Init();
        printf("[LIBRETRO] Initializing Config\n");
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Initializing Config\n");
        control->Init();
    }

    check_variables();
    co_switch(mainThread);
    PIC_AddEvent(leave_thread, 1000.0f / 60.0f, 0);

    try {
        printf("[LIBRETRO] Starting DOS shell\n");
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Starting DOS shell\n");
        control->StartUp();
    } catch (int) {
        printf("[LIBRETRO] Frontend asked to exit\n");
        if (log_cb) log_cb(RETRO_LOG_WARN, "[LIBRETRO] Frontend asked to exit\n");
        return;
    }
    printf("[LIBRETRO] DOSBox asked to exit\n");
    if (log_cb) log_cb(RETRO_LOG_WARN, "[LIBRETRO] DOSBox asked to exit\n");
    dosbox_exit = true;
    printf("[LIBRETRO] Exiting start_dosbox\n");
    if (log_cb) log_cb(RETRO_LOG_DEBUG, "[LIBRETRO] Exiting start_dosbox\n");
}

void wrap_dosbox() {
    printf("[LIBRETRO] Entering wrap_dosbox\n");
    if (log_cb) log_cb(RETRO_LOG_DEBUG, "[LIBRETRO] Entering wrap_dosbox\n");
    start_dosbox();
    printf("[LIBRETRO] Exiting wrap_dosbox\n");
    if (log_cb) log_cb(RETRO_LOG_DEBUG, "[LIBRETRO] Exiting wrap_dosbox\n");
}

void init_threads() noexcept {
    printf("[LIBRETRO] Entering init_threads\n");
    if (log_cb) log_cb(RETRO_LOG_DEBUG, "[LIBRETRO] Entering init_threads\n");
    if (!emuThread && !mainThread) {
        mainThread = co_active();
#ifdef __GENODE__
        emuThread = co_create((1 << 16) * sizeof(void*), wrap_dosbox);
#else
        emuThread = co_create(65536 * sizeof(void*) * 16, wrap_dosbox);
#endif
        if (!emuThread) {
            printf("[LIBRETRO] Failed to create emulator thread\n");
            if (log_cb) log_cb(RETRO_LOG_ERROR, "[LIBRETRO] Failed to create emulator thread\n");
        } else {
            printf("[LIBRETRO] Threads created: mainThread=%p, emuThread=%p\n", mainThread, emuThread);
            if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Threads created: mainThread=%p, emuThread=%p\n", mainThread, emuThread);
        }
    } else {
        printf("[LIBRETRO] Init called more than once\n");
        if (log_cb) log_cb(RETRO_LOG_WARN, "[LIBRETRO] Init called more than once\n");
    }
    printf("[LIBRETRO] Exiting init_threads\n");
    if (log_cb) log_cb(RETRO_LOG_DEBUG, "[LIBRETRO] Exiting init_threads\n");
}

void restart_program(std::vector<std::string>& /*parameters*/) {
    printf("[LIBRETRO] Program restart not supported\n");
    if (log_cb) log_cb(RETRO_LOG_WARN, "[LIBRETRO] Program restart not supported\n");
}

std::string normalize_path(std::string_view path) noexcept {
    std::string result{path};
    std::replace(result.begin(), result.end(), '/', PATH_SEPARATOR);
    std::replace(result.begin(), result.end(), '\\', PATH_SEPARATOR);
    return result;
}

unsigned retro_api_version() {
    return RETRO_API_VERSION;
}

void retro_set_environment(retro_environment_t cb) {
    printf("[LIBRETRO] Entering retro_set_environment\n");
    if (log_cb) log_cb(RETRO_LOG_DEBUG, "[LIBRETRO] Entering retro_set_environment\n");
    environ_cb = cb;

    bool allow_no_game = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &allow_no_game);
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable*>(vars));

    static const struct retro_controller_description ports_default[] = {
        {"Keyboard + Mouse", RETRO_DEVICE_KEYBOARD},
        {"Gamepad", RETRO_DEVICE_JOYPAD},
        {"Joystick", RETRO_DEVICE_JOYSTICK},
        {"Disconnected", RETRO_DEVICE_NONE},
        {nullptr, 0},
    };
    static const struct retro_controller_description ports_keyboard[] = {
        {"Keyboard + Mouse", RETRO_DEVICE_KEYBOARD},
        {"Disconnected", RETRO_DEVICE_NONE},
        {nullptr, 0},
    };
    static const struct retro_controller_info ports[] = {
        {ports_default, 4},
        {ports_default, 4},
        {ports_keyboard, 2},
        {ports_keyboard, 2},
        {ports_keyboard, 2},
        {ports_keyboard, 2},
        {nullptr, 0},
    };
    environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, const_cast<retro_controller_info*>(ports));

    const char* system_dir = nullptr;
    if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir) {
        retro_system_directory = system_dir;
        printf("[LIBRETRO] SYSTEM_DIRECTORY: %s\n", retro_system_directory.c_str());
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] SYSTEM_DIRECTORY: %s\n", retro_system_directory.c_str());
    }

    const char* save_dir = nullptr;
    if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir) {
        retro_save_directory = save_dir;
        printf("[LIBRETRO] SAVE_DIRECTORY: %s\n", retro_save_directory.c_str());
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] SAVE_DIRECTORY: %s\n", retro_save_directory.c_str());
    }

    const char* content_dir = nullptr;
    if (environ_cb(RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY, &content_dir) && content_dir) {
        retro_content_directory = content_dir;
        printf("[LIBRETRO] CONTENT_DIRECTORY: %s\n", retro_content_directory.c_str());
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] CONTENT_DIRECTORY: %s\n", retro_content_directory.c_str());
    }
    printf("[LIBRETRO] Exiting retro_set_environment\n");
    if (log_cb) log_cb(RETRO_LOG_DEBUG, "[LIBRETRO] Exiting retro_set_environment\n");
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
    printf("[LIBRETRO] Setting controller port %u to device %u\n", port, device);
    if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Setting controller port %u to device %u\n", port, device);
    connected[port] = false;
    gamepad[port] = false;
    switch (device) {
    case RETRO_DEVICE_JOYPAD:
        connected[port] = true;
        gamepad[port] = true;
        break;
    case RETRO_DEVICE_JOYSTICK:
        connected[port] = true;
        gamepad[port] = false;
        break;
    case RETRO_DEVICE_KEYBOARD:
    case RETRO_DEVICE_MOUSE:
    case RETRO_DEVICE_ANALOG:
        connected[port] = true;
        gamepad[port] = false;
        break;
    case RETRO_DEVICE_NONE:
        connected[port] = false;
        gamepad[port] = false;
        break;
    default:
        printf("[LIBRETRO] Unsupported device %u for port %u\n", device, port);
        if (log_cb) log_cb(RETRO_LOG_WARN, "[LIBRETRO] Unsupported device %u for port %u\n", device, port);
        connected[port] = false;
        gamepad[port] = false;
        break;
    }
    MAPPER_Init();
}

void retro_get_system_info(retro_system_info* info) {
    info->library_name = retro_library_name.c_str();
#if defined(GIT_VERSION)
    info->library_version = CORE_VERSION GIT_VERSION;
#else
    info->library_version = CORE_VERSION;
#endif
    info->valid_extensions = "exe|com|bat|conf";
    info->need_fullpath = true;
    info->block_extract = false;
}

void retro_get_system_av_info(retro_system_av_info* info) {
    info->geometry.base_width = 320;
    info->geometry.base_height = 200;
    info->geometry.max_width = 1024;
    info->geometry.max_height = 768;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps = 60.0;
    info->timing.sample_rate = static_cast<double>(MIXER_RETRO_GetFrequency());
}

void retro_init() {
    printf("[LIBRETRO] Entering retro_init\n");
    if (log_cb) log_cb(RETRO_LOG_DEBUG, "[LIBRETRO] Entering retro_init\n");

    struct retro_log_callback log;
    if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
        log_cb = log.log;
        printf("[LIBRETRO] Logger interface initialized\n");
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Logger interface initialized\n");
    } else {
        printf("[LIBRETRO] Logger interface unavailable\n");
        log_cb = nullptr;
    }

    static struct retro_midi_interface midi_interface;
    if (environ_cb(RETRO_ENVIRONMENT_GET_MIDI_INTERFACE, &midi_interface)) {
        printf("[LIBRETRO] MIDI interface initialized\n");
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] MIDI interface initialized\n");
    } else {
        printf("[LIBRETRO] MIDI interface unavailable\n");
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] MIDI interface unavailable\n");
    }

    RDOSGFXcolorMode = RETRO_PIXEL_FORMAT_XRGB8888;
    if (environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &RDOSGFXcolorMode)) {
        printf("[LIBRETRO] Pixel format set to XRGB8888\n");
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Pixel format set to XRGB8888\n");
    } else {
        printf("[LIBRETRO] Failed to set pixel format\n");
        if (log_cb) log_cb(RETRO_LOG_ERROR, "[LIBRETRO] Failed to set pixel format\n");
    }

    init_threads();
    printf("[LIBRETRO] Exiting retro_init\n");
    if (log_cb) log_cb(RETRO_LOG_DEBUG, "[LIBRETRO] Exiting retro_init\n");
}

void retro_deinit() {
    printf("[LIBRETRO] Entering retro_deinit\n");
    if (log_cb) log_cb(RETRO_LOG_DEBUG, "[LIBRETRO] Entering retro_deinit\n");
    frontend_exit = !dosbox_exit;

    if (control) {
        printf("[LIBRETRO] Cleaning up Config\n");
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Cleaning up Config\n");
        delete control;
        control = nullptr;
    }

    if (emuThread) {
        if (frontend_exit) {
            printf("[LIBRETRO] Frontend exit, switching to emulator thread\n");
            if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Frontend exit, switching to emulator thread\n");
            co_switch(emuThread);
        }
        printf("[LIBRETRO] Deleting emulator thread\n");
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Deleting emulator thread\n");
        co_delete(emuThread);
        emuThread = nullptr;
    }

    if (mainThread) {
        printf("[LIBRETRO] Deleting main thread\n");
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Deleting main thread\n");
        co_delete(mainThread);
        mainThread = nullptr;
    }
    printf("[LIBRETRO] Exiting retro_deinit\n");
    if (log_cb) log_cb(RETRO_LOG_DEBUG, "[LIBRETRO] Exiting retro_deinit\n");
}

bool retro_load_game(const retro_game_info* game) {
    printf("[LIBRETRO] Entering retro_load_game\n");
    if (log_cb) log_cb(RETRO_LOG_DEBUG, "[LIBRETRO] Entering retro_load_game\n");

    if (!emuThread) {
        printf("[LIBRETRO] Load game called without emulator thread\n");
        if (log_cb) log_cb(RETRO_LOG_ERROR, "[LIBRETRO] Load game called without emulator thread\n");
        return false;
    }

    const char slash = PATH_SEPARATOR;

    if (game && game->path) {
        loadPath = normalize_path(game->path);
        printf("[LIBRETRO] Game path: %s\n", loadPath.c_str());
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Game path: %s\n", loadPath.c_str());
        if (const size_t lastDot = loadPath.find_last_of('.'); lastDot != std::string::npos) {
            std::string extension = loadPath.substr(lastDot + 1);
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return std::tolower(c); });

            if (extension == "conf") {
                configPath = std::move(loadPath);
                loadPath.clear();
                printf("[LIBRETRO] Config file detected: %s\n", configPath.c_str());
                if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Config file detected: %s\n", configPath.c_str());
            }
        }
    } else {
        printf("[LIBRETRO] No game provided, using default config\n");
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] No game provided, using default config\n");
    }

    if (configPath.empty()) {
        configPath = normalize_path(retro_system_directory + slash + "DOSbox" + slash + "dosbox-libretro.conf");
        printf("[LIBRETRO] Loading default config: %s\n", configPath.c_str());
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Loading default config: %s\n", configPath.c_str());
    }

    check_variables();
    co_switch(emuThread);
    samplesPerFrame = MIXER_RETRO_GetFrequency() / 60;
    printf("[LIBRETRO] Game load completed, samplesPerFrame=%u\n", samplesPerFrame);
    if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Game load completed, samplesPerFrame=%u\n", samplesPerFrame);
    printf("[LIBRETRO] Exiting retro_load_game\n");
    return true;
}

bool retro_load_game_special(unsigned /*game_type*/, const retro_game_info* /*info*/, size_t /*num_info*/) {
    printf("[LIBRETRO] retro_load_game_special not supported\n");
    return false;
}

void retro_run() {
    printf("[LIBRETRO] Entering retro_run\n");
    if (log_cb) log_cb(RETRO_LOG_DEBUG, "[LIBRETRO] Entering retro_run\n");

    if (dosbox_exit && emuThread) {
        printf("[LIBRETRO] Shutting down DOSBox\n");
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Shutting down DOSBox\n");
        co_delete(emuThread);
        emuThread = nullptr;
        environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, nullptr);
        printf("[LIBRETRO] Exiting retro_run after shutdown\n");
        return;
    }

    if (RDOSGFXwidth != currentWidth || RDOSGFXheight != currentHeight) {
        printf("[LIBRETRO] Resolution changed %dx%d => %ldx%ld\n",
               currentWidth, currentHeight, RDOSGFXwidth, RDOSGFXheight);
        if (log_cb) {
            log_cb(RETRO_LOG_INFO, "[LIBRETRO] Resolution changed %dx%d => %ldx%ld\n",
                   currentWidth, currentHeight, RDOSGFXwidth, RDOSGFXheight);
        }
        retro_system_av_info new_av_info;
        retro_get_system_av_info(&new_av_info);
        new_av_info.geometry.base_width = RDOSGFXwidth;
        new_av_info.geometry.base_height = RDOSGFXheight;
        new_av_info.geometry.max_width = 1024;
        new_av_info.geometry.max_height = 768;
        new_av_info.geometry.aspect_ratio = 4.0f / 3.0f;
        environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &new_av_info);
        currentWidth = RDOSGFXwidth;
        currentHeight = RDOSGFXheight;
    }

    bool updated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        printf("[LIBRETRO] Core variables updated\n");
        if (log_cb) log_cb(RETRO_LOG_INFO, "[LIBRETRO] Core variables updated\n");
        check_variables();
    }

    if (emuThread) {
        MAPPER_Run(false);
        co_switch(emuThread);
        if (RDOSGFXhaveFrame) {
            printf("[LIBRETRO] Video callback: frame=%p, width=%lu, height=%lu, pitch=%lu\n",
                   RDOSGFXhaveFrame, RDOSGFXwidth, RDOSGFXheight, RDOSGFXpitch);
            if (log_cb) {
                log_cb(RETRO_LOG_DEBUG, "[LIBRETRO] Video callback: frame=%p, width=%lu, height=%lu, pitch=%lu\n",
                       RDOSGFXhaveFrame, RDOSGFXwidth, RDOSGFXheight, RDOSGFXpitch);
            }
            video_cb(RDOSGFXhaveFrame, RDOSGFXwidth, RDOSGFXheight, RDOSGFXpitch);
            RDOSGFXhaveFrame = nullptr;
        }
        printf("[LIBRETRO] Audio callback: samples=%u\n", samplesPerFrame);
        if (log_cb) log_cb(RETRO_LOG_DEBUG, "[LIBRETRO] Audio callback: samples=%u\n", samplesPerFrame);
        audio_batch_cb(reinterpret_cast<int16_t*>(audioData.data()), samplesPerFrame);
    } else {
        printf("[LIBRETRO] Run called without emulator thread\n");
        if (log_cb) log_cb(RETRO_LOG_WARN, "[LIBRETRO] Run called without emulator thread\n");
    }
    printf("[LIBRETRO] Exiting retro_run\n");
    if (log_cb) log_cb(RETRO_LOG_DEBUG, "[LIBRETRO] Exiting retro_run\n");
}

void retro_reset() {
    printf("[LIBRETRO] Resetting emulator\n");
    restart_program(control->startup_params);
}

void* retro_get_memory_data(unsigned type) {
    return type == RETRO_MEMORY_SYSTEM_RAM ? MemBase : nullptr;
}

size_t retro_get_memory_size(unsigned type) {
    return type == RETRO_MEMORY_SYSTEM_RAM ? g_memsize : 0;
}

size_t retro_serialize_size() { return 0; }
bool retro_serialize(void* /*data*/, size_t /*size*/) { return false; }
bool retro_unserialize(const void* /*data*/, size_t /*size*/) { return false; }
void retro_cheat_reset() {}
void retro_cheat_set(unsigned /*unused*/, bool /*unused1*/, const char* /*unused2*/) {}
void retro_unload_game() {}
unsigned retro_get_region() { return RETRO_REGION_NTSC; }

bool startup_state_capslock = false;
bool startup_state_numlock = false;

#if defined(__PS3__)
int gettimeofday(timeval* tv, void* /*tz*/) noexcept {
    int64_t time = sys_time_get_system_time();
    tv->tv_sec = time / 1000000;
    tv->tv_usec = time - (tv->tv_sec * 1000000);
    return 0;
}

int access(const char* fpath, int /*mode*/) noexcept {
    struct stat buffer;
    return stat(fpath, &buffer);
}
#endif