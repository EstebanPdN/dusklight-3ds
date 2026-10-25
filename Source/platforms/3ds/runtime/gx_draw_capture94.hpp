#pragma once
// Private, synchronous failure-only diagnostic. No renderer policy changes.
#include "gx_commands.hpp"
#include "gx_target_extent.hpp"
#include "gx_texture_commands.hpp"
#include "boot_stage.hpp"
#include "gx_pica_material.hpp"
#include <algorithm>
#include <cerrno>
#include <bit>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace dusk::native::gx::capture94 {
inline constexpr unsigned maxVertices=4096,maxIndices=24576,maxRows=8192,maxRowBytes=1024;
inline constexpr unsigned maxBytes=2*1024*1024;
inline constexpr std::uint64_t basis=14695981039346656037ull,prime=1099511628211ull;
inline std::uint64_t checksum(std::uint64_t hash,std::string_view bytes){
    for(unsigned char c:bytes){hash^=c;hash*=prime;}return hash;
}
enum class Failure : unsigned { none, bounds, vertex, index, rowLimit, byteLimit, write, textureMetadata,
                               open, flush, close, statusOpen, statusWrite, statusClose };
struct Receipt {
    Failure failure{};unsigned detail{},vertices{},indices{},rows{},bytes{};
    std::uint64_t hash=basis;
    bool complete{};
};
// A short write is a failure; the sink must never retain this borrowed row.
struct Sink {void* context{};std::size_t (*write)(void*,const char*,std::size_t){};};
struct Limits {unsigned vertices=maxVertices,indices=maxIndices,rows=maxRows,bytes=maxBytes;};
struct Writer {
    Sink sink;Limits limits;Receipt result;
    bool row(const char* tag,std::span<const u32> words){
        if(result.failure!=Failure::none)return false;
        char text[maxRowBytes];const auto n=std::strlen(tag);
        if(n>24||words.size()>100){result.failure=Failure::bounds;return false;}
        std::memcpy(text,tag,n);unsigned used=unsigned(n);
        for(auto value:words){const int count=std::snprintf(text+used,sizeof(text)-used," %08lx",static_cast<unsigned long>(value));
            if(count!=9||unsigned(count)>=sizeof(text)-used){result.failure=Failure::bounds;return false;}used+=unsigned(count);}
        text[used++]='\n';
        if(result.rows>=limits.rows){result.failure=Failure::rowLimit;return false;}
        if(used>limits.bytes-result.bytes){result.failure=Failure::byteLimit;return false;}
        if(!sink.write||sink.write(sink.context,text,used)!=used){result.failure=Failure::write;return false;}
        result.hash=checksum(result.hash,{text,used});++result.rows;result.bytes+=used;return true;
    }
    template<std::size_t N>bool row(const char* tag,const std::array<u32,N>& words){return row(tag,std::span<const u32>(words));}
};
using TextureResolver=bool(*)(const CommandRegisters&,unsigned,Texture&,const char*&);
// Serializes semantic decoder output, never struct padding, arbitrary pointers,
// array storage or a prefix masquerading as complete geometry. Float bits survive.
inline Receipt encode(const CommandDraw& draw,const TargetExtent& target,Sink sink,
                      TextureResolver resolve,Limits requested={}){
    Limits limits{std::min(requested.vertices,maxVertices),std::min(requested.indices,maxIndices),
                  std::min(requested.rows,maxRows),std::min(requested.bytes,maxBytes)};
    Writer w{sink,limits,{}};
    w.row("CAP94",std::array<u32,10>{1,unsigned(draw.primitive),draw.vertexCount,draw.indexCount,
          draw.plan.packedStride(),target.logicalWidth,target.logicalHeight,target.pixelWidth,target.pixelHeight,47});
    auto fail=[&](Failure failure,unsigned detail){
        // A best-effort explicit failure row; a write/limit failure is also
        // recorded by the independent fixed-size file receipt and OSReport.
        if(w.result.failure==Failure::none){w.row("FAIL",std::array<u32,2>{unsigned(failure),detail});
            if(w.result.failure==Failure::none)w.result.failure=failure;}
        w.result.detail=detail;return w.result;
    };
    const auto stride=draw.plan.packedStride();
    if(w.result.failure!=Failure::none)return w.result;
    if(!target.valid()||!draw.vertexCount||draw.vertexCount>limits.vertices||!stride||
       draw.vertices.size()!=std::size_t(draw.vertexCount)*stride||draw.indexCount>limits.indices||
       draw.indices.size()!=std::size_t(draw.indexCount)*2||(!draw.indices.empty()&&draw.indexCount%3))
        return fail(Failure::bounds,0);
    const auto& r=draw.registers;
    // CP array base addresses A0..AF are deliberately excluded. Decoded values
    // are the complete replay input; CP/VAT words are ancillary provenance.
    for(unsigned base=0;base<256;base+=16){
        std::array<u32,18> words{};words[0]=base;
        for(unsigned j=0;j<16;++j)if(base!=0xa0){words[1]|=u32(r.cpValid[base+j])<<j;words[2+j]=r.cp[base+j];}
        if(!w.row("CP",words))return w.result;
    }
    for(unsigned base=0;base<256;base+=16){
        std::array<u32,18> words{};words[0]=base;
        for(unsigned j=0;j<16;++j){words[1]|=u32(r.bpValid[base+j])<<j;words[2+j]=r.bp[base+j];}
        if(!w.row("BP",words))return w.result;
    }
    for(unsigned base=0;base<r.xf.size();base+=16){
        std::array<u32,18> words{};words[0]=base;
        for(unsigned j=0;j<16&&base+j<r.xf.size();++j){words[1]|=u32(r.xfValid[base+j])<<j;words[2+j]=r.xf[base+j];}
        if(!w.row("XF",words))return w.result;
    }
    std::array<u32,23> state{};unsigned at=0;
    state[at++]=r.bpMask;state[at++]=u32(r.vertexCacheEpoch);state[at++]=u32(r.vertexCacheEpoch>>32);
    state[at++]=u32(r.matrixIndexValid.to_ulong());for(auto v:r.matrixIndex)state[at++]=v;
    state[at++]=u32(r.tevColorValid.to_ulong());for(const auto& bank:r.tevColor)for(auto v:bank)state[at++]=v;
    if(!w.row("STATE",state))return w.result;
    for(unsigned i=0;i<r.tluts.size();++i){const auto& t=r.tluts[i];
        if(!w.row("TLUT",std::array<u32,5>{i,t.token,t.offset,t.entries,u32(r.tlutOverflow)}))return w.result;}
    // Exact bounded descriptors for resource admission, not texture sampling.
    // No texel, palette, BTI, game-allocation or process-memory payloads.
    for(unsigned map=0;map<8;++map){
        Texture t{};const char* why="resolver missing";const bool valid=resolve&&resolve(r,map,t,why);
        if(t.image.size()>UINT32_MAX||t.bti.size()>UINT32_MAX)return fail(Failure::textureMetadata,map);
        if(!w.row("TEX",std::array<u32,20>{map,u32(valid),u32(t.handle),u32(t.handle>>32),t.width,t.height,
           t.format,t.wrapS,t.wrapT,t.minFilter,t.magFilter,t.mipCount,t.minLod,t.maxLod,
           std::bit_cast<u32>(t.lodBias),u32(t.immutablePixels),u32(t.image.size()),u32(t.bti.size()),0,0}))return w.result;
        if(!valid){ // Bounded static resolver reason, independent of pointer values.
            std::array<u32,41> message{};message[0]=map;
            unsigned count=0;if(why)for(;count<160&&why[count];++count)message[1+count/4]|=u32(u8(why[count]))<<(8*(count%4));
            if(count==160)return fail(Failure::textureMetadata,map);
            if(!w.row("TERR",message))return w.result;
        }
    }
    for(unsigned i=0;i<draw.vertexCount;++i){
        DecodedVertex v;VertexDecodeError error{};
        if(!draw.vertex(i,v,&error))return fail(Failure::vertex,(i<<8)|unsigned(error));
        std::array<u32,48> words{};unsigned p=0;words[p++]=i;words[p++]=v.present;words[p++]=v.normalCount;
        for(auto x:v.matrix)words[p++]=x;
        auto floats=[&](const auto& xs){for(float x:xs)words[p++]=std::bit_cast<u32>(x);};
        floats(v.position);for(const auto& xs:v.normal)floats(xs);for(const auto& xs:v.color)floats(xs);for(const auto& xs:v.texcoord)floats(xs);
        if(!w.row("V",words))return w.result;
        ++w.result.vertices;
    }
    for(unsigned i=0;i<draw.indexCount;i+=32){
        std::array<u32,34> words{};words[0]=i;words[1]=std::min(32u,unsigned(draw.indexCount-i));
        for(unsigned j=0;j<words[1];++j){u16 index;if(!draw.triangleIndex(i+j,index))return fail(Failure::index,i+j);words[j+2]=index;}
        if(!w.row("I",words))return w.result;
        w.result.indices+=words[1];
    }
    // END authenticates every prior byte/row, plus exact semantic counts.
    const auto prior=w.result;
    if(w.row("END",std::array<u32,7>{1,prior.vertices,prior.indices,prior.rows,prior.bytes,u32(prior.hash),u32(prior.hash>>32)}))w.result.complete=true;
    return w.result;
}
inline bool selected(const char* why,const char* stage,int room,const BootStage* boot){
    return why&&stage&&boot&&std::strcmp(why,"TEV source/order unsupported")==0&&
           std::strncmp(stage,"F_SP108",8)==0&&room==0&&std::strcmp(boot->name,"F_SP108")==0&&
           boot->room==0&&boot->point==0&&boot->layer==-1;
}
// Optional private selection; never affects renderer admission or the CAP94 wire schema.
enum class SelectionStatus : unsigned { off, legacy, explicitTarget, invalidEnable, invalidTarget };
enum class FailureClass : unsigned { none, tevSourceOrder, indirectCoordinate, clippedDepth, tevOperation, tevAny };
struct SelectionDescriptor {
    SelectionStatus status{SelectionStatus::off};
    FailureClass failureClass{FailureClass::none};
    BootStage target{};
};
inline const char* selectionStatusName(SelectionStatus status){
    switch(status){
    case SelectionStatus::off:return "off";
    case SelectionStatus::legacy:return "legacy";
    case SelectionStatus::explicitTarget:return "explicit-target";
    case SelectionStatus::invalidEnable:return "invalid-enable";
    case SelectionStatus::invalidTarget:return "invalid-target";
    }return "invalid-status";
}
// The first line is an exact whitelisted literal; the rest uses the existing
// bounded BootStage grammar (name room point layer), with no extra tokens.
inline bool parseSelection108(std::string_view text,SelectionDescriptor& result){
    result={SelectionStatus::invalidTarget,FailureClass::none,{}};
    const auto end=text.find('\n');if(end==text.npos)return false;
    const auto literal=text.substr(0,end);
    FailureClass failureClass;
    if(literal=="indirect-coordinate")failureClass=FailureClass::indirectCoordinate;
    else if(literal=="tev-source-order")failureClass=FailureClass::tevSourceOrder;
    else if(literal=="clip-disabled")failureClass=FailureClass::clippedDepth;
    else if(literal=="tev-operation")failureClass=FailureClass::tevOperation;
    else if(literal=="tev-any")failureClass=FailureClass::tevAny;
    else return false;
    BootStage target;if(!parseBootStage(text.substr(end+1),target))return false;
    result={SelectionStatus::explicitTarget,failureClass,target};return true;
}
// Only exact canonical planner reasons with the TEV prefix. Successful visual
// fallback notices, arbitrary TEV text and non-TEV planner failures never match.
inline bool canonicalTevFailure74(const char* why){
    if(!why||!std::string_view(why).starts_with("TEV "))return false;
    constexpr MaterialPlanError errors[]={MaterialPlanError::operation,MaterialPlanError::source,
        MaterialPlanError::uninitialized,MaterialPlanError::konstFraction,
        MaterialPlanError::swap,MaterialPlanError::equation};
    for(auto error:errors)if(std::strcmp(why,materialPlanErrorText(error))==0)return true;
    return false;
}
inline bool selected108(const SelectionDescriptor& selection,const char* why,
                        const char* stage,int room,const BootStage* boot){
    if(selection.status==SelectionStatus::legacy)return selected(why,stage,room,boot);
    if(selection.status!=SelectionStatus::explicitTarget||!why||!stage||!boot)return false;
    const auto& t=selection.target;
    const char* reason=selection.failureClass==FailureClass::indirectCoordinate?"GX vertex indirect coordinate invalid":
                       selection.failureClass==FailureClass::tevSourceOrder?"TEV source/order unsupported":
                       selection.failureClass==FailureClass::clippedDepth?"GX disabled clipping outside depth/camera volume":
                       selection.failureClass==FailureClass::tevOperation?"TEV operation/scale/output unsupported":nullptr;
    const bool reasonMatches=selection.failureClass==FailureClass::tevAny?canonicalTevFailure74(why):
                             reason&&std::strcmp(why,reason)==0;
    return reasonMatches&&std::strncmp(stage,t.name,8)==0&&room==t.room&&
           std::strcmp(boot->name,t.name)==0&&boot->room==t.room&&boot->point==t.point&&boot->layer==t.layer;
}
inline std::size_t fileWrite(void* file,const char* data,std::size_t size){return std::fwrite(data,1,size,static_cast<FILE*>(file));}
inline constexpr const char* configPath="sdmc:/3ds/dusklight/faron-draw-capture94.txt";
// Fixed optional input only; absent preserves legacy Faron selection/paths.
inline constexpr const char* targetPath108="sdmc:/3ds/dusklight/draw-capture108-target.txt";
inline constexpr const char* dataPath="sdmc:/3ds/dusklight/faron-draw-capture94.txtdata";
inline constexpr const char* statusPath="sdmc:/3ds/dusklight/faron-draw-capture94.status";
// Fixed file set: two exclusive files, never overwrite a prior incident.
// Narrow failure-injection seam; defaults are the actual libc operations.
// Consulted only on the selected failure; never a per-frame interface.
struct FileChecks {int (*flush)(FILE*)=std::fflush;int (*close)(FILE*)=std::fclose;};
inline int formatReceipt(const Receipt& r,char* text,std::size_t size){
    return std::snprintf(text,size,"complete=%u failure=%u detail=%u vertices=%u indices=%u rows=%u bytes=%u fnv=%016llx\n",
       unsigned(r.complete),unsigned(r.failure),r.detail,r.vertices,r.indices,r.rows,r.bytes,static_cast<unsigned long long>(r.hash));
}
struct Capture {
    bool enabled{},attempted{};
    SelectionDescriptor selection{}; // Initialized, read-only startup reporting descriptor.
    bool armed()const{return enabled&&!attempted;}
    // Called once at startup. Missing, malformed or unreadable means OFF.
    bool initialize(){
        enabled=false;attempted=false;selection={};
        auto* f=std::fopen(configPath,"rb");if(!f)return false;
        char text[11];const auto n=std::fread(text,1,sizeof(text),f);bool ok=!std::ferror(f);if(std::fclose(f))ok=false;
        if(!ok||n!=10||std::memcmp(text,"enabled=1\n",10)!=0){selection.status=SelectionStatus::invalidEnable;return false;}
        // OFF never consults this optional file. Only ENOENT means no new config;
        // every other open/read/close/parse failure disables capture, no fallback.
        errno=0;f=std::fopen(targetPath108,"rb");
        if(!f){
            if(errno!=ENOENT){selection.status=SelectionStatus::invalidTarget;return false;}
            selection={SelectionStatus::legacy,FailureClass::tevSourceOrder,{}};
            parseBootStage("F_SP108 0 0 -1",selection.target);enabled=true;return true;
        }
        char target[128];const auto size=std::fread(target,1,sizeof(target),f);
        ok=!std::ferror(f);if(std::fclose(f))ok=false;
        selection.status=SelectionStatus::invalidTarget;
        if(!ok||size==sizeof(target)||!parseSelection108({target,size},selection))return false;
        enabled=true;return true;
    }

