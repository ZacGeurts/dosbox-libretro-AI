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

#include <assert.h>
#include <sstream>
#include <stddef.h>
#include <string>
#include <memory> // C++17 for unique_ptr
#include <cstdio> // For fprintf
#include "dosbox.h"
#include "cpu.h"
#include "debug.h"
#include "mapper.h"
#include "setup.h"
#include "programs.h"
#include "paging.h"
#include "lazyflags.h"
#include "support.h"

Bitu DEBUG_EnableDebugger(void);
extern void GFX_SetTitle(Bit32s cycles, Bits frameskip, bool paused);

#if 1
#undef LOG
#if defined(_MSC_VER)
#define LOG(X,Y)
#else
#define LOG(X,Y) CPU_LOG
#define CPU_LOG(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#endif
#endif

CPU_Regs cpu_regs;
CPUBlock cpu;
Segments Segs;

Bit32s CPU_Cycles = 0;
Bit32s CPU_CycleLeft = 3000;
Bit32s CPU_CycleMax = 3000;
Bit32s CPU_OldCycleMax = 3000;
Bit32s CPU_CyclePercUsed = 100;
Bit32s CPU_CycleLimit = -1;
Bit32s CPU_CycleUp = 0;
Bit32s CPU_CycleDown = 0;
Bit64s CPU_IODelayRemoved = 0;
CPU_Decoder* cpudecoder;
bool CPU_CycleAutoAdjust = false;
bool CPU_SkipCycleAutoAdjust = false;
Bitu CPU_AutoDetermineMode = 0;

Bitu CPU_ArchitectureType = CPU_ARCHTYPE_MIXED;

Bitu CPU_extflags_toggle = 0; // ID and AC flags may be toggled depending on emulated CPU architecture

Bitu CPU_PrefetchQueueSize = 0;

void CPU_Core_Full_Init(void);
void CPU_Core_Normal_Init(void);
void CPU_Core_Simple_Init(void);
#if (C_DYNAMIC_X86)
void CPU_Core_Dyn_X86_Init(void);
void CPU_Core_Dyn_X86_Cache_Init(bool enable_cache);
void CPU_Core_Dyn_X86_Cache_Close(void);
void CPU_Core_Dyn_X86_SetFPUMode(bool dh_fpu);
#elif (C_DYNREC)
void CPU_Core_Dynrec_Init(void);
void CPU_Core_Dynrec_Cache_Init(bool enable_cache);
void CPU_Core_Dynrec_Cache_Close(void);
#endif

/* In debug mode exceptions are tested and dosbox exits when 
 * a unhandled exception state is detected. 
 * USE CHECK_EXCEPT to raise an exception in that case to see if that exception
 * solves the problem.
 * 
 * In non-debug mode dosbox doesn't do detection (and hence doesn't crash at
 * that point). (game might crash later due to the unhandled exception) */

#if C_DEBUG
// #define CPU_CHECK_EXCEPT 1
// #define CPU_CHECK_IGNORE 1
 /* Use CHECK_EXCEPT when something doesn't work to see if a exception is 
 * needed that isn't enabled by default.*/
#else
/* NORMAL NO CHECKING => More Speed */
#define CPU_CHECK_IGNORE 1
#endif /* C_DEBUG */

#if defined(CPU_CHECK_IGNORE)
#define CPU_CHECK_COND(cond,msg,exc,sel) {	\
    if (cond) do {} while (0);				\
}
#elif defined(CPU_CHECK_EXCEPT)
#define CPU_CHECK_COND(cond,msg,exc,sel) {	\
    if (cond) {					\
        CPU_Exception(exc,sel);		\
        return;				\
    }					\
}
#else
#define CPU_CHECK_COND(cond,msg,exc,sel) {	\
    if (cond) E_Exit(msg);			\
}
#endif

void Descriptor::Load(PhysPt address) {
    CPU_LOG("Descriptor::Load: Loading from address 0x%zx", static_cast<uintptr_t>(address));
    cpu.mpl = 0;
    Bit32u* data = (Bit32u*)&saved;
    *data = mem_readd(address);
    *(data + 1) = mem_readd(address + 4);
    cpu.mpl = 3;
    CPU_LOG("Descriptor::Load: Loaded descriptor, base=0x%zx, limit=0x%zx", static_cast<uintptr_t>(GetBase()), static_cast<uintptr_t>(GetLimit()));
}

void Descriptor::Save(PhysPt address) {
    CPU_LOG("Descriptor::Save: Saving to address 0x%zx", static_cast<uintptr_t>(address));
    cpu.mpl = 0;
    Bit32u* data = (Bit32u*)&saved;
    mem_writed(address, *data);
    mem_writed(address + 4, *(data + 1));
    cpu.mpl = 3;
    CPU_LOG("Descriptor::Save: Saved descriptor, base=0x%zx, limit=0x%zx", static_cast<uintptr_t>(GetBase()), static_cast<uintptr_t>(GetLimit()));
}

bool CPU_PopSeg(SegNames seg, bool use32) {
    CPU_LOG("CPU_PopSeg: seg=%d, use32=%d", static_cast<int>(seg), use32);
    
    Bitu value;
    if (use32) {
        value = CPU_Pop32();
    } else {
        value = CPU_Pop16();
    }
    
    if (!cpu.pmode || (reg_flags & FLAG_VM)) {
        Segs.val[seg] = value;
        Segs.phys[seg] = value << 4;
        if (seg == cs) cpu.code.big = false;
        CPU_LOG("CPU_PopSeg: Real/VM mode, set seg=%d to 0x%lx, phys=0x%zx", 
                static_cast<int>(seg), value, static_cast<uintptr_t>(Segs.phys[seg]));
        return false;
    }
    
    if ((value & 0xfffc) == 0) {
        if (seg == ss) {
            CPU_LOG("CPU_PopSeg: Null SS selector, raising #GP(0)");
            return CPU_PrepareException(EXCEPTION_GP, 0);
        }
        Segs.val[seg] = 0;
        Segs.phys[seg] = 0;
        CPU_LOG("CPU_PopSeg: Null selector for seg=%d", static_cast<int>(seg));
        return false;
    }
    
    Descriptor desc;
    if (!cpu.gdt.GetDescriptor(value, desc)) {
        CPU_LOG("CPU_PopSeg: Selector 0x%lx beyond limits, raising #GP(0x%lx)", 
                value, value & 0xfffc);
        return CPU_PrepareException(EXCEPTION_GP, value & 0xfffc);
    }
    
    if (seg == ss) {
        if (((value & 3) != cpu.cpl) || (desc.DPL() != cpu.cpl)) {
            CPU_LOG("CPU_PopSeg: SS RPL or DPL != CPL, raising #GP(0x%lx)", value & 0xfffc);
            return CPU_PrepareException(EXCEPTION_GP, value & 0xfffc);
        }
        switch (desc.Type()) {
            case DESC_DATA_EU_RW_NA: case DESC_DATA_EU_RW_A:
            case DESC_DATA_ED_RW_NA: case DESC_DATA_ED_RW_A:
                break;
            default:
                CPU_LOG("CPU_PopSeg: SS not writable data segment, raising #GP(0x%lx)", 
                        value & 0xfffc);
                return CPU_PrepareException(EXCEPTION_GP, value & 0xfffc);
        }
        if (!desc.saved.seg.p) {
            CPU_LOG("CPU_PopSeg: SS not present, raising #SS(0x%lx)", value & 0xfffc);
            return CPU_PrepareException(EXCEPTION_SS, value & 0xfffc);
        }
        Segs.val[seg] = value;
        Segs.phys[seg] = desc.GetBase();
        if (desc.Big()) {
            cpu.stack.big = true;
            cpu.stack.mask = 0xffffffff;
            cpu.stack.notmask = 0;
        } else {
            cpu.stack.big = false;
            cpu.stack.mask = 0xffff;
            cpu.stack.notmask = 0xffff0000;
        }
        CPU_LOG("CPU_PopSeg: Set SS=0x%lx, base=0x%zx, big=%d", 
                value, static_cast<uintptr_t>(Segs.phys[seg]), cpu.stack.big);
        return false;
    }
    
    switch (desc.Type()) {
        case DESC_CODE_N_NC_A: case DESC_CODE_N_NC_NA:
        case DESC_CODE_R_NC_A: case DESC_CODE_R_NC_NA:
            if (((value & 3) != cpu.cpl) || (desc.DPL() != cpu.cpl)) {
                CPU_LOG("CPU_PopSeg: Code NC RPL or DPL != CPL, raising #GP(0x%lx)", 
                        value & 0xfffc);
                return CPU_PrepareException(EXCEPTION_GP, value & 0xfffc);
            }
            break;
        case DESC_CODE_N_C_A: case DESC_CODE_N_C_NA:
        case DESC_CODE_R_C_A: case DESC_CODE_R_C_NA:
            if (desc.DPL() > cpu.cpl) {
                CPU_LOG("CPU_PopSeg: Code C DPL > CPL, raising #GP(0x%lx)", value & 0xfffc);
                return CPU_PrepareException(EXCEPTION_GP, value & 0xfffc);
            }
            break;
        case DESC_DATA_EU_RO_NA: case DESC_DATA_EU_RO_A:
        case DESC_DATA_EU_RW_NA: case DESC_DATA_EU_RW_A:
        case DESC_DATA_ED_RO_NA: case DESC_DATA_ED_RO_A:
        case DESC_DATA_ED_RW_NA: case DESC_DATA_ED_RW_A:
            if (((value & 3) < cpu.cpl) || (desc.DPL() < cpu.cpl)) {
                CPU_LOG("CPU_PopSeg: Data RPL or DPL < CPL, raising #GP(0x%lx)", 
                        value & 0xfffc);
                return CPU_PrepareException(EXCEPTION_GP, value & 0xfffc);
            }
            break;
        default:
            CPU_LOG("CPU_PopSeg: Invalid descriptor type %ld, raising #GP(0x%lx)", 
                    desc.Type(), value & 0xfffc);
            return CPU_PrepareException(EXCEPTION_GP, value & 0xfffc);
    }
    
    if (!desc.saved.seg.p) {
        CPU_LOG("CPU_PopSeg: Segment not present, raising #NP(0x%lx)", value & 0xfffc);
        return CPU_PrepareException(EXCEPTION_NP, value & 0xfffc);
    }
    
    Segs.val[seg] = value;
    Segs.phys[seg] = desc.GetBase();
    CPU_LOG("CPU_PopSeg: Set seg=%d to 0x%lx, base=0x%zx", 
            static_cast<int>(seg), value, static_cast<uintptr_t>(Segs.phys[seg]));
    return false;
}

bool CPU_READ_CRX(Bitu cr,Bit32u & retvalue) {
	/* Check if privileged to access control registers */
	if (cpu.pmode && (cpu.cpl>0)) return CPU_PrepareException(EXCEPTION_GP,0);
	if ((cr==1) || (cr>4)) return CPU_PrepareException(EXCEPTION_UD,0);
	retvalue=CPU_GET_CRX(cr);
	return false;
}

inline void CPU_Push16(Bitu value) {
    Bit32u new_esp = (reg_esp & cpu.stack.notmask) | ((reg_esp - 2) & cpu.stack.mask);
    mem_writew(SegPhys(ss) + (new_esp & cpu.stack.mask), value);
    reg_esp = new_esp;
}

inline void CPU_Push32(Bitu value) {
    Bit32u new_esp = (reg_esp & cpu.stack.notmask) | ((reg_esp - 4) & cpu.stack.mask);
    mem_writed(SegPhys(ss) + (new_esp & cpu.stack.mask), value);
    reg_esp = new_esp;
}

inline Bitu CPU_Pop16(void) {
    Bitu val = mem_readw(SegPhys(ss) + (reg_esp & cpu.stack.mask));
    reg_esp = (reg_esp & cpu.stack.notmask) | ((reg_esp + 2) & cpu.stack.mask);
    return val;
}

inline Bitu CPU_Pop32(void) {
    Bitu val = mem_readd(SegPhys(ss) + (reg_esp & cpu.stack.mask));
    reg_esp = (reg_esp & cpu.stack.notmask) | ((reg_esp + 4) & cpu.stack.mask);
    return val;
}

