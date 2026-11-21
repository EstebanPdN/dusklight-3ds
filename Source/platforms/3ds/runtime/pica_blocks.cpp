#include "pica_blocks.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

namespace dusk::native::blocks {
namespace {
constexpr unsigned wordsOf(std::size_t bytes){return unsigned((bytes+3)/4);}
bool rowBit(const std::array<std::uint32_t,3>& mask,unsigned row){return row<96&&(mask[row/32]>>(row%32))&1;}
void setRowBit(std::array<std::uint32_t,3>& mask,unsigned row){if(row<96)mask[row/32]|=1u<<(row%32);}
bool wordValid(const gx::CommandRegisters& r,const Range& range,unsigned i){
    return range.bank?bool(r.bpValid[range.first+i]):bool(r.xfValid[range.first+i]);
}
const std::uint32_t* wordsAt(const gx::CommandRegisters& r,const Range& range){
    return range.bank?r.bp.data()+range.first:r.xf.data()+range.first;
}
// The uniform data registers of the vertex shader unit (GPUREG_VSH_FLOATUNIFORM_*).
constexpr std::uint32_t uniformConfig=0x2c0,uniformDataFirst=0x2c1,uniformDataLast=0x2c8;
}

bool planInputs(const gx::CommandRegisters& r,std::uint8_t coordinates,InputPlan& out){
    out={};
    auto add=[&](std::uint8_t bank,unsigned first,unsigned count){
        if(out.count>=maxRanges)return false;
        out.ranges[out.count++]={std::uint16_t(first),std::uint16_t(count),bank};
        return true;
    };
    // Ambient and material colours of both channels (planRasterColor).
    // Viewport, projection and its type (the clip projection, the fog range
    // and the vertex fog plan; NativeProjectionCache reads the same words).
    // Fog curve, range and colour (planVertexFog).
    if(!add(0,0x100a,4)||!add(0,0x101a,13)||!add(1,0xe8,11))return false;
    // The lights any channel names (light masks: bits 2-5 and 11-14).
    unsigned lights=0;
    for(unsigned reg=0x100e;reg<=0x1011;++reg)if(r.xfValid[reg]){const auto c=r.xf[reg];lights|=((c>>2)&15)|((c>>7)&240);}
    for(unsigned light=0;light<8;++light)if(lights&(1u<<light))if(!add(0,0x600+16*light,16))return false;
    // Texgens: the texture matrix each coordinate selects and its post
    // matrix (three rows each: the projected form reads three).
    for(unsigned c=0;c<8;++c)if(coordinates&(1u<<c)){
        const unsigned m=c<4?(r.matrixIndex[0]>>(6+6*c))&63:(r.matrixIndex[1]>>(6*(c-4)))&63;
        // A position matrix slot changes with every shape (the chain refuses it too).
        if(m<30||m>60)return false;
        if(m!=60&&!add(0,4*m,12))return false;
        if(!r.xfValid[0x1050+c])return false;
        const unsigned post=r.xf[0x1050+c]&63;
        if(post>61)return false;
        if(post!=61&&!add(0,0x500+4*post,12))return false;
    }
    return true;
}
unsigned inputWords(const InputPlan& plan){
    unsigned words=plan.count;
    for(unsigned i=0;i<plan.count;++i)words+=plan.ranges[i].count;
    return words;
}
void takeInputs(const gx::CommandRegisters& r,const InputPlan& plan,std::uint32_t* out){
    std::uint32_t* masks=out;
    for(unsigned i=0;i<plan.count;++i){const auto& range=plan.ranges[i];std::memcpy(masks,wordsAt(r,range),range.count*4u);masks+=range.count;}
    for(unsigned i=0;i<plan.count;++i){
        const auto& range=plan.ranges[i];std::uint32_t mask=0;
        for(unsigned w=0;w<range.count;++w)mask|=std::uint32_t(wordValid(r,range,w))<<w;
        masks[i]=mask;
    }
}
int changedInput(const gx::CommandRegisters& r,const InputPlan& plan,const std::uint32_t* kept){
    const std::uint32_t* values=kept;
    int changed=-1;
    for(unsigned i=0;i<plan.count&&changed<0;++i){
        const auto& range=plan.ranges[i];
        if(std::memcmp(values,wordsAt(r,range),range.count*4u))changed=int(i);
        values+=range.count;
    }
    for(unsigned i=0;i<plan.count&&changed<0;++i){
        const auto& range=plan.ranges[i];std::uint32_t mask=0;
        for(unsigned w=0;w<range.count;++w)mask|=std::uint32_t(wordValid(r,range,w))<<w;
        if(mask!=values[i])changed=int(i);
    }
    if(changed<0)return -1;
    const auto& range=plan.ranges[unsigned(changed)];
    if(range.bank)return inputFog;
    if(range.first==0x100a)return inputColour;
    if(range.first==0x101a)return inputProjection;
    if(range.first>=0x600&&range.first<0x680)return inputLight;
    return inputTexgen;
}
bool sameInputs(const gx::CommandRegisters& r,const InputPlan& plan,const std::uint32_t* kept){
    const std::uint32_t* values=kept;
    for(unsigned i=0;i<plan.count;++i){
        const auto& range=plan.ranges[i];
        if(std::memcmp(values,wordsAt(r,range),range.count*4u))return false;
        values+=range.count;
    }
    for(unsigned i=0;i<plan.count;++i){
        const auto& range=plan.ranges[i];std::uint32_t mask=0;
        for(unsigned w=0;w<range.count;++w)mask|=std::uint32_t(wordValid(r,range,w))<<w;
        if(mask!=values[i])return false;
    }
    return true;
}

// A command is its first parameter, a header (register, byte mask, count of
// further parameters, consecutive flag) and those parameters, padded to an
// even number of words (libctru GPUCMD_Add).
bool findUniformWords(std::span<const std::uint32_t> run,UniformLayout& out){
    out={};
    std::uint32_t row=0,word=0;bool wide=false,configured=false;
    for(std::size_t i=0;i+1<run.size();){
        const std::uint32_t header=run[i+1];
        const std::uint32_t reg=header&0xffff,extra=(header>>20)&0xff,mask=(header>>16)&0xf;
        const bool consecutive=header>>31;
        const std::size_t span=(2+extra+1)&~1u;
        if(i+span>run.size())return false;
        for(std::uint32_t k=0;k<=extra;++k){
            const std::size_t at=k==0?i:i+1+k;
            const std::uint32_t target=consecutive?reg+k:reg,value=run[at];
            if(target==uniformConfig){
                if(mask!=0xf)return false;
                row=value&0xff;wide=value>>31;word=0;configured=true;
                continue;
            }
            if(target<uniformDataFirst||target>uniformDataLast)continue;
            if(mask!=0xf||!configured||row>=96)return false;
            if(wide){
                setRowBit(out.float32Rows,row);
                const std::uint32_t rowWord=row*4+word;
                auto& last=out.segments[out.count?out.count-1:0];
                if(out.count&&last.at+last.count==at&&last.rowWord+last.count==rowWord)++last.count;
                else{
                    if(out.count>=maxSegments||at>0xffff)return false;
                    out.segments[out.count++]={std::uint16_t(at),1,std::uint16_t(rowWord)};
                }
                if(++word==4){word=0;++row;}
            }else{
                setRowBit(out.float24Rows,row);
                if(++word==3){word=0;++row;}
            }
        }
        i+=span;
    }
    return true;
}
WordPlace describeWord(std::span<const std::uint32_t> run,std::uint32_t where){
    WordPlace place;
    std::uint32_t row=0,word=0;bool wide=false;
    for(std::size_t i=0;i+1<run.size();){
        const std::uint32_t header=run[i+1];
        const std::uint32_t reg=header&0xffff,extra=(header>>20)&0xff;
        const bool consecutive=header>>31;
        const std::size_t span=(2+extra+1)&~1u;
        for(std::uint32_t k=0;k<=extra;++k){
            const std::size_t at=k==0?i:i+1+k;
            if(at>=run.size())break;
            const std::uint32_t target=consecutive?reg+k:reg;
            const bool data=target>=uniformDataFirst&&target<=uniformDataLast;
            if(at==where){place.reg=target;place.row=data?std::int32_t(row):-1;return place;}
            if(target==uniformConfig){row=run[at]&0xff;wide=run[at]>>31;word=0;}
            else if(data&&++word==(wide?4u:3u)){word=0;++row;}
        }
        if(where==i+1){place.reg=reg;place.header=true;return place;}
        if(where>=i&&where<i+span){place.reg=reg;return place;} // padding
        i+=span;
    }
    return place;
}

bool placePalette(const gx::CommandRegisters& r,unsigned position,const PaletteShape& s,gx::RigidVertexPlan& p){
    p.matrixOffsets.fill(-1);p.normalOffsets.fill(-1);p.matrices=0;p.compactPalette=false;p.paletteEnd=0;
    p.multiTexture=s.multiTexture;p.multiRowUnits=s.multiRowUnits;p.lights=s.lights;p.projectedTexture=false;
    p.normalsUnused=s.normalsUnused;p.pairedUnits=0;p.foldedOrigin={};
    if(position>27||position%3||!gx::addRigidMatrix(r,position,p))return false;
    if(s.compactPalette)gx::restartCompactPalette(p);
    for(unsigned n=0;n<s.matrixCount&&n<s.matrixOrder.size();++n)if(!gx::addRigidMatrix(r,s.matrixOrder[n]*3u,p))return false;
    if(p.matrixOffsets!=s.matrixOffsets||p.normalOffsets!=s.normalOffsets)return false;
    // finishCompactRigidUniforms: every row applied to (position,1) takes the
    // origin into its constant, once per distinct row set, all or nothing.
    const double o[3]{double(s.origin[0]),double(s.origin[1]),double(s.origin[2])};
    if(o[0]!=0||o[1]!=0||o[2]!=0){
        for(const bool apply:{false,true}){
            bool finite=true;
            for(unsigned m=0;m<p.matrixOffsets.size();++m){
                const int offset=p.matrixOffsets[m];bool earlier=false;
                for(unsigned k=0;k<m;++k)earlier|=p.matrixOffsets[k]==offset;
                if(offset<0||earlier)continue;
                for(unsigned row=0;row<3;++row){
                    auto& v=p.uniforms[unsigned(offset)+row];
                    const float value=float(double(v[3])+double(v[0])*o[0]+double(v[1])*o[1]+double(v[2])*o[2]);
                    if(apply)v[3]=value;else finite&=std::isfinite(value)&&std::abs(value)<=1e8f;
                }
            }
            if(!finite)return false;
        }
        p.foldedOrigin=s.origin;
    }
    return true;
}
PaletteRows paletteRows(const gx::RigidVertexPlan& p){
    const unsigned base=p.paletteBase(),end=p.uniformCount();
    return {6u,base,std::max(base,end)};
}
PaletteRows uploadRows(const gx::RigidVertexPlan& p){
    const unsigned count=std::min<unsigned>(p.uniformCount(),94);
    if(p.multiTexture)return {count,std::max(count,p.paletteLimit()),94u};
    if(p.compact||p.color.ambientVertex)return {count,std::max(count,93u),94u};
    return {count,count,count};
}
bool rowsCovered(const UniformLayout& layout,unsigned base,const PaletteRows& rows){
    std::array<std::uint32_t,12> covered{}; // 384 row words
    for(unsigned s=0;s<layout.count;++s){
        const auto& seg=layout.segments[s];
        for(unsigned w=0;w<seg.count;++w){const unsigned at=seg.rowWord+w;if(at<384)covered[at/32]|=1u<<(at%32);}
    }
    auto check=[&](unsigned first,unsigned end){
        for(unsigned row=first;row<end;++row){
            const unsigned id=base+row;
            if(id>=96||rowBit(layout.float24Rows,id))return false;
            for(unsigned w=0;w<4;++w){const unsigned at=id*4+w;if(!((covered[at/32]>>(at%32))&1))return false;}
        }
        return true;
    };
    return rows.firstEnd<=rows.second&&check(0,rows.firstEnd)&&check(rows.second,rows.end);
}
void patchRows(std::uint32_t* run,const UniformLayout& layout,unsigned base,const gx::RigidVertexPlan& plan,const PaletteRows& rows){
    for(const auto [first,end]:{std::pair<unsigned,unsigned>{0,rows.firstEnd},std::pair<unsigned,unsigned>{rows.second,rows.end}}){
        if(first>=end)continue;
        const unsigned low=(base+first)*4,high=(base+end)*4;
        for(unsigned s=0;s<layout.count;++s){
            const auto& seg=layout.segments[s];
            const unsigned from=std::max<unsigned>(low,seg.rowWord),to=std::min<unsigned>(high,seg.rowWord+seg.count);
            for(unsigned w=from;w<to;++w){
                // C3D_FVec holds w, z, y, x: word k of a row is component 3-k.
                const unsigned row=w/4-base,k=w%4;
                std::uint32_t value;std::memcpy(&value,&plan.uniforms[row][3-k],4);
                run[seg.at+(w-seg.rowWord)]=value;
            }
        }
    }
}

void Store::attach(std::uint32_t* p,std::uint32_t words,unsigned count,std::uint32_t* table,std::uint8_t* h){
    pool=p;poolWords=words;slots=count;offsets=table;heat=h;
    clear();
    if(heat)std::memset(heat,0,2*std::size_t(slots));
}
void Store::clear(){
    poolUsed=0;
    if(offsets)std::fill(offsets,offsets+slots,none);
}
unsigned Store::kept()const{
    unsigned n=0;
    for(unsigned i=0;i<slots;++i)n+=offsets[i]!=none;
    return n;
}
Block* Store::keep(unsigned slot,std::span<const std::uint32_t> run,const gx::CommandRegisters& r,const InputPlan& plan,
                   const UniformLayout& layout,std::span<const std::byte> facts){
    if(!pool||slot>=slots)return nullptr;
    const unsigned header=wordsOf(sizeof(Block)),values=inputWords(plan),planWords=wordsOf(sizeof(InputPlan)),
        layoutWords=wordsOf(sizeof(UniformLayout)),factWords=wordsOf(facts.size());
    const std::size_t total=std::size_t(header)+run.size()+values+planWords+layoutWords+factWords;
    if(run.empty()||run.size()>maxBlockWords||total>poolWords){drop(slot);++stats.refusals[unsigned(Refusal::size)];return nullptr;}
    // A new capture of the slot goes over its previous block when it fits
    // (recaptures after a miss would otherwise fill the pool with dead blocks).
    std::uint32_t start=none,capacity=0;
    if(const auto* old=find(slot);old&&old->capacity>=total){start=offsets[slot];capacity=old->capacity;++stats.inPlace;}
    drop(slot);
    if(start==none){
        if(total>poolWords-poolUsed){
            // Nothing in the pool is read by the GPU (blocks are copied into
            // the command buffer), so it can start again at once.
            clear();++stats.recycles;
        }
        start=poolUsed;capacity=std::uint32_t(total);poolUsed+=capacity;
    }
    std::uint32_t* at=pool+start;
    auto* b=new(at)Block{};b->capacity=capacity;
    std::uint32_t cursor=header;
    std::memcpy(at+cursor,run.data(),run.size()*4);b->wordCount=std::uint32_t(run.size());cursor+=std::uint32_t(run.size());
    b->inputsAt=cursor;takeInputs(r,plan,at+cursor);cursor+=values;
    b->planAt=cursor;new(at+cursor)InputPlan(plan);cursor+=planWords;
    b->layoutAt=cursor;new(at+cursor)UniformLayout(layout);cursor+=layoutWords;
    b->factsAt=cursor;b->factBytes=std::uint32_t(facts.size());
    if(factWords){at[cursor+factWords-1]=0;std::memcpy(at+cursor,facts.data(),facts.size());}
    cursor+=factWords;
    offsets[slot]=start;
    return b;
}
bool Store::resting(unsigned slot){
    if(slot>=slots||!heat[2*slot+1])return false;
    --heat[2*slot+1];return true;
}
bool Store::missed(unsigned slot){
    if(slot>=slots)return false;
    auto& h=heat[2*slot];
    unsigned misses=(h&15)+1,level=h>>4;
    const bool rest=misses>=2;
    if(rest){heat[2*slot+1]=std::uint8_t(16u<<level);if(level<3)++level;misses=0;}
    h=std::uint8_t(misses|(level<<4));
    return rest;
}
namespace {
// A count in at most six characters: exact below 100000, then k, M, G, T.
struct Count {
    char text[8];
    explicit Count(std::uint64_t value){
        static constexpr char units[]{0,'k','M','G','T','P'};
        unsigned unit=0;
        while(value>=100000&&unit+1<sizeof(units)){value/=1000;++unit;}
        if(value>=100000)value=99999;
        if(unit)std::snprintf(text,sizeof(text),"%lu%c",static_cast<unsigned long>(value),units[unit]);
        else std::snprintf(text,sizeof(text),"%lu",static_cast<unsigned long>(value));
    }
};
}
int formatReport(char* out,std::size_t size,unsigned line,Mode mode,const Statistics& s,const Store& store,double hitMilliseconds){
    const char* name=mode==Mode::off?"off":mode==Mode::verify?"verify":"on";
    auto miss=[&](Miss which){return Count(s.misses[unsigned(which)]);};
    if(line==0){
        return std::snprintf(out,size,"Native PICA blocks (%s): replays=%s hits=%s new=%s miss rec=%s in=%s dyn=%s tgt=%s sto=%s tex=%s pal=%s "
            "rest=%s room=%s kind=%s; in by col/prj/fog/lit/tex %s/%s/%s/%s/%s\n",name,Count(s.replays).text,Count(s.hits).text,miss(Miss::none).text,miss(Miss::record).text,
            miss(Miss::inputs).text,miss(Miss::dynamic).text,miss(Miss::target).text,miss(Miss::storage).text,miss(Miss::texture).text,
            miss(Miss::palette).text,miss(Miss::resting).text,miss(Miss::room).text,miss(Miss::kind).text,
            Count(s.inputsChanged[0]).text,Count(s.inputsChanged[1]).text,Count(s.inputsChanged[2]).text,Count(s.inputsChanged[3]).text,
            Count(s.inputsChanged[4]).text);
    }
    const auto& f=s.refusals;
    if(!(hitMilliseconds>=0&&hitMilliseconds<1e6))hitMilliseconds=0;
    // refused: kind/span/size/rows/pool; row-hits: rows-mode hits/attempts.
    return std::snprintf(out,size,"Native PICA blocks store: kept=%u %lu/%lu KiB captures=%s refused %s/%s/%s/%s/%s "
        "recycles=%s reuse=%s copied=%s KiB verify ok=%s bad=%s skip=%s row-hits=%s/%s hits %.2f ms/pic %.1f us/hit\n",store.kept(),
        static_cast<unsigned long>(store.used()/256),static_cast<unsigned long>(store.capacity()/256),Count(s.captures).text,
        Count(f[0]).text,Count(f[1]).text,Count(f[2]).text,Count(f[3]).text,Count(f[4]).text,Count(s.recycles).text,Count(s.inPlace).text,
        Count(s.wordsCopied/256).text,Count(s.verified).text,Count(s.verifyMismatches).text,Count(s.verifySkipped).text,
        Count(s.rowsHits).text,Count(s.rowsHits+s.rowsRefused).text,hitMilliseconds,
        s.profiledHits?double(s.hitTicks)/268.111856/double(s.profiledHits):0.0); // ARM11 ticks per microsecond
}
}
