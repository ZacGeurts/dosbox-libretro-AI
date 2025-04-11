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

#ifndef DOSBOX_PAGING_H
#define DOSBOX_PAGING_H

#ifndef DOSBOX_DOSBOX_H
#include "dosbox.h"
#endif
#ifndef DOSBOX_MEM_H
#include "mem.h"
#endif

#ifndef GEKKO
#define USE_FULL_TLB
#endif

class PageDirectory;

#define MEM_PAGE_SIZE	(4096)
#define XMS_START		(0x110)

#if defined(USE_FULL_TLB)
#define TLB_SIZE		(1024*1024)
#else
#define TLB_SIZE		65536
#define BANK_SHIFT		28
#define BANK_MASK		0xffff
#define TLB_BANKS		((1024*1024/TLB_SIZE)-1)
#endif

#define PFLAG_READABLE		0x1
#define PFLAG_WRITEABLE		0x2
#define PFLAG_HASROM		0x4
#define PFLAG_HASCODE		0x8
#define PFLAG_NOCODE		0x10
#define PFLAG_INIT			0x20

#define LINK_START	((1024+64)/4)
#define PAGING_LINKS (128*1024/4)

class PageHandler {
public:
    virtual ~PageHandler(void) { }
    virtual Bitu readb(PhysPt addr);
    virtual Bitu readw(PhysPt addr);
    virtual Bitu readd(PhysPt addr);
    virtual void writeb(PhysPt addr,Bitu val);
    virtual void writew(PhysPt addr,Bitu val);
    virtual void writed(PhysPt addr,Bitu val);
    virtual HostPt GetHostReadPt(Bitu phys_page);
    virtual HostPt GetHostWritePt(Bitu phys_page);
    virtual bool readb_checked(PhysPt addr,Bit8u * val);
    virtual bool readw_checked(PhysPt addr,Bit16u * val);
    virtual bool readd_checked(PhysPt addr,Bit32u * val);
    virtual bool writeb_checked(PhysPt addr,Bitu val);
    virtual bool writew_checked(PhysPt addr,Bitu val);
    virtual bool writed_checked(PhysPt addr,Bitu val);
    Bitu flags;
};

void PAGING_Enable(bool enabled);
bool PAGING_Enabled(void);

Bitu PAGING_GetDirBase(void);
void PAGING_SetDirBase(Bitu cr3);
void PAGING_InitTLB(void);
void PAGING_ClearTLB(void);

void PAGING_LinkPage(Bitu lin_page,Bitu phys_page);
void PAGING_LinkPage_ReadOnly(Bitu lin_page,Bitu phys_page);
void PAGING_UnlinkPages(Bitu lin_page,Bitu pages);
void PAGING_MapPage(Bitu lin_page,Bitu phys_page);
bool PAGING_MakePhysPage(Bitu & page);
bool PAGING_ForcePageInit(Bitu lin_addr);

void MEM_SetLFB(Bitu page, Bitu pages, PageHandler *handler, PageHandler *mmiohandler);
void MEM_SetPageHandler(Bitu phys_page, Bitu pages, PageHandler * handler);
void MEM_ResetPageHandler(Bitu phys_page, Bitu pages);

#ifdef _MSC_VER
#pragma pack (1)
#endif
struct X86_PageEntryBlock{
#ifdef WORDS_BIGENDIAN
    Bit32u		base:20;
    Bit32u		avl:3;
    Bit32u		g:1;
    Bit32u		pat:1;
    Bit32u		d:1;
    Bit32u		a:1;
    Bit32u		pcd:1;
    Bit32u		pwt:1;
    Bit32u		us:1;
    Bit32u		wr:1;
    Bit32u		p:1;
#else
    Bit32u		p:1;
    Bit32u		wr:1;
    Bit32u		us:1;
    Bit32u		pwt:1;
    Bit32u		pcd:1;
    Bit32u		a:1;
    Bit32u		d:1;
    Bit32u		pat:1;
    Bit32u		g:1;
    Bit32u		avl:3;
    Bit32u		base:20;
#endif
} GCC_ATTRIBUTE(packed);
#ifdef _MSC_VER
#pragma pack ()
#endif

union X86PageEntry {
    Bit32u load;
    X86_PageEntryBlock block;
};

#if !defined(USE_FULL_TLB)
typedef struct {
    HostPt read;
    HostPt write;
    PageHandler * readhandler;
    PageHandler * writehandler;
    Bit32u phys_page;
} tlb_entry;
#endif