inline PhysPt SelBase(Bitu sel) {
    if (cpu.cr0 & CR0_PROTECTION) {
        Descriptor desc;
        cpu.gdt.GetDescriptor(sel, desc);
        return desc.GetBase();
    }
    return sel << 4;
}

inline void CPU_SetFlags(Bitu word, Bitu mask) {
    mask |= CPU_extflags_toggle; // ID-flag and AC-flag can be toggled on CPUID-supporting CPUs
    reg_flags = (reg_flags & ~mask) | (word & mask) | 2;
    cpu.direction = 1 - ((reg_flags & FLAG_DF) >> 9);
}

inline bool CPU_PrepareException(Bitu which, Bitu error) {
    cpu.exception.which = which;
    cpu.exception.error = error;
    return true;
}

inline bool CPU_CLI(void) {
    if (cpu.pmode && ((!GETFLAG(VM) && (GETFLAG_IOPL < cpu.cpl)) || (GETFLAG(VM) && (GETFLAG_IOPL < 3)))) {
        return CPU_PrepareException(EXCEPTION_GP, 0);
    }
    SETFLAGBIT(IF, false);
    return false;
}

inline bool CPU_STI(void) {
    if (cpu.pmode && ((!GETFLAG(VM) && (GETFLAG_IOPL < cpu.cpl)) || (GETFLAG(VM) && (GETFLAG_IOPL < 3)))) {
        return CPU_PrepareException(EXCEPTION_GP, 0);
    }
    SETFLAGBIT(IF, true);
    return false;
}

inline bool CPU_POPF(Bitu use32) {
    if (cpu.pmode && GETFLAG(VM) && (GETFLAG(IOPL) != FLAG_IOPL)) {
        return CPU_PrepareException(EXCEPTION_GP, 0);
    }
    Bitu mask = FMASK_ALL;
    if (cpu.pmode && (cpu.cpl > 0)) mask &= (~FLAG_IOPL);
    if (cpu.pmode && !GETFLAG(VM) && (GETFLAG_IOPL < cpu.cpl)) mask &= (~FLAG_IF);
    if (use32)
        CPU_SetFlags(CPU_Pop32(), mask);
    else
        CPU_SetFlags(CPU_Pop16(), mask & 0xffff);
    DestroyConditionFlags();
    return false;
}

inline bool CPU_PUSHF(Bitu use32) {
    if (cpu.pmode && GETFLAG(VM) && (GETFLAG(IOPL) != FLAG_IOPL)) {
        return CPU_PrepareException(EXCEPTION_GP, 0);
    }
    FillFlags();
    if (use32)
        CPU_Push32(reg_flags & 0xfcffff);
    else
        CPU_Push16(reg_flags);
    return false;
}

void CPU_CheckSegments(void) {
    bool needs_invalidation = false;
    Descriptor desc;
    if (!cpu.gdt.GetDescriptor(SegValue(es), desc)) needs_invalidation = true;
    else switch (desc.Type()) {
        case DESC_DATA_EU_RO_NA: case DESC_DATA_EU_RO_A: case DESC_DATA_EU_RW_NA: case DESC_DATA_EU_RW_A:
        case DESC_DATA_ED_RO_NA: case DESC_DATA_ED_RO_A: case DESC_DATA_ED_RW_NA: case DESC_DATA_ED_RW_A:
        case DESC_CODE_N_NC_A: case DESC_CODE_N_NC_NA: case DESC_CODE_R_NC_A: case DESC_CODE_R_NC_NA:
            if (cpu.cpl > desc.DPL()) needs_invalidation = true; break;
        default: break;
    }
    if (needs_invalidation) CPU_SetSegGeneral(es, 0);

    needs_invalidation = false;
    if (!cpu.gdt.GetDescriptor(SegValue(ds), desc)) needs_invalidation = true;
    else switch (desc.Type()) {
        case DESC_DATA_EU_RO_NA: case DESC_DATA_EU_RO_A: case DESC_DATA_EU_RW_NA: case DESC_DATA_EU_RW_A:
        case DESC_DATA_ED_RO_NA: case DESC_DATA_ED_RO_A: case DESC_DATA_ED_RW_NA: case DESC_DATA_ED_RW_A:
        case DESC_CODE_N_NC_A: case DESC_CODE_N_NC_NA: case DESC_CODE_R_NC_A: case DESC_CODE_R_NC_NA:
            if (cpu.cpl > desc.DPL()) needs_invalidation = true; break;
        default: break;
    }
    if (needs_invalidation) CPU_SetSegGeneral(ds, 0);

    needs_invalidation = false;
    if (!cpu.gdt.GetDescriptor(SegValue(fs), desc)) needs_invalidation = true;
    else switch (desc.Type()) {
        case DESC_DATA_EU_RO_NA: case DESC_DATA_EU_RO_A: case DESC_DATA_EU_RW_NA: case DESC_DATA_EU_RW_A:
        case DESC_DATA_ED_RO_NA: case DESC_DATA_ED_RO_A: case DESC_DATA_ED_RW_NA: case DESC_DATA_ED_RW_A:
        case DESC_CODE_N_NC_A: case DESC_CODE_N_NC_NA: case DESC_CODE_R_NC_A: case DESC_CODE_R_NC_NA:
            if (cpu.cpl > desc.DPL()) needs_invalidation = true; break;
        default: break;
    }
    if (needs_invalidation) CPU_SetSegGeneral(fs, 0);

    needs_invalidation = false;
    if (!cpu.gdt.GetDescriptor(SegValue(gs), desc)) needs_invalidation = true;
    else switch (desc.Type()) {
        case DESC_DATA_EU_RO_NA: case DESC_DATA_EU_RO_A: case DESC_DATA_EU_RW_NA: case DESC_DATA_EU_RW_A:
        case DESC_DATA_ED_RO_NA: case DESC_DATA_ED_RO_A: case DESC_DATA_ED_RW_NA: case DESC_DATA_ED_RW_A:
        case DESC_CODE_N_NC_A: case DESC_CODE_N_NC_NA: case DESC_CODE_R_NC_A: case DESC_CODE_R_NC_NA:
            if (cpu.cpl > desc.DPL()) needs_invalidation = true; break;
        default: break;
    }
    if (needs_invalidation) CPU_SetSegGeneral(gs, 0);
}

class TaskStateSegment {
public:
    TaskStateSegment() : valid(false) {}
    bool IsValid(void) { return valid; }
    Bitu Get_back(void) {
        CPU_LOG("TaskStateSegment::Get_back: Reading backlink from base=0x%zx", static_cast<uintptr_t>(base));
        cpu.mpl = 0;
        Bit16u backlink = mem_readw(base);
        cpu.mpl = 3;
        return backlink;
    }
    void SaveSelector(void) {
        CPU_LOG("TaskStateSegment::SaveSelector: Saving selector=0x%zx", static_cast<uintptr_t>(selector));
        cpu.gdt.SetDescriptor(selector, desc);
    }
    void Get_SSx_ESPx(Bitu level, Bitu& _ss, Bitu& _esp) {
        CPU_LOG("TaskStateSegment::Get_SSx_ESPx: Reading SS:ESP for level=%lu from base=0x%zx", level, static_cast<uintptr_t>(base));
        cpu.mpl = 0;
        if (is386) {
            PhysPt where = base + offsetof(TSS_32, esp0) + level * 8;
            _esp = mem_readd(where);
            _ss = mem_readw(where + 4);
        } else {
            PhysPt where = base + offsetof(TSS_16, sp0) + level * 4;
            _esp = mem_readw(where);
            _ss = mem_readw(where + 2);
        }
        cpu.mpl = 3;
        CPU_LOG("TaskStateSegment::Get_SSx_ESPx: Got SS=0x%lx, ESP=0x%lx", _ss, _esp);
    }
    bool SetSelector(Bitu new_sel) {
        CPU_LOG("TaskStateSegment::SetSelector: Setting selector=0x%zx", static_cast<uintptr_t>(new_sel));
        valid = false;
        if ((new_sel & 0xfffc) == 0) {
            selector = 0;
            base = 0;
            limit = 0;
            is386 = 1;
            valid = true;
            return true;
        }
        if (new_sel & 4) return false;
        if (!cpu.gdt.GetDescriptor(new_sel, desc)) return false;
        switch (desc.Type()) {
            case DESC_286_TSS_A: case DESC_286_TSS_B:
            case DESC_386_TSS_A: case DESC_386_TSS_B:
                break;
            default:
                return false;
        }
        if (!desc.saved.seg.p) return false;
        selector = new_sel;
        valid = true;
        base = desc.GetBase();
        limit = desc.GetLimit();
        is386 = desc.Is386();
        CPU_LOG("TaskStateSegment::SetSelector: Set selector=0x%zx, base=0x%zx, limit=0x%zx, is386=%ld", static_cast<uintptr_t>(selector), static_cast<uintptr_t>(base), static_cast<uintptr_t>(limit), is386);
        return true;
    }
    TSS_Descriptor desc;
    Bitu selector;
    PhysPt base;
    Bitu limit;
    Bitu is386;
    bool valid;
};

TaskStateSegment cpu_tss;

enum TSwitchType {
    TSwitch_JMP, TSwitch_CALL_INT, TSwitch_IRET
};

