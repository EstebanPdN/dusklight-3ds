#include "stage_data.hpp"
#include <algorithm>
#include <cstring>

namespace dusk::native {
namespace {
std::uint32_t be32(const unsigned char* p) {
    return std::uint32_t(p[0])<<24 | std::uint32_t(p[1])<<16 |
           std::uint32_t(p[2])<<8 | p[3];
}
}
ResourceStatus StageData::open(ResourceSpan source) {
    bytes = {}; chunks = 0;
    if (!source.data || source.size < 4) return ResourceStatus::truncated;
    if (source.size > 1024*1024) return ResourceStatus::limitExceeded;
    const auto count = be32(source.data);
    if (count > 4096) return ResourceStatus::limitExceeded;
    const std::size_t tableEnd = 4 + std::size_t(count)*12;
    if (tableEnd > source.size) return ResourceStatus::truncated;
    for (unsigned i=0;i<count;++i) {
        const auto* header = source.data+4+i*12;
        const auto entries=be32(header+4), offset=be32(header+8);
        if (entries > 65536) return ResourceStatus::limitExceeded;
        for (unsigned c=0;c<4;++c)
            if (header[c]<0x20 || header[c]>0x7e) return ResourceStatus::malformed;
        for (unsigned j=0;j<i;++j)
            if (!std::memcmp(header,source.data+4+j*12,4)) return ResourceStatus::ambiguous;
        if (offset==0 && entries==0) continue;
        if (offset<tableEnd || offset>source.size || (entries && offset==source.size))
            return ResourceStatus::malformed;
    }
    bytes=source; chunks=count;
    return ResourceStatus::ready;
}
ResourceStatus StageData::chunk(std::uint32_t index, StageChunk& output) const {
    output={};
    if (!bytes.data) return ResourceStatus::notReady;
    if (index>=chunks) return ResourceStatus::notFound;
    const auto* header=bytes.data+4+index*12;
    std::memcpy(output.tag.data(),header,4);
    output.count=be32(header+4);output.offset=be32(header+8);
    if (output.offset==0 && output.count==0) return ResourceStatus::ready;
    std::size_t end=bytes.size;
    for (unsigned i=0;i<chunks;++i) {
        const auto offset=be32(bytes.data+4+i*12+8);
        if (offset>output.offset) end=std::min(end,std::size_t(offset));
    }
    output.contents={bytes.data+output.offset,end-output.offset};
    return ResourceStatus::ready;
}
ResourceStatus StageData::find(const std::array<char,4>& tag, StageChunk& output) const {
    output={};
    if (!bytes.data) return ResourceStatus::notReady;
    for (unsigned i=0;i<chunks;++i)
        if (!std::memcmp(bytes.data+4+i*12,tag.data(),4)) return chunk(i,output);
    return ResourceStatus::notFound;
}
ResourceStatus StageData::require(const std::array<char,4>& tag,std::size_t stride,
                                  StageChunk& output) const {
    const auto result=find(tag,output);
    if (result!=ResourceStatus::ready) return result;
    if (!stride || output.count>output.contents.size/stride)
        return ResourceStatus::truncated;
    return ResourceStatus::ready;
}
ResourceStatus inspectStageEnvironment(ResourceSpan stage,ResourceSpan room,
    unsigned roomNo,unsigned layer,StageEnvironmentLayout& output) {
    output={};
    if (roomNo>=64 || layer>=15) return ResourceStatus::malformed;
    StageData stageData,roomData;
    auto result=stageData.open(stage);
    if (result!=ResourceStatus::ready) return result;
    result=roomData.open(room);
    if (result!=ResourceStatus::ready) return result;
    StageEnvironmentLayout layout;
    result=stageData.require({'S','T','A','G'},60,layout.stag);
    if (result!=ResourceStatus::ready) return result;
    if (layout.stag.count!=1 || layout.stag.offset%4) return ResourceStatus::malformed;
    StageChunk layers;
    result=stageData.require({'E','V','L','Y'},15,layers);
    if (result!=ResourceStatus::ready && result!=ResourceStatus::notFound) return result;
    if (result==ResourceStatus::ready && roomNo<layers.count)
        layout.layer=layers.contents.data[roomNo*15+layer];
    if (layout.layer>=15) return ResourceStatus::malformed;
    const char suffix="0123456789abcde"[layout.layer];
    struct Table {std::array<char,4> tag;std::size_t stride;StageChunk* chunk;bool aligned;};
    const Table tables[]={
        {{'P','A','L',suffix},52,&layout.palette,true},
        {{'C','o','l',suffix},12,&layout.pselect,true},
        {{'E','n','v',suffix},65,&layout.environment,false},
        {{'V','R','B',suffix},21,&layout.vrbox,false},
    };
    for (const auto& table:tables) {
        result=stageData.require(table.tag,table.stride,*table.chunk);
        if (result==ResourceStatus::notFound) continue;
        if (result!=ResourceStatus::ready) return result;
        if (!table.chunk->count || (table.aligned && table.chunk->offset%4))
            return ResourceStatus::malformed;
    }
    result=roomData.require({'L','G','T',suffix},32,layout.roomLights);
    if (result!=ResourceStatus::ready && result!=ResourceStatus::notFound) return result;
    if (result==ResourceStatus::ready && layout.roomLights.offset%4)
        return ResourceStatus::malformed;
    if (layout.environment.count && roomNo>=layout.environment.count)
        return ResourceStatus::malformed;
    if (layout.environment.count && layout.pselect.count) {
        for (unsigned i=0;i<layout.environment.count*65;++i) {
            const auto index=layout.environment.contents.data[i];
            // Original environment maps values above 250 to selector zero.
            if (index<=250 && index>=layout.pselect.count) return ResourceStatus::malformed;
        }
    }
    if (layout.pselect.count && layout.palette.count) {
        for (unsigned i=0;i<layout.pselect.count;++i)
            for (unsigned j=0;j<6;++j) {
                const auto index=layout.pselect.contents.data[i*12+j];
                if (index!=255 && index>=layout.palette.count) return ResourceStatus::malformed;
            }
    }
    if (layout.palette.count && layout.vrbox.count) {
        for (unsigned i=0;i<layout.palette.count;++i)
            if (layout.palette.contents.data[i*52+44]>=layout.vrbox.count)
                return ResourceStatus::malformed;
    }
    output=layout;
    return ResourceStatus::ready;
}
}
