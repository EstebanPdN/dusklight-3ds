#include "native_packets.hpp"
#include "gx_bank_delta.hpp"
#include "gx_profile.hpp"
#include "gx_target_extent.hpp"
#include <dolphin/gx/GXCommandList.h>
#include <dolphin/gx/GXAurora.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <new>

namespace dusk::native::packets {
namespace {
using gx::CommandProcessor;using gx::CommandRegisters;using gx::CommandStream;using gx::PacketOp;
// The default of this build (native-packets.txt overrides it). Off: the
// host test (Tools/3ds/test_native_packets.py) and the alpha12-w5 verify gate
// prove capture mode exact. Alpha 12 kept it off: the fold that kept the game
// bank up to date (a checksum and a parse of every list of every packet, and
// every register word into an overlay) cost as much as running the lists
// (host: 985 of 1,609 ns per direct record). Alpha 14 checks each list
// against its template and builds the bank once per run, backwards, at the
// seal (see "the seal"). Still off by default: in the built-in Azahar every
// thread shares core 0 and the path cost 14.4 ms more per picture in the
// village (lead's A/B, s2 with `af`); `a` turns it on (direct records), and
// it then ships only where the worker has a core of its own.
constexpr Mode defaultMode=Mode::off;
// Direct records (`d`). The record J3D builds is proven equal to the capture
// at run time (verify mode compares the two word for word); the J3D code is
// not part of the host test.
constexpr bool defaultDirect=true;
// Record words of one packet: a J3D material packet takes a few hundred, and
// the stream keeps room for one command written through write() (its
// scratch, 64 KiB: gx_stream.cpp). A longer one ends its capture ("record
// full") and runs on the original path.
constexpr std::uint32_t packetWords=20*1024;
// The records of a run (shipped packets not sealed yet) stay where they were
// captured until the seal: `arenaWords` in all, a new packet starts only with
// packetWords free (the run is sealed first otherwise).
constexpr std::uint32_t arenaWords=64*1024,runRecords=1024,runRefs=8192,refsPerRecord=1024;

// A big-endian word of a list (lists are byte streams: any alignment).
u32 word(const u8* p){
    u32 v;std::memcpy(&v,p,4);
    if constexpr(std::endian::native==std::endian::little)return __builtin_bswap32(v);
    return v;
}
u16 half(const u8* p){return u16((u16(p[0])<<8)|p[1]);}
const void* pointerAt(const std::uint32_t* at){return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(std::uint64_t(at[0])|(std::uint64_t(at[1])<<32)));}
bool cpTaken(u32 reg){ // CommandProcessor::applyCp
    return (reg==0x30||reg==0x40||reg==0x50||reg==0x60||(reg>=0x70&&reg<0x98)||(reg>=0xb0&&reg<0xc0))&&
           !(reg>=0x78&&reg<0x80)&&!(reg>=0x88&&reg<0x90);
}
// The words an indexed load writes, from the array bytes in their order
// (CommandProcessor::applyIndexedWords).
u32 arrayWord(const u8* p,bool little){
    if(!little)return word(p);
    if constexpr(std::endian::native==std::endian::little){u32 v;std::memcpy(&v,p,4);return v;}
    return u32(p[0])|(u32(p[1])<<8)|(u32(p[2])<<16)|(u32(p[3])<<24);
}

// The net effect of captured operations on a bank, in a form that a few
// processor writes reproduce: the last value of every register they wrote
// (BP merged under the BP mask in effect, as applyBp merges), both TEV colour
// latches of 0xE0-0xE7 and the one written last, which family of the matrix
// index registers (CP 0x30/0x40, XF 0x1018/0x1019) was written last, the
// array bindings and the vertex cache invalidations. `ops` bounds the epoch
// changes the operations make (each makes at most one per epoch).
struct Overlay {
    std::array<u32,256> bp{},cp{};
    std::array<u32,8> bpBits{},cpBits{};
    std::array<u32,0x1058> xf{};
    std::array<u32,(0x1058+31)/32> xfBits{};
    std::array<std::array<u32,8>,2> latch{};
    std::uint16_t latchBits{};
    std::array<u8,8> lastBank{};
    std::array<u8,2> lastFamily{}; // 0 none, 1 CP, 2 XF
    struct Binding {std::uint64_t address{};u32 size{};bool little{},touched{};};
    std::array<Binding,16> bindings{};
    u32 invalidations{},mask{0xffffff},ops{};
    // Nothing is folded before the whole record was checked (checkRecord),
    // so a fold never stops part-way and needs no undo.
    static bool bit(const u32* bits,unsigned i){return bits[i/32]>>(i%32)&1;}
    static void set(u32* bits,unsigned i){bits[i/32]|=1u<<(i%32);}
    // Bits [first, first+count) set, a word of bits at a time.
    static void setRange(u32* bits,unsigned first,unsigned count){
        for(const unsigned last=first+count;first<last;){
            const unsigned w=first/32,upTo=std::min(last,(w+1)*32),n=upTo-first;
            bits[w]|=(n==32?~0u:((1u<<n)-1))<<(first%32);first=upTo;
        }
    }
    void clear(){
        bpBits={};cpBits={};xfBits={};latchBits=0;lastFamily={};
        for(auto& b:bindings)b.touched=false;
        invalidations=0;mask=0xffffff;ops=0;
    }
    u32 currentBp(const CommandRegisters& bank,unsigned reg)const{return bit(bpBits.data(),reg)?bp[reg]:bank.bp[reg];}
    // CommandProcessor::applyBp.
    void foldBp(const CommandRegisters& bank,u32 reg,u32 value){
        value&=0xffffff;
        if(reg==0xfe){mask=value;return;}
        const u32 merged=(currentBp(bank,reg)&~mask)|(value&mask);
        bp[reg]=merged;set(bpBits.data(),reg);mask=0xffffff;
        if(reg>=0xe0&&reg<=0xe7){
            const unsigned b=(merged>>23)&1,slot=reg-0xe0;
            latch[b][slot]=merged;latchBits|=std::uint16_t(1u<<(b*8+slot));lastBank[slot]=u8(b);
        }
    }
    void foldCp(u32 reg,u32 value){
        cp[reg]=value;set(cpBits.data(),reg);
        if(reg==0x30||reg==0x40)lastFamily[(reg-0x30)/16]=1;
    }
    void family(u32 address,u32 count){
        if(address<=0x1019&&address+count>0x1018)
            for(unsigned slot=0;slot<2;++slot)if(address<=0x1018+slot&&address+count>0x1018+slot)lastFamily[slot]=2;
    }
    // A run of host words (checked: inside XF).
    void foldXf(u32 address,const u32* values,u32 count){
        std::memcpy(xf.data()+address,values,count*sizeof(u32));setRange(xfBits.data(),address,count);family(address,count);
    }
    // A run of big-endian words of a list.
    void foldXfBytes(u32 address,const u8* values,u32 count){
        for(u32 i=0;i<count;++i)xf[address+i]=word(values+i*4);
        setRange(xfBits.data(),address,count);family(address,count);
    }
    void foldBinding(unsigned slot,std::uint64_t address,u32 size,bool little){bindings[slot]={address,size,little,true};}
};

// A display list or a written command as CommandProcessor::execute reads
// it, handed to `sink` one register operation at a time. Only register
// loads, invalidations and array bases.
template<class Sink> const char* parseList(Sink& sink,const u8* bytes,std::size_t size){
    if(size&&!bytes)return "list without bytes";
    for(std::size_t cursor=0;cursor<size;){
        const u8 code=bytes[cursor];const u8* rest=bytes+cursor;const std::size_t left=size-cursor;std::size_t step=1;
        if(code==GX_NOP){}
        else if(code==GX_CMD_INVL_VC){if(!sink.invalidate())return "list pool full";}
        else if(code==GX_LOAD_CP_REG){
            if(left<6)return "list truncated";
            step=6;if(!cpTaken(rest[1]))return "list cp register";
            if(!sink.cp(rest[1],word(rest+2)))return "list pool full";
        }else if(code==GX_LOAD_BP_REG){
            if(left<5)return "list truncated";
            step=5;const u32 value=word(rest+1),reg=value>>24;
            if(reg==0x52||reg==0x65)return "list bp copy or palette";
            if(!sink.bp(reg,value&0xffffff))return "list pool full";
        }else if(code==GX_LOAD_XF_REG){
            if(left<5)return "list truncated";
            const u32 count=u32(half(rest+1))+1,address=half(rest+3);
            if(address>=0x1058||count>0x1058-address)return "list xf range";
            step=5+std::size_t(count)*4;if(left<step)return "list truncated";
            if(!sink.xf(address,count,rest+5))return "list pool full";
        }else if(code==GX_AURORA){
            if(left<3)return "list truncated";
            const u16 sub=half(rest+1);
            if(sub<GX_AURORA_LOAD_ARRAYBASE||sub>=GX_AURORA_LOAD_ARRAYBASE+16)return "list aurora command";
            if(left<16)return "list truncated";
            if(rest[15]>1)return "list array format";
            step=16;
            if(!sink.binding(sub-GX_AURORA_LOAD_ARRAYBASE,(std::uint64_t(word(rest+3))<<32)|word(rest+7),word(rest+11),rest[15]!=0))
                return "list pool full";
        }else return "list draw or load";
        cursor+=step;
    }
    return nullptr;
}
// A command written through write() (a bytes operation), one register
// operation at a time, in order.
struct WrittenCommands {
    struct Command {u8 kind{};u32 reg{},value{},count{};const u8* values{};}; // 1 bp, 2 cp, 3 xf, 4 invalidate
    std::array<Command,8> c{};unsigned n{};bool full{};
    bool put(const Command& x){if(n==c.size()){full=true;return false;}c[n++]=x;return true;}
    bool invalidate(){return put({4,0,0,0,nullptr});}
    bool cp(u32 reg,u32 value){return put({2,reg,value,0,nullptr});}
    bool bp(u32 reg,u32 value){return put({1,reg,value,0,nullptr});}
    bool xf(u32 address,u32 count,const u8* values){return put({3,address,0,count,values});}
    bool binding(unsigned,std::uint64_t,u32,bool){full=true;return false;} // only typed binds bind arrays
};

// ---- list templates ----
// Every display list a record points at is parsed once into a template: two
// pool words per command, `kind | register << 8 | offset << 16` and an extra
// word. Before a record is shipped each of its lists is compared with its
// template at the bytes that decide the parse: opcodes, registers, XF counts
// and addresses, array base slots and byte orders, the NOP padding. Where
// they all match, CommandProcessor::execute takes the list exactly as the
// template says, command for command, so only the values can differ, and
// those are read from the list itself when they are needed (the seal). A
// list J3D rewrites with new values every picture (the differential lists)
// keeps its template: no checksum and no parse per picture. A list whose
// structure changed gets a new template.
enum TemplateKind:u32 {tBp=1,tFe,tCp,tXf,tInvalidate,tBase,tNops};
struct Template {
    const u8* data{};u32 size{},generation{};
    u32 first{},count{};       // entries, two pool words each
    u32 eFirst{},eCount{};     // pool words: entries the seal takes forwards (TEV colours BP 0xE0-0xE7, palette BP 0x64/0x65), in order
    u32 paletteLoads{};
    u32 covFirst{},covCount{}; // pool words: pairs (coverage word, bits)
    u32 commands{},invalidations{};
    const char* refused{};u8 recheck{};
    bool leadingBp{}; // the first BP command is a write (it would take a mask written before the list)
};
// The coverage space of a seal: BP registers 0-255 (all 24 bits written),
// CP registers, XF words, array binding slots.
constexpr unsigned covBp=0,covCp=256,covXf=512,covBinding=covXf+0x1058,coverageBits=covBinding+16,coverageWords=(coverageBits+31)/32;
struct Templates {
    static constexpr unsigned capacity=2048,probe=4,poolWords=96*1024,maxColourWrites=128;
    std::array<Template,capacity> entries{};
    u32* pool{};u32 used{},generation{1};unsigned victim{};
    std::uint64_t hits{},builds{},rebuilt{};
};
Templates* templates{};
bool tevColour(u32 reg){return reg>=0xe0&&reg<=0xe7;}
// BP writes the seal follows forwards: the TEV colour registers (a bank
// selected by the merged value) and the palette address and load (a load
// takes a palette slot: their order matters).
bool forwardBp(u32 reg){return tevColour(reg)||reg==0x64||reg==0x65;}
// The template of `bytes` into `t` (refused or not). False: the pool cannot
// hold it even empty.
bool buildTemplate(Template& t,const u8* bytes,u32 size){
    auto& T=*templates;
    t={};t.data=bytes;t.size=size;
    const std::size_t worst=std::size_t(size)*2+Templates::maxColourWrites+2*coverageWords;
    if((!bytes&&size)||size>65536||worst>Templates::poolWords){t.generation=T.generation;t.refused="list size";return true;}
    if(worst>Templates::poolWords-T.used){T.used=0;++T.generation;} // every template is built again on its next use
    t.generation=T.generation;
    u32* const base=T.pool+T.used;u32 n=0;
    std::array<u32,coverageWords> coverage{};
    std::array<u32,Templates::maxColourWrites> colours;u32 colourCount=0;
    auto cover=[&](unsigned bit){coverage[bit/32]|=1u<<(bit%32);};
    auto entry=[&](u32 kind,u32 reg,std::size_t offset,u32 extra){base[2*n]=kind|(reg<<8)|(u32(offset)<<16);base[2*n+1]=extra;++n;};
    auto refuse=[&](const char* why){t.refused=why;return true;};
    long pendingMask=-1,nops=-1;bool sawBp=false;
    for(std::size_t cursor=0;cursor<size;){
        const u8 code=bytes[cursor];const u8* rest=bytes+cursor;const std::size_t left=size-cursor;std::size_t step=1;
        if(code==GX_NOP){if(nops<0)nops=long(cursor);++cursor;continue;}
        if(nops>=0){entry(tNops,0,std::size_t(nops),u32(cursor-std::size_t(nops)));nops=-1;}
        if(code==GX_CMD_INVL_VC){entry(tInvalidate,0,cursor,0);++t.invalidations;}
        else if(code==GX_LOAD_CP_REG){
            if(left<6)return refuse("list truncated");
            step=6;if(!cpTaken(rest[1]))return refuse("list cp register");
            entry(tCp,rest[1],cursor,0);cover(covCp+rest[1]);
        }else if(code==GX_LOAD_BP_REG){
            if(left<5)return refuse("list truncated");
            step=5;const u32 reg=rest[1];
            if(reg==0x52)return refuse("list bp copy");
            if(!sawBp){sawBp=true;t.leadingBp=reg!=0xfe;}
            if(reg==0xfe){entry(tFe,reg,cursor,0);pendingMask=long(cursor);}
            else{
                if(forwardBp(reg)){
                    if(colourCount==colours.size())return refuse("list colour writes");
                    colours[colourCount++]=n;
                    if(reg==0x65)++t.paletteLoads;
                }
                if(!tevColour(reg)&&reg!=0x65)cover(covBp+reg);
                entry(tBp,reg,cursor,pendingMask>=0?0x80000000u|u32(pendingMask):0u);pendingMask=-1;
            }
        }else if(code==GX_LOAD_XF_REG){
            if(left<5)return refuse("list truncated");
            const u32 count=u32(half(rest+1))+1,address=half(rest+3);
            if(address>=0x1058||count>0x1058-address)return refuse("list xf range");
            step=5+std::size_t(count)*4;if(left<step)return refuse("list truncated");
            entry(tXf,0,cursor,((count-1)<<16)|address);
            for(u32 i=0;i<count;++i)cover(covXf+address+i);
        }else if(code==GX_AURORA){
            if(left<3)return refuse("list truncated");
            const u16 sub=half(rest+1);
            if(sub<GX_AURORA_LOAD_ARRAYBASE||sub>=GX_AURORA_LOAD_ARRAYBASE+16)return refuse("list aurora command");
            if(left<16)return refuse("list truncated");
            if(rest[15]>1)return refuse("list array format");
            step=16;entry(tBase,sub-GX_AURORA_LOAD_ARRAYBASE,cursor,rest[15]);cover(covBinding+sub-GX_AURORA_LOAD_ARRAYBASE);
        }else return refuse("list draw or load");
        ++t.commands;cursor+=step;
    }
    if(nops>=0)entry(tNops,0,std::size_t(nops),u32(size-std::size_t(nops)));
    if(pendingMask>=0)return refuse("list leaves a bp mask");
    t.first=T.used;t.count=n;
    u32 at=T.used+2*n;
    t.eFirst=at;t.eCount=colourCount;std::memcpy(T.pool+at,colours.data(),colourCount*sizeof(u32));at+=colourCount;
    t.covFirst=at;
    for(unsigned w=0;w<coverageWords;++w)if(coverage[w]){T.pool[at++]=w;T.pool[at++]=coverage[w];}
    t.covCount=(at-t.covFirst)/2;
    T.used=at;
    return true;
}
// The bytes still parse as the template says.
bool matches(const Template& t,const u8* bytes){
    const u32* e=templates->pool+t.first;
    for(u32 i=0;i<t.count;++i,e+=2){
        const u32 head=e[0],kind=head&0xff,reg=(head>>8)&0xff;const u8* at=bytes+(head>>16);
        switch(kind){
        case tBp:case tFe:if(at[0]!=GX_LOAD_BP_REG||at[1]!=reg)return false;break;
        case tCp:if(at[0]!=GX_LOAD_CP_REG||at[1]!=reg)return false;break;
        case tXf:if(at[0]!=GX_LOAD_XF_REG||word(at+1)!=e[1])return false;break;
        case tInvalidate:if(at[0]!=GX_CMD_INVL_VC)return false;break;
        case tBase:if(at[0]!=GX_AURORA||half(at+1)!=GX_AURORA_LOAD_ARRAYBASE+reg||at[15]!=e[1])return false;break;
        default:for(u32 k=0;k<e[1];++k)if(at[k]!=GX_NOP)return false;break;
        }
    }
    return true;
}
unsigned templateHome(const u8* bytes,u32 size){
    return unsigned((((std::uint32_t(reinterpret_cast<std::uintptr_t>(bytes))>>5)^(size<<11))*2654435761u)>>21)%Templates::capacity;
}
// The template of a list as it is now (built or built again when needed).
// Null: no template could be kept.
Template* templateFor(const u8* bytes,u32 size){
    auto& T=*templates;
    const unsigned home=templateHome(bytes,size);
    Template* free=nullptr;
    for(unsigned n=0;n<Templates::probe;++n){
        auto& e=T.entries[(home+n)%Templates::capacity];
        if(e.generation==T.generation&&e.data==bytes&&e.size==size){
            // A refused list is looked at again now and then: its storage may
            // hold another list by now.
            if(e.refused&&!++e.recheck){if(!buildTemplate(e,bytes,size))return nullptr;++T.rebuilt;return &e;}
            if(!e.refused&&!matches(e,bytes)){if(!buildTemplate(e,bytes,size))return nullptr;++T.rebuilt;return &e;}
            ++T.hits;return &e;
        }
        if(!free&&e.generation!=T.generation)free=&e;
    }
    Template& e=free?*free:T.entries[(home+(T.victim++%Templates::probe))%Templates::capacity];
    if(!buildTemplate(e,bytes,size))return nullptr;
    ++T.builds;return &e;
}
// The template a record operation was checked with, still in the pool.
Template* templateAgain(Template* t,const u8* bytes,u32 size){
    if(t&&t->generation==templates->generation&&t->data==bytes&&t->size==size&&!t->refused)return t;
    // Built again from the same bytes (they stay as they are while the
    // picture is painted): the same template.
    t=templateFor(bytes,size);
    return t&&!t->refused?t:nullptr;
}

// The operations of a record, one at a time.
struct Reader {
    const std::uint32_t* w;std::size_t n,at{};
    const std::uint32_t* op{};PacketOp kind{};std::uint32_t words{};
    bool damaged{};
    bool next(){
        if(at>=n)return false;
        const std::uint32_t h=w[at];words=h>>8;
        if(!words||words>n-at){damaged=true;return false;}
        op=w+at;kind=PacketOp(h&0xff);at+=words;
        return true;
    }
};
// The words of each operation (record checks).
bool soundOp(PacketOp kind,std::uint32_t words,const std::uint32_t* op){
    switch(kind){
    case PacketOp::bp:case PacketOp::cp:return words==3;
    case PacketOp::bpMasked:return words==4;
    case PacketOp::xf:return words>=3&&words==3+op[2];
    case PacketOp::list:return words==4;
    case PacketOp::bytes:return words>=2&&words==2+(op[1]+3)/4;
    case PacketOp::bind:return words==5;
    case PacketOp::indexed:return words>=4&&words==4+((op[1]>>16)>>12)+1;
    case PacketOp::shape:return words==6;
    }
    return false;
}

// One operation of a record kept for the seal: where it starts in the
// record and, for a list, the template it was checked with.
struct OpRef {u32 at{};Template* t{};};
struct RunRecord {const std::uint32_t* body{};u32 words{},refsFirst{},refsCount{};};
// The BP mask a written command leaves pending (0xffffff: none), given the
// one pending before it.
u32 writtenMask(const WrittenCommands& w,u32 pending){
    for(unsigned i=0;i<w.n;++i)if(w.c[i].kind==1)pending=w.c[i].reg==0xfe?w.c[i].value&0xffffff:0xffffff;
    return pending;
}
// Before a record is shipped: every list has a template that matches its
// bytes now (refs[k].t), and the BP mask never stays pending across an
// operation except from a mask write into the BP write right behind it (a
// typed write, or the first command of a written one). The record leaves no
// mask pending. Null: the record can be shipped.
const char* checkRecord(const std::uint32_t* body,std::size_t words,OpRef* refs,unsigned capacity,unsigned& count,unsigned& paletteLoads){
    count=0;paletteLoads=0;u32 pending=0xffffff;
    Reader r{body,words};
    while(r.next()){
        const auto* a=r.op;
        if(!soundOp(r.kind,r.words,a))return "record damaged";
        if(count==capacity)return "record has too many operations";
        refs[count]={u32(a-body),nullptr};
        const bool masked=pending!=0xffffff;
        switch(r.kind){
        case PacketOp::bp:
            if(a[1]==0x52)return "bp copy";
            paletteLoads+=a[1]==0x65;
            pending=a[1]==0xfe?a[2]&0xffffff:0xffffff;break;
        case PacketOp::bpMasked:
            if(a[1]==0x52||a[1]==0xfe)return "bp copy or mask";
            paletteLoads+=a[1]==0x65;
            pending=0xffffff;break;
        case PacketOp::cp:if(masked)return "bp mask across an operation";if(!cpTaken(a[1]))return "cp register";break;
        case PacketOp::xf:if(masked)return "bp mask across an operation";if(!a[2]||a[1]>=0x1058||a[2]>0x1058-a[1])return "xf range";break;
        case PacketOp::list:{
            if(masked)return "bp mask into a list";
            Template* t=templateFor(static_cast<const u8*>(pointerAt(a+1)),a[3]);
            if(!t)return "template pool";
            if(t->refused)return t->refused;
            refs[count].t=t;paletteLoads+=t->paletteLoads;break;}
        case PacketOp::bytes:{
            WrittenCommands w;
            if(const char* why=parseList(w,reinterpret_cast<const u8*>(a+2),a[1]))return w.full?"written command too long":why;
            if(masked&&(w.n==0||w.c[0].kind!=1))return "bp mask across an operation";
            pending=writtenMask(w,pending);
            break;}
        case PacketOp::bind:if(masked)return "bp mask across an operation";if((a[1]&0xff)>=16)return "binding slot";break;
        case PacketOp::indexed:{
            if(masked)return "bp mask across an operation";
            const u32 packed=a[1]>>16,n=(packed>>12)+1,address=packed&0xfff;
            if(address>=0x1058||n>0x1058-address)return "xf range";
            break;}
        case PacketOp::shape:if(masked)return "bp mask across an operation";break;
        default:return "record damaged";
        }
        ++count;
    }
    if(r.damaged)return "record damaged";
    if(pending!=0xffffff)return "pending bp mask";
    return nullptr;
}

// ---- the seal ----
// The bank after a run of shipped records, as the overlay a few processor
// writes reproduce (applyOverlay), built backwards: the last write of every
// register decides it, so the records are walked from the last operation to
// the first and a register already decided is skipped; a list whose every
// register is decided is skipped whole (its coverage). BP writes under a
// mask decide only the masked bits. The TEV colour registers (two banks
// selected by the merged value) are followed forwards first. Values come from
// the records and from the lists they point at (unchanged while the picture
// is painted: the painter's end seals what is pending).
// A palette load of the run in its order: the palette address register as
// it stood, the load's value and mask.
struct PaletteLoad {u32 address{},value{},mask{};};
constexpr unsigned runPaletteLoads=512;
struct SealScratch {
    std::array<u32,coverageWords> covered{};std::array<u32,256> bpCovered{},bpValue{};
    std::array<PaletteLoad,runPaletteLoads> palettes{};unsigned paletteCount{};
};
SealScratch* sealScratch{};
// The mask a BP write without a mask command of its own takes from the
// operation before it (checkRecord allows only these).
u32 incomingMask(const RunRecord& rec,const OpRef* refs,unsigned k){
    if(!k)return 0xffffff;
    const std::uint32_t* a=rec.body+refs[k-1].at;
    const auto kind=PacketOp(a[0]&0xff);
    if(kind==PacketOp::bp&&a[1]==0xfe)return a[2]&0xffffff;
    if(kind==PacketOp::bytes){
        WrittenCommands w;(void)parseList(w,reinterpret_cast<const u8*>(a+2),a[1]);
        return writtenMask(w,0xffffff);
    }
    return 0xffffff;
}
struct Backwards {
    Overlay& o;SealScratch& s;bool family[2]{};
    bool decided(unsigned bit)const{return s.covered[bit/32]>>(bit%32)&1;}
    void decide(unsigned bit){s.covered[bit/32]|=1u<<(bit%32);}
    void bp(u32 reg,u32 value,u32 mask){
        if(reg==0xfe||tevColour(reg)||reg==0x65)return;
        const u32 bits=mask&~s.bpCovered[reg]&0xffffff;
        if(!bits)return;
        if(!s.bpCovered[reg])s.bpValue[reg]=0;
        s.bpValue[reg]|=value&bits;s.bpCovered[reg]|=bits;
        if(s.bpCovered[reg]==0xffffff)decide(covBp+reg);
    }
    void cp(u32 reg,u32 value){
        if(decided(covCp+reg))return;
        decide(covCp+reg);o.cp[reg]=value;Overlay::set(o.cpBits.data(),reg);
        if(reg==0x30||reg==0x40){const unsigned slot=(reg-0x30)/16;if(!family[slot]){family[slot]=true;o.lastFamily[slot]=1;}}
    }
    void xf(u32 at,u32 value){
        if(decided(covXf+at))return;
        decide(covXf+at);o.xf[at]=value;Overlay::set(o.xfBits.data(),at);
        if(at==0x1018||at==0x1019){const unsigned slot=at-0x1018;if(!family[slot]){family[slot]=true;o.lastFamily[slot]=2;}}
    }
    void binding(unsigned slot,std::uint64_t address,u32 size,bool little){
        if(decided(covBinding+slot))return;
        decide(covBinding+slot);o.bindings[slot]={address,size,little,true};
    }
    bool listDecided(const Template& t)const{
        const u32* c=templates->pool+t.covFirst;
        for(u32 i=0;i<t.covCount;++i,c+=2)if((s.covered[c[0]]&c[1])!=c[1])return false;
        return true;
    }
    void list(const Template& t,const u8* b){
        o.ops+=t.commands;o.invalidations+=t.invalidations;
        if(listDecided(t))return;
        const u32* first=templates->pool+t.first;
        for(const u32* e=first+2*t.count;e!=first;){
            e-=2;
            const u32 head=e[0],kind=head&0xff,reg=(head>>8)&0xff;const u8* at=b+(head>>16);
            switch(kind){
            case tBp:bp(reg,word(at+1)&0xffffff,e[1]?word(b+(e[1]&0xffff)+1)&0xffffff:0xffffff);break;
            case tCp:cp(reg,word(at+2));break;
            case tXf:{const u32 n=(e[1]>>16)+1,address=e[1]&0xffff;for(u32 i=n;i--;)xf(address+i,word(at+5+4*i));break;}
            case tBase:binding(reg,(std::uint64_t(word(at+3))<<32)|word(at+7),word(at+11),at[15]!=0);break;
            default:break; // mask commands (taken with their write), invalidations (counted), padding
            }
        }
    }
    void written(const WrittenCommands& w,u32 incoming){
        // The mask each BP write takes, forwards; then the commands backwards.
        std::array<u32,8> masks{};u32 mask=incoming;
        for(unsigned i=0;i<w.n;++i)if(w.c[i].kind==1){
            if(w.c[i].reg==0xfe)mask=w.c[i].value&0xffffff;else{masks[i]=mask;mask=0xffffff;}
        }
        for(unsigned i=w.n;i--;){
            const auto& c=w.c[i];++o.ops;
            switch(c.kind){
            case 1:if(c.reg!=0xfe)bp(c.reg,c.value&0xffffff,masks[i]);break;
            case 2:cp(c.reg,c.value);break;
            case 3:for(u32 k=c.count;k--;)xf(c.reg+k,word(c.values+4*k));break;
            default:++o.invalidations;break;
            }
        }
    }
};
// The BP writes a run's seal takes forwards: the TEV colour registers into
// the overlay (Overlay::foldBp), and the palette loads with the palette
// address register as it stood at each (SealScratch::palettes).
struct Forwards {
    Overlay& o;const CommandRegisters& bank;SealScratch& s;
    u32 address{};bool addressValid{};
    void write(u32 reg,u32 value,u32 mask){
        if(tevColour(reg)){o.mask=mask;o.foldBp(bank,reg,value);}
        else if(reg==0x64){address=(address&~mask)|(value&mask);addressValid=true;}
        else if(reg==0x65&&s.paletteCount<runPaletteLoads)s.palettes[s.paletteCount++]={addressValid?address|0x80000000u:0u,value&0xffffff,mask};
        o.mask=0xffffff;
    }
    void record(const RunRecord& rec,const OpRef* refs){
        u32 mask=0xffffff;
        for(unsigned k=0;k<rec.refsCount;++k){
            const std::uint32_t* a=rec.body+refs[k].at;
            switch(PacketOp(a[0]&0xff)){
            case PacketOp::bp:
                if(a[1]==0xfe){mask=a[2]&0xffffff;break;}
                if(forwardBp(a[1]))write(a[1],a[2]&0xffffff,mask);
                mask=0xffffff;break;
            case PacketOp::bpMasked:
                if(forwardBp(a[1]))write(a[1],a[3]&0xffffff,a[2]&0xffffff);
                mask=0xffffff;break;
            case PacketOp::list:{
                const u8* b=static_cast<const u8*>(pointerAt(a+1));
                const Template* t=templateAgain(refs[k].t,b,a[3]);
                if(!t||!t->eCount)break;
                const u32* e=templates->pool+t->first;const u32* index=templates->pool+t->eFirst;
                for(u32 i=0;i<t->eCount;++i){
                    const u32* w=e+2*index[i];
                    write((w[0]>>8)&0xff,word(b+(w[0]>>16)+1)&0xffffff,w[1]?word(b+(w[1]&0xffff)+1)&0xffffff:0xffffff);
                }
                break;}
            case PacketOp::bytes:{
                WrittenCommands w;(void)parseList(w,reinterpret_cast<const u8*>(a+2),a[1]);
                for(unsigned i=0;i<w.n;++i)if(w.c[i].kind==1){
                    if(w.c[i].reg==0xfe){mask=w.c[i].value&0xffffff;continue;}
                    if(forwardBp(w.c[i].reg))write(w.c[i].reg,w.c[i].value&0xffffff,mask);
                    mask=0xffffff;
                }
                break;}
            default:break;
            }
        }
    }
};
// The palette loads of a run into a processor's bank, in their order, each
// under the palette address it was issued with (before applyOverlay: the
// overlay holds the final address and leaves the load register out).
bool applyPalettes(CommandProcessor& p,const SealScratch& s,std::uint64_t& written){
    bool ok=true;
    for(unsigned i=0;i<s.paletteCount;++i){
        const auto& load=s.palettes[i];
        if(load.address>>31)ok=p.writeBp(0x64,load.address&0xffffff)&&ok;
        ok=(load.mask==0xffffff?p.writeBp(0x65,load.value):p.writeBpMasked(0x65,load.mask,load.value))&&ok;
        written+=2;
    }
    return ok;
}
// The overlay of `count` records (each checked by checkRecord) over `bank`.
// False: a list lost its template and could not get it back (the pool cannot
// hold it): not expected, the caller treats it as a processor failure.
bool buildOverlay(Overlay& o,const CommandRegisters& bank,const RunRecord* records,unsigned count,const OpRef* refs){
    auto& s=*sealScratch;
    o.clear();s.paletteCount=0;
    Forwards forwards{o,bank,s,bank.bp[0x64],bool(bank.bpValid[0x64])};
    for(unsigned r=0;r<count;++r)forwards.record(records[r],refs+records[r].refsFirst);
    o.mask=0xffffff;
    Backwards back{o,s};bool ok=true;
    for(unsigned r=count;r--;){
        const auto& rec=records[r];const OpRef* ref=refs+rec.refsFirst;
        for(unsigned k=rec.refsCount;k--;){
            const std::uint32_t* a=rec.body+ref[k].at;
            switch(PacketOp(a[0]&0xff)){
            case PacketOp::bp:++o.ops;if(a[1]!=0xfe)back.bp(a[1],a[2]&0xffffff,incomingMask(rec,ref,k));break;
            case PacketOp::bpMasked:o.ops+=2;back.bp(a[1],a[3]&0xffffff,a[2]&0xffffff);break;
            case PacketOp::cp:++o.ops;back.cp(a[1],a[2]);break;
            case PacketOp::xf:++o.ops;for(u32 i=a[2];i--;)back.xf(a[1]+i,a[3+i]);break;
            case PacketOp::list:{
                const u8* b=static_cast<const u8*>(pointerAt(a+1));
                const Template* t=templateAgain(ref[k].t,b,a[3]);
                if(!t){ok=false;break;}
                back.list(*t,b);break;}
            case PacketOp::bytes:{
                WrittenCommands w;(void)parseList(w,reinterpret_cast<const u8*>(a+2),a[1]);
                back.written(w,w.n&&w.c[0].kind==1&&w.c[0].reg!=0xfe?incomingMask(rec,ref,k):0xffffff);break;}
            case PacketOp::bind:{
                const unsigned slot=a[1]&0xff;o.ops+=2;
                back.cp(0xb0+slot,(a[1]>>8)&0xff);
                back.binding(slot,reinterpret_cast<std::uintptr_t>(pointerAt(a+2)),a[4],(a[1]>>16)&1);
                break;}
            case PacketOp::indexed:{
                const u32 packed=a[1]>>16,n=(packed>>12)+1,address=packed&0xfff;
                const auto* bytes=reinterpret_cast<const u8*>(a+4);++o.ops;
                for(u32 i=n;i--;)back.xf(address+i,arrayWord(bytes+i*4,a[3]!=0));
                break;}
            default:break;
            }
        }
    }
    // BP registers: the decided bits over the bank's word.
    for(unsigned reg=0;reg<256;++reg)if(const u32 bits=s.bpCovered[reg]){
        o.bp[reg]=s.bpValue[reg]|(bank.bp[reg]&~bits&0xffffff);Overlay::set(o.bpBits.data(),reg);s.bpCovered[reg]=0;
    }
    s.covered.fill(0);
    return ok;
}
// The overlay into a processor's bank through its own entry points, so every
// word, valid bit, hash, log entry and derived flag is the processor's own
// result for the final values. Bindings and CP, then BP, then the TEV colour
// latches (the bank not written last first), then the matrix index family
// written first, the XF runs, and the family written last. `written` counts
// the values written.
bool applyOverlay(CommandProcessor& p,const Overlay& o,std::uint32_t epochAdvance,std::uint64_t& written){
    bool ok=true;
    for(unsigned slot=0;slot<16;++slot)if(o.bindings[slot].touched){
        const auto& b=o.bindings[slot];ok=p.rebind(slot,b.address,b.size,b.little)&&ok;++written;
    }
    for(unsigned reg=0;reg<256;++reg)if(Overlay::bit(o.cpBits.data(),reg)&&reg!=0x30&&reg!=0x40){ok=p.writeCp(u8(reg),o.cp[reg])&&ok;++written;}
    for(unsigned w=0;w<o.bpBits.size();++w)for(u32 bits=o.bpBits[w];bits;bits&=bits-1){
        const unsigned reg=w*32+unsigned(std::countr_zero(bits));
        if(reg>=0xe0&&reg<=0xe7)continue;
        ok=p.writeBp(u8(reg),o.bp[reg])&&ok;++written;
    }
    for(unsigned slot=0;slot<8;++slot){
        const unsigned last=o.lastBank[slot],other=last^1;
        if(o.latchBits>>(other*8+slot)&1){ok=p.writeBp(u8(0xe0+slot),o.latch[other][slot])&&ok;++written;}
        if(o.latchBits>>(last*8+slot)&1){ok=p.writeBp(u8(0xe0+slot),o.latch[last][slot])&&ok;++written;}
    }
    for(unsigned slot=0;slot<2;++slot)
        if(o.lastFamily[slot]==2&&Overlay::bit(o.cpBits.data(),0x30+16*slot)){ok=p.writeCp(u8(0x30+16*slot),o.cp[0x30+16*slot])&&ok;++written;}
    for(unsigned at=0;at<0x1058;){
        const u32 bits=o.xfBits[at/32]>>(at%32);
        if(!bits){at=(at/32+1)*32;continue;}
        at+=unsigned(std::countr_zero(bits));
        unsigned end=at;while(end<0x1058&&Overlay::bit(o.xfBits.data(),end))++end;
        ok=p.writeXf(u16(at),{o.xf.data()+at,end-at})&&ok;written+=end-at;at=end;
    }
    for(unsigned slot=0;slot<2;++slot)
        if(o.lastFamily[slot]!=2&&Overlay::bit(o.cpBits.data(),0x30+16*slot)){ok=p.writeCp(u8(0x30+16*slot),o.cp[0x30+16*slot])&&ok;++written;}
    p.invalidateVertexCache(o.invalidations);
    p.advanceEpochs(epochAdvance,epochAdvance);
    return ok;
}

struct Game {
    Mode mode=defaultMode;bool verify=false,configured=false;
    Hooks hooks{};bool installed=false;
    gx::PacketCapture capture{};
    CommandStream* stream{};
    bool open=false,storageFailed=false;
    Overlay* overlay{};bool runPending=false;
    // The run: records shipped and not sealed, in order, and their operations.
    std::uint32_t* arena{};std::uint32_t arenaUsed{};
    RunRecord* records{};unsigned recordCount{};
    OpRef* refs{};unsigned refsUsed{},runPalettes{};
    // Verify mode.
    CommandProcessor* scratchA{};CommandProcessor* scratchB{};CommandRegisters* before{};Overlay* verifyOverlay{};
    CommandProcessor::ArrayBindings arraysBefore{};
    // Verify mode, per shape of the packet: the worker's bank and bindings
    // (heap: the executable's static size stays as it is, main.cpp).
    struct Expected {std::uint32_t bank{},arrays{};};
    static constexpr unsigned expectedCapacity=256;
    Expected* expected{};unsigned expectedCount{},checked{};
    // Direct records (J3D builds the record itself, see the header).
    bool direct=defaultDirect;
    // Verify mode: the direct record of the packet, kept to be compared
    // word for word with the capture of the original code that follows, and
    // the hashes of the J3D state each one left.
    std::uint32_t* kept{};std::uint32_t keptUsed{};bool keptValid{};
    std::uint32_t directState{},realState{};bool realStateSet{};
    Statistics stats{},reported{};
} game;
// The worker's refusals (written by the consumer, read by the report).
std::atomic<std::uint64_t> workerFailures{};
// noteWorkerCore: written by the worker thread once, read by the game thread.
std::atomic<bool> workerSeparate{true};
bool sharedCoreForced{},sharedCoreReported{};

void countCause(const char* why){
    for(auto& c:game.stats.causes){
        if(c.why==why||!c.why){c.why=why;++c.count;return;}
    }
    ++game.stats.causes.back().count; // more causes than places: the last one counts the rest
}
std::uint32_t sum(const void* data,std::size_t bytes){
    const auto* w=static_cast<const std::uint32_t*>(data);std::uint32_t h=0x811C9DC5u;
    for(std::size_t i=0;i<bytes/4;++i)h=(h^w[i])*16777619u;
    return h;
}
std::uint32_t bankSum(const CommandRegisters& bank){return sum(&bank,sizeof(bank));}
// Array bindings are 16 records of handle (8 bytes), offset, size, the byte
// order flag, padding, live address (8 bytes): the 7 padding bytes after the
// flag hold whatever the stack held when the record was assigned, so they
// are left out of comparisons.
static_assert(sizeof(CommandProcessor::ArrayBindings)==16*32);
std::uint32_t bindingsSum(const CommandProcessor::ArrayBindings& arrays){
    std::uint32_t h=0x811C9DC5u;
    for(unsigned slot=0;slot<16;++slot){
        const auto* b=arrays.bytes.data()+slot*32;
        for(unsigned i=0;i<32;++i)if(i<17||i>=24)h=(h^b[i])*16777619u;
    }
    return h;
}
// Where two banks first differ, as a register name, epochs left out (the seal
// moves them on purpose). Empty: equal.
void describeDifference(const CommandRegisters& a,const CommandRegisters& b,char (&out)[32]){
    out[0]=0;
    constexpr std::size_t state=offsetof(CommandRegisters,stateEpoch)/4,setup=offsetof(CommandRegisters,vertexSetupEpoch)/4;
    const auto* x=gx::bankWordsOf(a);const auto* y=gx::bankWordsOf(b);
    for(std::size_t i=0;i<gx::bank_layout::words;++i){
        if(i==state||i==setup||x[i]==y[i])continue;
        namespace L=gx::bank_layout;
        if(i<L::bpAt)std::snprintf(out,sizeof(out),"CP 0x%02x",unsigned(i-L::cpAt));
        else if(i<L::xfAt)std::snprintf(out,sizeof(out),"BP 0x%02x",unsigned(i-L::bpAt));
        else if(i<L::xfAt+L::xfWords)std::snprintf(out,sizeof(out),"XF 0x%04x",unsigned(i-L::xfAt));
        else if(i<L::tailAt)std::snprintf(out,sizeof(out),"valid bits word %u",unsigned(i-L::cpValidAt));
        else std::snprintf(out,sizeof(out),"bank word %u (tail)",unsigned(i));
        return;
    }
}

bool storage(){
    if(game.overlay&&game.arena&&templates&&sealScratch&&game.records&&game.refs)return true;
    if(game.storageFailed)return false;
    if(!game.arena)game.arena=new(std::nothrow)std::uint32_t[arenaWords];
    if(!game.overlay)game.overlay=new(std::nothrow)Overlay;
    if(!templates)if((templates=new(std::nothrow)Templates))if(!(templates->pool=new(std::nothrow)u32[Templates::poolWords])){delete templates;templates=nullptr;}
    if(!sealScratch)sealScratch=new(std::nothrow)SealScratch;
    if(!game.records)game.records=new(std::nothrow)RunRecord[runRecords];
    if(!game.refs)game.refs=new(std::nothrow)OpRef[runRefs];
    if(!game.overlay||!game.arena||!templates||!sealScratch||!game.records||!game.refs){
        game.storageFailed=true;
        if(game.hooks.report)game.hooks.report("Native packets: no memory for the capture; every packet stays on the original path\n");
        return false;
    }
    return true;
}
bool verifyStorage(CommandProcessor& model){
    if(game.scratchA&&game.scratchB&&game.before&&game.verifyOverlay&&game.expected)return true;
    if(!game.expected)game.expected=new(std::nothrow)Game::Expected[Game::expectedCapacity];
    if(!game.scratchA)game.scratchA=new(std::nothrow)CommandProcessor(model.memoryRegistry());
    if(!game.scratchB)game.scratchB=new(std::nothrow)CommandProcessor(model.memoryRegistry());
    if(!game.before)game.before=new(std::nothrow)CommandRegisters;
    if(!game.verifyOverlay)game.verifyOverlay=new(std::nothrow)Overlay;
    if(game.scratchA)game.scratchA->setLiveArrayResolver(model.liveResolver());
    if(game.scratchB)game.scratchB->setLiveArrayResolver(model.liveResolver());
    return game.scratchA&&game.scratchB&&game.before&&game.verifyOverlay&&game.expected;
}

}
void replayUntil(const std::uint32_t* body,std::size_t words,CommandProcessor& p,const gx::TargetExtent& extent,DrawShape draw,
                 std::uint64_t* failures,unsigned stopBeforeShape);
