#include "native/pose.hpp"
#include <cassert>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
using namespace dusk::pica;
void put(std::vector<char>& d,unsigned at,std::uint32_t v){std::memcpy(d.data()+at,&v,4);}
void checksum(std::vector<char>& d){put(d,44,crc32(std::as_bytes(std::span(d)).first(44),std::as_bytes(std::span(d)).subspan(48)));}
int main(int argc,char** argv){
    assert(argc==2||argc==3);std::ifstream input(argv[1],std::ios::binary);assert(input.good());std::vector<char> bytes((std::istreambuf_iterator<char>(input)),{});
    PosePack pack;assert(PosePack::open(std::as_bytes(std::span(bytes)),pack)==PackError::none);
    std::vector<Affine> world(pack.nodeCount());std::vector<PoseMatrix> matrices(pack.matrixCount());PoseView view;
    const auto base=PosePack::identity();assert(pack.evaluate(base,{},world,matrices,view));
    for(unsigned n=1;n<=matrices.size();++n){Palette palette;assert(view.palette(std::span(&n,1),palette)&&palette.count==1);for(unsigned r=0;r<3;++r)assert(palette.position[r]==matrices[n-1].position[r]&&palette.normal[r]==matrices[n-1].normal[r]);}
    if(argc==3){std::ofstream out(argv[2],std::ios::binary);assert(out.good());std::uint32_t counts[]{pack.nodeCount(),pack.matrixCount()};out.write(reinterpret_cast<char*>(counts),sizeof(counts));out.write(reinterpret_cast<const char*>(world.data()),world.size()*sizeof(Affine));out.write(reinterpret_cast<const char*>(matrices.data()),matrices.size()*sizeof(PoseMatrix));assert(out.good());return 0;}
    assert(pack.nodeCount()==3&&pack.matrixCount()==3&&pack.anchorNode(2)==2&&pack.anchorNode(99)==0);
    assert(world[1][0][0]==2&&world[1][0][3]==10&&world[1][1][3]==5&&matrices[0].normal[0][0]==.5f);
    assert(matrices[1].position[0][3]==0&&matrices[1].position[1][3]==1.25f&&matrices[1].position[2][2]==.75f); // Authored weights are not renormalized.
    assert(matrices[1].position==matrices[2].position&&matrices[1].normal!=matrices[2].normal); // Authored linear and inverse policies differ.
    const auto id=std::uint32_t(1);Palette palette;assert(!view.palette({},palette)&&palette.count==0);std::uint32_t bad=4;assert(!view.palette(std::span(&bad,1),palette)&&palette.count==0);
    assert(view.palette(std::span(&id,1),palette));
    std::vector<Affine> locals(world.size());for(unsigned n=0;n<locals.size();++n)assert(pack.restLocal(n,locals[n]));Affine invalid;assert(!pack.restLocal(3,invalid));
    assert(!pack.evaluate(base,locals,world,matrices,view)&&view.nodes().empty()); // Animated inputs require explicit normal policy.
    std::array<NormalMode,3> modes{NormalMode::inverseTranspose,NormalMode::inverseTranspose,NormalMode::linear};
    assert(pack.evaluate(base,locals,world,matrices,view,modes));locals[1][1][3]=8;assert(pack.evaluate(base,locals,world,matrices,view,modes)&&world[1][1][3]==8);
    modes[2]=NormalMode(2);assert(!pack.evaluate(base,locals,world,matrices,view,modes)&&view.matrices().empty());modes[2]=NormalMode::linear;
    for(auto bits:{0x7fc00000u,0x7f800001u,0x00000001u,0x1fffffffu,0x7f800000u}){auto wrong=base;wrong[0][0]=std::bit_cast<float>(bits);const auto before=world;assert(!pack.evaluate(wrong,{},world,matrices,view)&&view.nodes().empty()&&world==before);}
    assert(!pack.evaluate(base,{},std::span(world).first(2),matrices,view)&&view.matrices().empty());
    assert(!pack.evaluate(world[0],{},world,matrices,view));assert(!pack.evaluate(base,std::span(world),world,matrices,view,modes));
    auto singular=base;singular[0]={};assert(!pack.evaluate(singular,{},world,matrices,view)&&view.nodes().empty());
    assert(!pack.evaluate(base,{},world,{reinterpret_cast<PoseMatrix*>(world.data()),3},view)); // Checked before dereference.
    unsigned rejected=0;
    for(unsigned n=0;n<bytes.size();++n){PosePack badPack;assert(PosePack::open(std::as_bytes(std::span(bytes)).first(n),badPack)!=PackError::none&&badPack.nodeCount()==0);++rejected;}
    for(unsigned mutation=0;mutation<15;++mutation){auto badBytes=bytes;
        const unsigned drawAt=48+3*64,termAt=drawAt+3*16;
        if(mutation==0)put(badBytes,16,8193);if(mutation==1)put(badBytes,48,1);if(mutation==2)put(badBytes,48+64+4,1);if(mutation==3)put(badBytes,48+56,1);
        if(mutation==4)put(badBytes,48+8,0x7f800001);if(mutation==5)put(badBytes,48+8,1);if(mutation==6)put(badBytes,drawAt,2);if(mutation==7)put(badBytes,drawAt+4,4);if(mutation==8)put(badBytes,drawAt+12,2);
        if(mutation==9)put(badBytes,drawAt+16+4,1);if(mutation==10)put(badBytes,drawAt+16+8,256);if(mutation==11)put(badBytes,termAt,4);if(mutation==12)put(badBytes,termAt+4,0x7f800000);if(mutation==13)put(badBytes,termAt+8,1);if(mutation==14)put(badBytes,40,1);
        checksum(badBytes);PosePack badPack;assert(PosePack::open(std::as_bytes(std::span(badBytes)),badPack)!=PackError::none&&badPack.nodeCount()==0);++rejected;
    }
    auto unaligned=bytes;unaligned.insert(unaligned.begin(),char(0));assert(PosePack::open(std::as_bytes(std::span(unaligned)).subspan(1),pack)==PackError::none);assert(pack.evaluate(base,{},world,matrices,view));
    std::printf("PASS native pose hierarchy, weighted matrices, independent normal policies, palette binding, failure invalidation, alias/domain checks and %u malformed/truncated refusals\n",rejected);
}