    Receipt observe(const CommandDraw& draw,const char* why,const char* stage,int room,const BootStage* boot,
                    TextureResolver resolve=resolveCommandTexture,FileChecks checks={}){
        Receipt result;
        if(!armed()||!selected108(selection,why,stage,room,boot))return result;
        attempted=true; // Consume even on open/write/close failure; never retry.
        auto* status=std::fopen(statusPath,"wx");
        if(!status){result.failure=Failure::statusOpen;return result;}
        auto* file=std::fopen(dataPath,"wx");
        if(!file)result.failure=Failure::open;
        else {
            // Disable libc's implicit heap buffer; rows are synchronously bounded.
            std::setvbuf(file,nullptr,_IONBF,0);
            result=encode(draw,currentTargetExtent,{file,fileWrite},resolve);
            if(checks.flush(file)){result.failure=Failure::flush;result.complete=false;}
            if(checks.close(file)){result.failure=Failure::close;result.complete=false;}
        }
        // Status bytes alone cannot prove fclose(status) succeeded: acceptance
        // additionally REQUIRES the successful returned receipt logged by the
        // caller AFTER observe() returns. A close failure may leave intact bytes.
        char text[256];const int n=formatReceipt(result,text,sizeof(text));
        if(n<=0||unsigned(n)>=sizeof(text)||std::fprintf(status,"CAP94_STATUS 1 %s",text)<0||checks.flush(status)){
            result.failure=Failure::statusWrite;result.complete=false;
        }
        if(checks.close(status)){result.failure=Failure::statusClose;result.complete=false;}
        return result;
    }
};
}
