#include "native/pack.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <vector>
using namespace dusk::pica;
namespace {
unsigned word(const std::vector<std::byte>& bytes,unsigned offset){unsigned v=0;for(unsigned n=0;n<4;++n)v|=std::to_integer<unsigned>(bytes[offset+n])<<(8*n);return v;}
}
int main(int argc,char** argv){
    if(argc!=2)return 2;
    std::ifstream file(argv[1],std::ios::binary|std::ios::ate);
    const auto size=file.tellg();if(!file||size<32||size>(64<<20))return 2;
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));file.seekg(0);
    if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))return 2;
    MeshPack pack;if(MeshPack::open(bytes,pack)!=PackError::none)return 1;
    unsigned vertices=0,indices=0,maximumPalette=0,rigidMeshes=0;
    for(unsigned n=0;n<pack.size();++n){
        MeshView view;GeometryInfo info;
        if(pack.mesh(word(bytes,32+n*32),view)!=PackError::none||!inspectPackedGeometry(view.vertices,view.indices,info))return 1;
        vertices+=info.vertices;indices+=info.indices;maximumPalette=std::max(maximumPalette,info.highestBone+1);
        if(info.rigidFirst)++rigidMeshes;
    }
    std::cout<<"{\"native_meshes\":"<<pack.size()<<",\"vertices\":"<<vertices<<",\"indices\":"<<indices<<",\"maximum_palette\":"<<maximumPalette<<",\"rigid_meshes\":"<<rigidMeshes<<",\"gpu_executed\":false}\n";
}