bool CPU_SwitchTask(Bitu new_tss_selector, TSwitchType tstype, Bitu old_eip) {
    CPU_LOG("CPU_SwitchTask: Switching to selector=0x%zx, type=%d, old_eip=0x%lx", static_cast<uintptr_t>(new_tss_selector), tstype, old_eip);
    FillFlags();
    TaskStateSegment new_tss;
    if (!new_tss.SetSelector(new_tss_selector)) 
        E_Exit("Illegal TSS for switch, selector=%zx, switchtype=%zx", static_cast<uintptr_t>(new_tss_selector), static_cast<uintptr_t>(tstype));
    if (tstype == TSwitch_IRET) {
        if (!new_tss.desc.IsBusy())
            E_Exit("TSS not busy for IRET");
    } else {
        if (new_tss.desc.IsBusy())
            E_Exit("TSS busy for JMP/CALL/INT");
    }
    Bitu new_cr3 = 0;
    Bitu new_eax, new_ebx, new_ecx, new_edx, new_esp, new_ebp, new_esi, new_edi;
    Bitu new_es, new_cs, new_ss, new_ds, new_fs, new_gs;
    Bitu new_ldt, new_eip, new_eflags;
    if (new_tss.is386) {
        new_cr3 = mem_readd(new_tss.base + offsetof(TSS_32, cr3));
        new_eip = mem_readd(new_tss.base + offsetof(TSS_32, eip));
        new_eflags = mem_readd(new_tss.base + offsetof(TSS_32, eflags));
        new_eax = mem_readd(new_tss.base + offsetof(TSS_32, eax));
        new_ecx = mem_readd(new_tss.base + offsetof(TSS_32, ecx));
        new_edx = mem_readd(new_tss.base + offsetof(TSS_32, edx));
        new_ebx = mem_readd(new_tss.base + offsetof(TSS_32, ebx));
        new_esp = mem_readd(new_tss.base + offsetof(TSS_32, esp));
        new_ebp = mem_readd(new_tss.base + offsetof(TSS_32, ebp));
        new_edi = mem_readd(new_tss.base + offsetof(TSS_32, edi));
        new_esi = mem_readd(new_tss.base + offsetof(TSS_32, esi));
        new_es = mem_readw(new_tss.base + offsetof(TSS_32, es));
        new_cs = mem_readw(new_tss.base + offsetof(TSS_32, cs));
        new_ss = mem_readw(new_tss.base + offsetof(TSS_32, ss));
        new_ds = mem_readw(new_tss.base + offsetof(TSS_32, ds));
        new_fs = mem_readw(new_tss.base + offsetof(TSS_32, fs));
        new_gs = mem_readw(new_tss.base + offsetof(TSS_32, gs));
        new_ldt = mem_readw(new_tss.base + offsetof(TSS_32, ldt));
        CPU_LOG("CPU_SwitchTask: Loaded 386 TSS, eip=0x%lx, cs=0x%lx, ss=0x%lx, esp=0x%lx", new_eip, new_cs, new_ss, new_esp);
    } else {
        E_Exit("286 task switch");
        new_cr3 = new_eip = new_eflags = new_eax = new_ecx = new_edx = new_ebx = 0;
        new_esp = new_ebp = new_edi = new_esi = new_es = new_cs = new_ss = new_ds = new_fs = new_gs = new_ldt = 0;
    }
    if (tstype == TSwitch_JMP || tstype == TSwitch_IRET) {
        cpu_tss.desc.SetBusy(false);
        cpu_tss.SaveSelector();
    }
    Bit32u old_flags = reg_flags;
    if (tstype == TSwitch_IRET) old_flags &= (~FLAG_NT);
    if (cpu_tss.is386) {
        mem_writed(cpu_tss.base + offsetof(TSS_32, eflags), old_flags);
        mem_writed(cpu_tss.base + offsetof(TSS_32, eip), old_eip);
        mem_writed(cpu_tss.base + offsetof(TSS_32, eax), reg_eax);
        mem_writed(cpu_tss.base + offsetof(TSS_32, ecx), reg_ecx);
        mem_writed(cpu_tss.base + offsetof(TSS_32, edx), reg_edx);
        mem_writed(cpu_tss.base + offsetof(TSS_32, ebx), reg_ebx);
        mem_writed(cpu_tss.base + offsetof(TSS_32, esp), reg_esp);
        mem_writed(cpu_tss.base + offsetof(TSS_32, ebp), reg_ebp);
        mem_writed(cpu_tss.base + offsetof(TSS_32, esi), reg_esi);
        mem_writed(cpu_tss.base + offsetof(TSS_32, edi), reg_edi);
        mem_writed(cpu_tss.base + offsetof(TSS_32, es), SegValue(es));
        mem_writed(cpu_tss.base + offsetof(TSS_32, cs), SegValue(cs));
        mem_writed(cpu_tss.base + offsetof(TSS_32, ss), SegValue(ss));
        mem_writed(cpu_tss.base + offsetof(TSS_32, ds), SegValue(ds));
        mem_writed(cpu_tss.base + offsetof(TSS_32, fs), SegValue(fs));
        mem_writed(cpu_tss.base + offsetof(TSS_32, gs), SegValue(gs));
    } else {
        E_Exit("286 task switch");
    }
    if (tstype == TSwitch_CALL_INT) {
        if (new_tss.is386) {
            mem_writed(new_tss.base + offsetof(TSS_32, back), cpu_tss.selector);
        } else {
            mem_writew(new_tss.base + offsetof(TSS_16, back), cpu_tss.selector);
        }
        new_eflags |= FLAG_NT;
    }
    if (tstype == TSwitch_JMP || tstype == TSwitch_CALL_INT) {
        new_tss.desc.SetBusy(true);
        new_tss.SaveSelector();
    }
    if (new_tss_selector == cpu_tss.selector) {
        reg_eip = old_eip;
        new_cs = SegValue(cs);
        new_ss = SegValue(ss);
        new_ds = SegValue(ds);
        new_es = SegValue(es);
        new_fs = SegValue(fs);
        new_gs = SegValue(gs);
    } else {
        PAGING_SetDirBase(new_cr3);
        if (new_tss.is386) {
            reg_eip = new_eip;
            CPU_SetFlags(new_eflags, FMASK_ALL | FLAG_VM);
            reg_eax = new_eax;
            reg_ecx = new_ecx;
            reg_edx = new_edx;
            reg_ebx = new_ebx;
            reg_esp = new_esp;
            reg_ebp = new_ebp;
            reg_edi = new_edi;
            reg_esi = new_esi;
        } else {
            E_Exit("286 task switch");
        }
    }
    if (reg_flags & FLAG_VM) {
        SegSet16(cs, new_cs);
        cpu.code.big = false;
        cpu.cpl = 3;
    } else {
        if (new_ldt != 0) CPU_LLDT(new_ldt);
        Descriptor cs_desc;
        cpu.cpl = new_cs & 3;
        if (!cpu.gdt.GetDescriptor(new_cs, cs_desc))
            E_Exit("Task switch with CS beyond limits");
        if (!cs_desc.saved.seg.p)
            E_Exit("Task switch with non present code-segment");
        switch (cs_desc.Type()) {
            case DESC_CODE_N_NC_A: case DESC_CODE_N_NC_NA:
            case DESC_CODE_R_NC_A: case DESC_CODE_R_NC_NA:
                if (cpu.cpl != cs_desc.DPL()) E_Exit("Task CS RPL != DPL");
                goto doconforming;
            case DESC_CODE_N_C_A: case DESC_CODE_N_C_NA:
            case DESC_CODE_R_C_A: case DESC_CODE_R_C_NA:
                if (cpu.cpl < cs_desc.DPL()) E_Exit("Task CS RPL < DPL");
doconforming:
                Segs.phys[cs] = cs_desc.GetBase();
                cpu.code.big = cs_desc.Big() > 0;
                Segs.val[cs] = new_cs;
                break;
            default:
                E_Exit("Task switch CS Type %zu", static_cast<uintptr_t>(cs_desc.Type()));
        }
    }
    CPU_SetSegGeneral(es, new_es);
    CPU_SetSegGeneral(ss, new_ss);
    CPU_SetSegGeneral(ds, new_ds);
    CPU_SetSegGeneral(fs, new_fs);
    CPU_SetSegGeneral(gs, new_gs);
    if (!cpu_tss.SetSelector(new_tss_selector)) {
        CPU_LOG("CPU_SwitchTask: Set TSS selector %lX failed", new_tss_selector);
    }
    CPU_LOG("CPU_SwitchTask: Completed, CPL=%ld, CS=0x%x, IP=0x%x, SS=0x%x, SP=0x%x", cpu.cpl, SegValue(cs), reg_eip, SegValue(ss), reg_esp);
    return true;
}

bool CPU_IO_Exception(Bitu port, Bitu size) {
    CPU_LOG("CPU_IO_Exception: Checking port=0x%lx, size=%lu", port, size);
    if (cpu.pmode && ((GETFLAG_IOPL < cpu.cpl) || GETFLAG(VM))) {
        cpu.mpl = 0;
        if (!cpu_tss.is386) goto doexception;
        PhysPt bwhere = cpu_tss.base + 0x66;
        Bitu ofs = mem_readw(bwhere);
        if (ofs > cpu_tss.limit) goto doexception;
        bwhere = cpu_tss.base + ofs + (port / 8);
        Bitu map = mem_readw(bwhere);
        Bitu mask = (0xffff >> (16 - size)) << (port & 7);
        if (map & mask) goto doexception;
        cpu.mpl = 3;
    }
    CPU_LOG("CPU_IO_Exception: Access allowed");
    return false;
doexception:
    cpu.mpl = 3;
    CPU_LOG("CPU_IO_Exception: Exception triggered for port=0x%lx", port);
    return CPU_PrepareException(EXCEPTION_GP, 0);
}

void CPU_Exception(Bitu which, Bitu error) {
    CPU_LOG("CPU_Exception: which=%ld, error=0x%lx", which, error);
    cpu.exception.error = error;
    CPU_Interrupt(which, CPU_INT_EXCEPTION | ((which >= 8) ? CPU_INT_HAS_ERROR : 0), reg_eip);
}

