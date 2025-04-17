/*
 *  Copyright (C) 2002-2025  The DOSBox Team
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

#if (C_DYNREC)

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cstdlib>
#include <array>
#include <algorithm>

#if defined(WIN32)
#include <windows.h>
#endif

#if (C_HAVE_MPROTECT)
#include <sys/mman.h>
#include <limits.h>
#ifndef PAGESIZE
#define PAGESIZE 4096
#endif
#endif

#include "callback.h"
#include "regs.h"
#include "mem.h"
#include "cpu.h"
#include "debug.h"
#include "paging.h"
#include "inout.h"
#include "lazyflags.h"
#include "pic.h"

// Cache parameters optimized for balance between speed and timing accuracy
#define CACHE_MAXSIZE   (8192)          // Max block size for efficient execution
#define CACHE_TOTAL     (1024*1024*12)  // 12MB total cache, balanced for modern systems
#define CACHE_PAGES     (768)           // Reduced from 1024 for interrupt checks
#define CACHE_BLOCKS    (192*1024)      // Compromise between 128K and 256K
#define CACHE_ALIGN     (64)            // Cache line alignment for performance
#define DYN_HASH_SHIFT  (3)             // 512 hash entries for fast lookups
#define DYN_PAGE_HASH   (4096>>DYN_HASH_SHIFT)
#define DYN_LINKS       (24)            // Balanced linking for speed and interrupts

// Link cache for fast block transitions
#define LINK_CACHE_SIZE 48              // Reduced for interrupt frequency
static std::array<CacheBlockDynRec*, LINK_CACHE_SIZE> link_cache{};

#if C_FPU
#define CPU_FPU 1
#endif

// Register definitions
#define DRC_REG_EAX 0
#define DRC_REG_ECX 1
#define DRC_REG_EDX 2
#define DRC_REG_EBX 3
#define DRC_REG_ESP 4
#define DRC_REG_EBP 5
#define DRC_REG_ESI 6
#define DRC_REG_EDI 7

#define DRC_SEG_ES 0
#define DRC_SEG_CS 1
#define DRC_SEG_SS 2
#define DRC_SEG_DS 3
#define DRC_SEG_FS 4
#define DRC_SEG_GS 5

#define DRCD_REG_VAL(reg) (&cpu_regs.regs[reg].dword)
#define DRCD_SEG_VAL(seg) (&Segs.val[seg])
#define DRCD_SEG_PHYS(seg) (&Segs.phys[seg])
#define DRCD_REG_BYTE(reg,idx) (&cpu_regs.regs[reg].byte[idx?BH_INDEX:BL_INDEX])
#define DRCD_REG_WORD(reg,dwrd) ((dwrd)?((void*)(&cpu_regs.regs[reg].dword[DW_INDEX])):((void*)(&cpu_regs.regs[reg].word[W_INDEX])))

enum class BlockReturn {
    Normal = 0,
    Cycles,
    Link1,
    Link2,
    Opcode,
#if (C_DEBUG)
    OpcodeFull,
#endif
    Iret,
    CallBack,
    SMCBlock
};

#define SMC_CURRENT_BLOCK 0xffff

[[noreturn]] static void IllegalOptionDynrec(const char* msg) {
    E_Exit("DynrecCore: illegal option in %s", msg);
}

static struct CoreDynrec {
    BlockReturn (*runcode)(Bit8u*);
    Bitu callback;
    Bitu readdata;
    std::array<Bit32u, 8> protected_regs{};
} core_dynrec;

#include "core_dynrec/cache.h"

#define X86         0x01
#define X86_64      0x02
#define MIPSEL      0x03
#define ARMV4LE     0x04
#define ARMV7LE     0x05
#define POWERPC     0x04

#if C_TARGETCPU == X86_64
#include "core_dynrec/risc_x64.h"
#elif C_TARGETCPU == X86
#include "core_dynrec/risc_x86.h"
#elif C_TARGETCPU == MIPSEL
#include "core_dynrec/risc_mipsel32.h"
#elif (C_TARGETCPU == ARMV4LE) || (C_TARGETCPU == ARMV7LE)
#include "core_dynrec/risc_armv4le.h"
#elif C_TARGETCPU == POWERPC
#include "core_dynrec/risc_ppc.h"
#endif

#include "core_dynrec/decoder.h"

// Block linking with timing-aware cache
static CacheBlockDynRec* LinkBlocks(BlockReturn ret) {
    CacheBlockDynRec* block = nullptr;
    const Bitu temp_ip = SegPhys(cs) + reg_eip;
    const Bitu link_hash = (temp_ip >> 3) & (LINK_CACHE_SIZE - 1);

    fprintf(stderr, "[DYNREC] LinkBlocks: ret=%d, temp_ip=0x%lx, link_hash=0x%lx\n", 
            static_cast<int>(ret), temp_ip, link_hash);

    // Check link cache
    if (link_cache[link_hash] && link_cache[link_hash]->cache.start == temp_ip) {
        block = link_cache[link_hash];
        fprintf(stderr, "[DYNREC] LinkBlocks: Found cached block at 0x%lx, linking to it\n", 
                temp_ip);
        cache.block.running->LinkTo(ret == BlockReturn::Link2, block);
        return block;
    }

    auto* temp_handler = static_cast<CodePageHandlerDynRec*>(get_tlb_readhandler(temp_ip));
    if (temp_handler->flags & PFLAG_HASCODE) {
        block = temp_handler->FindCacheBlock(temp_ip & 4095);
        if (block) {
            fprintf(stderr, "[DYNREC] LinkBlocks: Found block at 0x%lx in handler, linking\n", 
                    temp_ip);
            cache.block.running->LinkTo(ret == BlockReturn::Link2, block);
            link_cache[link_hash] = block;
            __builtin_prefetch(block->cache.start);
        } else {
            fprintf(stderr, "[DYNREC] LinkBlocks: No block found at 0x%lx in handler\n", 
                    temp_ip);
        }
    } else {
        fprintf(stderr, "[DYNREC] LinkBlocks: Handler at 0x%lx has no code\n", temp_ip);
    }
    return block;
}

Bits CPU_Core_Dynrec_Run() {
    // TLB with timing checks
    static PhysPt last_ip_page = 0;
    static CodePageHandlerDynRec* last_handler = nullptr;
    static Bitu pit_check_counter = 0;

    fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Starting, cs=0x%lx, eip=0x%lx\n", 
            SegValue(cs), reg_eip);

    for (;;) {
        const PhysPt ip_point = SegPhys(cs) + reg_eip;
        fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: ip_point=0x%lx\n", ip_point);

#if C_HEAVY_DEBUG
        if (DEBUG_HeavyIsBreakpoint()) {
            fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Breakpoint hit, returning debugCallback\n");
            return debugCallback;
        }
#endif

        CodePageHandlerDynRec* chandler = nullptr;
        if ((ip_point & ~(PAGESIZE - 1)) == last_ip_page && last_handler) {
            chandler = last_handler;
            fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Using cached handler for page 0x%lx\n", 
                    last_ip_page);
        } else {
            fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Looking up handler for ip_point=0x%lx\n", 
                    ip_point);
            if (GCC_UNLIKELY(MakeCodePage(ip_point, chandler))) {
                fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: MakeCodePage failed, raising exception %ld\n", 
                        cpu.exception.which);
                CPU_Exception(cpu.exception.which, cpu.exception.error);
                continue;
            }
            last_ip_page = ip_point & ~(PAGESIZE - 1);
            last_handler = chandler;
            fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Updated last_ip_page=0x%lx, handler=%p\n", 
                    last_ip_page, chandler);
        }

        if (GCC_UNLIKELY(!chandler)) {
            fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: No handler, falling back to normal core\n");
            return CPU_Core_Normal_Run();
        }

        CacheBlockDynRec* block = chandler->FindCacheBlock(ip_point & 4095);
        if (!block) {
            fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: No block at 0x%lx, creating new\n", 
                    ip_point);
            if (!chandler->invalidation_map || chandler->invalidation_map[ip_point & 4095] < 2) {
                block = CreateCacheBlock(chandler, ip_point, 48); // Balanced block size
                fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Created block at 0x%lx\n", ip_point);
            } else {
                const Bitu old_cycles = CPU_Cycles;
                CPU_Cycles = 1;
                fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Invalidation map hit, running normal core\n");
                const Bits nc_retcode = CPU_Core_Normal_Run();
                if (!nc_retcode) {
                    CPU_Cycles = old_cycles - 1;
                    fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Normal core returned 0, continuing\n");
                    continue;
                }
                CPU_CycleLeft += old_cycles;
                fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Normal core returned %ld\n", nc_retcode);
                return nc_retcode;
            }
        }

run_block:
        cache.block.running = block;
        fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Running block at 0x%lx, start=%p\n", 
                ip_point, block->cache.start);
        const BlockReturn ret = core_dynrec.runcode(block->cache.start);
        fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Block returned %d\n", static_cast<int>(ret));

        // Periodic PIT check to ensure timing-critical devices (e.g., PCSpeaker)
        if (++pit_check_counter >= 16) {
            if (PIC_IRQCheck & 0x1) {
                fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: PIT IRQ pending, returning CBRET_NONE\n");
                return CBRET_NONE; // Force PIT IRQ
            }
            pit_check_counter = 0;
            fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: PIT check, no IRQ\n");
        }

        switch (ret) {
        case BlockReturn::Iret:
#if C_DEBUG
#if C_HEAVY_DEBUG
            if (DEBUG_HeavyIsBreakpoint()) {
                fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: IRET with breakpoint, returning debugCallback\n");
                return debugCallback;
            }
#endif
#endif
            if (!GETFLAG(TF)) {
                if (GETFLAG(IF) && PIC_IRQCheck) {
                    fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: IRET with IRQ pending, returning CBRET_NONE\n");
                    return CBRET_NONE;
                }
                fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: IRET, continuing\n");
                break;
            }
            cpudecoder = CPU_Core_Dynrec_Trap_Run;
            fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: IRET with TF, switching to trap run\n");
            return CBRET_NONE;

        case BlockReturn::Normal:
#if C_DEBUG
#if C_HEAVY_DEBUG
            if (DEBUG_HeavyIsBreakpoint()) {
                fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Normal with breakpoint, returning debugCallback\n");
                return debugCallback;
            }
#endif
#endif
            fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Normal return, continuing\n");
            break;

        case BlockReturn::Cycles:
#if C_DEBUG
#if C_HEAVY_DEBUG
            if (DEBUG_HeavyIsBreakpoint()) {
                fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Cycles with breakpoint, returning debugCallback\n");
                return debugCallback;
            }
#endif
#endif
            fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Cycles exhausted, returning CBRET_NONE\n");
            return CBRET_NONE;

        case BlockReturn::CallBack:
            FillFlags();
            fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Callback, returning 0x%lx\n", 
                    core_dynrec.callback);
            return core_dynrec.callback;

        case BlockReturn::SMCBlock:
            cpu.exception.which = 0;
            fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: SMCBlock, clearing exception\n");
            [[fallthrough]];
        case BlockReturn::Opcode:
            CPU_CycleLeft += CPU_Cycles;
            CPU_Cycles = 1;
            fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Opcode/SMC, running normal core\n");
            return CPU_Core_Normal_Run();

#if C_DEBUG
        case BlockReturn::OpcodeFull:
            CPU_CycleLeft += CPU_Cycles;
            CPU_Cycles = 1;
            fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: OpcodeFull, running full core\n");
            return CPU_Core_Full_Run();
#endif

        case BlockReturn::Link1:
        case BlockReturn::Link2:
            fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Linking block, ret=%d\n", 
                    static_cast<int>(ret));
            block = LinkBlocks(ret);
            if (block) {
                fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Linked to block at 0x%lx\n", 
                        SegPhys(cs) + reg_eip);
                goto run_block;
            }
            fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: No block to link, continuing\n");
            break;

        default:
            fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Invalid return code %d\n", 
                    static_cast<int>(ret));
            E_Exit("Invalid return code %d", static_cast<int>(ret));
        }
    }
    fprintf(stderr, "[DYNREC] CPU_Core_Dynrec_Run: Exiting with CBRET_NONE\n");
    return CBRET_NONE;
}

Bits CPU_Core_Dynrec_Trap_Run() {
    const Bits oldCycles = CPU_Cycles;
    CPU_Cycles = 1;
    cpu.trap_skip = false;

    const Bits ret = CPU_Core_Normal_Run();
    if (!cpu.trap_skip) CPU_HW_Interrupt(1);

    CPU_Cycles = oldCycles - 1;
    cpudecoder = &CPU_Core_Dynrec_Run;
    return ret;
}

void CPU_Core_Dynrec_Init() {
    link_cache.fill(nullptr);
}

void CPU_Core_Dynrec_Cache_Init(bool enable_cache) {
    cache_init(enable_cache);
}

void CPU_Core_Dynrec_Cache_Close() {
    cache_close();
}

#endif