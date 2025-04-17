/*
 *  Copyright (C) 2002-2013  The DOSBox Team
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

#ifndef DOSBOX_MEM_H
#define DOSBOX_MEM_H

#ifndef DOSBOX_DOSBOX_H
#include "dosbox.h"
#endif

#include <endian.h>

typedef Bit32u PhysPt;
typedef Bit8u* HostPt;
typedef Bit32u RealPt;

typedef Bit32s MemHandle;

constexpr Bit32u MEM_PAGESIZE = 4096;

extern HostPt MemBase;
HostPt GetMemBase(void);

bool MEM_A20_Enabled(void);
void MEM_A20_Enable(bool enable);

HostPt MEM_GetBlockPage(void);
Bitu MEM_FreeTotal(void);
Bitu MEM_FreeLargest(void);
Bitu MEM_TotalPages(void);
Bitu MEM_AllocatedPages(MemHandle handle);
MemHandle MEM_AllocatePages(Bitu pages, bool sequence);
MemHandle MEM_GetNextFreePage(void);
PhysPt MEM_AllocatePage(void);
void MEM_ReleasePages(MemHandle handle);
bool MEM_ReAllocatePages(MemHandle& handle, Bitu pages, bool sequence);

MemHandle MEM_NextHandle(MemHandle handle);
MemHandle MEM_NextHandleAt(MemHandle handle, Bitu where);

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "Big-endian platforms not supported in this optimized build");

[[gnu::always_inline]] static inline Bit8u host_readb(HostPt off) {
    return *(Bit8u*)off;
}
[[gnu::always_inline]] static inline Bit16u host_readw(HostPt off) {
    return *(Bit16u*)off;
}
[[gnu::always_inline]] static inline Bit32u host_readd(HostPt off) {
    return *(Bit32u*)off;
}
[[gnu::always_inline]] static inline void host_writeb(HostPt off, Bit8u val) {
    *(Bit8u*)off = val;
}
[[gnu::always_inline]] static inline void host_writew(HostPt off, Bit16u val) {
    *(Bit16u*)off = val;
}
[[gnu::always_inline]] static inline void host_writed(HostPt off, Bit32u val) {
    *(Bit32u*)off = val;
}

[[gnu::always_inline]] static inline void var_write(Bit8u* var, Bit8u val) {
    host_writeb((HostPt)var, val);
}
[[gnu::always_inline]] static inline void var_write(Bit16u* var, Bit16u val) {
    host_writew((HostPt)var, val);
}
[[gnu::always_inline]] static inline void var_write(Bit32u* var, Bit32u val) {
    host_writed((HostPt)var, val);
}

Bit8u mem_readb(PhysPt pt);
Bit16u mem_readw(PhysPt pt);
Bit32u mem_readd(PhysPt pt);

void mem_writeb(PhysPt pt, Bit8u val);
void mem_writew(PhysPt pt, Bit16u val);
void mem_writed(PhysPt pt, Bit32u val);

[[gnu::always_inline]] static inline void phys_writeb(PhysPt addr, Bit8u val) {
    HostPt ptr = MemBase + addr;
    __builtin_prefetch(ptr + 1, 1, 1);
    host_writeb(ptr, val);
}
[[gnu::always_inline]] static inline void phys_writew(PhysPt addr, Bit16u val) {
    HostPt ptr = MemBase + addr;
    __builtin_prefetch(ptr + 2, 1, 1);
    host_writew(ptr, val);
}
[[gnu::always_inline]] static inline void phys_writed(PhysPt addr, Bit32u val) {
    HostPt ptr = MemBase + addr;
    __builtin_prefetch(ptr + 4, 1, 1);
    host_writed(ptr, val);
}

[[gnu::always_inline]] static inline Bit8u phys_readb(PhysPt addr) {
    HostPt ptr = MemBase + addr;
    __builtin_prefetch(ptr + 1, 0, 1);
    return host_readb(ptr);
}
[[gnu::always_inline]] static inline Bit16u phys_readw(PhysPt addr) {
    HostPt ptr = MemBase + addr;
    __builtin_prefetch(ptr + 2, 0, 1);
    return host_readw(ptr);
}
[[gnu::always_inline]] static inline Bit32u phys_readd(PhysPt addr) {
    HostPt ptr = MemBase + addr;
    __builtin_prefetch(ptr + 4, 0, 1);
    return host_readd(ptr);
}

void MEM_BlockWrite(PhysPt pt, void const* const data, Bitu size);
void MEM_BlockRead(PhysPt pt, void* data, Bitu size);
void MEM_BlockCopy(PhysPt dest, PhysPt src, Bitu size);
void MEM_StrCopy(PhysPt pt, char* data, Bitu size);

void mem_memcpy(PhysPt dest, PhysPt src, Bitu size);
Bitu mem_strlen(PhysPt pt);
void mem_strcpy(PhysPt dest, PhysPt src);

[[gnu::always_inline]] static inline Bit8u real_readb(Bit16u seg, Bit16u off) {
    return mem_readb((seg << 4) + off);
}
[[gnu::always_inline]] static inline Bit16u real_readw(Bit16u seg, Bit16u off) {
    return mem_readw((seg << 4) + off);
}
[[gnu::always_inline]] static inline Bit32u real_readd(Bit16u seg, Bit16u off) {
    return mem_readd((seg << 4) + off);
}

[[gnu::always_inline]] static inline void real_writeb(Bit16u seg, Bit16u off, Bit8u val) {
    if (!MemBase) {
        LOG_MSG("real_writeb: Memory not initialized, skipping write to %04x:%04x", seg, off);
        return;
    }
    mem_writeb((seg << 4) + off, val);
}
[[gnu::always_inline]] static inline void real_writew(Bit16u seg, Bit16u off, Bit16u val) {
    if (!MemBase) {
        LOG_MSG("real_writew: Memory not initialized, skipping write to %04x:%04x", seg, off);
        return;
    }
    mem_writew((seg << 4) + off, val);
}
[[gnu::always_inline]] static inline void real_writed(Bit16u seg, Bit16u off, Bit32u val) {
    if (!MemBase) {
        LOG_MSG("real_writed: Memory not initialized, skipping write to %04x:%04x", seg, off);
        return;
    }
    mem_writed((seg << 4) + off, val);
}

[[gnu::always_inline]] static inline Bit16u RealSeg(RealPt pt) {
    return (Bit16u)(pt >> 16);
}

[[gnu::always_inline]] static inline Bit16u RealOff(RealPt pt) {
    return (Bit16u)(pt & 0xffff);
}

[[gnu::always_inline]] static inline PhysPt Real2Phys(RealPt pt) {
    return (RealSeg(pt) << 4) + RealOff(pt);
}

[[gnu::always_inline]] static inline PhysPt PhysMake(Bit16u seg, Bit16u off) {
    return (seg << 4) + off;
}

[[gnu::always_inline]] static inline RealPt RealMake(Bit16u seg, Bit16u off) {
    return (seg << 16) + off;
}

[[gnu::always_inline]] static inline void RealSetVec(Bit8u vec, RealPt pt) {
    mem_writed(vec << 2, pt);
}

[[gnu::always_inline]] static inline void RealSetVec(Bit8u vec, RealPt pt, RealPt& old) {
    old = mem_readd(vec << 2);
    mem_writed(vec << 2, pt);
}

[[gnu::always_inline]] static inline RealPt RealGetVec(Bit8u vec) {
    return mem_readd(vec << 2);
}

#endif