Bit8u lastint;
void CPU_Interrupt(Bitu num, Bitu type, Bitu oldeip) {
    CPU_LOG("CPU_Interrupt: num=0x%lx, type=0x%lx, oldeip=0x%lx", num, type, oldeip);
    lastint = num;
    FillFlags();
#if C_DEBUG
    switch (num) {
        case 0xcd:
#if C_HEAVY_DEBUG
            CPU_LOG("CPU_Interrupt: Call to interrupt 0xCD, this is BAD");
            DEBUG_HeavyWriteLogInstruction();
            E_Exit("Call to interrupt 0xCD this is BAD");
#endif
            break;
        case 0x03:
            if (DEBUG_Breakpoint()) {
                CPU_Cycles = 0;
                return;
            }
            break;
    }
#endif
    if (!cpu.pmode) {
        CPU_Push16(reg_flags & 0xffff);
        CPU_Push16(SegValue(cs));
        CPU_Push16(oldeip);
        SETFLAGBIT(IF, false);
        SETFLAGBIT(TF, false);
        PhysPt base = cpu.idt.GetBase();
        reg_eip = mem_readw(base + (num << 2));
        Segs.val[cs] = mem_readw(base + (num << 2) + 2);
        Segs.phys[cs] = Segs.val[cs] << 4;
        cpu.code.big = false;
        CPU_LOG("CPU_Interrupt: Real mode, set CS=0x%x, IP=0x%x", SegValue(cs), reg_eip);
        return;
    } else {
        if ((reg_flags & FLAG_VM) && (type & CPU_INT_SOFTWARE) && !(type & CPU_INT_NOIOPLCHECK)) {
            if ((reg_flags & FLAG_IOPL) != FLAG_IOPL) {
                CPU_Exception(EXCEPTION_GP, 0);
                return;
            }
        }
        Descriptor gate;
        if (!cpu.idt.GetDescriptor(num << 3, gate)) {
            CPU_Exception(EXCEPTION_GP, num * 8 + 2 + (type & CPU_INT_SOFTWARE) ? 0 : 1);
            return;
        }
        if ((type & CPU_INT_SOFTWARE) && (gate.DPL() < cpu.cpl)) {
            CPU_Exception(EXCEPTION_GP, num * 8 + 2);
            return;
        }
        switch (gate.Type()) {
            case DESC_286_INT_GATE: case DESC_386_INT_GATE:
            case DESC_286_TRAP_GATE: case DESC_386_TRAP_GATE:
            {
                CPU_CHECK_COND(!gate.saved.seg.p,
                    "INT:Gate segment not present",
                    EXCEPTION_NP, num * 8 + 2 + (type & CPU_INT_SOFTWARE) ? 0 : 1)
                Descriptor cs_desc;
                Bitu gate_sel = gate.GetSelector();
                Bitu gate_off = gate.GetOffset();
                CPU_CHECK_COND((gate_sel & 0xfffc) == 0,
                    "INT:Gate with CS zero selector",
                    EXCEPTION_GP, (type & CPU_INT_SOFTWARE) ? 0 : 1)
                CPU_CHECK_COND(!cpu.gdt.GetDescriptor(gate_sel, cs_desc),
                    "INT:Gate with CS beyond limit",
                    EXCEPTION_GP, (gate_sel & 0xfffc) + (type & CPU_INT_SOFTWARE) ? 0 : 1)
                Bitu cs_dpl = cs_desc.DPL();
                CPU_CHECK_COND(cs_dpl > cpu.cpl,
                    "Interrupt to higher privilege",
                    EXCEPTION_GP, (gate_sel & 0xfffc) + (type & CPU_INT_SOFTWARE) ? 0 : 1)
                switch (cs_desc.Type()) {
                    case DESC_CODE_N_NC_A: case DESC_CODE_N_NC_NA:
                    case DESC_CODE_R_NC_A: case DESC_CODE_R_NC_NA:
                        if (cs_dpl < cpu.cpl) {
                            CPU_CHECK_COND(!cs_desc.saved.seg.p,
                                "INT:Inner level:CS segment not present",
                                EXCEPTION_NP, (gate_sel & 0xfffc) + (type & CPU_INT_SOFTWARE) ? 0 : 1)
                            CPU_CHECK_COND((reg_flags & FLAG_VM) && (cs_dpl != 0),
                                "V86 interrupt calling codesegment with DPL>0",
                                EXCEPTION_GP, gate_sel & 0xfffc)
                            Bitu n_ss, n_esp;
                            Bitu o_ss, o_esp;
                            o_ss = SegValue(ss);
                            o_esp = reg_esp;
                            cpu_tss.Get_SSx_ESPx(cs_dpl, n_ss, n_esp);
                            CPU_CHECK_COND((n_ss & 0xfffc) == 0,
                                "INT:Gate with SS zero selector",
                                EXCEPTION_TS, (type & CPU_INT_SOFTWARE) ? 0 : 1)
                            Descriptor n_ss_desc;
                            CPU_CHECK_COND(!cpu.gdt.GetDescriptor(n_ss, n_ss_desc),
                                "INT:Gate with SS beyond limit",
                                EXCEPTION_TS, (n_ss & 0xfffc) + (type & CPU_INT_SOFTWARE) ? 0 : 1)
                            CPU_CHECK_COND(((n_ss & 3) != cs_dpl) || (n_ss_desc.DPL() != cs_dpl),
                                "INT:Inner level with CS_DPL!=SS_DPL and SS_RPL",
                                EXCEPTION_TS, (n_ss & 0xfffc) + (type & CPU_INT_SOFTWARE) ? 0 : 1)
                            switch (n_ss_desc.Type()) {
                                case DESC_DATA_EU_RW_NA: case DESC_DATA_EU_RW_A:
                                case DESC_DATA_ED_RW_NA: case DESC_DATA_ED_RW_A:
                                    break;
                                default:
                                    E_Exit("INT:Inner level:Stack segment not writable.");
                            }
                            CPU_CHECK_COND(!n_ss_desc.saved.seg.p,
                                "INT:Inner level with nonpresent SS",
                                EXCEPTION_SS, (n_ss & 0xfffc) + (type & CPU_INT_SOFTWARE) ? 0 : 1)
                            Segs.phys[ss] = n_ss_desc.GetBase();
                            Segs.val[ss] = n_ss;
                            if (n_ss_desc.Big()) {
                                cpu.stack.big = true;
                                cpu.stack.mask = 0xffffffff;
                                cpu.stack.notmask = 0;
                                reg_esp = n_esp;
                            } else {
                                cpu.stack.big = false;
                                cpu.stack.mask = 0xffff;
                                cpu.stack.notmask = 0xffff0000;
                                reg_sp = n_esp & 0xffff;
                            }
                            cpu.cpl = cs_dpl;
                            if (gate.Type() & 0x8) {
                                if (reg_flags & FLAG_VM) {
                                    CPU_Push32(SegValue(gs)); SegSet16(gs, 0x0);
                                    CPU_Push32(SegValue(fs)); SegSet16(fs, 0x0);
                                    CPU_Push32(SegValue(ds)); SegSet16(ds, 0x0);
                                    CPU_Push32(SegValue(es)); SegSet16(es, 0x0);
                                }
                                CPU_Push32(o_ss);
                                CPU_Push32(o_esp);
                            } else {
                                if (reg_flags & FLAG_VM) E_Exit("V86 to 16-bit gate");
                                CPU_Push16(o_ss);
                                CPU_Push16(o_esp);
                            }
                            goto do_interrupt;
                        }
                        if (cs_dpl != cpu.cpl)
                            E_Exit("Non-conforming intra privilege INT with DPL!=CPL");
                    case DESC_CODE_N_C_A: case DESC_CODE_N_C_NA:
                    case DESC_CODE_R_C_A: case DESC_CODE_R_C_NA:
                        CPU_CHECK_COND(!cs_desc.saved.seg.p,
                            "INT:Same level:CS segment not present",
                            EXCEPTION_NP, (gate_sel & 0xfffc) + (type & CPU_INT_SOFTWARE) ? 0 : 1)
                        if ((reg_flags & FLAG_VM) && (cs_dpl < cpu.cpl))
                            E_Exit("V86 interrupt doesn't change to pl0");
do_interrupt:
                        if (gate.Type() & 0x8) {
                            CPU_Push32(reg_flags);
                            CPU_Push32(SegValue(cs));
                            CPU_Push32(oldeip);
                            if (type & CPU_INT_HAS_ERROR) CPU_Push32(cpu.exception.error);
                        } else {
                            CPU_Push16(reg_flags & 0xffff);
                            CPU_Push16(SegValue(cs));
                            CPU_Push16(oldeip);
                            if (type & CPU_INT_HAS_ERROR) CPU_Push16(cpu.exception.error);
                        }
                        break;
                    default:
                        E_Exit("INT:Gate Selector points to illegal descriptor with type %zx", static_cast<uintptr_t>(cs_desc.Type()));
                }
                Segs.val[cs] = (gate_sel & 0xfffc) | cpu.cpl;
                Segs.phys[cs] = cs_desc.GetBase();
                cpu.code.big = cs_desc.Big() > 0;
                reg_eip = gate_off;
                if (!(gate.Type() & 1)) {
                    SETFLAGBIT(IF, false);
                }
                SETFLAGBIT(TF, false);
                SETFLAGBIT(NT, false);
                SETFLAGBIT(VM, false);
                CPU_LOG("CPU_Interrupt: Gate to %lX:%lX big %ld %s", gate_sel, gate_off, cs_desc.Big(), gate.Type() & 0x8 ? "386" : "286");
                return;
            }
            case DESC_TASK_GATE:
                CPU_CHECK_COND(!gate.saved.seg.p,
                    "INT:Gate segment not present",
                    EXCEPTION_NP, num * 8 + 2 + (type & CPU_INT_SOFTWARE) ? 0 : 1)
                CPU_SwitchTask(gate.GetSelector(), TSwitch_CALL_INT, oldeip);
                if (type & CPU_INT_HAS_ERROR) {
                    if (cpu_tss.is386) CPU_Push32(cpu.exception.error);
                    else CPU_Push16(cpu.exception.error);
                }
                return;
            default:
                E_Exit("Illegal descriptor type %zX for int %zX", static_cast<uintptr_t>(gate.Type()), static_cast<uintptr_t>(num));
        }
    }
}

void CPU_IRET(bool use32, Bitu oldeip) {
    CPU_LOG("CPU_IRET: use32=%d, oldeip=0x%lx", use32, oldeip);
    if (!cpu.pmode) {
        if (use32) {
            reg_eip = CPU_Pop32();
            SegSet16(cs, CPU_Pop32());
            CPU_SetFlags(CPU_Pop32(), FMASK_ALL);
        } else {
            reg_eip = CPU_Pop16();
            SegSet16(cs, CPU_Pop16());
            CPU_SetFlags(CPU_Pop16(), FMASK_ALL & 0xffff);
        }
        cpu.code.big = false;
        DestroyConditionFlags();
        CPU_LOG("CPU_IRET: Real mode, set CS=0x%x, IP=0x%x", SegValue(cs), reg_eip);
        return;
    } else {
        if (reg_flags & FLAG_VM) {
            if ((reg_flags & FLAG_IOPL) != FLAG_IOPL) {
                CPU_Exception(EXCEPTION_GP, 0);
                return;
            } else {
                if (use32) {
                    Bit32u new_eip = mem_readd(SegPhys(ss) + (reg_esp & cpu.stack.mask));
                    Bit32u tempesp = (reg_esp & cpu.stack.notmask) | ((reg_esp + 4) & cpu.stack.mask);
                    Bit32u new_cs = mem_readd(SegPhys(ss) + (tempesp & cpu.stack.mask));
                    tempesp = (tempesp & cpu.stack.notmask) | ((tempesp + 4) & cpu.stack.mask);
                    Bit32u new_flags = mem_readd(SegPhys(ss) + (tempesp & cpu.stack.mask));
                    reg_esp = (tempesp & cpu.stack.notmask) | ((tempesp + 4) & cpu.stack.mask);
                    reg_eip = new_eip;
                    SegSet16(cs, (Bit16u)(new_cs & 0xffff));
                    CPU_SetFlags(new_flags, FMASK_NORMAL | FLAG_NT);
                } else {
                    Bit16u new_eip = mem_readw(SegPhys(ss) + (reg_esp & cpu.stack.mask));
                    Bit32u tempesp = (reg_esp & cpu.stack.notmask) | ((reg_esp + 2) & cpu.stack.mask);
                    Bit16u new_cs = mem_readw(SegPhys(ss) + (tempesp & cpu.stack.mask));
                    tempesp = (tempesp & cpu.stack.notmask) | ((tempesp + 2) & cpu.stack.mask);
                    Bit16u new_flags = mem_readw(SegPhys(ss) + (tempesp & cpu.stack.mask));
                    reg_esp = (tempesp & cpu.stack.notmask) | ((tempesp + 2) & cpu.stack.mask);
                    reg_eip = (Bit32u)new_eip;
                    SegSet16(cs, new_cs);
                    CPU_SetFlags(new_flags, FMASK_NORMAL | FLAG_NT);
                }
                cpu.code.big = false;
                DestroyConditionFlags();
                CPU_LOG("CPU_IRET: V86 mode, set CS=0x%x, IP=0x%x", SegValue(cs), reg_eip);
                return;
            }
        }
        if (GETFLAG(NT)) {
            if (GETFLAG(VM)) E_Exit("Pmode IRET with VM bit set");
            CPU_CHECK_COND(!cpu_tss.IsValid(),
                "TASK Iret without valid TSS",
                EXCEPTION_TS, cpu_tss.selector & 0xfffc)
            if (!cpu_tss.desc.IsBusy()) {
                CPU_LOG("CPU_IRET: TSS not busy");
            }
            Bitu back_link = cpu_tss.Get_back();
            CPU_SwitchTask(back_link, TSwitch_IRET, oldeip);
            return;
        }
        Bitu n_cs_sel, n_eip, n_flags;
        Bit32u tempesp;
        if (use32) {
            n_eip = mem_readd(SegPhys(ss) + (reg_esp & cpu.stack.mask));
            tempesp = (reg_esp & cpu.stack.notmask) | ((reg_esp + 4) & cpu.stack.mask);
            n_cs_sel = mem_readd(SegPhys(ss) + (tempesp & cpu.stack.mask)) & 0xffff;
            tempesp = (tempesp & cpu.stack.notmask) | ((tempesp + 4) & cpu.stack.mask);
            n_flags = mem_readd(SegPhys(ss) + (tempesp & cpu.stack.mask));
            tempesp = (tempesp & cpu.stack.notmask) | ((tempesp + 4) & cpu.stack.mask);
            if ((n_flags & FLAG_VM) && (cpu.cpl == 0)) {
                reg_esp = tempesp;
                reg_eip = n_eip & 0xffff;
                Bitu n_ss, n_esp, n_es, n_ds, n_fs, n_gs;
                n_esp = CPU_Pop32();
                n_ss = CPU_Pop32() & 0xffff;
                n_es = CPU_Pop32() & 0xffff;
                n_ds = CPU_Pop32() & 0xffff;
                n_fs = CPU_Pop32() & 0xffff;
                n_gs = CPU_Pop32() & 0xffff;
                CPU_SetFlags(n_flags, FMASK_ALL | FLAG_VM);
                DestroyConditionFlags();
                cpu.cpl = 3;
                CPU_SetSegGeneral(ss, n_ss);
                CPU_SetSegGeneral(es, n_es);
                CPU_SetSegGeneral(ds, n_ds);
                CPU_SetSegGeneral(fs, n_fs);
                CPU_SetSegGeneral(gs, n_gs);
                reg_esp = n_esp;
                cpu.code.big = false;
                SegSet16(cs, n_cs_sel);
                CPU_LOG("CPU_IRET: Back to V86: CS=0x%x, IP=0x%x, SS=0x%x, SP=0x%x, FLAGS=0x%lx", SegValue(cs), reg_eip, SegValue(ss), reg_esp, reg_flags);
                return;
            }
            if (n_flags & FLAG_VM) E_Exit("IRET from pmode to v86 with CPL!=0");
        } else {
            n_eip = mem_readw(SegPhys(ss) + (reg_esp & cpu.stack.mask));
            tempesp = (reg_esp & cpu.stack.notmask) | ((reg_esp + 2) & cpu.stack.mask);
            n_cs_sel = mem_readw(SegPhys(ss) + (tempesp & cpu.stack.mask));
            tempesp = (tempesp & cpu.stack.notmask) | ((tempesp + 2) & cpu.stack.mask);
            n_flags = mem_readw(SegPhys(ss) + (tempesp & cpu.stack.mask));
            n_flags |= (reg_flags & 0xffff0000);
            tempesp = (tempesp & cpu.stack.notmask) | ((tempesp + 2) & cpu.stack.mask);
            if (n_flags & FLAG_VM) E_Exit("VM Flag in 16-bit iret");
        }
        CPU_CHECK_COND((n_cs_sel & 0xfffc) == 0,
            "IRET:CS selector zero",
            EXCEPTION_GP, 0)
        Bitu n_cs_rpl = n_cs_sel & 3;
        Descriptor n_cs_desc;
        CPU_CHECK_COND(!cpu.gdt.GetDescriptor(n_cs_sel, n_cs_desc),
            "IRET:CS selector beyond limits",
            EXCEPTION_GP, n_cs_sel & 0xfffc)
        CPU_CHECK_COND(n_cs_rpl < cpu.cpl,
            "IRET to lower privilege",
            EXCEPTION_GP, n_cs_sel & 0xfffc)
        switch (n_cs_desc.Type()) {
            case DESC_CODE_N_NC_A: case DESC_CODE_N_NC_NA:
            case DESC_CODE_R_NC_A: case DESC_CODE_R_NC_NA:
                CPU_CHECK_COND(n_cs_rpl != n_cs_desc.DPL(),
                    "IRET:NC:DPL!=RPL",
                    EXCEPTION_GP, n_cs_sel & 0xfffc)
                break;
            case DESC_CODE_N_C_A: case DESC_CODE_N_C_NA:
            case DESC_CODE_R_C_A: case DESC_CODE_R_C_NA:
                CPU_CHECK_COND(n_cs_desc.DPL() > n_cs_rpl,
                    "IRET:C:DPL>RPL",
                    EXCEPTION_GP, n_cs_sel & 0xfffc)
                break;
            default:
                E_Exit("IRET:Illegal descriptor type %zX", static_cast<uintptr_t>(n_cs_desc.Type()));
        }
        CPU_CHECK_COND(!n_cs_desc.saved.seg.p,
            "IRET with nonpresent code segment",
            EXCEPTION_NP, n_cs_sel & 0xfffc)
        if (n_cs_rpl == cpu.cpl) {
            reg_esp = tempesp;
            Segs.phys[cs] = n_cs_desc.GetBase();
            cpu.code.big = n_cs_desc.Big() > 0;
            Segs.val[cs] = n_cs_sel;
            reg_eip = n_eip;
            Bitu mask = cpu.cpl ? (FMASK_NORMAL | FLAG_NT) : FMASK_ALL;
            if (GETFLAG_IOPL < cpu.cpl) mask &= (~FLAG_IF);
            CPU_SetFlags(n_flags, mask);
            DestroyConditionFlags();
            CPU_LOG("CPU_IRET: Same level: CS=0x%lx, IP=0x%lx, big=%d", n_cs_sel, n_eip, cpu.code.big);
        } else {
            Bitu n_ss, n_esp;
            if (use32) {
                n_esp = mem_readd(SegPhys(ss) + (tempesp & cpu.stack.mask));
                tempesp = (tempesp & cpu.stack.notmask) | ((tempesp + 4) & cpu.stack.mask);
                n_ss = mem_readd(SegPhys(ss) + (tempesp & cpu.stack.mask)) & 0xffff;
            } else {
                n_esp = mem_readw(SegPhys(ss) + (tempesp & cpu.stack.mask));
                tempesp = (tempesp & cpu.stack.notmask) | ((tempesp + 2) & cpu.stack.mask);
                n_ss = mem_readw(SegPhys(ss) + (tempesp & cpu.stack.mask));
            }
            CPU_CHECK_COND((n_ss & 0xfffc) == 0,
                "IRET:Outer level:SS selector zero",
                EXCEPTION_GP, 0)
            Descriptor n_ss_desc;
            CPU_CHECK_COND(!cpu.gdt.GetDescriptor(n_ss, n_ss_desc),
                "IRET:Outer level:SS beyond limit",
                EXCEPTION_GP, n_ss & 0xfffc)
            CPU_CHECK_COND(((n_ss & 3) != n_cs_rpl) || (n_ss_desc.DPL() != n_cs_rpl),
                "IRET:Outer level:SS rpl!=CS rpl",
                EXCEPTION_GP, n_ss & 0xfffc)
            switch (n_ss_desc.Type()) {
                case DESC_DATA_EU_RW_NA: case DESC_DATA_EU_RW_A:
                case DESC_DATA_ED_RW_NA: case DESC_DATA_ED_RW_A:
                    break;
                default:
                    E_Exit("IRET:Outer level:Stack segment not writable");
            }
            CPU_CHECK_COND(!n_ss_desc.saved.seg.p,
                "IRET:Outer level:Stack segment not present",
                EXCEPTION_SS, n_ss & 0xfffc)
            Segs.phys[cs] = n_cs_desc.GetBase();
            cpu.code.big = n_cs_desc.Big() > 0;
            Segs.val[cs] = n_cs_sel;
            Bitu mask = cpu.cpl ? (FMASK_NORMAL | FLAG_NT) : FMASK_ALL;
            if (GETFLAG_IOPL < cpu.cpl) mask &= (~FLAG_IF);
            CPU_SetFlags(n_flags, mask);
            DestroyConditionFlags();
            cpu.cpl = n_cs_rpl;
            reg_eip = n_eip;
            Segs.val[ss] = n_ss;
            Segs.phys[ss] = n_ss_desc.GetBase();
            if (n_ss_desc.Big()) {
                cpu.stack.big = true;
                cpu.stack.mask = 0xffffffff;
                cpu.stack.notmask = 0;
                reg_esp = n_esp;
            } else {
                cpu.stack.big = false;
                cpu.stack.mask = 0xffff;
                cpu.stack.notmask = 0xffff0000;
                reg_sp = n_esp & 0xffff;
            }
            CPU_CheckSegments();
            CPU_LOG("CPU_IRET: Outer level: CS=0x%lx, IP=0x%lx, big=%d", n_cs_sel, n_eip, cpu.code.big);
        }
        return;
    }
}

