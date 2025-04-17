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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <algorithm>
#include <array>
#include <charconv>
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
#include "midi.h"
#include "mixer.h"
#include "control.h"
#include "pic.h"
#include "joystick.h"
#include "ints/int10.h"
#include "mem.h"
#include <cstdio> // Add for snprintf

#define RETRO_DEVICE_JOYSTICK RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1)

retro_log_printf_t log_cb = nullptr; // Definition of log_cb

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
MachineType machine = MCH_VGA;
SVGACards svgaCard = SVGA_None;

/* Input variables */
int current_port = 0;
bool autofire = false;
std::array<bool, kMaxPorts> gamepad{};
std::array<bool, kMaxPorts> connected{};
bool emulated_mouse = false;
unsigned deadzone = 0;

/* Core options */
bool use_core_options = true;
bool adv_core_options = false;

/* Directories */
std::string retro_save_directory;
std::string retro_system_directory;
std::string retro_content_directory;
std::string retro_library_name = "DOSBox";

/* Libretro variables */
retro_video_refresh_t video_cb = nullptr;
retro_audio_sample_batch_t audio_batch_cb = nullptr;
retro_input_poll_t poll_cb = nullptr;
retro_input_state_t input_cb = nullptr;
retro_environment_t environ_cb = nullptr;
//struct retro_midi_interface* retro_midi_interface = nullptr;

/* DOSBox state */
std::string loadPath;
std::string configPath;
bool dosbox_exit = false;
bool frontend_exit = false;
bool is_restarting = false;

/* Video variables */
extern Bit8u RDOSGFXbuffer[1024 * 768 * 4];
extern Bitu RDOSGFXwidth, RDOSGFXheight, RDOSGFXpitch;
extern unsigned RDOSGFXcolorMode;
extern void* RDOSGFXhaveFrame;
unsigned currentWidth = 0;
unsigned currentHeight = 0;

/* Audio variables */
alignas(16) std::array<uint8_t, 829 * 4> audioData{};
uint32_t samplesPerFrame = 735;

/* Callbacks */
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t /*cb*/) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }

/* Helper functions */
bool update_dosbox_variable(std::string_view section, std::string_view var, std::string_view val) noexcept {
    if (Section* section_ptr = control->GetSection(std::string{section}); section_ptr) {
        if (Section_prop* secprop = dynamic_cast<Section_prop*>(section_ptr)) {
            section_ptr->ExecuteDestroy(false);
            std::string inputline = std::string{var} + '=' + std::string{val};
            bool result = section_ptr->HandleInputline(inputline.c_str());
            section_ptr->ExecuteInit(false);
            return result;
        }
    }
    return false;
}

/* Libretro core implementation */
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
    {"dosbox_sblaster_opl_mode", "Sound Blaster OPL Mode; auto|cms|op12|dualop12|op13|op13gold|none"},
    {"dosbox_sblaster_opl_emu", "Sound Blaster OPL Provider; default|compat|fast|mame"},
    {"dosbox_pcspeaker", "Enable PC-Speaker; false|true"},
    {"dosbox_tandy", "Enable Tandy Sound System; auto|on|off"},
    {"dosbox_disney", "Enable Disney Sound Source; false|true"},
#if defined(C_IPX)
    {"dosbox_ipx", "Enable IPX over UDP; false|true"},
#endif
    {nullptr, nullptr},
};

