/* SPDX-License-Identifier: GPL-2.0-only */
/* Device-tree parsing only; see NativeMaliBoard.h for the pinned facts. The
 * rules are NativeAdrenoBoard's: validate everything, refuse instead of
 * guessing, and write nothing when a parse fails. */
#include "NativeMaliBoard.h"
#include <string.h>

enum { CellBytes = 4 };

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|uint32_t(p[3]);
}

static bool nameIs(const char *text,size_t bytes,const char *wanted)
{
    const size_t length=strlen(wanted);
    return bytes==length && memcmp(text,wanted,length)==0;
}

IOReturn NativeMaliBoard::parseReg(const uint8_t *cells,size_t length,
    uint32_t address_cells,uint32_t size_cells,NativeMaliWindow *windows,
    size_t capacity,size_t *count)
{
    if(!count)return kIOReturnBadArgument;
    *count=0;
    if(!cells || !length || (address_cells<1 || address_cells>2) ||
        (size_cells<1 || size_cells>2))return kIOReturnBadArgument;
    const size_t per_window=(address_cells+size_cells)*(size_t)CellBytes;
    if(length%per_window)return kIOReturnBadArgument;
    const size_t total=length/per_window;
    if(!windows)return kIOReturnBadArgument;
    if(total>capacity || total>NativeMaliMaxWindows)return kIOReturnNoResources;
    NativeMaliWindow parsed[NativeMaliMaxWindows];
    for(size_t index=0;index<total;++index) {
        const uint8_t *at=cells+index*per_window;
        uint64_t base=0,bytes=0;
        for(uint32_t cell=0;cell<address_cells;++cell) {
            if(base>>32)return kIOReturnBadArgument; /* more than 64 bits */
            base=(base<<32)|be32(at+cell*CellBytes);
        }
        for(uint32_t cell=0;cell<size_cells;++cell) {
            if(bytes>>32)return kIOReturnBadArgument;
            bytes=(bytes<<32)|be32(at+(address_cells+cell)*CellBytes);
        }
        if(!bytes || base+bytes<base)return kIOReturnBadArgument;
        parsed[index]={base,bytes};
    }
    /* Only now, with every window validated, does the caller's buffer change. */
    for(size_t index=0;index<total;++index)windows[index]=parsed[index];
    *count=total;
    return kIOReturnSuccess;
}

size_t NativeMaliBoard::stringCount(const uint8_t *names,size_t length)
{
    if(!names || !length)return 0;
    size_t count=0,at=0;
    while(at<length) {
        const uint8_t *nul=static_cast<const uint8_t *>(memchr(names+at,0,length-at));
        if(!nul || nul==names+at)return 0; /* truncated or empty string */
        ++count;
        at=size_t(nul-names)+1;
    }
    return at==length?count:0;
}

IOReturn NativeMaliBoard::stringAt(const uint8_t *names,size_t length,size_t index,
    const char **text,size_t *bytes)
{
    if(!names || !length || !text || !bytes)return kIOReturnBadArgument;
    size_t at=0;
    for(size_t i=0;i<=index;++i) {
        const uint8_t *nul=static_cast<const uint8_t *>(memchr(names+at,0,length-at));
        if(!nul || nul==names+at)return kIOReturnBadArgument;
        if(i==index) {
            *text=reinterpret_cast<const char *>(names+at);
            *bytes=size_t(nul-(names+at));
            return kIOReturnSuccess;
        }
        at=size_t(nul-names)+1;
    }
    return kIOReturnBadArgument;
}