namespace {
// The pending run into the game bank.
bool seal(CommandStream& s){
    if(!game.runPending)return true;
    game.runPending=false;s.setNativePending(false);
    static const auto index=registerGxNamedProfile("native.seal");GxNamedProfileScope profile(index);
    const auto began=game.hooks.now?game.hooks.now():0;
    std::uint64_t written=0;
    auto& p=s.nativeProcessor();auto& o=*game.overlay;
    bool ok=buildOverlay(o,p.registers(),game.records,game.recordCount,game.refs);
    ok=applyPalettes(p,*sealScratch,written)&&ok;
    ok=applyOverlay(p,o,o.ops+1,written)&&ok;
    o.clear();
    game.recordCount=0;game.refsUsed=0;game.arenaUsed=0;game.runPalettes=0;
    // Verify mode of the queue: what it believes the worker holds is stale.
    if(gxworker::queue().verifying())gxworker::queue().invalidateSent();
    ++game.stats.runs;game.stats.sealedWords+=written;
    if(game.hooks.now)game.stats.sealTicks+=game.hooks.now()-began;
    if(!ok)s.nativeReject();
    return ok;
}
// A shape as duskNativeDrawShape and, when it declines, J3DShape::drawFast
// draw it, under the caster slot it was issued with.
void drawShapeNow(CommandStream& s,std::uint32_t generation,const void* list,std::uint32_t size,unsigned caster){
    const auto& h=game.hooks;
    const unsigned saved=h.casterSlot?h.casterSlot():0;
    if(h.setCasterSlot)h.setCasterSlot(caster);
    if(!h.drawShape||!h.drawShape(generation,list,size)){
        if(h.staticGeometry)h.staticGeometry(generation,list);
        (void)(s.flushAutomaticSu()&&s.call({static_cast<const u8*>(list),size}));
        if(h.staticGeometry)h.staticGeometry(0,nullptr);
    }
    if(h.setCasterSlot)h.setCasterSlot(saved);
}
// The record's operations on the game thread's processor, in their order,
// through the entry points the stream would have called: the original path,
// later. `check` (verify mode) sees the bank before each shape is queued.
bool flush(CommandStream& s,const std::uint32_t* body,std::size_t words,bool check){
    auto& p=s.nativeProcessor();
    Reader r{body,words};
    while(r.next()){
        const auto* a=r.op;bool ok=true;
        if(!soundOp(r.kind,r.words,a))break;
        switch(r.kind){
        case PacketOp::bp:ok=p.writeBp(u8(a[1]),a[2]);break;
        case PacketOp::bpMasked:ok=p.writeBpMasked(u8(a[1]),a[2],a[3]);break;
        case PacketOp::cp:ok=p.writeCp(u8(a[1]),a[2]);break;
        case PacketOp::xf:ok=p.writeXf(u16(a[1]),{a+3,a[2]});break;
        case PacketOp::list:ok=p.executeList({static_cast<const u8*>(pointerAt(a+1)),a[3]});break;
        case PacketOp::bytes:ok=p.execute({reinterpret_cast<const u8*>(a+2),a[1]});break;
        case PacketOp::bind:ok=p.bindArray(a[1]&0xff,pointerAt(a+2),a[4],u8(a[1]>>8),(a[1]>>16)&1);break;
        case PacketOp::indexed:ok=p.loadIndexed(u8(a[1]),u16(a[2]),u16(a[1]>>16));break;
        case PacketOp::shape:
            if(check){
                // The bank and bindings the original path queues this shape
                // with, against the worker's replay of the record.
                const unsigned i=game.checked++;
                if(i<game.expectedCount){
                    ++game.stats.verifiedShapes;
                    const auto& e=game.expected[i];
                    if(e.bank!=bankSum(p.registers())||e.arrays!=bindingsSum(p.currentArrayBindings())){
                        if(!game.stats.mirrorMismatches){
                            // The worker's bank at this shape, for the name of the word.
                            (void)game.scratchA->adopt(*game.before,game.arraysBefore);
                            std::uint64_t failures=0;
                            replayUntil(body,words,*game.scratchA,gx::currentTargetExtent,
                                +[](const gxworker::QueuedShape&,const CommandRegisters&,const CommandProcessor::ArrayBindings&,const gx::TargetExtent&){},
                                &failures,i);
                            char difference[32];describeDifference(p.registers(),game.scratchA->registers(),difference);
                            std::snprintf(game.stats.firstMismatch,sizeof(game.stats.firstMismatch),"shape %u %s",i,difference[0]?difference:"epoch or bindings");
                        }
                        ++game.stats.mirrorMismatches;
                    }
                }
            }
            drawShapeNow(s,a[1],pointerAt(a+2),a[4],a[5]);break;
        default:ok=false;break;
        }
        if(!ok){s.nativeReject();return false;}
    }
    return true;
}
// The capture ends early or cannot be shipped: what is pending first, then
// this packet's operations on the original path.
bool settleCapture(CommandStream& s,const std::uint32_t* body,std::size_t words){
    s.setPacketCapture(nullptr);
    if(!seal(s))return false;
    return flush(s,body,words,false);
}
// PacketSettle (gx_stream.hpp): an entry point the capture cannot take, or
// any use of the stream while packets are pending.
bool settle(CommandStream& s){
    if(s.capturing()){
        ++game.stats.ended;game.stats.staleReads+=game.capture.staleReads;game.capture.staleReads=0;
        const char* why=game.capture.ended?game.capture.ended:"GPU work";
        if(!game.stats.firstEnd)game.stats.firstEnd=why;
        countCause(why);
        return settleCapture(s,game.capture.words,game.capture.used);
    }
    return seal(s);
}

// Verify mode: the packet is replayed on a scratch copy as the worker would,
// folded and sealed into another copy, then runs on the original path; the
// shapes and the final bank are compared.
void verifyPacket(CommandStream& s,const std::uint32_t* body,std::size_t words){
    auto& p=s.nativeProcessor();
    if(!verifyStorage(p)){flush(s,body,words,false);return;}
    *game.before=p.registers();game.arraysBefore=p.arrayBindings();
    game.expectedCount=0;game.checked=0;
    (void)game.scratchA->adopt(*game.before,game.arraysBefore);
    std::uint64_t failures=0;
    replay(body,words,*game.scratchA,gx::currentTargetExtent,
        +[](const gxworker::QueuedShape&,const CommandRegisters& bank,const CommandProcessor::ArrayBindings& arrays,const gx::TargetExtent&){
            if(game.expectedCount<Game::expectedCapacity)game.expected[game.expectedCount++]={bankSum(bank),bindingsSum(arrays)};
        },&failures);
    game.stats.workerFailures+=failures;
    auto& o=*game.verifyOverlay;
    // Nothing is pending in verify mode: the run arrays are free.
    unsigned refs=0,palettes=0;
    const char* why=checkRecord(body,words,game.refs,refsPerRecord,refs,palettes);
    if(!why&&palettes>runPaletteLoads)why="palette loads";
    flush(s,body,words,true);
    ++game.stats.verifiedPackets;
    if(why){++game.stats.foldRefused;if(!game.stats.firstFold)game.stats.firstFold=why;countCause(why);return;}
    const RunRecord one{body,std::uint32_t(words),0,refs};
    if(!buildOverlay(o,*game.before,&one,1,game.refs))++game.stats.sealMismatches;
    (void)game.scratchB->adopt(*game.before,game.arraysBefore);
    std::uint64_t written=0;
    applyPalettes(*game.scratchB,*sealScratch,written);
    applyOverlay(*game.scratchB,o,o.ops+1,written);
    char difference[32];describeDifference(p.registers(),game.scratchB->registers(),difference);
    if(difference[0]||bindingsSum(p.currentArrayBindings())!=bindingsSum(game.scratchB->currentArrayBindings())){
        if(!game.stats.sealMismatches)std::snprintf(game.stats.firstMismatch,sizeof(game.stats.firstMismatch),"seal %s",difference[0]?difference:"bindings");
        ++game.stats.sealMismatches;
    }
}
}