void check_variables() noexcept {
    static unsigned cycles = 0, cycles_fine = 0;
    static unsigned cycles_multiplier = 0, cycles_multiplier_fine = 0;
    static bool update_cycles = false;

    retro_variable var{};

    var.key = "dosbox_use_options";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        use_core_options = std::string_view{var.value} == "true";
    }

    var.key = "dosbox_adv_options";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        bool new_adv = std::string_view{var.value} == "true";
        if (new_adv != adv_core_options) {
            adv_core_options = new_adv;
            environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable*>(adv_core_options ? vars_advanced : vars));
        }
    }

    if (!use_core_options) {
        return;
    }

    var.key = "dosbox_machine_type";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        std::string_view machine_type{var.value};
        if (machine_type == "hercules") {
            machine = MCH_HERC;
            svgaCard = SVGA_None;
        } else if (machine_type == "cga") {
            machine = MCH_CGA;
            svgaCard = SVGA_None;
        } else if (machine_type == "pcjr") {
            machine = MCH_PCJR;
            svgaCard = SVGA_None;
        } else if (machine_type == "tandy") {
            machine = MCH_TANDY;
            svgaCard = SVGA_None;
        } else if (machine_type == "ega") {
            machine = MCH_EGA;
            svgaCard = SVGA_None;
        } else if (machine_type == "svga_s3") {
            machine = MCH_VGA;
            svgaCard = SVGA_S3Trio;
        } else if (machine_type == "svga_et4000") {
            machine = MCH_VGA;
            svgaCard = SVGA_TsengET4K;
        } else if (machine_type == "svga_et3000") {
            machine = MCH_VGA;
            svgaCard = SVGA_TsengET3K;
        } else if (machine_type == "svga_paradise") {
            machine = MCH_VGA;
            svgaCard = SVGA_ParadisePVGA1A;
        } else if (machine_type == "vesa_nolfb") {
            machine = MCH_VGA;
            svgaCard = SVGA_S3Trio;
            int10.vesa_nolfb = true;
        } else if (machine_type == "vesa_oldvbe") {
            machine = MCH_VGA;
            svgaCard = SVGA_S3Trio;
            int10.vesa_oldvbe = true;
        } else {
            machine = MCH_VGA;
            svgaCard = SVGA_None;
        }
        update_dosbox_variable("dosbox", "machine", machine_type);
    }

    var.key = "dosbox_emulated_mouse";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        bool new_mouse = std::string_view{var.value} == "true";
        if (new_mouse != emulated_mouse) {
            emulated_mouse = new_mouse;
            MAPPER_Init();
        }
    }

    var.key = "dosbox_emulated_mouse_deadzone";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        unsigned new_deadzone = 0;
        std::from_chars(var.value, var.value + ::strlen(var.value), new_deadzone);
        if (new_deadzone != deadzone) {
            deadzone = new_deadzone;
            MAPPER_Init();
        }
    }

    var.key = "dosbox_cpu_cycles_mode";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        update_cycles = true;
    }

    var.key = "dosbox_cpu_cycles";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        std::from_chars(var.value, var.value + ::strlen(var.value), cycles);
        update_cycles = true;
    }

    var.key = "dosbox_cpu_cycles_multiplier";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        std::from_chars(var.value, var.value + ::strlen(var.value), cycles_multiplier);
        update_cycles = true;
    }

    var.key = "dosbox_cpu_cycles_fine";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        std::from_chars(var.value, var.value + ::strlen(var.value), cycles_fine);
        update_cycles = true;
    }

    var.key = "dosbox_cpu_cycles_multiplier_fine";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        std::from_chars(var.value, var.value + ::strlen(var.value), cycles_multiplier_fine);
        update_cycles = true;
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
        if (var.value && std::string_view{var.value} == "fixed") {
            char s[16];
            auto result = std::to_chars(s, s + sizeof(s), cycles * cycles_multiplier + cycles_fine * cycles_multiplier_fine);
            *result.ptr = '\0'; // Null-terminate
            update_dosbox_variable("cpu", "cycles", s);
        } else if (var.value) {
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
}

void leave_thread(Bitu /*unused*/) noexcept {
    MIXER_CallBack(nullptr, audioData.data(), samplesPerFrame * 4);
    co_switch(mainThread);
    PIC_AddEvent(leave_thread, 1000.0f / 60.0f, 0);
}