void CPU_JMP(bool use32, Bitu selector, Bitu offset, Bitu oldeip) {
    CPU_LOG("CPU_JMP: use32=%d, selector=0x%lx, offset=0x%lx, oldeip=0x%lx", use32, selector, offset, oldeip);
    if (!cpu.pmode || (reg_flags & FLAG_VM)) {
        if (!use32) {
            reg_eip = offset & 0xffff;
        } else {
            reg_eip = offset;
        }
        SegSet16(cs, selector);
        cpu.code.big = false;
        CPU_LOG("CPU_JMP: Real/VM mode, set CS=0x%x, IP=0x%x", SegValue(cs), reg_eip);
        return;
    } else {
        CPU_CHECK_COND((selector & 0xfffc) == 0,
            "JMP:CS selector zero",
            EXCEPTION_GP, 0)
        Bitu rpl = selector & 3;
        Descriptor desc;
        CPU_CHECK_COND(!cpu.gdt.GetDescriptor(selector, desc),
            "JMP:CS beyond limits",
            EXCEPTION_GP, selector & 0xfffc)
        switch (desc.Type()) {
            case DESC_CODE_N_NC_A: case DESC_CODE_N_NC_NA:
            case DESC_CODE_R_NC_A: case DESC_CODE_R_NC_NA:
                CPU_CHECK_COND(rpl > cpu.cpl,
                    "JMP:NC:RPL>CPL",
                    EXCEPTION_GP, selector & 0xfffc)
                CPU_CHECK_COND(cpu.cpl != desc.DPL(),
                    "JMP:NC:RPL != DPL",
                    EXCEPTION_GP, selector & 0xfffc)
                CPU_LOG("CPU_JMP: Code:NC to %lX:%lX big %ld", selector, offset, desc.Big());
                goto CODE_jmp;
            case DESC_CODE_N_C_A: case DESC_CODE_N_C_NA:
            case DESC_CODE_R_C_A: case DESC_CODE_R_C_NA:
                CPU_LOG("CPU_JMP: Code:C to %lX:%lX big %ld", selector, offset, desc.Big());
                CPU_CHECK_COND(cpu.cpl < desc.DPL(),
                    "JMP:C:CPL < DPL",
                    EXCEPTION_GP, selector & 0xfffc)
CODE_jmp:
                if (!desc.saved.seg.p) {
                    CPU_Exception(EXCEPTION_NP, selector & 0xfffc);
                    return;
                }
                Segs.phys[cs] = desc.GetBase();
                cpu.code.big = desc.Big() > 0;
                Segs.val[cs] = (selector & 0xfffc) | cpu.cpl;
                reg_eip = offset;
                CPU_LOG("CPU_JMP: Set CS=0x%x, IP=0x%x, big=%d", SegValue(cs), reg_eip, cpu.code.big);
                return;
            case DESC_386_TSS_A:
                CPU_CHECK_COND(desc.DPL() < cpu.cpl,
                    "JMP:TSS:dpl<cpl",
                    EXCEPTION_GP, selector & 0xfffc)
                CPU_CHECK_COND(desc.DPL() < rpl,
                    "JMP:TSS:dpl<rpl",
                    EXCEPTION_GP, selector & 0xfffc)
                CPU_LOG("CPU_JMP: TSS to %lX", selector);
                CPU_SwitchTask(selector, TSwitch_JMP, oldeip);
                break;
            default:
                E_Exit("JMP Illegal descriptor type %zX", static_cast<uintptr_t>(desc.Type()));
        }
    }
}