bool install(const Hooks& hooks){
    game.hooks=hooks;game.installed=hooks.canQueue&&hooks.drawShape;
    CommandStream::setPacketSettle(&settle);
    // GPU work during a capture (none is expected inside a material packet)
    // first sends the captured shapes the original way.
    gxworker::Queue::captureHook=endCapture;
    return game.installed;
}
void noteWorkerCore(bool separate){workerSeparate.store(separate,std::memory_order_release);}
void configure(Mode mode,bool verify,bool direct){game.mode=mode;game.verify=verify&&mode!=Mode::off;game.direct=direct&&mode!=Mode::off;game.configured=true;}
void configureFromSd(){
    if(game.configured)return;
    char text[8]{};
    if(auto* file=std::fopen("sdmc:/3ds/DuskLight/native-packets.txt","rb")){(void)std::fread(text,1,sizeof(text)-1,file);std::fclose(file);}
    Mode chosen=defaultMode;bool verify=false,direct=defaultDirect;
    for(const char c:text){
        if(c=='0')chosen=Mode::off;
        else if(c=='b')chosen=Mode::bg;
        else if(c=='a')chosen=Mode::all;
        else if(c=='v')verify=true;
        else if(c=='d')direct=true;
        else if(c=='c')direct=false;
        else if(c=='f')sharedCoreForced=true;
    }
    configure(chosen,verify,direct);
    if(game.hooks.report){
        char line[160];
        std::snprintf(line,sizeof(line),"Native packets: %s%s%s (SD switch native-packets.txt: 0 off, b rooms, a all, d direct, c capture, v verify)\n",
            chosen==Mode::off?"off":chosen==Mode::bg?"room model packets":"every rigid material packet",game.direct?", direct records":"",
            game.verify?", verify":"");
        game.hooks.report(line);
    }
}
Mode mode(){return game.mode;}
bool verifying(){return game.verify;}
bool capturing(){return game.open&&game.stream&&game.stream->capturing();}
void endCapture(){if(capturing())settle(*game.stream);}