IOReturn NativeMaliBoard::parseInterrupts(const uint8_t *cells,size_t length,
    uint32_t cells_per_entry,const uint8_t *names,size_t names_length,
    NativeMaliInterruptSet *set)
{
    if(!set)return kIOReturnBadArgument;
    if(!cells || !length || !names || !names_length)return kIOReturnBadArgument;
    /* MT6768's GIC specifier is <type number flags>; a different width means a
     * different binding, so it is refused rather than reinterpreted. */
    if(cells_per_entry!=3)return kIOReturnUnsupported;
    const size_t per_entry=3*(size_t)CellBytes;
    if(length%per_entry)return kIOReturnBadArgument;
    const size_t entries=length/per_entry;
    if(entries!=NativeMaliIrqCount)return kIOReturnUnsupported;
    if(stringCount(names,names_length)!=entries)return kIOReturnUnsupported;
    NativeMaliInterruptSet parsed{};
    uint32_t seen=0;
    for(size_t index=0;index<entries;++index) {
        const char *text=nullptr;
        size_t bytes=0;
        if(stringAt(names,names_length,index,&text,&bytes))return kIOReturnBadArgument;
        int slot=-1;
        if(nameIs(text,bytes,"GPU"))slot=NativeMaliIrqGpu;
        else if(nameIs(text,bytes,"MMU"))slot=NativeMaliIrqMmu;
        else if(nameIs(text,bytes,"JOB"))slot=NativeMaliIrqJob;
        else if(nameIs(text,bytes,"EVENT"))slot=NativeMaliIrqEvent;
        else if(nameIs(text,bytes,"PWR"))slot=NativeMaliIrqPwr;
        if(slot<0)return kIOReturnUnsupported;          /* a source we do not know */
        if(seen&(1u<<slot))return kIOReturnBadArgument; /* named twice */
        seen|=1u<<slot;
        const uint8_t *at=cells+index*per_entry;
        const uint32_t type=be32(at),number=be32(at+CellBytes),flags=be32(at+2*CellBytes);
        if(type>1 || number>1023)return kIOReturnBadArgument; /* SPI/PPI range only */
        parsed.entry[slot]={type,number,flags};
    }
    if(seen!=(1u<<NativeMaliIrqCount)-1)return kIOReturnUnsupported;
    *set=parsed;
    return kIOReturnSuccess;
}

IOReturn NativeMaliBoard::checkTriggers(const NativeMaliInterruptSet *set)
{
    if(!set)return kIOReturnBadArgument;
    for(unsigned slot=0;slot<NativeMaliIrqCount;++slot) {
        const NativeMaliInterrupt &irq=set->entry[slot];
        /* GIC SPI (type 0) numbered from 32, asserted level-low. */
        if(irq.type!=0 || irq.flags!=NativeMaliIrqLevelLow || irq.number<32)
            return kIOReturnUnsupported;
    }
    return kIOReturnSuccess;
}

IOReturn NativeMaliBoard::parseClocks(const uint8_t *cells,size_t length,
    uint32_t cells_per_entry,const uint8_t *names,size_t names_length,
    NativeMaliClock *clocks,size_t capacity,size_t *count)
{
    if(!count)return kIOReturnBadArgument;
    *count=0;
    if(!cells || !length || !names || !names_length)return kIOReturnBadArgument;
    if(cells_per_entry>2)return kIOReturnUnsupported; /* only the first argument is kept */
    const size_t per_entry=(1+cells_per_entry)*(size_t)CellBytes;
    if(length%per_entry)return kIOReturnBadArgument;
    const size_t entries=length/per_entry;
    if(!clocks)return kIOReturnBadArgument;
    if(entries>capacity || entries>NativeMaliMaxClocks)return kIOReturnNoResources;
    if(stringCount(names,names_length)!=entries)return kIOReturnUnsupported;
    NativeMaliClock parsed[NativeMaliMaxClocks]{};
    const char *text[NativeMaliMaxClocks]={};
    size_t text_bytes[NativeMaliMaxClocks]={};
    for(size_t index=0;index<entries;++index) {
        if(stringAt(names,names_length,index,&text[index],&text_bytes[index]))
            return kIOReturnBadArgument;
        for(size_t other=0;other<index;++other)
            if(text_bytes[other]==text_bytes[index] &&
                memcmp(text[other],text[index],text_bytes[index])==0)
                return kIOReturnBadArgument; /* duplicate clock name */
        const uint8_t *at=cells+index*per_entry;
        const uint32_t phandle=be32(at);
        if(!phandle || phandle==0xffffffffu)return kIOReturnBadArgument;
        const uint32_t id=cells_per_entry?be32(at+CellBytes):0;
        parsed[index]={phandle,id};
    }
    for(size_t index=0;index<entries;++index)clocks[index]=parsed[index];
    *count=entries;
    return kIOReturnSuccess;
}
