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

#if (C_DYNREC)

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>

#if defined (WIN32)
#include <windows.h>
#include <winbase.h>
#endif

#if (C_HAVE_MPROTECT)
#include <sys/mman.h>
#include <limits.h>
#ifndef PAGESIZE
#define PAGESIZE 4096
#endif
#endif /* C_HAVE_MPROTECT */

#include "callback.h"
#include "regs.h"
#include "mem.h"
#include "cpu.h"
#include "debug.h"
#include "paging.h"
#include "inout.h"
#include "lazyflags.h"
#include "pic.h"

// Optimized cache parameters
#define CACHE_MAXSIZE   (8192)          // Increased from 4096*2
#define CACHE_TOTAL     (1024*1024*16)  // Doubled to 16MB
#define CACHE_PAGES     (1024)          // Increased from 512
#define CACHE_BLOCKS    (256*1024)      // Doubled to 256K
#define CACHE_ALIGN     (64)            // Aligned to typical cache line size
#define DYN_HASH_SHIFT  (3)             // Reduced shift for larger hash table
#define DYN_PAGE_HASH   (4096>>DYN_HASH_SHIFT) // 512 entries
#define DYN_LINKS       (32)            // Increased to 32 for more aggressive linking

// Fast lookup table for recently linked blocks
#define LINK_CACHE_SIZE 64
static CacheBlockDynRec *link_cache[LINK_CACHE_SIZE];

//#define DYN_LOG 1 //Turn Logging on.

#if C_FPU
#define CPU_FPU 1
#endif

// Register definitions unchanged
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

enum BlockReturn {
    BR_Normal=0,
    BR_Cycles,
    BR_Link1, BR_Link2,
    BR_Opcode,
#if (C_DEBUG)
    BR_OpcodeFull,
#endif
    BR_Iret,
    BR_CallBack,
    BR_SMCBlock
};

#define SMC_CURRENT_BLOCK 0xffff

static void IllegalOptionDynrec(const char* msg) {
    E_Exit("DynrecCore: illegal option in %s", msg);
}

static struct {
    BlockReturn (*runcode)(Bit8u*);
    Bitu callback;
    Bitu readdata;
    Bit32u protected_regs[8];
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

// Optimized block linking with link cache
CacheBlockDynRec * LinkBlocks(BlockReturn ret) {
    CacheBlockDynRec * block = NULL;
    Bitu temp_ip = SegPhys(cs) + reg_eip;
    
    // Check link cache first
    Bitu link_hash = (temp_ip >> 3) & (LINK_CACHE_SIZE - 1);
    if (link_cache[link_hash] && link_cache[link_hash]->cache.start == temp_ip) {
        block = link_cache[link_hash];
        cache.block.running->LinkTo(ret == BR_Link2, block);
        return block;
    }

    CodePageHandlerDynRec * temp_handler = (CodePageHandlerDynRec *)get_tlb_readhandler(temp_ip);
    if (temp_handler->flags & PFLAG_HASCODE) {
        block = temp_handler->FindCacheBlock(temp_ip & 4095);
        if (block) {
            cache.block.running->LinkTo(ret == BR_Link2, block);
            link_cache[link_hash] = block; // Store in link cache
            __builtin_prefetch(block->cache.start); // Prefetch block
        }
    }
    return block;
}

Bits CPU_Core_Dynrec_Run(void) {
    // Simple TLB for page validation
    static PhysPt last_ip_page = 0;
    static CodePageHandlerDynRec *last_handler = NULL;

    for (;;) {
        PhysPt ip_point = SegPhys(cs) + reg_eip;
        #if C_HEAVY_DEBUG
            if (DEBUG_HeavyIsBreakpoint()) return debugCallback;
        #endif

        CodePageHandlerDynRec * chandler = NULL;
        // Fast path: reuse last handler if page hasn't changed
        if ((ip_point & ~(PAGESIZE - 1)) == last_ip_page && last_handler) {
            chandler = last_handler;
        } else {
            if (GCC_UNLIKELY(MakeCodePage(ip_point, chandler))) {
                CPU_Exception(cpu.exception.which, cpu.exception.error);
                continue;
            }
            last_ip_page = ip_point & ~(PAGESIZE - 1);
            last_handler = chandler;
        }

        if (GCC_UNLIKELY(!chandler)) return CPU_Core_Normal_Run();

        CacheBlockDynRec * block = chandler->FindCacheBlock(ip_point & 4095);
        if (!block) {
            // Optimized SMC check with bloom filter (assumed in cache.h)
            if (!chandler->invalidation_map || (chandler->invalidation_map[ip_point & 4095] < 2)) {
                block = CreateCacheBlock(chandler, ip_point, 64); // Increased to 64 instructions
            } else {
                Bitu old_cycles = CPU_Cycles;
                CPU_Cycles = 1;
                Bits nc_retcode = CPU_Core_Normal_Run();
                if (!nc_retcode) {
                    CPU_Cycles = old_cycles - 1;
                    continue;
                }
                CPU_CycleLeft += old_cycles;
                return nc_retcode;
            }
        }

run_block:
        cache.block.running = NULL;
        BlockReturn ret = core_dynrec.runcode(block->cache.start);

        switch (ret) {
        case BR_Iret:
            #if C_DEBUG
            #if C_HEAVY_DEBUG
                if (DEBUG_HeavyIsBreakpoint()) return debugCallback;
            #endif
            #endif
            if (!GETFLAG(TF)) {
                if (GETFLAG(IF) && PIC_IRQCheck) return CBRET_NONE;
                break;
            }
            cpudecoder = CPU_Core_Dynrec_Trap_Run;
            return CBRET_NONE;

        case BR_Normal:
            #if C_DEBUG
            #if C_HEAVY_DEBUG
                if (DEBUG_HeavyIsBreakpoint()) return debugCallback;
            #endif
            #endif
            break;

        case BR_Cycles:
            #if C_DEBUG
            #if C_HEAVY_DEBUG
                if (DEBUG_HeavyIsBreakpoint()) return debugCallback;
            #endif
            #endif
            return CBRET_NONE;

        case BR_CallBack:
            FillFlags();
            return core_dynrec.callback;

        case BR_SMCBlock:
            cpu.exception.which = 0;
            // Fallthrough
        case BR_Opcode:
            CPU_CycleLeft += CPU_Cycles;
            CPU_Cycles = 1;
            return CPU_Core_Normal_Run();

        #if (C_DEBUG)
        case BR_OpcodeFull:
            CPU_CycleLeft += CPU_Cycles;
            CPU_Cycles = 1;
            return CPU_Core_Full_Run();
        #endif

        case BR_Link1:
        case BR_Link2:
            block = LinkBlocks(ret);
            if (block) goto run_block;
            break;

        default:
            E_Exit("Invalid return code %d", ret);
        }
    }
    return CBRET_NONE;
}

Bits CPU_Core_Dynrec_Trap_Run(void) {
    Bits oldCycles = CPU_Cycles;
    CPU_Cycles = 1;
    cpu.trap_skip = false;

    Bits ret = CPU_Core_Normal_Run();
    if (!cpu.trap_skip) CPU_HW_Interrupt(1);

    CPU_Cycles = oldCycles - 1;
    cpudecoder = &CPU_Core_Dynrec_Run;
    return ret;
}

void CPU_Core_Dynrec_Init(void) {
    // Initialize link cache
    memset(link_cache, 0, sizeof(link_cache));
}

void CPU_Core_Dynrec_Cache_Init(bool enable_cache) {
    cache_init(enable_cache);
}

void CPU_Core_Dynrec_Cache_Close(void) {
    cache_close();
}

#endif