bool begin(int kind){
    if(game.mode==Mode::off||!game.installed)return false;
    if(kind==notRigid||(game.mode==Mode::bg&&kind!=roomModel)){++game.stats.notEligible;return false;}
    if(!sharedCoreForced&&!workerSeparate.load(std::memory_order_acquire)){
        if(!sharedCoreReported&&game.hooks.report){
            sharedCoreReported=true;
            game.hooks.report("Native packets: the GX worker shares the game thread's core; packets stay on the original path (native-packets.txt f ships them anyway)\n");
        }
        ++game.stats.queueRefused;return false;
    }
    auto* s=gx::commandStream();
    if(!s||game.open||s->capturing()||!s->nativeCaptureReady()||gx::materialMemoRecording()){++game.stats.streamRefused;return false;}
    if(!game.hooks.canQueue()){++game.stats.queueRefused;return false;}
    if(!storage()){++game.stats.storageRefused;return false;}
    // A full run is sealed before the next packet is captured.
    if(game.runPending&&(arenaWords-game.arenaUsed<packetWords||game.recordCount==runRecords||runRefs-game.refsUsed<refsPerRecord||
                         game.runPalettes+64>runPaletteLoads)){
        if(!seal(*s)){++game.stats.streamRefused;return false;}
    }
    auto& c=game.capture;
    c.words=game.arena+game.arenaUsed;
    c.capacity=packetWords;c.used=0;c.ops=0;c.shapes=0;c.matrices={};c.ended=nullptr;c.staleReads=0;
    game.stream=s;game.open=true;++game.stats.captured;
    s->setPacketCapture(&c);
    return true;
}
bool captureShape(std::uint32_t generation,const void* list,std::uint32_t size,unsigned caster){
    if(!capturing())return false;
    auto& c=game.capture;auto& s=*game.stream;
    if(!list||!size||c.capacity-c.used<6){
        if(!c.ended)c.ended=!size?"empty shape list":"record full";
        settle(s);
        return false;
    }
    std::uint32_t* at=c.words+c.used;c.used+=6;++c.ops;++c.shapes;
    at[0]=std::uint32_t(PacketOp::shape)|(6u<<8);at[1]=generation;
    const auto value=std::uint64_t(reinterpret_cast<std::uintptr_t>(list));at[2]=std::uint32_t(value);at[3]=std::uint32_t(value>>32);
    at[4]=size;at[5]=caster;
    return true;
}
void compareDirect(const gx::PacketCapture& c){
    if(!game.keptValid)return;
    game.keptValid=false;
    ++game.stats.directCompared;
    if(game.keptUsed!=c.used||std::memcmp(game.kept,c.words,c.used*sizeof(std::uint32_t))){
        if(!game.stats.directMismatches){
            // The first operation that differs, for the log.
            Reader a{game.kept,game.keptUsed},b{c.words,c.used};unsigned index=0;
            while(a.next()&&b.next()&&a.words==b.words&&!std::memcmp(a.op,b.op,a.words*4))++index;
            std::snprintf(game.stats.firstDirect,sizeof(game.stats.firstDirect),"op %u (%u against %u)",index,
                a.op?unsigned(a.kind):0u,b.op?unsigned(b.kind):0u);
        }
        ++game.stats.directMismatches;
    }
    if(game.realStateSet&&game.realState!=game.directState)++game.stats.directStateMismatches;
    game.realStateSet=false;
}
// ---- direct records ----
namespace {
// One operation of the direct record, in the capture's format (gx_stream.cpp
// captureWords). Null: no capture, or the record is full.
std::uint32_t* directWords(PacketOp op,std::uint32_t words){
    if(!capturing())return nullptr;
    auto& c=game.capture;
    if(c.capacity-c.used<words)return nullptr;
    std::uint32_t* at=c.words+c.used;at[0]=std::uint32_t(op)|(words<<8);c.used+=words;++c.ops;
    return at;
}
void touchSetup(){game.stream->nativeProcessor().touchVertexSetup();}
void putPointer(std::uint32_t* at,const void* pointer){
    const auto value=std::uint64_t(reinterpret_cast<std::uintptr_t>(pointer));at[0]=std::uint32_t(value);at[1]=std::uint32_t(value>>32);
}
// A command written through write(): its bytes, padded with zeros.
bool directBytes(const u8* bytes,std::uint32_t count){
    auto* at=directWords(PacketOp::bytes,2+(count+3)/4);
    if(!at)return false;
    at[1]=count;at[1+(count+3)/4]=0;std::memcpy(at+2,bytes,count);
    if(bytes[0]==GX_LOAD_CP_REG||bytes[0]==GX_LOAD_XF_REG)touchSetup();
    return true;
}
}
bool directEnabled(){
    return game.direct&&game.mode!=Mode::off&&game.installed&&(sharedCoreForced||workerSeparate.load(std::memory_order_relaxed));
}
bool directVerifying(){return game.direct&&game.verify;}
bool directBegin(int kind){
    if(!begin(kind))return false;
    ++game.stats.directTried;
    return true;
}
bool directList(const void* list,std::uint32_t size){
    if(game.stream&&!game.stream->finish())return false;
    auto* at=directWords(PacketOp::list,4);
    if(!at)return false;
    putPointer(at+1,list);at[3]=size;touchSetup();
    return true;
}
bool directCurrentMatrix(std::uint32_t a,std::uint32_t b){
    // J3DCurrentMtx::load: CP 0x30, CP 0x40, XF 0x1018 (two words), as GXCmd bytes.
    const u8 cpA[6]{GX_LOAD_CP_REG,0x30,u8(a>>24),u8(a>>16),u8(a>>8),u8(a)};
    const u8 cpB[6]{GX_LOAD_CP_REG,0x40,u8(b>>24),u8(b>>16),u8(b>>8),u8(b)};
    const u8 xfAB[13]{GX_LOAD_XF_REG,0,1,0x10,0x18,u8(a>>24),u8(a>>16),u8(a>>8),u8(a),u8(b>>24),u8(b>>16),u8(b>>8),u8(b)};
    return directBytes(cpA,6)&&directBytes(cpB,6)&&directBytes(xfAB,13);
}
bool directBind(unsigned slot,const void* data,std::uint32_t size,std::uint32_t stride){
    if(slot>=16||(!data&&size))return false;
    auto* at=directWords(PacketOp::bind,5);
    if(!at)return false;
    at[1]=slot|((stride&0xff)<<8)|(1u<<16);putPointer(at+2,data);at[4]=size;
    if(slot>=12)game.capture.matrices[slot-12]={reinterpret_cast<std::uintptr_t>(data),size,std::uint8_t(stride),true,true};
    touchSetup();
    return true;
}
bool directIndexed(unsigned code,unsigned index,unsigned packed){
    const u32 count=(packed>>12)+1,address=packed&0xfff;
    if(!(code==GX_LOAD_INDX_A||code==GX_LOAD_INDX_B||code==GX_LOAD_INDX_C||code==GX_LOAD_INDX_D)||address>=0x1058||count>0x1058-address)return false;
    const auto& m=game.capture.matrices[(code-GX_LOAD_INDX_A)/8];
    const std::uint64_t at=std::uint64_t(index)*m.stride;
    const auto* source=reinterpret_cast<const u8*>(m.address);
    if(!m.known||!source||at>m.size||count*4>m.size-at)return false;
    auto* out=directWords(PacketOp::indexed,4+count);
    if(!out)return false;
    out[1]=code|(std::uint32_t(packed)<<16);out[2]=index;out[3]=m.little;std::memcpy(out+4,source+at,count*4);
    return true;
}
bool directShape(std::uint32_t generation,const void* list,std::uint32_t size){
    if(!list||!size)return false;
    auto* at=directWords(PacketOp::shape,6);
    if(!at)return false;
    ++game.capture.shapes;
    at[1]=generation;putPointer(at+2,list);at[4]=size;
    // Caster slot (8 bits) and, above it, the owner for the measured dump (gx_profile.hpp).
    at[5]=((game.hooks.casterSlot?game.hooks.casterSlot():0)&0xffffu)|(std::uint32_t(gxPaintOwner)<<16);
    return true;
}
bool directOpen(){return capturing();}
void directEnd(std::uint32_t stateHash){
    if(!capturing()){++game.stats.directIncomplete;return;}
    ++game.stats.directRecords;
    if(!game.verify){end();return;}
    // Verify: kept, then the original code runs (captured) and is compared.
    auto& c=game.capture;
    if(!game.kept)game.kept=new(std::nothrow)std::uint32_t[packetWords];
    if(game.kept){std::memcpy(game.kept,c.words,c.used*sizeof(std::uint32_t));game.keptUsed=c.used;game.keptValid=true;game.directState=stateHash;}
    game.stream->setPacketCapture(nullptr);game.open=false; // dropped: nothing of it ran
}
void directAbort(const char* why){
    if(game.open&&game.stream)game.stream->setPacketCapture(nullptr);
    game.open=false;
    ++game.stats.directFallbacks;countCause(why);
}
void directFallback(const char* why){++game.stats.directFallbacks;countCause(why);}
void directRealState(std::uint32_t stateHash){game.realState=stateHash;game.realStateSet=true;}
void end(){
    if(!game.open)return;
    game.open=false;
    auto& s=*game.stream;
    if(!s.capturing()){ // ended early: it ran on the original path
        if(game.keptValid){game.keptValid=false;++game.stats.directIncomplete;}
        return;
    }
    static const auto index=registerGxNamedProfile("native.packetEnd");GxNamedProfileScope profile(index);
    auto& c=game.capture;
    s.setPacketCapture(nullptr);
    game.stats.staleReads+=c.staleReads;c.staleReads=0;
    if(c.ended){
        // A command the capture kept only to keep the order.
        ++game.stats.ended;if(!game.stats.firstEnd)game.stats.firstEnd=c.ended;countCause(c.ended);
        settleCapture(s,c.words,c.used);return;
    }
    if(game.verify){
        // Nothing is ever pending in verify mode: the bank is current.
        compareDirect(c);
        verifyPacket(s,c.words,c.used);return;
    }
    auto& p=s.nativeProcessor();
    auto& queue=gxworker::queue();
    const bool prelude=!game.runPending;
    if(!queue.nativeFits(prelude?p.bankStores().used:0,c.used)){
        ++game.stats.queueRefused;settleCapture(s,c.words,c.used);return;
    }
    // Every list against its template (begin left room for the operations).
    const auto began=gxSparseVertexProfiling&&game.hooks.now?game.hooks.now():0;
    unsigned refs=0,palettes=0;
    const char* why=checkRecord(c.words,c.used,game.refs+game.refsUsed,refsPerRecord,refs,palettes);
    if(!why&&game.runPalettes+palettes>runPaletteLoads)why="palette loads";
    if(why){
        ++game.stats.foldRefused;if(!game.stats.firstFold)game.stats.firstFold=why;countCause(why);
        settleCapture(s,c.words,c.used);return;
    }
    if(began)game.stats.foldTicks+=game.hooks.now()-began;
    if(!queue.pushNative(p.registers(),p.bankStores(),p.currentArrayBindings(),gx::currentTargetExtent,c.words,c.used,prelude,c.shapes)){
        // Not expected after nativeFits: nothing of it was shipped.
        ++game.stats.queueRefused;settleCapture(s,c.words,c.used);return;
    }
    if(prelude)p.restartBankLog();
    // Kept for the seal where it was captured.
    game.records[game.recordCount++]={c.words,c.used,game.refsUsed,refs};
    game.refsUsed+=refs;game.arenaUsed+=c.used;game.runPalettes+=palettes;
    game.runPending=true;s.setNativePending(true);
    ++game.stats.packets;game.stats.shapes+=c.shapes;game.stats.recordWords+=c.used;
}