struct PagingBlock {
    Bitu			cr3;
    Bitu			cr2;
    struct {
        Bitu page;
        PhysPt addr;
    } base;
#if defined(USE_FULL_TLB)
    struct {
        HostPt read[TLB_SIZE];
        HostPt write[TLB_SIZE];
        PageHandler * readhandler[TLB_SIZE];
        PageHandler * writehandler[TLB_SIZE];
        Bit32u	phys_page[TLB_SIZE];
    } tlb;
#else
    tlb_entry tlbh[TLB_SIZE];
    tlb_entry *tlbh_banks[TLB_BANKS];
#endif
    struct {
        Bitu used;
        Bit32u entries[PAGING_LINKS];
    } links;
    Bit32u		firstmb[LINK_START];
    bool		enabled;
};

extern PagingBlock paging;

PageHandler * MEM_GetPageHandler(Bitu phys_page);

Bit16u mem_unalignedreadw(PhysPt address);
Bit32u mem_unalignedreadd(PhysPt address);
void mem_unalignedwritew(PhysPt address,Bit16u val);
void mem_unalignedwrited(PhysPt address,Bit32u val);

bool mem_unalignedreadw_checked(PhysPt address,Bit16u * val);
bool mem_unalignedreadd_checked(PhysPt address,Bit32u * val);
bool mem_unalignedwritew_checked(PhysPt address,Bit16u val);
bool mem_unalignedwrited_checked(PhysPt address,Bit32u val);

#if defined(USE_FULL_TLB)

[[gnu::always_inline, gnu::hot]] static inline HostPt get_tlb_read(PhysPt address) {
    return paging.tlb.read[address >> 12];
}
[[gnu::always_inline, gnu::hot]] static inline HostPt get_tlb_write(PhysPt address) {
    return paging.tlb.write[address >> 12];
}
[[gnu::always_inline, gnu::hot]] static inline PageHandler* get_tlb_readhandler(PhysPt address) {
    return paging.tlb.readhandler[address >> 12];
}
[[gnu::always_inline, gnu::hot]] static inline PageHandler* get_tlb_writehandler(PhysPt address) {
    return paging.tlb.writehandler[address >> 12];
}

[[gnu::always_inline]] static inline PhysPt PAGING_GetPhysicalPage(PhysPt linePage) {
    return paging.tlb.phys_page[linePage >> 12] << 12;
}

[[gnu::always_inline]] static inline PhysPt PAGING_GetPhysicalAddress(PhysPt linAddr) {
    return (paging.tlb.phys_page[linAddr >> 12] << 12) | (linAddr & 0xfff);
}

#else

void PAGING_InitTLBBank(tlb_entry **bank);

[[gnu::always_inline, gnu::hot]] static inline tlb_entry *get_tlb_entry(PhysPt address) {
    Bitu index = address >> 12;
    if (TLB_BANKS && (index > TLB_SIZE)) {
        Bitu bank = (address >> BANK_SHIFT) - 1;
        if (!paging.tlbh_banks[bank]) [[gnu::unlikely]]
            PAGING_InitTLBBank(&paging.tlbh_banks[bank]);
        return &paging.tlbh_banks[bank][index & BANK_MASK];
    }
    return &paging.tlbh[index];
}

[[gnu::always_inline]] static inline HostPt get_tlb_read(PhysPt address) {
    return get_tlb_entry(address)->read;
}
[[gnu::always_inline]] static inline HostPt get_tlb_write(PhysPt address) {
    return get_tlb_entry(address)->write;
}
[[gnu::always_inline]] static inline PageHandler* get_tlb_readhandler(PhysPt address) {
    return get_tlb_entry(address)->readhandler;
}
[[gnu::always_inline]] static inline PageHandler* get_tlb_writehandler(PhysPt address) {
    return get_tlb_entry(address)->writehandler;
}

[[gnu::always_inline]] static inline PhysPt PAGING_GetPhysicalPage(PhysPt linePage) {
    return get_tlb_entry(linePage)->phys_page << 12;
}

[[gnu::always_inline]] static inline PhysPt PAGING_GetPhysicalAddress(PhysPt linAddr) {
    return (get_tlb_entry(linAddr)->phys_page << 12) | (linAddr & 0xfff);
}
#endif

[[gnu::always_inline, gnu::hot]] static inline Bit8u mem_readb_inline(PhysPt address) {
    HostPt tlb_addr = get_tlb_read(address);
    return tlb_addr ? host_readb(tlb_addr + address) : get_tlb_readhandler(address)->readb(address);
}

[[gnu::always_inline, gnu::hot]] static inline Bit16u mem_readw_inline(PhysPt address) {
    if ((address & 0xfff) < 0xfff) [[gnu::likely]] {
        HostPt tlb_addr = get_tlb_read(address);
        return tlb_addr ? host_readw(tlb_addr + address) : get_tlb_readhandler(address)->readw(address);
    }
    return mem_unalignedreadw(address);
}

