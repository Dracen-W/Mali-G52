/* SPDX-License-Identifier: GPL-2.0-only */
/* NativeMaliBoard over the real MT6768 device tree and over hand-built cells.
 * No device access: this qualifies the parser only, not the board. */
#include "XnuMemoryFixture.h"
#include "fdt.h"
#include "NativeMaliBoard.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

static void put(uint8_t *at,uint32_t value)
{
    at[0]=uint8_t(value>>24);at[1]=uint8_t(value>>16);at[2]=uint8_t(value>>8);at[3]=uint8_t(value);
}
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|uint32_t(p[3]);
}
static size_t reg(uint8_t *out,const uint32_t (*cells)[4],size_t windows)
{
    for(size_t i=0;i<windows;++i)for(size_t c=0;c<4;++c)put(out+i*16+c*4,cells[i][c]);
    return windows*16;
}
static size_t interrupts(uint8_t *out,const uint32_t (*cells)[3],size_t entries)
{
    for(size_t i=0;i<entries;++i)for(size_t c=0;c<3;++c)put(out+i*12+c*4,cells[i][c]);
    return entries*12;
}
static size_t clocks(uint8_t *out,const uint32_t (*cells)[2],size_t entries)
{
    for(size_t i=0;i<entries;++i)for(size_t c=0;c<2;++c)put(out+i*8+c*4,cells[i][c]);
    return entries*8;
}
static bool containsString(const uint8_t *data,size_t bytes,const char *wanted)
{
    const size_t want=std::strlen(wanted);
    for(size_t at=0;at<bytes;) {
        const char *text=reinterpret_cast<const char *>(data+at);
        const size_t length=strnlen(text,bytes-at);
        if(length==want && std::memcmp(text,wanted,want)==0)return true;
        at+=length+1;
    }
    return false;
}
static size_t stringCountOf(const uint8_t *data,size_t bytes)
{
    size_t count=0,at=0;
    while(at<bytes) {
        const size_t length=strnlen(reinterpret_cast<const char *>(data+at),bytes-at);
        if(!length)return 0;
        ++count;
        at+=length+1;
    }
    return count;
}

/* The real property bytes of /mali@13040000 and /gpufreq in mt6768.dtb. */
static const uint32_t regCells[][4]={{0,0x13040000,0,0x4000}};
/* <0 274 8> <0 275 8> <0 276 8> <0 277 8> <0 278 8>: type 0 (SPI), flags 8. */
static const uint32_t irqCells[][3]={{0,0x112,8},{0,0x113,8},{0,0x114,8},{0,0x115,8},{0,0x116,8}};
static const char irqNames[]="GPU\0MMU\0JOB\0EVENT\0PWR";
/* <&topckgen 0x54> <&topckgen 0x26> <&topckgen 0x06> <&mfgcfg 0> then the four
 * scpsys power-domain clocks (phandle 0x27 = /scpsys@10001000). */
static const uint32_t clockCells[][2]={{0x14,0x54},{0x14,0x26},{0x14,0x06},{0x60,0x00},
    {0x27,0x09},{0x27,0x04},{0x27,0x07},{0x27,0x08}};
static const char clockNames[]="clk_mux\0clk_main_parent\0clk_sub_parent\0subsys_mfg_cg\0"
    "mtcmos_mfg_async\0mtcmos_mfg\0mtcmos_mfg_core0\0mtcmos_mfg_core1";

