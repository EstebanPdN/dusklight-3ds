#include "application_dump.hpp"
#include "dump_io.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace dusk::native::appdump {
const char* exclusion(const Region& r) {
    if (std::uint64_t(r.base)+r.size>0x1e800000u && r.base<0x1ff82000u)
        return "hardware-system-reserved-window";
    if (!(r.perm&1)) return "not-readable";
    if (r.perm&~7u) return "unknown-permissions";
    switch(r.state) {
    case 4: case 5: case 7: case 11: return nullptr; // CODE PRIVATE CONTINUOUS LOCKED
    case 0:return "free"; case 1:return "reserved"; case 2:return "io-device";
    case 3:return "static-ownership-uncertified";
    case 6:return "shared-ownership-uncertified";
    case 8:case 9:case 10:return "unsafe-alias";
    default:return "unknown-state";
    }
}
bool validRegion(const Region& r,std::uint32_t cursor) {
    const auto end=std::uint64_t(r.base)+r.size;
    return r.size && !(r.base&4095) && !(r.size&4095) &&
        r.base<=cursor && end>cursor && end<=addressLimit;
}
bool Trigger::update(std::uint32_t keys,std::uint64_t ms,bool busy) {
    const auto held=keys&chord;
    if ((held&((1u<<8)|(1u<<9)))==((1u<<8)|(1u<<9))) suppress_=true;
    if (!held && !busy) {ready_=true;timing_=suppress_=false;return false;}
    if (busy) {ready_=timing_=false;suppress_=true;return false;}
    if (held==chord) {
        suppress_=true;
        if (ready_) {ready_=false;timing_=true;since_=ms;}
        if (timing_ && ms<since_) {timing_=false;return false;}
        if (timing_ && ms-since_>=1000) {timing_=false;return true;}
    } else if (timing_) {timing_=ready_=false;} // partial release cannot rearm
    return false;
}
const char* resultText(Result r) {
    switch(r) {
    case Result::success:return "Volcado completo verificado";
    case Result::cancelled:return "Volcado cancelado";
    case Result::invalidMap:return "Error: mapa de memoria no valido";
    case Result::insufficientSpace:return "Error: espacio SD insuficiente";
    case Result::readError:return "Error: memoria cambio o no es legible";
    case Result::screensError:return "Error al capturar pantalla";
    case Result::pathError:return "Error al crear carpeta exclusiva";
    default:return "Error de escritura o verificacion SD";
    }
}
namespace {
constexpr std::uint64_t overhead=4*1024*1024; // map/inventory, allocation rounding, screenshots
constexpr unsigned maxChunks=unsigned(maxPayload/chunkBytes)+unsigned(maxRegions);
bool write(Services& s,Workspace& w,const char* path,const void* data,std::size_t n,
           std::uint32_t& hash,bool stream=false) {
    if(s.write)return s.write(s.context,path,data,n,w.verify,sizeof(w.verify),hash,stream);
    return stream ? writeStreamedDumpFile(path,data,n,w.verify,sizeof(w.verify),hash) :
        writeVerifiedDumpFile(path,data,n,w.verify,sizeof(w.verify),hash);
}
std::uint64_t rounded(Services& s,std::uint64_t n) {
    return ((n+s.allocationUnit-1)/s.allocationUnit)*s.allocationUnit;
}
bool space(Services& s,std::uint64_t bytes) {
    std::uint64_t free{};return s.space(s.context,free)&&hasDumpSpace(free,rounded(s,bytes),overhead);
}
bool poll(Services& s){return !s.poll||s.poll(s.context);}
void progress(Services& s,const char* text,std::uint64_t n,std::uint64_t total) {
    if(s.progress)s.progress(s.context,text,n,total);
}
bool path(Workspace& w,const char* dir,const char* name) {
    const auto n=std::snprintf(w.path,sizeof(w.path),"%s/%s",dir,name);
    return n>0&&std::size_t(n)<sizeof(w.path);
}
bool verifyFile(const char* p,Workspace& w,std::uint64_t size,std::uint32_t expected) {
    auto* f=std::fopen(p,"rb");if(!f)return false;
    bool ok=true;std::uint32_t hash=2166136261u;
    while(size){auto n=std::size_t(std::min<std::uint64_t>(size,sizeof(w.verify)));
        if(std::fread(w.verify,1,n,f)!=n){ok=false;break;}
        hash=dumpHash(w.verify,n,hash);size-=n;
    }
    if(std::fgetc(f)!=EOF||std::ferror(f))ok=false;
    if(std::fclose(f))ok=false;
    return ok&&hash==expected;
}
}
Result run(const char* root,const Metadata& m,Services& s,Workspace& w,char (&output)[256]) {
    output[0]=0;
    if(!root||!s.query||!s.read||!s.space||!s.screens)return Result::invalidMap;
    if(!s.allocationUnit || s.allocationUnit>1024*1024 ||
       (s.allocationUnit&(s.allocationUnit-1)))return Result::invalidMap;
    // Workspace exclusion is mandatory on target, optional for synthetic maps.
    const std::uint64_t exEnd=std::uint64_t(s.excludedBase)+s.excludedSize;
    if((s.excludedBase&4095)||(s.excludedSize&4095)||exEnd>addressLimit)return Result::invalidMap;
    Blob top{},bottom{};
    if(!s.screens(s.context,top,bottom)||!top.data||!top.bytes||top.bytes>400*240*4||
       !bottom.data||bottom.bytes!=320*240*2)return Result::screensError;
    // Snapshot/map after the borrowed screenshot allocation exists, before UI.
    std::size_t count=0;std::uint32_t cursor=0;std::uint64_t total=0;
    while(cursor<addressLimit) {
        if(!poll(s))return Result::cancelled;
        if(count==maxRegions)return Result::invalidMap;
        Region r{};
        if(!s.query(s.context,cursor,r)||!validRegion(r,cursor))return Result::invalidMap;
        r.size=std::uint32_t(std::uint64_t(r.base)+r.size-cursor);r.base=cursor;
        w.regions[count++]=r;
        if(!exclusion(r)) {
            auto bytes=std::uint64_t(r.size);
            const auto a=std::max<std::uint64_t>(r.base,s.excludedBase);
            const auto b=std::min<std::uint64_t>(std::uint64_t(r.base)+r.size,exEnd);
            if(b>a)bytes-=b-a;
            total+=bytes;if(total>maxPayload)return Result::invalidMap;
        }
        cursor=r.base+r.size;
    }
    if(!total)return Result::invalidMap;
    // Conservatively round EVERY possible chunk/file to the actual cluster.
    // 4 MiB also covers text inventories and small-file metadata, even when
    // page-sized map regions produce short files on a large-cluster card.
    std::uint64_t allocated=rounded(s,top.bytes)+rounded(s,bottom.bytes);
    for(std::size_t i=0;i<count;++i) {
        const auto& r=w.regions[i];if(exclusion(r))continue;
        const auto end=std::uint64_t(r.base)+r.size;
        for(std::uint64_t a=r.base;a<end;) {
            if(s.excludedSize&&a>=s.excludedBase&&a<exEnd){a=std::min(end,exEnd);continue;}
            auto n=std::min<std::uint64_t>(chunkBytes,end-a);
            if(s.excludedSize&&a<s.excludedBase)n=std::min<std::uint64_t>(n,s.excludedBase-a);
            allocated+=rounded(s,n);a+=n;
        }
    }
    if(s.supplement)allocated+=8ull*1024*1024; // full VRAM + LCD raw + inventory
    if(!space(s,allocated))return Result::insufficientSpace;
    // Root prepared by adapter. mkdir reserves a new namespace, never reuses it.
    char dir[256]{};bool created=false;
    for(unsigned i=0;i<128;++i) {
        const auto n=std::snprintf(dir,sizeof(dir),"%s/app-%llu-f%llu-%03u",root,
            (unsigned long long)m.timestamp,(unsigned long long)m.frame,i);
        if(n<0||std::size_t(n)>=sizeof(dir)-32)return Result::pathError;
        if(!mkdir(dir,0700)){created=true;break;}
        if(errno!=EEXIST)return Result::pathError;
    }
    if(!created)return Result::pathError;
    unsigned chunks=0; // includes attempted file, so failed partial writes are removed
    auto cleanup=[&] {
        if(s.removeSupplement)s.removeSupplement(s.context,dir);
        for(unsigned i=0;i<chunks&&i<maxChunks;++i){char name[40];std::snprintf(name,sizeof(name),"memory-%05u.bin",i);path(w,dir,name);unlink(w.path);}
        for(const char* name:{"top.rgba8","bottom.rgb565","map.txt","manifest.txt",".complete.tmp","COMPLETE"}){path(w,dir,name);unlink(w.path);}
        rmdir(dir); // bounded best effort; leaves incomplete private evidence on SD errors
    };
    auto fail=[&](Result r){cleanup();progress(s,resultText(r),0,total);return r;};
    std::uint32_t topHash{},bottomHash{};
    // Persist original game screens BEFORE the first progress write. No large copy.
    path(w,dir,"top.rgba8");if(!write(s,w,w.path,top.data,top.bytes,topHash,true))return fail(Result::ioError);
    path(w,dir,"bottom.rgb565");if(!write(s,w,w.path,bottom.data,bottom.bytes,bottomHash,true))return fail(Result::ioError);
    Supplement supplement{};
    if(s.supplement){
        if(!poll(s))return fail(Result::cancelled);
        if(!s.supplement(s.context,dir,w,supplement))return fail(supplement.cancelled?Result::cancelled:Result::ioError);
        if(!supplement.inventoryBytes||supplement.inventoryBytes>4096||supplement.payloadBytes>8ull*1024*1024)return fail(Result::ioError);
    }
    progress(s,"Volcando RAM + LCD + VRAM",0,total);
    path(w,dir,"map.txt");auto* map=std::fopen(w.path,"wx");
    if(!map)return fail(Result::ioError);
    bool mapOkay=true;std::uint64_t mapBytes=0;std::uint32_t mapHash=2166136261u;
    auto line=[&](const char* format,auto... args){
        if(!mapOkay)return;
        const auto n=std::snprintf(w.text,sizeof(w.text),format,args...);
        if(n<0||std::size_t(n)>=sizeof(w.text)){mapOkay=false;return;}
        mapOkay=std::fwrite(w.text,1,n,map)==std::size_t(n);
        mapHash=dumpHash(w.text,n,mapHash);mapBytes+=n;
    };
    line("%s","schema=1 coherence=chunked-live-non-atomic\nrange base size perm state pageflags decision\n");
    Result status=Result::success;std::uint64_t done=0;
    for(std::size_t i=0;i<count&&status==Result::success;++i) {
        const auto& r=w.regions[i];const char* why=exclusion(r);
        line("range %08x %08x %x %u %08x %s\n",unsigned(r.base),unsigned(r.size),unsigned(r.perm),unsigned(r.state),unsigned(r.flags),why?why:"eligible");
        if(why)continue;
        const auto end=std::uint64_t(r.base)+r.size;
        for(std::uint64_t a=r.base;a<end;) {
            if(!mapOkay){status=Result::ioError;break;}
            if(!poll(s)){status=Result::cancelled;break;}
            if(s.excludedSize&&a>=s.excludedBase&&a<exEnd) {
                const auto next=std::min(end,exEnd);
                line("exclude %08x %08x diagnostic-workspace\n",unsigned(a),unsigned(next-a));a=next;continue;
            }
            auto n=std::size_t(std::min<std::uint64_t>(chunkBytes,end-a));
            if(s.excludedSize&&a<s.excludedBase)n=std::size_t(std::min<std::uint64_t>(n,s.excludedBase-a));
            if(chunks==maxChunks){status=Result::invalidMap;break;}
            // Recheck space between chunks; a concurrent writer can still race.
            if(!space(s,n)){status=Result::insufficientSpace;break;}
            if(!s.read(s.context,r,std::uint32_t(a),w.data,n)){status=Result::readError;break;}
            char name[40];std::snprintf(name,sizeof(name),"memory-%05u.bin",chunks++);
            path(w,dir,name);std::uint32_t hash{};
            if(!write(s,w,w.path,w.data,n,hash,true)){status=Result::ioError;break;}
            line("chunk %s %08x %08x fnv1a32=%08x\n",name,unsigned(a),unsigned(n),unsigned(hash));
            a+=n;done+=n;progress(s,"Volcando y verificando RAM",done,total);
        }
    }
    if(!flushDumpFile(map))mapOkay=false;
    map=nullptr;
    path(w,dir,"map.txt");
    if(!mapOkay||!verifyFile(w.path,w,mapBytes,mapHash))status=Result::ioError;
    if(status!=Result::success)return fail(status);
    if(done!=total)return fail(Result::invalidMap);
    if(!poll(s))return fail(Result::cancelled);
    if(!space(s,0))return fail(Result::insufficientSpace);
    char supplementLine[256]{};
    if(s.supplement)std::snprintf(supplementLine,sizeof(supplementLine),
        "file=extras.txt bytes=%llu fnv1a32=%08x\nsupplement_bytes=%llu\ndisplay_source=GSP-presented-LCD-frontbuffers\n",
        (unsigned long long)supplement.inventoryBytes,unsigned(supplement.inventoryHash),(unsigned long long)supplement.payloadBytes);
    const int n=std::snprintf(w.text,sizeof(w.text),
        "format=DUSKLIGHT_APPLICATION_DUMP_1\nmode=full-readable-allowlisted-application\nchord=L+R+A hold=1000ms\n"
        "coherence=chunked-live-non-atomic\nworkers=running-not-suspended\nregisters=unsupported\n"
        "kernel=not-captured\nforeign-processes=not-captured\nmap-stability=best-effort-query-before-copy-not-pinned\n"
        "build=%.127s\nframe=%llu\ntimestamp_ms_2000=%llu\nscene=%.15s\nroom=%d\n"
        "new3ds=%u\nlauncher_application_region_bytes=%u\nkernel_version=%08x\ncompiler=" __VERSION__ "\nbuild_profile=native-ARM11\n"
        "player_camera=unavailable\nfirmware_cfw=unavailable\nheap_peak=unavailable\n"
        "scope=[00000000,40000000)\npolicy=READ-and-CODE-PRIVATE-CONTINUOUS-LOCKED\n"
        "excluded=see-map-static-shared-io-alias-unknown-unreadable-workspace\n"
        "ram_bytes=%llu\nchunks=%u\nreserve_bytes=8388608\nallocation_unit=%u\npreflight_allocated_bytes=%llu\n"
        "file=top.rgba8 bytes=%lu fnv1a32=%08x layout=PICA-untiled-RGBA8-rotated\n"
        "file=bottom.rgb565 bytes=%lu fnv1a32=%08x layout=libctru-rotated-RGB565\n"
        "file=map.txt bytes=%llu fnv1a32=%08x\n"
        "%smemory_inventory=map.txt\nprivacy=private-live-game-bytes-never-upload\n",
        m.build,(unsigned long long)m.frame,(unsigned long long)m.timestamp,m.scene,m.room,unsigned(m.new3ds),unsigned(m.applicationRegionBytes),unsigned(m.kernelVersion),
        (unsigned long long)done,chunks,unsigned(s.allocationUnit),(unsigned long long)allocated,(unsigned long)top.bytes,unsigned(topHash),
        (unsigned long)bottom.bytes,unsigned(bottomHash),(unsigned long long)mapBytes,unsigned(mapHash),supplementLine);
    if(n<0||std::size_t(n)>=sizeof(w.text))return fail(Result::ioError);
    path(w,dir,"manifest.txt");std::uint32_t manifestHash{};
    if(!write(s,w,w.path,w.text,n,manifestHash))return fail(Result::ioError);
    const auto markerSize=std::snprintf(w.text,sizeof(w.text),"verified=1\nmanifest_fnv1a32=%08x\nmap_fnv1a32=%08x\n",unsigned(manifestHash),unsigned(mapHash));
    path(w,dir,".complete.tmp");std::uint32_t markerHash{};
    if(!write(s,w,w.path,w.text,markerSize,markerHash))return fail(Result::ioError);
    if(!poll(s))return fail(Result::cancelled);
    char marker[256];std::snprintf(marker,sizeof(marker),"%s",w.path);
    path(w,dir,"COMPLETE");
    if(std::rename(marker,w.path))return fail(Result::ioError);
    std::snprintf(output,sizeof(output),"%s",dir);
    progress(s,resultText(Result::success),done,total);
    return Result::success;
}
}