void replay(const std::uint32_t* body,std::size_t words,CommandProcessor& p,const gx::TargetExtent& extent,DrawShape draw,std::uint64_t* failures){
    replayUntil(body,words,p,extent,draw,failures,~0u);
}
void replayUntil(const std::uint32_t* body,std::size_t words,CommandProcessor& p,const gx::TargetExtent& extent,DrawShape draw,
                 std::uint64_t* failures,unsigned stopBeforeShape){
    Reader r{body,words};std::uint64_t refused=0;unsigned shapes=0;
    while(r.next()){
        const auto* a=r.op;bool ok=true;
        if(!soundOp(r.kind,r.words,a)){++refused;break;}
        switch(r.kind){
        case PacketOp::bp:ok=p.writeBp(u8(a[1]),a[2]);break;
        // The two writes leave the bank as writeBpMasked does (its shortcut
        // for an unchanged value only skips work); the typed-write counters
        // stay the game thread's.
        case PacketOp::bpMasked:ok=p.writeBp(0xfe,a[2])&&p.writeBp(u8(a[1]),a[3]);break;
        case PacketOp::cp:ok=p.writeCp(u8(a[1]),a[2]);break;
        case PacketOp::xf:ok=p.writeXf(u16(a[1]),{a+3,a[2]});break;
        // execute, never executeList: the compiled-list cache belongs to the
        // game thread (both give the same bank).
        case PacketOp::list:ok=p.execute({static_cast<const u8*>(pointerAt(a+1)),a[3]});break;
        case PacketOp::bytes:ok=p.execute({reinterpret_cast<const u8*>(a+2),a[1]});break;
        case PacketOp::bind:ok=p.bindArray(a[1]&0xff,pointerAt(a+2),a[4],u8(a[1]>>8),(a[1]>>16)&1);break;
        case PacketOp::indexed:{
            const u32 packed=a[1]>>16,count=(packed>>12)+1;
            ok=p.loadIndexedValues(u8(a[1]),u16(packed),{reinterpret_cast<const u8*>(a+4),count*4},a[3]!=0);break;}
        case PacketOp::shape:{
            if(shapes++==stopBeforeShape)return;
            gxworker::QueuedShape shape;
            shape.generation=a[1];shape.list=pointerAt(a+2);shape.listBytes=a[4];shape.casterSlot=a[5]&0xffffu;shape.owner=a[5]>>16;
            draw(shape,p.registers(),p.currentArrayBindings(),extent);break;}
        default:ok=false;break;
        }
        if(!ok){++refused;p.clearFailure();}
    }
    if(r.damaged)++refused;
    if(refused){
        if(failures)*failures+=refused;
        else workerFailures.fetch_add(refused,std::memory_order_relaxed);
    }
}

