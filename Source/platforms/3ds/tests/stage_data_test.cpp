#include "../runtime/stage_data.hpp"
#include "../runtime/disc.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace dusk::native;
namespace {
void word(std::vector<unsigned char>& b,unsigned at,unsigned value) {
    for(unsigned i=0;i<4;++i)b[at+i]=value>>(24-i*8);
}
std::vector<unsigned char> fixture() {
    std::vector<unsigned char>b(160);
    word(b,0,2);std::memcpy(b.data()+4,"PAL0",4);word(b,8,2);word(b,12,32);
    std::memcpy(b.data()+16,"Env0",4);word(b,20,0);word(b,24,0);return b;
}
void synthetic() {
    StageData view;StageChunk c;
    assert(view.find({'P','A','L','0'},c)==ResourceStatus::notReady);
    auto b=fixture();const auto original=b;
    assert(view.open({b.data(),b.size()})==ResourceStatus::ready && view.chunkCount()==2);
    assert(view.require({'P','A','L','0'},52,c)==ResourceStatus::ready);
    assert(c.count==2 && c.offset==32 && c.contents.size==128);
    assert(view.require({'P','A','L','0'},80,c)==ResourceStatus::truncated);
    assert(view.chunk(1,c)==ResourceStatus::ready && c.count==0 && c.contents.data==nullptr);
    assert(view.chunk(2,c)==ResourceStatus::notFound);
    assert(view.find({'S','T','A','G'},c)==ResourceStatus::notFound && b==original);
    b=fixture();word(b,24,80);assert(view.open({b.data(),b.size()})==ResourceStatus::ready);
    assert(view.require({'P','A','L','0'},52,c)==ResourceStatus::truncated); // Neighbour boundary.
    b=fixture();word(b,12,12);assert(view.open({b.data(),b.size()})==ResourceStatus::malformed);
    b=fixture();word(b,12,161);assert(view.open({b.data(),b.size()})==ResourceStatus::malformed);
    b=fixture();word(b,12,160);assert(view.open({b.data(),b.size()})==ResourceStatus::malformed);
    b=fixture();word(b,20,1);assert(view.open({b.data(),b.size()})==ResourceStatus::malformed);
    b=fixture();std::memcpy(b.data()+16,"PAL0",4);word(b,24,144);
    assert(view.open({b.data(),b.size()})==ResourceStatus::ambiguous);
    b=fixture();b[4]=0;assert(view.open({b.data(),b.size()})==ResourceStatus::malformed);
    b=fixture();word(b,0,4097);assert(view.open({b.data(),b.size()})==ResourceStatus::limitExceeded);
    b=fixture();word(b,8,65537);assert(view.open({b.data(),b.size()})==ResourceStatus::limitExceeded);
    b=fixture();assert(view.open({b.data(),27})==ResourceStatus::truncated);
    assert(view.chunk(0,c)==ResourceStatus::notReady); // Failed opens clear borrowed state.
    std::vector<unsigned char> stage(312),room(4);
    word(stage,0,6);
    const char* tags[]={"STAG","EVLY","VRB0","PAL0","Env0","Col0"};
    const unsigned offsets[]={80,140,156,180,232,300};
    for(unsigned i=0;i<6;++i) {
        std::memcpy(stage.data()+4+i*12,tags[i],4);word(stage,8+i*12,1);word(stage,12+i*12,offsets[i]);
    }
    StageEnvironmentLayout layout;
    auto validate=[&]{return inspectStageEnvironment({stage.data(),stage.size()},{room.data(),room.size()},0,0,layout);};
    assert(validate()==ResourceStatus::ready && layout.palette.count==1 && layout.layer==0);
    stage[140]=15;assert(validate()==ResourceStatus::malformed);stage[140]=0;
    stage[232]=1;assert(validate()==ResourceStatus::malformed);stage[232]=255;assert(validate()==ResourceStatus::ready);stage[232]=0;
    stage[300]=1;assert(validate()==ResourceStatus::malformed);stage[300]=255;assert(validate()==ResourceStatus::ready);stage[300]=0;
    stage[180+44]=1;assert(validate()==ResourceStatus::malformed);stage[180+44]=0;
    assert(inspectStageEnvironment({stage.data(),stage.size()},{room.data(),room.size()},64,0,layout)==ResourceStatus::malformed);
    assert(inspectStageEnvironment({stage.data(),stage.size()},{room.data(),room.size()},0,15,layout)==ResourceStatus::malformed);
    std::puts("PASS bounded original DZS/DZR table and consumed record extents; bytes immutable");
}
std::vector<unsigned char> inspect(Disc& disc,const char* arcName,const char* resourceName) {
    DiscFile f;assert(disc.findFile(arcName,f)==DiscFileStatus::ready && f.size()<=8*1024*1024);
    std::vector<unsigned char> archive(f.size());assert(disc.readFile(f,0,archive.data(),archive.size()));
    std::vector<unsigned char> expanded;
    if(archive.size()>=4 && !std::memcmp(archive.data(),"Yaz0",4)) {
        unsigned size=0;assert(yaz0ExpandedSize(archive.data(),archive.size(),8*1024*1024,size)==ResourceStatus::ready);
        expanded.resize(size);std::size_t written;
        assert(decodeYaz0(archive.data(),archive.size(),expanded.data(),expanded.size(),8*1024*1024,written)==ResourceStatus::ready);
        archive.swap(expanded);
    }
    RarcArchive rarc;assert(rarc.open(archive.data(),archive.size(),{8*1024*1024,4096,65536,255})==ResourceStatus::ready);
    RarcEntry resource;assert(rarc.findFile(resourceName,resource)==ResourceStatus::ready);
    StageData view;assert(view.open(resource.contents)==ResourceStatus::ready);
    std::printf("{\"archive\":\"%s\",\"resource\":\"%s\",\"bytes\":%zu,\"chunks\":[",arcName,resourceName,resource.contents.size);
    for(unsigned i=0;i<view.chunkCount();++i) {
        StageChunk c;assert(view.chunk(i,c)==ResourceStatus::ready);
        std::printf("%s{\"tag\":\"%.4s\",\"count\":%u,\"offset\":%u,\"bytes\":%zu}",i?",":"",c.tag.data(),c.count,c.offset,c.contents.size);
    }
    std::puts("]}");
    return {resource.contents.data,resource.contents.data+resource.contents.size};
}
}
int main(int argc,char** argv) {
    synthetic();
    if(argc==2) {
        Disc disc;assert(disc.open(argv[1])==DiscStatus::ready);
        auto stage=inspect(disc,"/res/Stage/F_SP103/STG_00.arc","stage.dzs");
        auto room=inspect(disc,"/res/Stage/F_SP103/R00_00.arc","room.dzr");
        StageEnvironmentLayout layout;
        const auto status=inspectStageEnvironment({stage.data(),stage.size()},{room.data(),room.size()},0,0,layout);
        std::printf("environment validation: %s\n",resourceStatusText(status));
        assert(status==ResourceStatus::ready);
        std::printf("{\"environment_layer\":%u,\"palettes\":%u,\"selectors\":%u,\"environments\":%u,\"sky_colors\":%u,\"room_lights\":%u}\n",layout.layer,layout.palette.count,layout.pselect.count,layout.environment.count,layout.vrbox.count,layout.roomLights.count);
    }
}