int main(int argc,char **argv)
{
    const char *dtbPath=argc>1?argv[1]:"tests/data/mt6768.dtb";
    static NativeMaliWindow windows[NativeMaliMaxWindows];
    NativeMaliInterruptSet set{};
    NativeMaliClock named[NativeMaliMaxClocks]{};

    /* ---- reg: the real mali window, then the other cell formats ---- */
    {
        uint8_t raw[64];
        const size_t bytes=reg(raw,regCells,1);
        size_t count=0;
        CHECK(NativeMaliBoard::parseReg(raw,bytes,2,2,windows,NativeMaliMaxWindows,&count)==kIOReturnSuccess);
        CHECK(count==1 && windows[0].base==0x13040000 && windows[0].bytes==0x4000);
        /* One-cell windows and a 2/1 node both parse. */
        uint8_t one[8];
        put(one,0x13040000);put(one+4,0x4000);
        CHECK(NativeMaliBoard::parseReg(one,8,1,1,windows,1,&count)==kIOReturnSuccess);
        CHECK(count==1 && windows[0].base==0x13040000 && windows[0].bytes==0x4000);
        uint8_t mixed[12];
        put(mixed,0);put(mixed+4,0x13040000);put(mixed+8,0x4000);
        CHECK(NativeMaliBoard::parseReg(mixed,12,2,1,windows,1,&count)==kIOReturnSuccess);
        CHECK(count==1 && windows[0].base==0x13040000 && windows[0].bytes==0x4000);
        /* Two address cells really are 64 bits wide: a base above 4 GiB keeps
         * its high word instead of being folded into the low one. */
        uint8_t wide[16];
        put(wide,1);put(wide+4,0);put(wide+8,0);put(wide+12,0x4000);
        CHECK(NativeMaliBoard::parseReg(wide,16,2,2,windows,1,&count)==kIOReturnSuccess);
        CHECK(count==1 && windows[0].base==UINT64_C(0x100000000) && windows[0].bytes==0x4000);
        /* And the size side too. */
        put(wide,0);put(wide+4,0x13040000);put(wide+8,1);put(wide+12,0);
        CHECK(NativeMaliBoard::parseReg(wide,16,2,2,windows,1,&count)==kIOReturnSuccess);
        CHECK(count==1 && windows[0].base==0x13040000 &&
            windows[0].bytes==UINT64_C(0x100000000));
    }
    /* Refusals precede any output: nothing is written on failure. */
    {
        uint8_t raw[64];
        const size_t bytes=reg(raw,regCells,1);
        size_t count=99;
        std::memset(windows,0x5a,sizeof(windows));
        CHECK(NativeMaliBoard::parseReg(nullptr,bytes,2,2,windows,1,&count)==kIOReturnBadArgument);
        CHECK(NativeMaliBoard::parseReg(raw,bytes,0,2,windows,1,&count)==kIOReturnBadArgument);
        CHECK(NativeMaliBoard::parseReg(raw,bytes,3,2,windows,1,&count)==kIOReturnBadArgument);
        CHECK(NativeMaliBoard::parseReg(raw,bytes,2,3,windows,1,&count)==kIOReturnBadArgument);
        CHECK(NativeMaliBoard::parseReg(raw,bytes,2,2,windows,0,&count)==kIOReturnNoResources);
        CHECK(NativeMaliBoard::parseReg(raw,bytes-1,2,2,windows,1,&count)==kIOReturnBadArgument);
        CHECK(NativeMaliBoard::parseReg(raw,bytes,2,2,nullptr,1,&count)==kIOReturnBadArgument);
        CHECK(NativeMaliBoard::parseReg(raw,bytes,2,2,windows,1,nullptr)==kIOReturnBadArgument);
        CHECK(windows[0].base==UINT64_C(0x5a5a5a5a5a5a5a5a));
        /* A zero size and an address wrap are refused. */
        uint8_t zero[16];
        put(zero,0);put(zero+4,0x13040000);put(zero+8,0);put(zero+12,0);
        CHECK(NativeMaliBoard::parseReg(zero,16,2,2,windows,1,&count)==kIOReturnBadArgument);
        uint8_t wrap[16];
        put(wrap,0xffffffff);put(wrap+4,0xfffff000);put(wrap+8,0);put(wrap+12,0x2000);
        CHECK(NativeMaliBoard::parseReg(wrap,16,2,2,windows,1,&count)==kIOReturnBadArgument);
        CHECK(windows[0].base==UINT64_C(0x5a5a5a5a5a5a5a5a));
        /* More windows than the caller offered, and more than the parser holds. */
        uint8_t many[16*(NativeMaliMaxWindows+1)];
        for(size_t i=0;i<NativeMaliMaxWindows+1;++i) {
            put(many+i*16,0);put(many+i*16+4,0x13040000);
            put(many+i*16+8,0);put(many+i*16+12,0x4000);
        }
        CHECK(NativeMaliBoard::parseReg(many,sizeof(many),2,2,windows,NativeMaliMaxWindows,&count)==kIOReturnNoResources);
        CHECK(NativeMaliBoard::parseReg(many,sizeof(many),2,2,windows,sizeof(many)/16,&count)==kIOReturnNoResources);
    }

    /* ---- interrupts: by name, not by property order ---- */
    {
        uint8_t raw[64];
        const size_t bytes=interrupts(raw,irqCells,5);
        const size_t names=sizeof(irqNames);
        CHECK(NativeMaliBoard::parseInterrupts(raw,bytes,3,
            reinterpret_cast<const uint8_t *>(irqNames),names,&set)==kIOReturnSuccess);
        CHECK(set.entry[NativeMaliIrqGpu].number==274);
        CHECK(set.entry[NativeMaliIrqMmu].number==275);
        CHECK(set.entry[NativeMaliIrqJob].number==276);
        CHECK(set.entry[NativeMaliIrqEvent].number==277);
        CHECK(set.entry[NativeMaliIrqPwr].number==278);
        for(unsigned i=0;i<NativeMaliIrqCount;++i) {
            CHECK(set.entry[i].type==0);
            CHECK(set.entry[i].flags==8);
            CHECK(set.entry[i].flags==NativeMaliIrqLevelLow);  /* the DT wins */
        }
        /* Every source must be an SPI asserted level-low before it is wired. */
        CHECK(NativeMaliBoard::checkTriggers(&set)==kIOReturnSuccess);
        {
            NativeMaliInterruptSet drifted=set;
            drifted.entry[NativeMaliIrqJob].flags=NativeMaliIrqLevelHigh;
            CHECK(NativeMaliBoard::checkTriggers(&drifted)==kIOReturnUnsupported);
            drifted=set;
            drifted.entry[NativeMaliIrqMmu].flags=2;   /* edge falling */
            CHECK(NativeMaliBoard::checkTriggers(&drifted)==kIOReturnUnsupported);
            drifted=set;
            drifted.entry[NativeMaliIrqGpu].type=1;    /* PPI, not SPI */
            CHECK(NativeMaliBoard::checkTriggers(&drifted)==kIOReturnUnsupported);
            drifted=set;
            drifted.entry[NativeMaliIrqPwr].number=31; /* below the SPI range */
            CHECK(NativeMaliBoard::checkTriggers(&drifted)==kIOReturnUnsupported);
            CHECK(NativeMaliBoard::checkTriggers(nullptr)==kIOReturnBadArgument);
            /* The refused copy never touched the parsed set. */
            CHECK(NativeMaliBoard::checkTriggers(&set)==kIOReturnSuccess);
        }
        /* Panfrost's order is job, mmu, gpu: a decoder that trusted position
         * would read 276/275/274 here. The names must win. */
        static const uint32_t permuted[][3]={{0,0x114,8},{0,0x113,8},{0,0x112,8},{0,0x116,8},{0,0x115,8}};
        static const char permutedNames[]="JOB\0MMU\0GPU\0PWR\0EVENT";
        uint8_t raw2[64];
        const size_t bytes2=interrupts(raw2,permuted,5);
        CHECK(NativeMaliBoard::parseInterrupts(raw2,bytes2,3,
            reinterpret_cast<const uint8_t *>(permutedNames),sizeof(permutedNames),&set)==kIOReturnSuccess);
        CHECK(set.entry[NativeMaliIrqGpu].number==274);
        CHECK(set.entry[NativeMaliIrqMmu].number==275);
        CHECK(set.entry[NativeMaliIrqJob].number==276);
        CHECK(set.entry[NativeMaliIrqEvent].number==277);
        CHECK(set.entry[NativeMaliIrqPwr].number==278);
    }
    /* Refusals, and no output on failure. */
    {
        uint8_t raw[64];
        const size_t bytes=interrupts(raw,irqCells,5);
        const uint8_t *names=reinterpret_cast<const uint8_t *>(irqNames);
        const size_t namesLength=sizeof(irqNames);
        static const char unknown[]="GPU\0MMU\0JOB\0EVENT\0GPU";      /* duplicate */
        static const char stranger[]="GPU\0MMU\0JOB\0EVENT\0XYZ";     /* unknown source */
        static const char shortList[]="GPU\0MMU\0JOB\0EVENT";         /* four names */
        std::memset(&set,0x5a,sizeof(set));
        CHECK(NativeMaliBoard::parseInterrupts(raw,bytes,3,
            reinterpret_cast<const uint8_t *>(unknown),sizeof(unknown),&set)==kIOReturnBadArgument);
        CHECK(NativeMaliBoard::parseInterrupts(raw,bytes,3,
            reinterpret_cast<const uint8_t *>(stranger),sizeof(stranger),&set)==kIOReturnUnsupported);
        CHECK(NativeMaliBoard::parseInterrupts(raw,bytes,3,
            reinterpret_cast<const uint8_t *>(shortList),sizeof(shortList),&set)==kIOReturnUnsupported);
        /* A list whose last string has no NUL terminator is malformed. */
        const uint8_t *cut=names;
        CHECK(NativeMaliBoard::parseInterrupts(raw,bytes,3,cut,namesLength-1,&set)==kIOReturnUnsupported);
        /* Only a three-cell GIC specifier is accepted here. */
        CHECK(NativeMaliBoard::parseInterrupts(raw,bytes,2,names,namesLength,&set)==kIOReturnUnsupported);
        CHECK(NativeMaliBoard::parseInterrupts(raw,bytes-1,3,names,namesLength,&set)==kIOReturnBadArgument);
        CHECK(NativeMaliBoard::parseInterrupts(nullptr,bytes,3,names,namesLength,&set)==kIOReturnBadArgument);
        CHECK(NativeMaliBoard::parseInterrupts(raw,bytes,3,nullptr,namesLength,&set)==kIOReturnBadArgument);
        CHECK(NativeMaliBoard::parseInterrupts(raw,bytes,3,names,namesLength,nullptr)==kIOReturnBadArgument);
        CHECK(set.entry[0].number==0x5a5a5a5a);
        /* A source outside the GIC SPI/PPI range is a different binding. */
        static const uint32_t wide[][3]={{0,0x112,8},{0,0x113,8},{0,0x114,8},{0,0x115,8},{2,0x116,8}};
        uint8_t raw2[64];
        const size_t bytes2=interrupts(raw2,wide,5);
        CHECK(NativeMaliBoard::parseInterrupts(raw2,bytes2,3,names,namesLength,&set)==kIOReturnBadArgument);
        CHECK(set.entry[0].number==0x5a5a5a5a);
        /* stringCount / stringAt */
        CHECK(NativeMaliBoard::stringCount(names,namesLength)==5);
        CHECK(NativeMaliBoard::stringCount(names,0)==0);
        CHECK(NativeMaliBoard::stringCount(names,namesLength-1)==0);
        const char *text=nullptr;
        size_t textBytes=0;
        CHECK(NativeMaliBoard::stringAt(names,namesLength,3,&text,&textBytes)==kIOReturnSuccess);
        CHECK(textBytes==5 && std::memcmp(text,"EVENT",5)==0);
        CHECK(NativeMaliBoard::stringAt(names,namesLength,5,&text,&textBytes)==kIOReturnBadArgument);
        CHECK(NativeMaliBoard::stringAt(names,namesLength,0,nullptr,&textBytes)==kIOReturnBadArgument);
    }

    /* ---- clocks: named refs, providers not assumed ---- */
    {
        uint8_t raw[64];
        const size_t bytes=clocks(raw,clockCells,8);
        size_t count=0;
        CHECK(NativeMaliBoard::parseClocks(raw,bytes,1,
            reinterpret_cast<const uint8_t *>(clockNames),sizeof(clockNames),
            named,NativeMaliMaxClocks,&count)==kIOReturnSuccess);
        CHECK(count==8);
        CHECK(named[0].phandle==0x14 && named[0].id==0x54);   /* clk_mux, topckgen */
        CHECK(named[1].phandle==0x14 && named[1].id==0x26);   /* clk_main_parent */
        CHECK(named[2].phandle==0x14 && named[2].id==0x06);   /* clk_sub_parent */
        CHECK(named[3].phandle==0x60 && named[3].id==0x00);   /* subsys_mfg_cg, mfgcfg */
        CHECK(named[4].phandle==0x27 && named[4].id==0x09);   /* mtcmos_mfg_async, scpsys */
        CHECK(named[5].phandle==0x27 && named[5].id==0x04);
        CHECK(named[6].phandle==0x27 && named[6].id==0x07);
        CHECK(named[7].phandle==0x27 && named[7].id==0x08);
        /* Refusals. */
        static const char duplicate[]="clk_mux\0clk_mux\0clk_sub_parent\0subsys_mfg_cg\0"
            "mtcmos_mfg_async\0mtcmos_mfg\0mtcmos_mfg_core0\0mtcmos_mfg_core1";
        CHECK(NativeMaliBoard::parseClocks(raw,bytes,1,
            reinterpret_cast<const uint8_t *>(duplicate),sizeof(duplicate),
            named,NativeMaliMaxClocks,&count)==kIOReturnBadArgument);
        CHECK(NativeMaliBoard::parseClocks(raw,bytes,1,
            reinterpret_cast<const uint8_t *>(clockNames),sizeof(clockNames),
            named,7,&count)==kIOReturnNoResources);
        CHECK(NativeMaliBoard::parseClocks(raw,bytes,2,
            reinterpret_cast<const uint8_t *>(clockNames),sizeof(clockNames),
            named,NativeMaliMaxClocks,&count)==kIOReturnBadArgument);
        CHECK(NativeMaliBoard::parseClocks(raw,bytes,3,
            reinterpret_cast<const uint8_t *>(clockNames),sizeof(clockNames),
            named,NativeMaliMaxClocks,&count)==kIOReturnUnsupported);
        CHECK(NativeMaliBoard::parseClocks(raw,bytes-1,1,
            reinterpret_cast<const uint8_t *>(clockNames),sizeof(clockNames),
            named,NativeMaliMaxClocks,&count)==kIOReturnBadArgument);
        CHECK(NativeMaliBoard::parseClocks(raw,bytes,1,
            reinterpret_cast<const uint8_t *>(clockNames),sizeof(clockNames)-2,
            named,NativeMaliMaxClocks,&count)==kIOReturnUnsupported);
        /* A zero or invalid phandle names no provider. */
        static const uint32_t zeroed[][2]={{0,0x54},{0x14,0x26},{0x14,0x06},{0x60,0x00},
            {0x27,0x09},{0x27,0x04},{0x27,0x07},{0x27,0x08}};
        uint8_t raw2[64];
        const size_t bytes2=clocks(raw2,zeroed,8);
        CHECK(NativeMaliBoard::parseClocks(raw2,bytes2,1,
            reinterpret_cast<const uint8_t *>(clockNames),sizeof(clockNames),
            named,NativeMaliMaxClocks,&count)==kIOReturnBadArgument);
        /* Cell-per-entry 0 keeps the id at zero. */
        static const uint32_t plain[][2]={{0x14,0},{0x60,0},{0x27,0},{0x27,0},
            {0x27,0},{0x27,0},{0x14,0},{0x14,0}};
        uint8_t raw3[32];
        for(size_t i=0;i<8;++i)put(raw3+i*4,plain[i][0]);
        CHECK(NativeMaliBoard::parseClocks(raw3,32,0,
            reinterpret_cast<const uint8_t *>(clockNames),sizeof(clockNames),
            named,NativeMaliMaxClocks,&count)==kIOReturnSuccess);
        CHECK(count==8 && named[0].phandle==0x14 && named[0].id==0);
    }

    /* ---- the real device tree ---- */
    {
        Fdt fdt;
        CHECK(fdt.open(dtbPath));
        CHECK(fdt.nodeExists("/mali@13040000"));
        CHECK(!fdt.nodeExists("/mali@deadbeef"));
        Fdt::Property compatible{};
        CHECK(fdt.property("/mali@13040000","compatible",&compatible));
        CHECK(stringCountOf(compatible.data,compatible.bytes)==2);
        CHECK(containsString(compatible.data,compatible.bytes,"mediatek,mali"));
        CHECK(containsString(compatible.data,compatible.bytes,"arm,mali-valhall"));
        Fdt::Property regProperty{};
        CHECK(fdt.property("/mali@13040000","reg",&regProperty));
        CHECK(regProperty.bytes==16);
        size_t count=0;
        CHECK(NativeMaliBoard::parseReg(regProperty.data,regProperty.bytes,2,2,
            windows,NativeMaliMaxWindows,&count)==kIOReturnSuccess);
        CHECK(count==1 && windows[0].base==UINT64_C(0x13040000) && windows[0].bytes==0x4000);
        /* The roadmap's own fact: no iommus, the GPU has its own MMU. */
        Fdt::Property unused{};
        CHECK(!fdt.property("/mali@13040000","iommus",&unused));
        Fdt::Property irqProperty{},irqNameProperty{};
        CHECK(fdt.property("/mali@13040000","interrupts",&irqProperty));
        CHECK(fdt.property("/mali@13040000","interrupt-names",&irqNameProperty));
        CHECK(irqProperty.bytes==60);
        CHECK(NativeMaliBoard::parseInterrupts(irqProperty.data,irqProperty.bytes,3,
            irqNameProperty.data,irqNameProperty.bytes,&set)==kIOReturnSuccess);
        CHECK(set.entry[NativeMaliIrqGpu].number==274);
        CHECK(set.entry[NativeMaliIrqMmu].number==275);
        CHECK(set.entry[NativeMaliIrqJob].number==276);
        CHECK(set.entry[NativeMaliIrqEvent].number==277);
        CHECK(set.entry[NativeMaliIrqPwr].number==278);
        CHECK(set.entry[NativeMaliIrqGpu].type==0);
        CHECK(NativeMaliBoard::checkTriggers(&set)==kIOReturnSuccess);
        /* The providers really publish #clock-cells = 1 and these phandles. */
        static const struct {const char *path;uint32_t phandle;} providers[]={
            {"/topckgen@10000000",0x14},{"/mfg_cfg@13000000",0x60},{"/scpsys@10001000",0x27}};
        for(const auto &provider:providers) {
            Fdt::Property cells{},phandle{};
            CHECK(fdt.property(provider.path,"#clock-cells",&cells));
            CHECK(cells.bytes==4 && be32(cells.data)==1);
            CHECK(fdt.property(provider.path,"phandle",&phandle));
            CHECK(phandle.bytes==4 && be32(phandle.data)==provider.phandle);
        }
        Fdt::Property clocksProperty{},clockNameProperty{};
        CHECK(fdt.property("/gpufreq","clocks",&clocksProperty));
        CHECK(fdt.property("/gpufreq","clock-names",&clockNameProperty));
        CHECK(clocksProperty.bytes==64);
        CHECK(NativeMaliBoard::parseClocks(clocksProperty.data,clocksProperty.bytes,1,
            clockNameProperty.data,clockNameProperty.bytes,
            named,NativeMaliMaxClocks,&count)==kIOReturnSuccess);
        CHECK(count==8);
        CHECK(named[0].phandle==0x14 && named[0].id==0x54);
        CHECK(named[1].phandle==0x14 && named[1].id==0x26);
        CHECK(named[2].phandle==0x14 && named[2].id==0x06);
        CHECK(named[3].phandle==0x60 && named[3].id==0x00);
        CHECK(named[4].phandle==0x27 && named[4].id==0x09);
        CHECK(named[5].phandle==0x27 && named[5].id==0x04);
        CHECK(named[6].phandle==0x27 && named[6].id==0x07);
        CHECK(named[7].phandle==0x27 && named[7].id==0x08);
        const char *first=nullptr;
        size_t firstBytes=0;
        CHECK(NativeMaliBoard::stringAt(clockNameProperty.data,clockNameProperty.bytes,
            0,&first,&firstBytes)==kIOReturnSuccess);
        CHECK(firstBytes==7 && std::memcmp(first,"clk_mux",7)==0);
        CHECK(NativeMaliBoard::stringCount(clockNameProperty.data,
            clockNameProperty.bytes)==8);
    }

    std::printf("PASS: board parser over %s (mali reg 0x13040000/0x4000, five "
        "interrupts by name 274..278, eight named clocks) plus the hand-built "
        "alternate formats and malformed/limit refusals\n",dtbPath);
}