void CPU_CALL(bool use32, Bitu selector, Bitu offset, Bitu oldeip) {
    CPU_LOG("CPU_CALL: use32=%d, selector=0x%lx, offset=0x%lx, oldeip=0x%lx", use32, selector, offset, oldeip);
    if (!cpu.pmode || (reg_flags & FLAG_VM)) {
        if (!use32) {
            CPU_Push16(SegValue(cs));
            CPU_Push16(oldeip);
            reg_eip = offset & 0xffff;
        } else {
            CPU_Push32(SegValue(cs));
            CPU_Push32(oldeip);
            reg_eip = offset;
        }
        cpu.code.big = false;
        SegSet16(cs, selector);
        CPU_LOG("CPU_CALL: Real/VM mode, set CS=0x%x, IP=0x%x", SegValue(cs), reg_eip);
        return;
    } else {
        CPU_CHECK_COND((selector & 0xfffc) == 0,
            "CALL:CS selector zero",
            EXCEPTION_GP, 0)
        Bitu rpl = selector & 3;
        Descriptor call;
        CPU_CHECK_COND(!cpu.gdt.GetDescriptor(selector, call),
            "CALL:CS beyond limits",
            EXCEPTION_GP, selector & 0xfffc)
        switch (call.Type()) {
            case DESC_CODE_N_NC_A: case DESC_CODE_N_NC_NA:
            case DESC_CODE_R_NC_A: case DESC_CODE_R_NC_NA:
                CPU_CHECK_COND(rpl > cpu.cpl,
                    "CALL:CODE:NC:RPL>CPL",
                    EXCEPTION_GP, selector & 0xfffc)
                CPU_CHECK_COND(call.DPL() != cpu.cpl,
                    "CALL:CODE:NC:DPL!=CPL",
                    EXCEPTION_GP, selector & 0xfffc)
                CPU_LOG("CPU_CALL: CODE:NC to %lX:%lX", selector, offset);
                goto call_code;
            case DESC_CODE_N_C_A: case DESC_CODE_N_C_NA:
            case DESC_CODE_R_C_A: case DESC_CODE_R_C_NA:
                CPU_CHECK_COND(call.DPL() > cpu.cpl,
                    "CALL:CODE:C:DPL>CPL",
                    EXCEPTION_GP, selector & 0xfffc)
                CPU_LOG("CPU_CALL: CODE:C to %lX:%lX", selector, offset);
call_code:
                if (!call.saved.seg.p) {
                    CPU_Exception(EXCEPTION_NP, selector & 0xfffc);
                    return;
                }
                if (!use32) {
                    CPU_Push16(SegValue(cs));
                    CPU_Push16(oldeip);
                    reg_eip = offset & 0xffff;
                } else {
                    CPU_Push32(SegValue(cs));
                    CPU_Push32(oldeip);
                    reg_eip = offset;
                }
                Segs.phys[cs] = call.GetBase();
                cpu.code.big = call.Big() > 0;
                Segs.val[cs] = (selector & 0xfffc) | cpu.cpl;
                CPU_LOG("CPU_CALL: Set CS=0x%x, IP=0x%x, big=%d", SegValue(cs), reg_eip, cpu.code.big);
                return;
            case DESC_386_CALL_GATE:
            case DESC_286_CALL_GATE:
            {
                CPU_CHECK_COND(call.DPL() < cpu.cpl,
                    "CALL:Gate:Gate DPL<CPL",
                    EXCEPTION_GP, selector & 0xfffc)
                CPU_CHECK_COND(call.DPL() < rpl,
                    "CALL:Gate:Gate DPL<RPL",
                    EXCEPTION_GP, selector & 0xfffc)
                CPU_CHECK_COND(!call.saved.seg.p,
                    "CALL:Gate:Segment not present",
                    EXCEPTION_NP, selector & 0xfffc)
                Descriptor n_cs_desc;
                Bitu n_cs_sel = call.GetSelector();
                CPU_CHECK_COND((n_cs_sel & 0xfffc) == 0,
                    "CALL:Gate:CS selector zero",
                    EXCEPTION_GP, 0)
                CPU_CHECK_COND(!cpu.gdt.GetDescriptor(n_cs_sel, n_cs_desc),
                    "CALL:Gate:CS beyond limits",
                    EXCEPTION_GP, n_cs_sel & 0xfffc)
                Bitu n_cs_dpl = n_cs_desc.DPL();
                CPU_CHECK_COND(n_cs_dpl > cpu.cpl,
                    "CALL:Gate:CS DPL>CPL",
                    EXCEPTION_GP, n_cs_sel & 0xfffc)
                CPU_CHECK_COND(!n_cs_desc.saved.seg.p,
                    "CALL:Gate:CS not present",
                    EXCEPTION_NP, n_cs_sel & 0xfffc)
                Bitu n_eip = call.GetOffset();
                switch (n_cs_desc.Type()) {
                    case DESC_CODE_N_NC_A: case DESC_CODE_N_NC_NA:
                    case DESC_CODE_R_NC_A: case DESC_CODE_R_NC_NA:
                        if (n_cs_dpl < cpu.cpl) {
                            Bitu n_ss_sel, n_esp;
                            Descriptor n_ss_desc;
                            cpu_tss.Get_SSx_ESPx(n_cs_dpl, n_ss_sel, n_esp);
                            CPU_CHECK_COND((n_ss_sel & 0xfffc) == 0,
                                "CALL:Gate:NC:SS selector zero",
                                EXCEPTION_TS, 0)
                            CPU_CHECK_COND(!cpu.gdt.GetDescriptor(n_ss_sel, n_ss_desc),
                                "CALL:Gate:Invalid SS selector",
                                EXCEPTION_TS, n_ss_sel & 0xfffc)
                            CPU_CHECK_COND(((n_ss_sel & 3) != n_cs_desc.DPL()) || (n_ss_desc.DPL() != n_cs_desc.DPL()),
                                "CALL:Gate:Invalid SS selector privileges",
                                EXCEPTION_TS, n_ss_sel & 0xfffc)
                            switch (n_ss_desc.Type()) {
                                case DESC_DATA_EU_RW_NA: case DESC_DATA_EU_RW_A:
                                case DESC_DATA_ED_RW_NA: case DESC_DATA_ED_RW_A:
                                    break;
                                default:
                                    E_Exit("Call:Gate:SS no writable data segment");
                            }
                            CPU_CHECK_COND(!n_ss_desc.saved.seg.p,
                                "CALL:Gate:Stack segment not present",
                                EXCEPTION_SS, n_ss_sel & 0xfffc)
                            Bitu o_esp = reg_esp;
                            Bitu o_ss = SegValue(ss);
                            PhysPt o_stack = SegPhys(ss) + (reg_esp & cpu.stack.mask);
                            if (call.saved.gate.paramcount & 31) {
                                if (call.Type() == DESC_386_CALL_GATE) {
                                    for (Bits i = (call.saved.gate.paramcount & 31) - 1; i >= 0; i--)
                                        mem_readd(o_stack + i * 4);
                                } else {
                                    for (Bits i = (call.saved.gate.paramcount & 31) - 1; i >= 0; i--)
                                        mem_readw(o_stack + i * 2);
                                }
                            }
                            Segs.val[ss] = n_ss_sel;
                            Segs.phys[ss] = n_ss_desc.GetBase();
                            if (n_ss_desc.Big()) {
                                cpu.stack.big = true;
                                cpu.stack.mask = 0xffffffff;
                                cpu.stack.notmask = 0;
                                reg_esp = n_esp;
                            } else {
                                cpu.stack.big = false;
                                cpu.stack.mask = 0xffff;
                                cpu.stack.notmask = 0xffff0000;
                                reg_sp = n_esp & 0xffff;
                            }
                            cpu.cpl = n_cs_desc.DPL();
                            Bit16u oldcs = SegValue(cs);
                            Segs.phys[cs] = n_cs_desc.GetBase();
                            Segs.val[cs] = (n_cs_sel & 0xfffc) | cpu.cpl;
                            cpu.code.big = n_cs_desc.Big() > 0;
                            reg_eip = n_eip;
                            if (!use32) reg_eip &= 0xffff;
                            if (call.Type() == DESC_386_CALL_GATE) {
                                CPU_Push32(o_ss);
                                CPU_Push32(o_esp);
                                if (call.saved.gate.paramcount & 31)
                                    for (Bits i = (call.saved.gate.paramcount & 31) - 1; i >= 0; i--)
                                        CPU_Push32(mem_readd(o_stack + i * 4));
                                CPU_Push32(oldcs);
                                CPU_Push32(oldeip);
                            } else {
                                CPU_Push16(o_ss);
                                CPU_Push16(o_esp);
                                if (call.saved.gate.paramcount & 31)
                                    for (Bits i = (call.saved.gate.paramcount & 31) - 1; i >= 0; i--)
                                        CPU_Push16(mem_readw(o_stack + i * 2));
                                CPU_Push16(oldcs);
                                CPU_Push16(oldeip);
                            }
                            CPU_LOG("CPU_CALL: Gate to inner level, set CS=0x%x, IP=0x%x, SS=0x%x, SP=0x%x", SegValue(cs), reg_eip, SegValue(ss), reg_esp);
                            break;
                        } else if (n_cs_dpl > cpu.cpl)
                            E_Exit("CALL:GATE:CS DPL>CPL");
                    case DESC_CODE_N_C_A: case DESC_CODE_N_C_NA:
                    case DESC_CODE_R_C_A: case DESC_CODE_R_C_NA:
                        if (call.Type() == DESC_386_CALL_GATE) {
                            CPU_Push32(SegValue(cs));
                            CPU_Push32(oldeip);
                        } else {
                            CPU_Push16(SegValue(cs));
                            CPU_Push16(oldeip);
                        }
                        Segs.phys[cs] = n_cs_desc.GetBase();
                        Segs.val[cs] = (n_cs_sel & 0xfffc) | cpu.cpl;
                        cpu.code.big = n_cs_desc.Big() > 0;
                        reg_eip = n_eip;
                        if (!use32) reg_eip &= 0xffff;
                        CPU_LOG("CPU_CALL: Gate to same level, set CS=0x%x, IP=0x%x", SegValue(cs), reg_eip);
                        break;
                    default:
                        E_Exit("CALL:GATE:CS no executable segment");
                }
                return;
            }
            case DESC_386_TSS_A:
                CPU_CHECK_COND(call.DPL() < cpu.cpl,
                    "CALL:TSS:dpl<cpl",
                    EXCEPTION_GP, selector & 0xfffc)
                CPU_CHECK_COND(call.DPL() < rpl,
                    "CALL:TSS:dpl<rpl",
                    EXCEPTION_GP, selector & 0xfffc)
                CPU_CHECK_COND(!call.saved.seg.p,
                    "CALL:TSS:Segment not present",
                    EXCEPTION_NP, selector & 0xfffc)
                CPU_LOG("CPU_CALL: TSS to %lX", selector);
                CPU_SwitchTask(selector, TSwitch_CALL_INT, oldeip);
                break;
            case DESC_DATA_EU_RW_NA:
            case DESC_INVALID:
                CPU_Exception(EXCEPTION_GP, selector & 0xfffc);
                return;
            default:
                E_Exit("CALL:Descriptor type %zx unsupported", static_cast<uintptr_t>(call.Type()));
        }
    }
}

