// Vertex program per plan kind (runtime/gx_vertex_variant.hpp): the key and
// the variant the runtime chooses for each plan of a vertex-plans.bin file
// (recorded by gx_pica.cpp, SD file vertex-plans.txt, or written by
// Tools/3ds/test_vertex_variants.py for its sweep). One line per plan:
// "<recorded key> <key> <variant>". The script runs the general program and
// the chosen variant on the plan's rows and vertices and compares the bits.
#include "../runtime/gx_vertex_variant.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace dusk::native::gx;
namespace {
// gx_pica.cpp VertexPlanRecord.
struct Record {
    std::uint32_t key,lights,stride,vertexCount;
    UniformRows rows;
    std::array<std::uint8_t,16*36> vertices;
};
static_assert(sizeof(Record)==2096);
}
int main(int argc,char** argv){
    if(argc==2&&!std::strcmp(argv[1],"--table")){
        for(unsigned v=1;v<vertexVariantCount;++v)
            std::printf("%u %08lx %08lx %s\n",v,static_cast<unsigned long>(vertexVariants[v].mask),
                        static_cast<unsigned long>(vertexVariants[v].value),vertexVariants[v].name);
        return 0;
    }
    if(argc!=2){std::fprintf(stderr,"usage: vertex_variant_test plans.bin | --table\n");return 2;}
    std::FILE* f=std::fopen(argv[1],"rb");
    if(!f){std::perror(argv[1]);return 2;}
    std::uint32_t head[4]{};
    if(std::fread(head,sizeof(head),1,f)!=1||head[0]!=0x52505644u||head[1]!=1||head[3]!=sizeof(Record)){
        std::fprintf(stderr,"%s: not a vertex-plans.bin of version 1\n",argv[1]);return 2;
    }
    std::vector<Record> records(head[2]);
    if(std::fread(records.data(),sizeof(Record),records.size(),f)!=records.size()){std::fprintf(stderr,"short file\n");return 2;}
    std::fclose(f);
    for(const auto& r:records){
        const std::uint32_t key=vertexPlanKey(r.rows,r.lights,r.key&vertexKey::fogOnly);
        std::printf("%08lx %08lx %u\n",static_cast<unsigned long>(r.key),static_cast<unsigned long>(key),vertexVariantFor(key));
    }
    return 0;
}
