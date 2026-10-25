#include "../runtime/disc.hpp"
#include "../runtime/resource_archive.hpp"
#include "../runtime/texture_gx.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <map>
#include <vector>
#include <deque>
#include <span>
// Read-only original startup resource metadata, using the production parsers.
// No pixels or private archive payloads are exported; no runtime acceptance.
using namespace dusk::native;
using Bytes=std::vector<unsigned char>;
using Span=std::span<const unsigned char>;
static unsigned word(Span b,unsigned at){assert(at+4<=b.size());return unsigned(b[at])<<24|unsigned(b[at+1])<<16|unsigned(b[at+2])<<8|b[at+3];}
static void expand(Bytes& b){
    if(b.size()<4||std::memcmp(b.data(),"Yaz0",4))return;
    unsigned size{};assert(yaz0ExpandedSize(b.data(),b.size(),64*1024*1024,size)==ResourceStatus::ready);
    Bytes out(size);std::size_t written{};
    assert(decodeYaz0(b.data(),b.size(),out.data(),out.size(),64*1024*1024,written)==ResourceStatus::ready&&written==size);b.swap(out);
}
static void inspect(Disc& disc,const char* path){
    DiscFile file;assert(disc.findFile(path,file)==DiscFileStatus::ready&&file.size()<=64*1024*1024);
    Bytes bytes(file.size());assert(disc.readFile(file,0,bytes.data(),bytes.size()));expand(bytes);
    RarcArchive archive;assert(archive.open(bytes.data(),bytes.size(),{64*1024*1024,4096,65536,255})==ResourceStatus::ready);
    unsigned models{},headers{},external{},unsupported{},samplerAliases{};std::size_t cpuBytes{},gpuBytes{};
    std::map<const void*,BtiInfo> images;std::deque<Bytes> inner;
    auto admit=[&](Span source){
        ++headers;if(!word(source,28)){++external;return;}
        BtiInfo info;assert(inspectBti({source.data(),source.size()},1024,info)==ResourceStatus::ready);
        NativeTexturePlan plan;if(!inspectNativeTexture(source,plan)){++unsupported;return;}
        auto [it,inserted]=images.emplace(info.image.data,info);
        if(!inserted){assert(it->second.format==info.format&&it->second.width==info.width&&it->second.height==info.height&&it->second.mipCount==info.mipCount);++samplerAliases;return;}
        cpuBytes+=32+plan.sourceBytes;gpuBytes+=plan.outputBytes;
    };
    for(unsigned n=0;n<archive.entryCount();++n){
        RarcEntry entry;assert(archive.entry(n,entry)==ResourceStatus::ready);if(entry.directory)continue;
        Span resource(entry.contents.data,entry.contents.size);
        if(resource.size()>=4&&!std::memcmp(resource.data(),"Yaz0",4)){inner.emplace_back(resource.begin(),resource.end());expand(inner.back());resource=inner.back();}
        const auto length=std::strlen(entry.name);
        if(length>=4&&!std::strcmp(entry.name+length-4,".bti")){assert(resource.size()>=32);admit(resource);}
        if(resource.size()<32||std::memcmp(resource.data(),"J3D2",4)||
            (std::memcmp(resource.data()+4,"bmd",3)&&std::memcmp(resource.data()+4,"bdl",3)))continue;
        ++models;assert(word(resource,8)==resource.size());std::size_t at=32;
        for(unsigned block=0;block<word(resource,12);++block){
            assert(at+8<=resource.size());const auto size=word(resource,at+4);assert(size>=8&&size<=resource.size()-at);
            if(!std::memcmp(resource.data()+at,"TEX1",4)){
                Span tex=resource.subspan(at,size);assert(size>=32);const unsigned count=unsigned(tex[8])<<8|tex[9],base=word(tex,12);
                assert(!count||(base>=32&&base<=size&&count<=(size-base)/32));
                for(unsigned t=0;t<count;++t)admit(tex.subspan(base+32*t));
            }
            at+=size;
        }
    }
    std::printf("{\"archive\":\"%s\",\"archive_bytes\":%zu,\"models\":%u,\"headers\":%u,\"external\":%u,\"unsupported\":%u,\"image_aliases\":%u,\"unique_images\":%zu,\"retained_cpu_bytes\":%zu,\"potential_gpu_bytes\":%zu,\"payload_saved\":false}\n",
        path,bytes.size(),models,headers,external,unsupported,samplerAliases,images.size(),cpuBytes,gpuBytes);
}
static void inspectGrassRel(Disc& disc){
    DiscFile file;assert(disc.findFile("/RELS.arc",file)==DiscFileStatus::ready&&file.size()<=64*1024*1024);
    Bytes bytes(file.size());assert(disc.readFile(file,0,bytes.data(),bytes.size()));expand(bytes);
    RarcArchive archive;assert(archive.open(bytes.data(),bytes.size(),{64*1024*1024,4096,65536,255})==ResourceStatus::ready);
    RarcEntry rel;assert(archive.findFile("d_a_grass.rel",rel)==ResourceStatus::ready);
    const auto stored=rel.contents.size;Bytes decoded(rel.contents.data,rel.contents.data+stored);expand(decoded);
    assert(decoded.size()>stored&&stored<0x7680+0x800&&0x7e80+0x800<=decoded.size());
    for(unsigned offset:{0x7680u,0x7e80u}){
        // Exercise both original requested texture slices, without exporting
        // their pixels. The actual extent is the complete expanded REL.
        assert(offset<=decoded.size()&&0x800<=decoded.size()-offset);
        Bytes transient(decoded.begin()+offset,decoded.begin()+offset+0x800);assert(transient.size()==0x800);
    }
    std::printf("{\"archive\":\"/RELS.arc\",\"resource\":\"d_a_grass.rel\",\"stored_bytes\":%zu,\"expanded_bytes\":%zu,\"original_grass_ranges_fit_expanded_only\":true,\"payload_saved\":false}\n",stored,decoded.size());
}
int main(int argc,char** argv){assert(argc==2);Disc disc;assert(disc.open(argv[1])==DiscStatus::ready);
    for(const char* path:{"/res/Object/LogoUs.arc","/res/Object/Title.arc","/res/Object/Always.arc","/res/Object/Alink.arc"})inspect(disc,path);
    inspectGrassRel(disc);
}