[[gnu::always_inline, gnu::hot]] static inline Bit32u mem_readd_inline(PhysPt address) {
    if ((address & 0xfff) < 0xffd) [[gnu::likely]] {
        HostPt tlb_addr = get_tlb_read(address);
        return tlb_addr ? host_readd(tlb_addr + address) : get_tlb_readhandler(address)->readd(address);
    }
    return mem_unalignedreadd(address);
}

[[gnu::always_inline, gnu::hot]] static inline void mem_writeb_inline(PhysPt address, Bit8u val) {
    HostPt tlb_addr = get_tlb_write(address);
    if (tlb_addr) [[gnu::likely]]
        host_writeb(tlb_addr + address, val);
    else
        get_tlb_writehandler(address)->writeb(address, val);
}

[[gnu::always_inline, gnu::hot]] static inline void mem_writew_inline(PhysPt address, Bit16u val) {
    if ((address & 0xfff) < 0xfff) [[gnu::likely]] {
        HostPt tlb_addr = get_tlb_write(address);
        if (tlb_addr) [[gnu::likely]]
            host_writew(tlb_addr + address, val);
        else
            get_tlb_writehandler(address)->writew(address, val);
    } else
        mem_unalignedwritew(address, val);
}

[[gnu::always_inline, gnu::hot]] static inline void mem_writed_inline(PhysPt address, Bit32u val) {
    if ((address & 0xfff) < 0xffd) [[gnu::likely]] {
        HostPt tlb_addr = get_tlb_write(address);
        if (tlb_addr) [[gnu::likely]]
            host_writed(tlb_addr + address, val);
        else
            get_tlb_writehandler(address)->writed(address, val);
    } else
        mem_unalignedwrited(address, val);
}

[[gnu::always_inline]] static inline bool mem_readb_checked(PhysPt address, Bit8u * val) {
    HostPt tlb_addr = get_tlb_read(address);
    if (tlb_addr) [[gnu::likely]] {
        *val = host_readb(tlb_addr + address);
        return false;
    }
    return get_tlb_readhandler(address)->readb_checked(address, val);
}

[[gnu::always_inline]] static inline bool mem_readw_checked(PhysPt address, Bit16u * val) {
    if ((address & 0xfff) < 0xfff) [[gnu::likely]] {
        HostPt tlb_addr = get_tlb_read(address);
        if (tlb_addr) [[gnu::likely]] {
            *val = host_readw(tlb_addr + address);
            return false;
        }
        return get_tlb_readhandler(address)->readw_checked(address, val);
    }
    return mem_unalignedreadw_checked(address, val);
}

[[gnu::always_inline]] static inline bool mem_readd_checked(PhysPt address, Bit32u * val) {
    if ((address & 0xfff) < 0xffd) [[gnu::likely]] {
        HostPt tlb_addr = get_tlb_read(address);
        if (tlb_addr) [[gnu::likely]] {
            *val = host_readd(tlb_addr + address);
            return false;
        }
        return get_tlb_readhandler(address)->readd_checked(address, val);
    }
    return mem_unalignedreadd_checked(address, val);
}

[[gnu::always_inline]] static inline bool mem_writeb_checked(PhysPt address, Bit8u val) {
    HostPt tlb_addr = get_tlb_write(address);
    if (tlb_addr) [[gnu::likely]] {
        host_writeb(tlb_addr + address, val);
        return false;
    }
    return get_tlb_writehandler(address)->writeb_checked(address, val);
}

[[gnu::always_inline]] static inline bool mem_writew_checked(PhysPt address, Bit16u val) {
    if ((address & 0xfff) < 0xfff) [[gnu::likely]] {
        HostPt tlb_addr = get_tlb_write(address);
        if (tlb_addr) [[gnu::likely]] {
            host_writew(tlb_addr + address, val);
            return false;
        }
        return get_tlb_writehandler(address)->writew_checked(address, val);
    }
    return mem_unalignedwritew_checked(address, val);
}

[[gnu::always_inline]] static inline bool mem_writed_checked(PhysPt address, Bit32u val) {
    if ((address & 0xfff) < 0xffd) [[gnu::likely]] {
        HostPt tlb_addr = get_tlb_write(address);
        if (tlb_addr) [[gnu::likely]] {
            host_writed(tlb_addr + address, val);
            return false;
        }
        return get_tlb_writehandler(address)->writed_checked(address, val);
    }
    return mem_unalignedwrited_checked(address, val);
}

#endif