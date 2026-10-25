#include "copy_lifecycle_probe.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/stat.h>
namespace dusk::native::copyprobe {
namespace {
State data;
void add(std::uint64_t& value,std::uint64_t delta=1){
    if(delta>std::numeric_limits<std::uint64_t>::max()-value){data.counterOverflow=true;value=std::numeric_limits<std::uint64_t>::max();}
    else value+=delta;
}
void count(Counter c){add(data.counters[c]);}
Image* find(std::uint64_t handle){if(!handle)return nullptr;for(auto& image:data.images)if(image.handle==handle)return &image;return nullptr;}
void record(Kind kind,Reason reason,const Image* image=nullptr){
    add(data.sequence);
    if(data.size==eventCapacity){data.eventOverflow=true;add(data.droppedEvents);}
    else ++data.size;
    auto& e=data.events[data.head];e={};data.head=(data.head+1)%eventCapacity;
    e.sequence=data.sequence;e.frame=data.frame;e.globalSequence=data.globalSequence;e.kind=kind;e.reason=reason;
    if(image){
        e.handle=image->handle;e.serial=image->serial;e.imageSequence=image->imageSequence;
        e.original=image->original;e.cloneHeader=image->cloneHeader;e.clonePayload=image->clonePayload;
        e.allocationBytes=image->allocationBytes;e.pixelBytes=image->pixelBytes;
        e.nativeBytes=image->nativeBytes;e.snapshotBytes=image->snapshotBytes;
        e.format=image->format;e.width=image->width;e.height=image->height;
        e.eligibleCount=image->eligibleCount;e.usefulCount=image->usefulCount;e.wastedCount=image->wastedCount;
        e.invalidationReason=image->lastInvalidation;
        e.flags=(image->resident?1:0)|(image->dirty?2:0)|(image->candidate?4:0);
    }
}
void waste(Image& image,Reason why){
    if(!image.candidate)return;
    image.candidate=false;add(image.wastedCount);count(potentialWasted);
    switch(why){
    case Reason::global:count(wastedGlobal);break;
    case Reason::image:count(wastedImage);break;
    case Reason::replaced:count(wastedReplace);break;
    case Reason::shutdown:count(wastedDestroy);break;
    default:count(wastedRetire);break;
    }
}
unsigned unresolved(){unsigned n=0;for(const auto& image:data.images)n+=image.candidate;return n;}
const char* kinds[]={"register","copy","palette","image-invalidate","global-invalidate","bind-attempt","decode","flush-unknown","bind-success","retire","destroy"};
const char* reasons[]={"none","eligible","not-resident","depth-excluded","palette-excluded","protected-draw","layout-excluded","miss","dirty","hit","image","global","evict","release","shutdown","replaced"};
const char* names[]={"registrations","copies","residentCopies","eligibleCopies","excludedDepth","excludedPalette",
    "excludedLayout","nonresidentCopies","protectedCopies","bindAttempts","misses","dirtyBinds","hits","decodes","flushesUnknown",
    "binds","bindsBeforeGlobal","paletteChanges","imageInvalidations","globalInvalidations","evictions","releases","destructions",
    "potentialUseful","potentialWasted","wastedGlobal","wastedImage","wastedRetire","wastedReplace","wastedDestroy"};
static_assert(std::size(names)==Counter::counterCount);
using U=unsigned long long;
struct Writer {
    std::FILE* file{};std::size_t bytes{};bool okay{true};
    template<class... T> void line(const char* format,T... args){
        if(!okay)return;
        char buffer[768];const int n=std::snprintf(buffer,sizeof(buffer),format,args...);
        if(n<0||std::size_t(n)>=sizeof(buffer)||std::size_t(n)>reportLimit-bytes){okay=false;return;}
        okay=std::fwrite(buffer,1,std::size_t(n),file)==std::size_t(n);bytes+=std::size_t(n);
    }
};
}
const State& state(){return data;}
void initialize(){
    if(data.initialized)return;
    data.initialized=true;
    auto* f=std::fopen(optionPath,"rb");if(!f)return;
    char option[32];const auto n=std::fread(option,1,sizeof(option),f);
    const bool okay=!std::ferror(f)&&n==sizeof(optionValue)-1&&std::memcmp(option,optionValue,n)==0;
    const auto closed=std::fclose(f);enabled=okay&&closed==0;
}
void setFrame(std::uint64_t frame){if(enabled)data.frame=frame;}
void registration(std::uint64_t handle,const void* original,const void* header,const void* payload,
                  std::size_t allocationBytes,std::size_t pixelBytes,unsigned format,unsigned width,unsigned height){
    if(!enabled||!handle)return;
    if(find(handle))return;
    for(auto& image:data.images)if(!image.handle){
        image={};image.handle=handle;image.original=reinterpret_cast<std::uintptr_t>(original);
        image.cloneHeader=reinterpret_cast<std::uintptr_t>(header);image.clonePayload=reinterpret_cast<std::uintptr_t>(payload);
        image.allocationBytes=allocationBytes;image.pixelBytes=pixelBytes;image.format=format;image.width=width;image.height=height;
        count(registrations);record(Kind::registration,Reason::none,&image);return;
    }
    data.trackingOverflow=true;
}
void copy(std::uint64_t handle,Reason eligibility,bool resident,bool dirty,std::size_t nativeBytes,std::size_t snapshotBytes){
    if(!enabled)return;
    auto* image=find(handle);if(!image){data.trackingOverflow=true;return;}
    waste(*image,Reason::replaced);add(data.contentSerial);image->serial=data.contentSerial;
    image->copyGlobal=data.globalSequence;image->boundBeforeGlobal=false;image->hitReported=false;
    image->resident=resident;image->dirty=dirty;image->nativeBytes=nativeBytes;image->snapshotBytes=snapshotBytes;
    image->candidate=eligibility==Reason::eligible;add(image->copyCount);count(copies);
    if(resident)count(residentCopies);
    switch(eligibility){
    case Reason::eligible:count(eligibleCopies);add(image->eligibleCount);break;
    case Reason::depth:count(excludedDepth);break;
    case Reason::palette:count(excludedPalette);break;
    case Reason::notResident:count(nonresidentCopies);break;
    case Reason::protectedDraw:count(protectedCopies);break;
    default:count(excludedLayout);break;
    }
    record(Kind::copy,eligibility,image);
}
void palette(std::uint64_t handle){if(enabled)if(auto* image=find(handle)){count(paletteChanges);record(Kind::palette,Reason::palette,image);}}
void imageInvalidate(std::uint64_t handle){
    if(enabled)if(auto* image=find(handle)){
        add(image->imageSequence);image->lastInvalidation=Reason::image;image->dirty=true;
        waste(*image,Reason::image);count(imageInvalidations);record(Kind::imageInvalidate,Reason::image,image);
    }
}
void globalInvalidate(){
    if(!enabled)return;
    add(data.globalSequence);count(globalInvalidations);record(Kind::globalInvalidate,Reason::global);
    for(auto& image:data.images)if(image.handle){image.lastInvalidation=Reason::global;image.dirty=true;waste(image,Reason::global);}
}
void bindAttempt(std::uint64_t handle,Reason why){
    if(enabled)if(auto* image=find(handle)){
        count(bindAttempts);if(why==Reason::miss)count(misses);else if(why==Reason::dirty)count(dirtyBinds);else count(hits);
        if(why!=Reason::hit||!image->hitReported)record(Kind::bindAttempt,why,image);
    }
}
void decode(std::uint64_t handle,Reason why){
    if(enabled)if(auto* image=find(handle)){count(decodes);add(image->decodeCount);record(Kind::decode,why,image);}
}
void flushUnknown(std::uint64_t handle){
    if(enabled)if(auto* image=find(handle)){count(flushesUnknown);record(Kind::flushUnknown,Reason::none,image);}
}
void bindSuccess(std::uint64_t handle,Reason why){
    if(enabled)if(auto* image=find(handle)){
        image->resident=true;image->dirty=false;count(binds);add(image->bindCount);
        if(image->serial&&!image->boundBeforeGlobal&&image->copyGlobal==data.globalSequence){
            image->boundBeforeGlobal=true;count(bindsBeforeGlobal);add(image->beforeGlobalCount);
        }
        if(image->candidate){image->candidate=false;count(potentialUseful);add(image->usefulCount);}
        if(why!=Reason::hit||!image->hitReported)record(Kind::bindSuccess,why,image);
        if(why==Reason::hit)image->hitReported=true;
    }
}
void retire(std::uint64_t handle,Reason why){
    if(enabled)if(auto* image=find(handle)){
        waste(*image,why);image->resident=false;
        if(why==Reason::evict)count(evictions);else count(releases);
        record(Kind::retire,why,image);
    }
}
void destroy(std::uint64_t handle){
    if(enabled)if(auto* image=find(handle)){waste(*image,Reason::shutdown);count(destructions);record(Kind::destroy,Reason::shutdown,image);*image={};}
}
bool writeReport(bool final){
    if(!enabled)return true;
    auto* f=std::fopen(stagingPath,"wb");if(!f){add(data.writeFailures);return false;}
    Writer w{f};
    w.line("copy-lifecycle-v1 PRIVATE metadata-only final=%u frame=%llu sequence=%llu serial=%llu global=%llu\n",unsigned(final),U(data.frame),U(data.sequence),U(data.contentSerial),U(data.globalSequence));
    w.line("stateBytes=%llu eventBytes=%llu imageBytes=%llu ringCapacity=%u imageCapacity=%u reportLimit=%llu stagingPayloadBytes=0 snapshotPayloadCopied=0\n",U(sizeof(State)),U(sizeof(Event)),U(sizeof(Image)),eventCapacity,imageCapacity,U(reportLimit));
    w.line("eventOverflow=%u trackingOverflow=%u counterOverflow=%u droppedEvents=%llu previousWriteFailures=%llu unresolved=%u flushResult=UNKNOWN originalCoherence=UNPROVED\n",unsigned(data.eventOverflow),unsigned(data.trackingOverflow),unsigned(data.counterOverflow),U(data.droppedEvents),U(data.writeFailures),unresolved());
    for(unsigned n=0;n<Counter::counterCount;++n)w.line("count %s=%llu\n",names[n],U(data.counters[n]));
    unsigned activeImages=0;
    for(const auto& image:data.images)if(image.handle){
        ++activeImages;
        w.line("image handle=imageID:%llu serial=%llu imageSeq=%llu copyGlobal=%llu copies=%llu binds=%llu decodes=%llu beforeGlobal=%llu lastInvalidation=%s eligible=%llu useful=%llu wasted=%llu\n",U(image.handle),U(image.serial),U(image.imageSequence),U(image.copyGlobal),U(image.copyCount),U(image.bindCount),U(image.decodeCount),U(image.beforeGlobalCount),reasons[unsigned(image.lastInvalidation)],U(image.eligibleCount),U(image.usefulCount),U(image.wastedCount));
    }
    const auto first=(data.head+eventCapacity-data.size)%eventCapacity;
    for(unsigned n=0;n<data.size;++n){const auto& e=data.events[(first+n)%eventCapacity];
        w.line("event seq=%llu frame=%llu kind=%s reason=%s handle=imageID:%llu serial=%llu global=%llu imageSeq=%llu fmt=%u size=%ux%u alloc=%llu gx=%llu native=%llu snapshot=%llu flags=%u original=0x%llx cloneHeader=0x%llx clonePayload=0x%llx lastInvalidation=%s eligible=%llu useful=%llu wasted=%llu\n",U(e.sequence),U(e.frame),kinds[unsigned(e.kind)],reasons[unsigned(e.reason)],U(e.handle),U(e.serial),U(e.globalSequence),U(e.imageSequence),e.format,e.width,e.height,U(e.allocationBytes),U(e.pixelBytes),U(e.nativeBytes),U(e.snapshotBytes),unsigned(e.flags),U(e.original),U(e.cloneHeader),U(e.clonePayload),reasons[unsigned(e.invalidationReason)],U(e.eligibleCount),U(e.usefulCount),U(e.wastedCount));
    }
    const bool aggregatesComplete=!data.trackingOverflow&&!data.counterOverflow;
    w.line("END final=%u aggregatesComplete=%u eventsComplete=%u lifecycleComplete=%u activeImages=%u eligible=%llu useful=%llu wasted=%llu unresolved=%u bytesBeforeEnd=%llu\n",unsigned(final),unsigned(aggregatesComplete),unsigned(aggregatesComplete&&!data.eventOverflow),unsigned(final&&aggregatesComplete&&!unresolved()&&!activeImages),activeImages,U(data.counters[eligibleCopies]),U(data.counters[potentialUseful]),U(data.counters[potentialWasted]),unresolved(),U(w.bytes));
    const bool closed=std::fclose(f)==0;
    const auto fail=[](){add(data.writeFailures);std::remove(stagingPath);return false;};
    if(!w.okay||!closed)return fail(); // Staging is never a published witness.
    struct stat primary{};
    const auto exists=::stat(reportPath,&primary);
    if(exists!=0){
        if(errno!=ENOENT)return fail();
        // libctru stat retries a directory open, which can mask a file-open
        // error as ENOENT. Its read-only fopen path returns the file error.
        auto* witness=std::fopen(reportPath,"rb");
        if(witness){std::fclose(witness);return fail();}
        if(errno!=ENOENT)return fail();
    }
    const bool rotate=exists==0;
    if(rotate){
        if(!S_ISREG(primary.st_mode))return fail();
        // libctru may delete an existing destination before a failed retry.
        // The source primary still witnesses the last report if rotation fails.
        if(std::rename(reportPath,recoveryPath)!=0)return fail();
    }
    // Render owner only, fixed isolated paths: primary is now absent. Never
    // replace a destination with staging through libctru's destructive retry.
    if(std::rename(stagingPath,reportPath)!=0){
        if(rotate)std::rename(recoveryPath,reportPath); // Failed rollback keeps recovery.
        return fail(); // A lone recovery was never rotated or removed.
    }
    // New primary is complete before retiring the older witness. A cleanup
    // error leaves two complete reports and is still a returned write failure.
    if(std::remove(recoveryPath)!=0&&errno!=ENOENT){add(data.writeFailures);return false;}
    return true;
}
}