void start_dosbox() {
    fprintf(stderr, "[DOSBOX] Starting DOSBox, loadPath=%s, configPath=%s\n", loadPath.c_str(), configPath.c_str());

    std::array<const char*, 2> argv = {"dosbox", loadPath.empty() ? nullptr : loadPath.c_str()};
    CommandLine com_line(loadPath.empty() ? 1 : 2, argv.data());
    Config myconf(&com_line);
    control = &myconf;
    fprintf(stderr, "[DOSBOX] CommandLine initialized, argc=%d\n", com_line.GetCount());

    fprintf(stderr, "[DOSBOX] Checking core variables\n");
    check_variables();

    fprintf(stderr, "[DOSBOX] Initializing DOSBox subsystems\n");
    DOSBOX_Init();

    // Ensure [cpu] section exists and has default properties
    Section* cpu_section = control->AddSection_prop("cpu", nullptr);
    Section_prop* cpu_prop = static_cast<Section_prop*>(cpu_section);
    cpu_prop->Add_string("core", Property::Changeable::Always, "auto");
    cpu_prop->Add_string("cputype", Property::Changeable::Always, "auto");
    cpu_prop->Add_int("cycles", Property::Changeable::Always, 3000);
    cpu_prop->Add_int("cycleup", Property::Changeable::Always, 100);
    cpu_prop->Add_int("cycledown", Property::Changeable::Always, 100);

    if (!configPath.empty()) {
        fprintf(stderr, "[DOSBOX] Parsing config file: %s\n", configPath.c_str());
        control->ParseConfigFile(configPath.c_str());
    }

    if (!is_restarting) {
        fprintf(stderr, "[DOSBOX] Initializing Config\n");
        control->Init();
    }
    fprintf(stderr, "[DOSBOX] Re-checking core variables\n");
    check_variables();

    fprintf(stderr, "[DOSBOX] Switching to main thread\n");
    co_switch(mainThread);
    fprintf(stderr, "[DOSBOX] Scheduling frontend interrupt\n");
    PIC_AddEvent(leave_thread, 1000.0f / 60.0f, 0);

    fprintf(stderr, "[DOSBOX] Starting DOSBox main loop\n");
    try {
        control->StartUp();
        fprintf(stderr, "[DOSBOX] StartUp completed\n");
    } catch (int) {
        fprintf(stderr, "[DOSBOX] Frontend requested exit during StartUp\n");
        return;
    }

    fprintf(stderr, "[DOSBOX] DOSBox requested exit\n");
    dosbox_exit = true;
}

void wrap_dosbox() {
    start_dosbox();
    co_switch(mainThread);

    while (true) {
        if (log_cb) {
            log_cb(RETRO_LOG_ERROR, "Running a dead DOSBox instance\n");
        }
        co_switch(mainThread);
    }
}

void init_threads() noexcept {
    if (!emuThread && !mainThread) {
        fprintf(stderr, "[THREAD] Creating main and emulator threads\n");
        mainThread = co_active();
#ifdef __GENODE__
        emuThread = co_create((1 << 16) * sizeof(void*), wrap_dosbox);
#else
        emuThread = co_create(65536 * sizeof(void*) * 16, wrap_dosbox);
#endif
        if (!emuThread) {
            fprintf(stderr, "[THREAD] Failed to create emulator thread\n");
        } else {
            fprintf(stderr, "[THREAD] Emulator thread created successfully\n");
        }
    } else {
        fprintf(stderr, "[THREAD] Init called more than once\n");
    }
}

void restart_program(std::vector<std::string>& /*parameters*/) {
    if (log_cb) {
        log_cb(RETRO_LOG_WARN, "Program restart not supported\n");
    }
    return;

    if (emuThread) {
        if (frontend_exit) {
            co_switch(emuThread);
        }
        co_delete(emuThread);
        emuThread = nullptr;
    }

    co_delete(mainThread);
    mainThread = nullptr;

    is_restarting = true;
    init_threads();
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
        if (log_cb) {
            log_cb(RETRO_LOG_INFO, "SYSTEM_DIRECTORY: %s\n", retro_system_directory.c_str());
        }
    }

    const char* save_dir = nullptr;
    if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir) {
        retro_save_directory = save_dir;
        if (log_cb) {
            log_cb(RETRO_LOG_INFO, "SAVE_DIRECTORY: %s\n", retro_save_directory.c_str());
        }
    }

    const char* content_dir = nullptr;
    if (environ_cb(RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY, &content_dir) && content_dir) {
        retro_content_directory = content_dir;
        if (log_cb) {
            log_cb(RETRO_LOG_INFO, "CONTENT_DIRECTORY: %s\n", retro_content_directory.c_str());
        }
    }
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
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
    default:
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
    struct retro_log_callback log;
    if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
        log_cb = log.log;
        fprintf(stderr, "[INIT] Logger interface initialized\n");
    } else {
        log_cb = nullptr;
        fprintf(stderr, "[INIT] Logger interface failed to initialize\n");
    }

    static struct retro_midi_interface midi_interface;
    if (environ_cb(RETRO_ENVIRONMENT_GET_MIDI_INTERFACE, &midi_interface)) {
        retro_midi_interface = &midi_interface;
        fprintf(stderr, "[INIT] MIDI interface initialized\n");
    } else {
        retro_midi_interface = nullptr;
        fprintf(stderr, "[INIT] MIDI interface unavailable\n");
    }

    RDOSGFXcolorMode = RETRO_PIXEL_FORMAT_XRGB8888;
    if (environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &RDOSGFXcolorMode)) {
        fprintf(stderr, "[INIT] Pixel format set to XRGB8888\n");
    } else {
        fprintf(stderr, "[INIT] Failed to set pixel format\n");
    }

    fprintf(stderr, "[INIT] Starting thread initialization\n");
    init_threads();
    fprintf(stderr, "[INIT] Thread initialization completed\n");
}

