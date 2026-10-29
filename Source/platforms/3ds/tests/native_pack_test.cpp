#include "native/pack.hpp"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <cstdio>
using namespace dusk::pica;
namespace {
std::uint32_t read(const std::vector<std::byte>& b,unsigned at){std::uint32_t v=0;for(unsigned n=0;n<4;++n)v|=std::uint32_t(std::to_integer<unsigned>(b[at+n]))<<(8*n);return v;}
void write(std::vector<std::byte>& b,unsigned at,std::uint32_t v){for(unsigned n=0;n<4;++n)b[at+n]=std::byte(v>>(8*n));}
void repair(std::vector<std::byte>& b){write(b,28,crc32(std::span(b).first(28),std::span(b).subspan(32,read(b,24))));}
}
int main(int argc,char** argv){
    assert(argc==2);std::ifstream input(argv[1],std::ios::binary);assert(input.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(input)),{});std::vector<std::byte> bytes(raw.size());std::memcpy(bytes.data(),raw.data(),raw.size());
    const char text[]="123456789";assert(crc32(std::as_bytes(std::span(text,9)))==0xcbf43926);
    MeshPack pack;assert(MeshPack::open(bytes,pack)==PackError::none&&pack.size()==2);
    MeshView mesh;assert(pack.mesh(3,mesh)==PackError::none&&mesh.vertices.size()==240&&mesh.indices.size()==12);
    GeometryInfo info;assert(inspectPackedGeometry(mesh.vertices,mesh.indices,info)&&info.highestBone==2);
    assert(pack.mesh(17,mesh)==PackError::none&&mesh.vertices.size()==180);
    assert(pack.mesh(99,mesh)==PackError::missing&&mesh.vertices.empty());
    std::vector<std::byte> unaligned(bytes.size()+1);std::memcpy(unaligned.data()+1,bytes.data(),bytes.size());
    assert(MeshPack::open(std::span(unaligned).subspan(1),pack)==PackError::none&&pack.mesh(3,mesh)==PackError::none);
    assert(inspectPackedGeometry(mesh.vertices,mesh.indices,info));
    unsigned rejected=0;
    for(unsigned n=0;n<bytes.size();++n){MeshPack bad;assert(MeshPack::open(std::span(bytes).first(n),bad)!=PackError::none&&bad.size()==0);++rejected;}
    for(unsigned n=0;n<96;++n){auto bad=bytes;bad[n]^=std::byte{1};MeshPack p;assert(MeshPack::open(bad,p)!=PackError::none);++rejected;}
    for(auto [offset,value]:std::vector<std::pair<unsigned,std::uint32_t>>{{32,0},{64,3},{36,65537},{40,65536},{44,0xffffffff},{48,0xffffffff},{52,4},{56,4}}){
        auto bad=bytes;write(bad,offset,value);repair(bad);MeshPack p;assert(MeshPack::open(bad,p)!=PackError::none);++rejected;
    }
    {auto bad=bytes;auto at=read(bad,48)+read(bad,56);bad[at]=std::byte{1};MeshPack p;assert(MeshPack::open(bad,p)==PackError::layout);++rejected;}
    {auto bad=bytes;auto at=read(bad,44);bad[at]^=std::byte{1};MeshPack p;assert(MeshPack::open(bad,p)==PackError::none);assert(p.mesh(3,mesh)==PackError::checksum&&mesh.vertices.empty());++rejected;}
    // Parsed metadata is owned, not borrowed. Later descriptor edits cannot
    // turn a formerly bounded view into an out-of-bounds span.
    {auto edited=bytes;MeshPack p;assert(MeshPack::open(edited,p)==PackError::none);write(edited,44,0xffffffff);assert(p.mesh(3,mesh)==PackError::none&&mesh.vertices.size()==240);}
    // A well-checksummed structurally valid pack can still contain bad native
    // attributes. GPU admission must validate them before allocation/submission.
    {auto bad=bytes;const auto vo=read(bad,44),io=read(bad,48);write(bad,vo,0x7fc00000);
     write(bad,60,crc32(std::span(bad).subspan(vo,read(bad,52)),std::span(bad).subspan(io,read(bad,56))));repair(bad);
     MeshPack p;assert(MeshPack::open(bad,p)==PackError::none&&p.mesh(3,mesh)==PackError::none);assert(!inspectPackedGeometry(mesh.vertices,mesh.indices,info));++rejected;}
    std::printf("PASS native pack: two independent Python-authored meshes, unaligned inputs, CRC oracle, owned metadata, attribute admission and %u malformed/truncated refusals\n",rejected);
}