const Statistics& statistics(){
    if(templates){game.stats.listsCached=templates->hits;game.stats.listsParsed=templates->builds;game.stats.listsChanging=templates->rebuilt;}
    return game.stats;
}
void report(unsigned pictures){
    auto& st=game.stats;
    if(game.mode==Mode::off||!game.installed||!game.hooks.report||!pictures)return;
    st.pictures+=pictures;
    const auto& was=game.reported;
    const double n=double(pictures);
    auto per=[&](std::uint64_t now,std::uint64_t before){return double(now-before)/n;};
    const double ms=game.hooks.ticksPerMillisecond?double(st.sealTicks-was.sealTicks)/double(game.hooks.ticksPerMillisecond)/n:0;
    const std::uint64_t worker=workerFailures.load(std::memory_order_relaxed)+st.workerFailures;
    (void)statistics(); // the list cache's counts
    // Each line stays under the 255 characters OSReport keeps.
    char line[240];
    std::snprintf(line,sizeof(line),"Native packets (%s%s%s), last %u pictures, per picture: captured %.1f, direct %.1f, shipped %.1f "
        "(shapes %.1f, words %.0f), runs %.1f, sealed words %.0f in %.3f ms\n",
        game.mode==Mode::bg?"rooms":"rigid",game.direct?", direct":"",game.verify?", verify: none shipped":"",pictures,
        per(st.captured,was.captured),per(st.directRecords,was.directRecords),per(st.packets,was.packets),per(st.shapes,was.shapes),
        per(st.recordWords,was.recordWords),per(st.runs,was.runs),per(st.sealedWords,was.sealedWords),ms);
    game.hooks.report(line);
    std::snprintf(line,sizeof(line),"Native packets on the original path, per picture: not eligible %.1f, queue %.1f, stream %.1f, "
        "storage %.1f, ended early %.1f, refused %.1f; stale reads %.1f; worker refusals %llu; list templates used %.1f built %.1f\n",
        per(st.notEligible,was.notEligible),per(st.queueRefused,was.queueRefused),per(st.streamRefused,was.streamRefused),
        per(st.storageRefused,was.storageRefused),per(st.ended,was.ended),per(st.foldRefused,was.foldRefused),
        per(st.staleReads,was.staleReads),static_cast<unsigned long long>(worker),per(st.listsCached,was.listsCached),
        per(st.listsParsed+st.listsChanging,was.listsParsed+was.listsChanging));
    game.hooks.report(line);
    if(game.direct){
        std::snprintf(line,sizeof(line),"Native packets direct, per picture: tried %.1f, built %.1f, fallbacks %.1f; verify so far: compared %llu, "
            "record mismatches %llu, state mismatches %llu, incomplete %llu%s%s\n",
            per(st.directTried,was.directTried),per(st.directRecords,was.directRecords),per(st.directFallbacks,was.directFallbacks),
            static_cast<unsigned long long>(st.directCompared),static_cast<unsigned long long>(st.directMismatches),
            static_cast<unsigned long long>(st.directStateMismatches),static_cast<unsigned long long>(st.directIncomplete),
            st.firstDirect[0]?", first: ":"",st.firstDirect);
        game.hooks.report(line);
    }
    if(game.verify){
        std::snprintf(line,sizeof(line),"Native packets verify (all so far): packets %llu shapes %llu mirror mismatches %llu seal mismatches %llu%s%s\n",
            static_cast<unsigned long long>(st.verifiedPackets),static_cast<unsigned long long>(st.verifiedShapes),
            static_cast<unsigned long long>(st.mirrorMismatches),static_cast<unsigned long long>(st.sealMismatches),
            st.firstMismatch[0]?", first: ":"",st.firstMismatch);
        game.hooks.report(line);
    }
    // The causes behind "ended early" and "fold refused", as many lines as
    // they need.
    int used=0;bool any=false;
    auto flush=[&]{if(used>0){line[used]='\n';line[used+1]=0;game.hooks.report(line);}used=0;};
    for(const auto& c:st.causes){
        if(!c.why||!c.count)continue;
        char item[64];const int length=std::snprintf(item,sizeof(item)," %s %llu;",c.why,static_cast<unsigned long long>(c.count));
        if(length<=0)continue;
        if(used&&used+length>int(sizeof(line))-2)flush();
        if(!used)used=std::snprintf(line,sizeof(line),"Native packets kept on the original path, by cause:");
        if(used+length<=int(sizeof(line))-2){std::memcpy(line+used,item,std::size_t(length));used+=length;any=true;}
    }
    if(any)flush();
    for(auto& c:st.causes)c.count=0;
    game.reported=st;
}
}