void CPU_RET(bool use32, Bitu bytes, Bitu oldeip) {
    CPU_LOG("CPU_RET: use32=%d, bytes=%lu, oldeip=0x%lx", use32, bytes, oldeip);
    if (!cpu.pmode || (reg_flags & FLAG_VM)) {
        Bitu new_ip, new_cs;
        if (!use32) {
            new_ip = CPU_Pop16();
            new_cs = CPU_Pop16();
        } else {
            new_ip = CPU_Pop32();
            new_cs = CPU_Pop32() & 0xffff;
        }
        reg_esp += bytes;
        SegSet16(cs, new_cs);
        reg_eip = new_ip;
        cpu.code.big = false;
        CPU_LOG("CPU_RET: Real/VM mode, set CS=0x%x, IP=0x%x", SegValue(cs), reg_eip);
        return;
    } else {
        Bitu offset, selector;
        if (!use32) selector = mem_readw(SegPhys(ss) + (reg_esp & cpu.stack.mask) + 2);
        else selector = mem_readd(SegPhys(ss) + (reg_esp & cpu.stack.mask) + 4) & 0xffff;
        Descriptor desc;
        Bitu rpl = selector & 3;
        if (rpl < cpu.cpl) {
            CPU_Exception(EXCEPTION_GP, selector & 0xfffc);
            return;
        }
        CPU_CHECK_COND((selector & 0xfffc) == 0,
            "RET:CS selector zero",
            EXCEPTION_GP, 0)
        CPU_CHECK_COND(!cpu.gdt.GetDescriptor(selector, desc),
            "RET:CS beyond limits",
            EXCEPTION_GP, selector & 0xfffc)
        if (cpu.cpl == rpl) {
            switch (desc.Type()) {
                case DESC_CODE_N_NC_A: case DESC_CODE_N_NC_NA:
                case DESC_CODE_R_NC_A: case DESC_CODE_R_NC_NA:
                    CPU_CHECK_COND(cpu.cpl != desc.DPL(),
                        "RET to NC segment of other privilege",
                        EXCEPTION_GP, selector & 0xfffc)
                    goto RET_same_level;
                case DESC_CODE_N_C_A: case DESC_CODE_N_C_NA:
                case DESC_CODE_R_C_A: case DESC_CODE_R_C_NA:
                    CPU_CHECK_COND(desc.DPL() > cpu.cpl,
                        "RET to C segment of higher privilege",
                        EXCEPTION_GP, selector & 0xfffc)
                    break;
                default:
                    E_Exit("RET from illegal descriptor type %zX", static_cast<uintptr_t>(desc.Type()));
            }
RET_same_level:
            if (!desc.saved.seg.p) {
                CPU_Exception(EXCEPTION_NP, selector & 0xfffc);
                return;
            }
            if (!use32) {
                offset = CPU_Pop16();
                selector = CPU_Pop16();
            } else {
                offset = CPU_Pop32();
                selector = CPU_Pop32() & 0xffff;
            }
            Segs.phys[cs] = desc.GetBase();
            cpu.code.big = desc.Big() > 0;
            Segs.val[cs] = selector;
            reg_eip = offset;
            if (cpu.stack.big) {
                reg_esp += bytes;
            } else {
                reg_sp += bytes;
            }
            CPU_LOG("CPU_RET: Same level to %lX:%lX RPL %lX DPL %lX", selector, offset, rpl, desc.DPL());
            return;
        } else {
            switch (desc.Type()) {
                case DESC_CODE_N_NC_A: case DESC_CODE_N_NC_NA:
                case DESC_CODE_R_NC_A: case DESC_CODE_R_NC_NA:
                    CPU_CHECK_COND(desc.DPL() != rpl,
                        "RET to outer NC segment with DPL!=RPL",
                        EXCEPTION_GP, selector & 0xfffc)
                    break;
                case DESC_CODE_N_C_A: case DESC_CODE_N_C_NA:
                case DESC_CODE_R_C_A: case DESC_CODE_R_C_NA:
                    CPU_CHECK_COND(desc.DPL() > rpl,
                        "RET to outer C segment with DPL>RPL",
                        EXCEPTION_GP, selector & 0xfffc)
                    break;
                default:
                    E_Exit("RET from illegal descriptor type %zX", static_cast<uintptr_t>(desc.Type()));
            }
            CPU_CHECK_COND(!desc.saved.seg.p,
                "RET:Outer level:CS not present",
                EXCEPTION_NP, selector & 0xfffc)
            Bitu n_esp, n_ss;
            if (use32) {
                offset = CPU_Pop32();
                selector = CPU_Pop32() & 0xffff;
                reg_esp += bytes;
                n_esp = CPU_Pop32();
                n_ss = CPU_Pop32() & 0xffff;
            } else {
                offset = CPU_Pop16();
                selector = CPU_Pop16();
                reg_esp += bytes;
                n_esp = CPU_Pop16();
                n_ss = CPU_Pop16();
            }
            CPU_CHECK_COND((n_ss & 0xfffc) == 0,
                "RET to outer level with SS selector zero",
                EXCEPTION_GP, 0)
            Descriptor n_ss_desc;
            CPU_CHECK_COND(!cpu.gdt.GetDescriptor(n_ss, n_ss_desc),
                "RET:SS beyond limits",
                EXCEPTION_GP, n_ss & 0xfffc)
            CPU_CHECK_COND(((n_ss & 3) != rpl) || (n_ss_desc.DPL() != rpl),
                "RET to outer segment with invalid SS privileges",
                EXCEPTION_GP, n_ss & 0xfffc)
            switch (n_ss_desc.Type()) {
                case DESC_DATA_EU_RW_NA: case DESC_DATA_EU_RW_A:
                case DESC_DATA_ED_RW_NA: case DESC_DATA_ED_RW_A:
                    break;
                default:
                    E_Exit("RET:SS selector type no writable data segment");
            }
            CPU_CHECK_COND(!n_ss_desc.saved.seg.p,
                "RET:Stack segment not present",
                EXCEPTION_SS, n_ss & 0xfffc)
            cpu.cpl = rpl;
            Segs.phys[cs] = desc.GetBase();
            cpu.code.big = desc.Big() > 0;
            Segs.val[cs] = (selector & 0xfffc) | cpu.cpl;
            reg_eip = offset;
            Segs.val[ss] = n_ss;
            Segs.phys[ss] = n_ss_desc.GetBase();
            if (n_ss_desc.Big()) {
                cpu.stack.big = true;
                cpu.stack.mask = 0xffffffff;
                cpu.stack.notmask = 0;
                reg_esp = n_esp + bytes;
            } else {
                cpu.stack.big = false;
                cpu.stack.mask = 0xffff;
                cpu.stack.notmask = 0xffff0000;
                reg_sp = (n_esp & 0xffff) + bytes;
            }
            CPU_CheckSegments();
            CPU_LOG("CPU_RET: Outer level to %lX:%lX RPL %lX DPL %lX", selector, offset, rpl, desc.DPL());
            return;
        }
    }
}

inline Bitu CPU_SLDT(void) {
    return cpu.gdt.SLDT();
}

inline bool CPU_LLDT(Bitu selector) {
    CPU_LOG("CPU_LLDT: selector=0x%lx", selector);
    if (!cpu.gdt.LLDT(selector)) {
        CPU_LOG("CPU_LLDT: Failed, selector=%lX", selector);
        return true;
    }
    CPU_LOG("CPU_LLDT: Set to %lX", selector);
    return false;
}

inline Bitu CPU_STR(void) {
    return cpu_tss.selector;
}

inline bool CPU_LTR(Bitu selector) {
    CPU_LOG("CPU_LTR: selector=0x%lx", selector);
    if ((selector & 0xfffc) == 0) {
        cpu_tss.SetSelector(selector);
        return false;
    }
    TSS_Descriptor desc;
    if ((selector & 4) || (!cpu.gdt.GetDescriptor(selector, desc))) {
        CPU_LOG("CPU_LTR: Failed, selector=%lX", selector);
        return CPU_PrepareException(EXCEPTION_GP, selector);
    }
    if ((desc.Type() == DESC_286_TSS_A) || (desc.Type() == DESC_386_TSS_A)) {
        if (!desc.saved.seg.p) {
            CPU_LOG("CPU_LTR: Failed, selector=%lX (not present)", selector);
            return CPU_PrepareException(EXCEPTION_NP, selector);
        }
        if (!cpu_tss.SetSelector(selector)) E_Exit("LTR failed, selector=%zX", static_cast<uintptr_t>(selector));
        cpu_tss.desc.SetBusy(true);
        cpu_tss.SaveSelector();
    } else {
        CPU_LOG("CPU_LTR: Failed, selector=%lX (type=%lX)", selector, desc.Type());
        return CPU_PrepareException(EXCEPTION_GP, selector);
    }
    return false;
}

inline void CPU_LGDT(Bitu limit, Bitu base) {
    CPU_LOG("CPU_LGDT: base=0x%lx, limit=0x%lx", base, limit);
    cpu.gdt.SetLimit(limit);
    cpu.gdt.SetBase(base);
}

inline void CPU_LIDT(Bitu limit, Bitu base) {
    CPU_LOG("CPU_LIDT: base=0x%lx, limit=0x%lx", base, limit);
    cpu.idt.SetLimit(limit);
    cpu.idt.SetBase(base);
}

inline Bitu CPU_SGDT_base(void) {
    return cpu.gdt.GetBase();
}

inline Bitu CPU_SGDT_limit(void) {
    return cpu.gdt.GetLimit();
}

inline Bitu CPU_SIDT_base(void) {
    return cpu.idt.GetBase();
}

inline Bitu CPU_SIDT_limit(void) {
    return cpu.idt.GetLimit();
}

static bool printed_cycles_auto_info = false;
void CPU_SET_CRX(Bitu cr, Bitu value) {
    CPU_LOG("CPU_SET_CRX: cr=%ld, value=0x%lx", cr, value);
    switch (cr) {
        case 0:
        {
            value |= 0x8; // Simulate CR0_FPUENABLE (bit 3, assuming EM bit cleared)
            if (cpu.cr0 == value) return;
            Bitu changed = cpu.cr0 ^ value;
            if (changed & 0x1) { // CR0_PROTECTION is CR0_PE (bit 0)
                cpu.pmode = (value & 0x1) != 0;
                if (cpu.pmode) {
                    cpu.cpl = 0;
                    CPU_SetSegGeneral(ds, 0);
                    CPU_SetSegGeneral(es, 0);
                    CPU_SetSegGeneral(fs, 0);
                    CPU_SetSegGeneral(gs, 0);
                    CPU_SetSegGeneral(ss, 0);
                    Segs.val[cs] = 0;
                    Segs.phys[cs] = 0;
                    cpu.code.big = false;
                }
            }
            if (changed & 0x80000000) { // CR0_PAGING is CR0_PG (bit 31)
                if (value & 0x80000000) {
                    if (!(cpu.cr0 & 0x1)) {
                        LOG_MSG("Paging enabled without PE bit set, ignoring");
                        return;
                    }
                    PAGING_Enable(true);
                } else {
                    PAGING_Enable(false);
                }
            }
            cpu.cr0 = value;
            CPU_LOG("CPU_SET_CRX: Set CR0=0x%lx, pmode=%d", cpu.cr0, cpu.pmode);
            break;
        }
        default:
            E_Exit("Write %zx to unsupported control register %zx", static_cast<uintptr_t>(value), static_cast<uintptr_t>(cr));
    }
}

inline Bitu CPU_GET_CRX(Bitu cr) {
    CPU_LOG("CPU_GET_CRX: cr=%ld, returning 0x%lx", cr, cpu.cr0);
    switch (cr) {
        case 0: return cpu.cr0;
        default: E_Exit("Reading unsupported control register %zx", static_cast<uintptr_t>(cr));
    }
    return 0;
}

bool CPU_SetSegGeneral(SegNames seg,Bitu value) {
	value &= 0xffff;
	if (!cpu.pmode || (reg_flags & FLAG_VM)) {
		Segs.val[seg]=value;
		Segs.phys[seg]=value << 4;
		if (seg==ss) {
			cpu.stack.big=false;
			cpu.stack.mask=0xffff;
			cpu.stack.notmask=0xffff0000;
		}
		return false;
	} else {
		if (seg==ss) {
			// Stack needs to be non-zero
			if ((value & 0xfffc)==0) {
				E_Exit("CPU_SetSegGeneral: Stack segment zero");
//				return CPU_PrepareException(EXCEPTION_GP,0);
			}
			Descriptor desc;
			if (!cpu.gdt.GetDescriptor(value,desc)) {
				E_Exit("CPU_SetSegGeneral: Stack segment beyond limits");
//				return CPU_PrepareException(EXCEPTION_GP,value & 0xfffc);
			}
			if (((value & 3)!=cpu.cpl) || (desc.DPL()!=cpu.cpl)) {
				E_Exit("CPU_SetSegGeneral: Stack segment with invalid privileges");
//				return CPU_PrepareException(EXCEPTION_GP,value & 0xfffc);
			}

			switch (desc.Type()) {
			case DESC_DATA_EU_RW_NA:		case DESC_DATA_EU_RW_A:
			case DESC_DATA_ED_RW_NA:		case DESC_DATA_ED_RW_A:
				break;
			default:
				//Earth Siege 1
				return CPU_PrepareException(EXCEPTION_GP,value & 0xfffc);
			}

			if (!desc.saved.seg.p) {
//				E_Exit("CPU_SetSegGeneral: Stack segment not present");	// or #SS(sel)
				return CPU_PrepareException(EXCEPTION_SS,value & 0xfffc);
			}

			Segs.val[seg]=value;
			Segs.phys[seg]=desc.GetBase();
			if (desc.Big()) {
				cpu.stack.big=true;
				cpu.stack.mask=0xffffffff;
				cpu.stack.notmask=0;
			} else {
				cpu.stack.big=false;
				cpu.stack.mask=0xffff;
				cpu.stack.notmask=0xffff0000;
			}
		} else {
			if ((value & 0xfffc)==0) {
				Segs.val[seg]=value;
				Segs.phys[seg]=0;	// ??
				return false;
			}
			Descriptor desc;
			if (!cpu.gdt.GetDescriptor(value,desc)) {
				return CPU_PrepareException(EXCEPTION_GP,value & 0xfffc);
			}
			switch (desc.Type()) {
			case DESC_DATA_EU_RO_NA:		case DESC_DATA_EU_RO_A:
			case DESC_DATA_EU_RW_NA:		case DESC_DATA_EU_RW_A:
			case DESC_DATA_ED_RO_NA:		case DESC_DATA_ED_RO_A:
			case DESC_DATA_ED_RW_NA:		case DESC_DATA_ED_RW_A:
			case DESC_CODE_R_NC_A:			case DESC_CODE_R_NC_NA:
				if (((value & 3)>desc.DPL()) || (cpu.cpl>desc.DPL())) {
					// extreme pinball
					return CPU_PrepareException(EXCEPTION_GP,value & 0xfffc);
				}
				break;
			case DESC_CODE_R_C_A:			case DESC_CODE_R_C_NA:
				break;
			default:
				// gabriel knight
				return CPU_PrepareException(EXCEPTION_GP,value & 0xfffc);

			}
			if (!desc.saved.seg.p) {
				// win
				return CPU_PrepareException(EXCEPTION_NP,value & 0xfffc);
			}

			Segs.val[seg]=value;
			Segs.phys[seg]=desc.GetBase();
		}

		return false;
	}
}