void retro_deinit() {
    fprintf(stderr, "[DEINIT] Entering retro_deinit, frontend_exit=%d, dosbox_exit=%d\n", frontend_exit, dosbox_exit);

    frontend_exit = !dosbox_exit;

    if (emuThread) {
        if (frontend_exit) {
            fprintf(stderr, "[DEINIT] Frontend exit, switching to emulator thread\n");
            co_switch(emuThread);
        }
        fprintf(stderr, "[DEINIT] Deleting emulator thread\n");
        co_delete(emuThread);
        emuThread = nullptr;
    }
    fprintf(stderr, "[DEINIT] Deinitialization complete\n");
}

bool retro_load_game(const retro_game_info* game) {
    if (!emuThread) {
        fprintf(stderr, "[LOAD] Load game called without emulator thread\n");
        return false;
    }

    const char slash = PATH_SEPARATOR;
    fprintf(stderr, "[LOAD] Starting game load, game=%p\n", game);

    if (game) {
        loadPath = normalize_path(game->path);
        fprintf(stderr, "[LOAD] Game path: %s\n", loadPath.c_str());
        if (const size_t lastDot = loadPath.find_last_of('.'); lastDot != std::string::npos) {
            std::string extension = loadPath.substr(lastDot + 1);
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return std::tolower(c); });

            if (extension == "conf") {
                configPath = std::move(loadPath);
                loadPath.clear();
                fprintf(stderr, "[LOAD] Config file detected: %s\n", configPath.c_str());
            } else if (configPath.empty()) {
                configPath = normalize_path(retro_system_directory + slash + "DOSbox" + slash + "dosbox-libretro.conf");
                fprintf(stderr, "[LOAD] Loading default config: %s\n", configPath.c_str());
            }
        }
    } else {
        fprintf(stderr, "[LOAD] No game provided, using default config\n");
    }

    fprintf(stderr, "[LOAD] Switching to emulator thread\n");
    co_switch(emuThread);
    samplesPerFrame = MIXER_RETRO_GetFrequency() / 60;
    fprintf(stderr, "[LOAD] Game load completed, samplesPerFrame=%u\n", samplesPerFrame);
    return true;
}

bool retro_load_game_special(unsigned /*game_type*/, const retro_game_info* /*info*/, size_t /*num_info*/) {
    return false;
}

void retro_run() {
    fprintf(stderr, "[RUN] Entering retro_run, dosbox_exit=%d, emuThread=%p\n", dosbox_exit, emuThread);

    if (dosbox_exit && emuThread) {
        fprintf(stderr, "[RUN] DOSBox exited, shutting down core\n");
        co_delete(emuThread);
        emuThread = nullptr;
        environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, nullptr);
        return;
    }

    if (RDOSGFXwidth != currentWidth || RDOSGFXheight != currentHeight) {
        if (log_cb) {
            log_cb(RETRO_LOG_INFO, "Resolution changed %dx%d => %dx%d\n", currentWidth, currentHeight, RDOSGFXwidth, RDOSGFXheight);
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
        check_variables();
    }

    if (emuThread) {
        MAPPER_Run(false);
        co_switch(emuThread);
        video_cb(RDOSGFXhaveFrame, RDOSGFXwidth, RDOSGFXheight, RDOSGFXpitch);
        RDOSGFXhaveFrame = nullptr;
        audio_batch_cb(reinterpret_cast<int16_t*>(audioData.data()), samplesPerFrame);
    } else if (log_cb) {
        log_cb(RETRO_LOG_WARN, "Run called without emulator thread\n");
    }

    if (retro_midi_interface && retro_midi_interface->output_enabled()) {
        retro_midi_interface->flush();
    }
}

void retro_reset() {
    restart_program(control->startup_params);
}

/* Stubs */
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