PhysPt SegPhys(Bitu seg) {
    return Segs.phys[seg];
}

Bitu SegValue(Bitu seg) {
    return Segs.val[seg];
}

void CPU_SetupFPU(bool force) {
    CPU_LOG("CPU_SetupFPU: force=%d", force);
    // Placeholder: FPU setup not implemented
    CPU_LOG("CPU_SetupFPU: FPU setup skipped (not implemented)");
}

void CPU_FPU_ESC0(Bitu op1, Bitu rm) {
    CPU_LOG("CPU_FPU_ESC0: op1=0x%lx, rm=0x%lx", op1, rm);
    // Placeholder: FPU instruction handling not implemented
}

void CPU_FPU_ESC1(Bitu op1, Bitu rm) {
    CPU_LOG("CPU_FPU_ESC1: op1=0x%lx, rm=0x%lx", op1, rm);
    // Placeholder: FPU instruction handling not implemented
}

// Continue with other FPU escape functions (ESC2 to ESC7) as needed
void CPU_FPU_ESC2(Bitu op1, Bitu rm) { CPU_LOG("CPU_FPU_ESC2: op1=0x%lx, rm=0x%lx", op1, rm); }
void CPU_FPU_ESC3(Bitu op1, Bitu rm) { CPU_LOG("CPU_FPU_ESC3: op1=0x%lx, rm=0x%lx", op1, rm); }
void CPU_FPU_ESC4(Bitu op1, Bitu rm) { CPU_LOG("CPU_FPU_ESC4: op1=0x%lx, rm=0x%lx", op1, rm); }
void CPU_FPU_ESC5(Bitu op1, Bitu rm) { CPU_LOG("CPU_FPU_ESC5: op1=0x%lx, rm=0x%lx", op1, rm); }
void CPU_FPU_ESC6(Bitu op1, Bitu rm) { CPU_LOG("CPU_FPU_ESC6: op1=0x%lx, rm=0x%lx", op1, rm); }
void CPU_FPU_ESC7(Bitu op1, Bitu rm) { CPU_LOG("CPU_FPU_ESC7: op1=0x%lx, rm=0x%lx", op1, rm); }

void CPU_HLT(Bitu oldeip) {
    CPU_LOG("CPU_HLT: oldeip=0x%lx", oldeip);
    if (cpu.pmode && cpu.cpl != 0) {
        CPU_LOG("CPU_HLT: HLT in pmode with CPL=%ld, raising #GP", cpu.cpl);
        CPU_Exception(EXCEPTION_GP, 0);
        return;
    }
    reg_eip = oldeip;
    // Simulate CPU_IODelay(100) with a placeholder
    CPU_Cycles = 0;
    CPU_LOG("CPU_HLT: Halted");
}

void CPU_DebugException(void) {
    CPU_LOG("CPU_DebugException");
    cpu.exception.which = 1; // Simulate EXCEPTION_DB (debug exception)
    CPU_Interrupt(1, CPU_INT_EXCEPTION, reg_eip);
}

void CPU_Cycles_AutoAdjust(void) {
    if (!CPU_CycleAutoAdjust) return;
    if (!printed_cycles_auto_info) {
        printed_cycles_auto_info = true;
        LOG_MSG("Cycles: Auto adjustment enabled");
    }
    // Placeholder for cycle adjustment logic
    CPU_LOG("CPU_Cycles_AutoAdjust: Adjusting cycles");
}

void CPU_SetCycleMax(Bitu cycles) {
    CPU_LOG("CPU_SetCycleMax: cycles=%ld", cycles);
    CPU_CycleMax = cycles;
    CPU_CycleLeft = 0;
    CPU_Cycles = 0;
    if (CPU_CycleAutoAdjust) {
        CPU_CyclePercUsed = 100;
    }
    GFX_SetTitle(CPU_CycleMax, -1, false);
    CPU_LOG("CPU_SetCycleMax: Set to %d", CPU_CycleMax);
}

void CPU_SetCyclePerc(int perc) {
    CPU_LOG("CPU_SetCyclePerc: perc=%d", perc);
    if (perc < 1) perc = 1;
    if (perc > 1000) perc = 1000;
    CPU_CyclePercUsed = perc;
    CPU_SetCycleMax((CPU_CycleMax * perc) / 100);
    CPU_LOG("CPU_SetCyclePerc: Set to %d%%, new max=%d", perc, CPU_CycleMax);
}

void CPU_Change_Config(Section* newconfig) {
    CPU_LOG("CPU_Change_Config: newconfig=%p", newconfig);
    if (!newconfig) {
        CPU_LOG("CPU_Change_Config: Null config, aborting");
        return;
    }
    Section_prop* section = static_cast<Section_prop*>(newconfig);
    if (!section) {
        CPU_LOG("CPU_Change_Config: Invalid section type, aborting");
        return;
    }

    // Log available properties for debugging
    int prop_count = 0;
    while (section->Get_prop(prop_count)) {
        Property* prop = section->Get_prop(prop_count);
        CPU_LOG("CPU_Change_Config: Property %d: %s", prop_count, prop->GetValue().ToString().c_str());
        prop_count++;
    }
    CPU_LOG("CPU_Change_Config: Total properties found: %d", prop_count);

    // Get properties with fallback to defaults if missing
    Property* p_core = section->Get_prop(0);      // core
    Property* p_cycles = section->Get_prop(1);    // cycles
    Property* p_cycleup = section->Get_prop(2);   // cycleup
    Property* p_cycledown = section->Get_prop(3); // cycledown
    Property* p_arch = section->Get_prop(4);      // cputype

    // Default values
    std::string core = p_core ? p_core->GetValue().ToString() : "auto";
    std::string cputype = p_arch ? p_arch->GetValue().ToString() : "auto";
    int cycles = p_cycles ? static_cast<int>(p_cycles->GetValue()) : 3000;
    int cycleup = p_cycleup ? static_cast<int>(p_cycleup->GetValue()) : 100;
    int cycledown = p_cycledown ? static_cast<int>(p_cycledown->GetValue()) : 100;

    if (!p_core || !p_cycles || !p_cycleup || !p_cycledown || !p_arch) {
        CPU_LOG("CPU_Change_Config: Some properties missing, using defaults: core=%s, cputype=%s, cycles=%d, cycleup=%d, cycledown=%d",
                core.c_str(), cputype.c_str(), cycles, cycleup, cycledown);
    } else {
        CPU_LOG("CPU_Change_Config: core=%s, cputype=%s, cycles=%d, cycleup=%d, cycledown=%d",
                core.c_str(), cputype.c_str(), cycles, cycleup, cycledown);
    }

    if (core == "auto") {
        CPU_AutoDetermineMode |= CPU_AUTODETERMINE_CORE;
    } else if (core == "normal") {
        cpudecoder = &CPU_Core_Normal_Run;
        CPU_AutoDetermineMode &= ~CPU_AUTODETERMINE_CORE;
    } else if (core == "simple") {
        cpudecoder = &CPU_Core_Simple_Run;
        CPU_AutoDetermineMode &= ~CPU_AUTODETERMINE_CORE;
    } else if (core == "full") {
        cpudecoder = &CPU_Core_Full_Run;
        CPU_AutoDetermineMode &= ~CPU_AUTODETERMINE_CORE;
    }
#if (C_DYNAMIC_X86)
    else if (core == "dynamic") {
        cpudecoder = &CPU_Core_Dyn_X86_Run;
        CPU_AutoDetermineMode &= ~CPU_AUTODETERMINE_CORE;
    }
#endif
#if (C_DYNREC)
    else if (core == "dynrec") {
        cpudecoder = &CPU_Core_Dynrec_Run;
        CPU_AutoDetermineMode &= ~CPU_AUTODETERMINE_CORE;
    }
#endif
    else {
        CPU_LOG("CPU_Change_Config: Unknown core: %s, defaulting to auto", core.c_str());
        CPU_AutoDetermineMode |= CPU_AUTODETERMINE_CORE;
    }

    if (cputype == "auto") {
        CPU_ArchitectureType = CPU_ARCHTYPE_MIXED;
    } else if (cputype == "386" || cputype == "386_fast") {
        CPU_ArchitectureType = CPU_ARCHTYPE_386FAST;
    } else if (cputype == "386_prefetch") {
        CPU_ArchitectureType = CPU_ARCHTYPE_386FAST;
        CPU_PrefetchQueueSize = 16;
    } else if (cputype == "386_slow") {
        CPU_ArchitectureType = CPU_ARCHTYPE_386SLOW;
    } else if (cputype == "486" || cputype == "486_prefetch") {
        CPU_ArchitectureType = CPU_ARCHTYPE_486NEW;
        if (cputype == "486_prefetch") CPU_PrefetchQueueSize = 16;
    } else if (cputype == "486_slow") {
        CPU_ArchitectureType = CPU_ARCHTYPE_486OLD;
    } else if (cputype == "pentium") {
        CPU_ArchitectureType = CPU_ARCHTYPE_PENTIUM;
    } else if (cputype == "pentium_mmx") {
        CPU_ArchitectureType = CPU_ARCHTYPE_P55C;
    } else {
        CPU_LOG("CPU_Change_Config: Unknown cputype: %s, defaulting to auto", cputype.c_str());
        CPU_ArchitectureType = CPU_ARCHTYPE_MIXED;
    }

    CPU_CycleMax = cycles;
    CPU_CycleUp = cycleup;
    CPU_CycleDown = cycledown;

    if (CPU_CycleMax <= 0) {
        CPU_CycleAutoAdjust = true;
        CPU_CycleMax = 3000;
        CPU_CyclePercUsed = 100;
    } else {
        CPU_CycleAutoAdjust = false;
    }

    CPU_SetCycleMax(CPU_CycleMax);
    CPU_LOG("CPU_Change_Config: Configuration applied successfully");
}

void CPU_Init(Section* sec) {
    CPU_LOG("CPU_Init: section=%p", sec);
    CPU_Change_Config(sec);
    // Skip FPU setup (not implemented)
    cpu.cr0 = 0x8 | 0x2; // Simulate CR0_FPUENABLE | CR0_MONITORPROCESSOR
    cpu.cpl = 0;
    cpu.pmode = false;
    cpu.stack.mask = 0xffff;
    cpu.stack.notmask = 0xffff0000;
    cpu.stack.big = false;
    cpu.code.big = false;
    Segs.val[cs] = 0xf000;
    Segs.phys[cs] = 0xffff0000;
    reg_eip = 0xfff0;
    reg_flags = FLAG_IF;
    CPU_SetSegGeneral(ds, 0);
    CPU_SetSegGeneral(es, 0);
    CPU_SetSegGeneral(fs, 0);
    CPU_SetSegGeneral(gs, 0);
    CPU_SetSegGeneral(ss, 0);
    CPU_Cycles = 0;
    CPU_CycleLeft = 0;
    CPU_IODelayRemoved = 0;
    CPU_PrefetchQueueSize = 0;
    CPU_LOG("CPU_Init: CPU initialized, CS=0x%x, IP=0x%x", SegValue(cs), reg_eip);
}

void CPU_ShutDown(Section* sec) {
    CPU_LOG("CPU_ShutDown: section=%p", sec);
#if (C_DYNAMIC_X86)
    CPU_Core_Dyn_X86_Cache_Close();
#endif
#if (C_DYNREC)
    CPU_Core_Dynrec_Cache_Close();
#endif
    CPU_LOG("CPU_ShutDown: CPU shut down");
}

void init_dosbox_cpu(void) {
    CPU_LOG("init_dosbox_cpu: Initializing CPU");
    // Access CPU section without 'control' (assume global or alternative access)
    Section* sec = static_cast<Section_prop*>(NULL); // Placeholder: Replace with actual section retrieval
    if (!sec) {
        E_Exit("No CPU section found in configuration");
    }
    CPU_Init(sec);
    CPU_LOG("init_dosbox_cpu: CPU initialization complete");
}