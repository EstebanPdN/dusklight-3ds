// Mac-only: skinned shapes of every model on the disc and the draws a wider
// matrix palette leaves of them, planned by the runtime's own planner
// (runtime/skin_palette.hpp, the code J3DShape.cpp runs). Built and run by
// Tools/3ds/model_prep.py --skin; nothing here is linked on the 3DS.
//
// Per skinned shape: its matrix groups (today one draw each), the distinct
// matrices its vertices name, the material's lights and texture units, and
// the uniform rows a merged draw needs. Layouts compared (rows of the 94 the
// rigid programs declare; the program's constants take the other two):
//   rows6   the multi-texture program's layout with six rows per matrix
//           (separate normal rows): the Alpha 14 merge.
//   rows3   the same layout, three rows per matrix (the normal from the
//           position rows), the rows after four lights: what skinPalette
//           draws (runtime/skin_palette.hpp capacityForUnits).
//   slim    a new program with ten fixed rows, lights from the top (none when
//           unlit), two rows per texture unit, three per matrix.
// Draw counts keep each run one batch of the command processor (4096 raw
// vertices, 16384 indices, 64 KiB), as the runtime does.
#include "model_prep_walk.hpp"
#include "../../Source/platforms/3ds/runtime/skin_palette.hpp"
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace modelprep;
namespace sk = dusk::native::skin;
namespace {
// Lights a lit draw has at run time: the files give actors all eight (mask
// 0xff), the runtime's colour plan drops the black ones (gx_raster_color.cpp),
// so the count depends on the place; --lights N sets it (0: the file's mask).
unsigned runtimeLights=0;
// --capacity N: palette entries per merged draw for every shape (what the
// runtime plans with), instead of the rows3 capacity of each material.
unsigned fixedCapacity=0;
struct MaterialRows {bool lit{};unsigned lights{},units{};};
// MAT3 of material node `i`: channel 0 lighting and light mask
// (J3DColorChanInfo: enable, material source, light mask, ...), texture units
// (texture numbers in use, the units a draw samples at most).
MaterialRows materialRows(const Bytes& f,const Model& m,int i){
    MaterialRows out{};
    std::uint32_t n=0;
    const auto* mat3=blockOf(f,"MAT3",n);
    if(!mat3||i<0||unsigned(i)>=m.materials.size())return out;
    const auto* d=mat3+be32(mat3+12)+m.materials[i].remap*0x14C;
    const unsigned chan=be16(d+0x0C);
    const std::uint32_t chanTable=be32(mat3+0x28);
    if(chan!=0xFFFF&&chanTable&&chanTable+chan*8+8<=n){
        const auto* c=mat3+chanTable+chan*8;
        out.lit=c[0]!=0;out.lights=out.lit?unsigned(__builtin_popcount(c[2])):0;
        if(out.lit&&runtimeLights)out.lights=std::min(out.lights,runtimeLights);
    }
    for(int k=0;k<8;++k)if(be16(d+0x84+2*k)!=0xFFFF)++out.units;
    return out;
}
// Bytes per vertex in the lists and the matrix byte's place; false for a
// descriptor the merge does not take (texture matrix indices: environment
// maps chosen per bone; direct attributes; NBT).
bool vertexLayout(const Shape& s,sk::VertexLayout& out,bool& textureMatrices){
    out={};textureMatrices=false;bool matrix=false;
    for(std::size_t k=0;k+8<=s.vcd.size();k+=8){
        const auto attr=be32(s.vcd.data()+k),type=be32(s.vcd.data()+k+4);
        if(attr==0xFF)break;
        if(!type)continue;
        if(attr==0){if(type!=1)return false;out.matrixAt=out.stride;out.stride+=1;matrix=true;continue;}
        if(attr<=8){textureMatrices=true;out.stride+=1;continue;}
        if(attr==25||type==1)return false;
        out.stride+=type==2?1:2;
    }
    return matrix&&!textureMatrices;
}
// Every planned shape is checked: each vertex of each merged list names the
// matrix its group's slot held under GX's slot rules (a slot a group does
// not load keeps the previous group's matrix).
std::uint64_t comparedVertices=0,mismatchedVertices=0,listsWritten=0,listFailures=0;
// Planned shapes with a weighted (envelope) matrix in a run: J3D may give
// those a normal matrix of their own (the inverse transpose), which the
// three-row palette cannot hold; the runtime then draws the shape the
// original way that picture (J3DNativeSkinPalette.h, "scaled").
std::uint64_t plannedShapes=0,envelopeShapes=0,envelopeEntries=0,plannedEntries=0;
struct Count {
    std::uint64_t models{},skinnedShapes{},groups{},rows6{},rows3{},slim{},p16{},p26{},mergedBytes{},listBytes{};
    std::uint64_t refused[unsigned(sk::Refusal::count)]{};
    std::uint64_t textureMatrixShapes{},layoutShapes{};
    void add(const Count& o){
        models+=o.models;skinnedShapes+=o.skinnedShapes;groups+=o.groups;rows6+=o.rows6;rows3+=o.rows3;slim+=o.slim;
        p16+=o.p16;p26+=o.p26;mergedBytes+=o.mergedBytes;listBytes+=o.listBytes;
        for(unsigned r=0;r<unsigned(sk::Refusal::count);++r)refused[r]+=o.refused[r];
        textureMatrixShapes+=o.textureMatrixShapes;layoutShapes+=o.layoutShapes;
    }
};
unsigned rows6Capacity(unsigned lights,unsigned units){
    lights=lights?lights:1;units=units<1?1:units>3?3:units;
    const unsigned base=19+4*lights,limit=81+4*(3-units);
    // The Alpha 14 merge drew a group alone when its matrices did not fit.
    return std::max(sk::slots,limit>=base+6?1+(limit-base)/6:1);
}
unsigned slimCapacity(const MaterialRows& m){
    // Below ten entries a group may not fit alone (planShape refuses).
    const unsigned units=m.units<1?1:m.units>3?3:m.units;
    const unsigned fixed=10+4*(m.lit?m.lights:0)+2*units;
    return fixed>=94?0:std::max(sk::slots,std::min((94-fixed)/3,sk::maxMatrices));
}
struct ShapeLine {std::string text;};
// Draws of one skinned shape under every layout; detail lines when asked.
void surveyShape(const Bytes& f,const Model& m,const Shape& s,unsigned index,Count& c,std::vector<std::string>* lines){
    sk::VertexLayout layout;bool textureMatrices=false;
    const bool layoutOk=vertexLayout(s,layout,textureMatrices);
    ++c.skinnedShapes;c.groups+=s.groups.size();
    if(!layoutOk){
        if(textureMatrices)++c.textureMatrixShapes;else ++c.layoutShapes;
        c.rows6+=s.groups.size();c.rows3+=s.groups.size();c.slim+=s.groups.size();c.p16+=s.groups.size();c.p26+=s.groups.size();
        if(lines){char b[200];std::snprintf(b,sizeof(b),"    shape %2u: groups %2zu  refused (%s)",index,s.groups.size(),textureMatrices?"texture matrix per bone":"vertex layout");lines->push_back(b);}
        return;
    }
    std::uint32_t n=0;
    const auto* shp1=blockOf(f,"SHP1",n);
    const auto mtxTable=be32(shp1+28),initOff=be32(shp1+12),mtxInitOff=be32(shp1+36);
    const unsigned mtxInit=be16(shp1+initOff+s.initIndex*0x28+6);
    std::vector<sk::GroupInput> groups(s.groups.size());
    for(std::size_t g=0;g<s.groups.size();++g){
        const auto* mi=shp1+mtxInitOff+(mtxInit+g)*8;
        const unsigned count=be16(mi+2),first=be32(mi+4);
        groups[g].list=f.data()+s.groups[g].dlOffset;groups[g].bytes=s.groups[g].dlSize;
        groups[g].loads.fill(0xffff);
        for(unsigned i=0;i<count&&i<sk::slots;++i)groups[g].loads[i]=be16(shp1+mtxTable+2*(first+i));
        c.listBytes+=s.groups[g].dlSize;
    }
    auto keyOf=[&](std::uint16_t use)->std::uint32_t{
        if(use>=m.drawIndex.size())return 0xffffffffu-use;
        return (std::uint32_t(m.drawWeighted[use])<<16)|m.drawIndex[use];
    };
    const auto mat=materialRows(f,m,s.material);
    sk::ShapePlan plan;
    auto draws=[&](unsigned capacity,sk::Refusal* why=nullptr){
        const auto r=sk::planShape(std::span<const sk::GroupInput>(groups),layout,keyOf,capacity,plan);
        if(why)*why=r;
        return r==sk::Refusal::none?plan.runs:unsigned(groups.size());
    };
    const unsigned cap3=fixedCapacity?fixedCapacity:sk::capacityForUnits(mat.units),cap6=rows6Capacity(mat.lit?mat.lights:0,mat.units),capSlim=slimCapacity(mat);
    const unsigned d6=draws(cap6),dSlim=draws(capSlim),d16=draws(16),d26=draws(26);
    sk::Refusal why{};
    const unsigned d3=draws(cap3,&why);
    ++c.refused[unsigned(why)];
    unsigned matrices=0;
    if(why==sk::Refusal::none){
        for(unsigned r=0;r<plan.runs;++r)c.mergedBytes+=sk::mergedBytes(plan.run[r]);
        std::vector<std::uint32_t> expected,merged;
        std::array<std::uint32_t,sk::slots> slot{};
        for(std::size_t g=0;g<groups.size();++g){
            for(unsigned k=0;k<sk::slots;++k)if(groups[g].loads[k]!=0xffff)slot[k]=keyOf(groups[g].loads[k]);
            const auto* l=groups[g].list;
            for(std::uint32_t at=0;at<groups[g].bytes;){
                if(!l[at]){++at;continue;}
                const unsigned n=(unsigned(l[at+1])<<8)|l[at+2];
                for(unsigned v=0;v<n;++v)expected.push_back(slot[l[at+3+v*layout.stride+layout.matrixAt]/3]);
                at+=3+n*layout.stride;
            }
        }
        for(unsigned r=0;r<plan.runs;++r){
            std::vector<std::uint8_t> list(sk::mergedBytes(plan.run[r]));
            ++listsWritten;
            if(!sk::writeMerged(std::span<const sk::GroupInput>(groups),layout,plan,r,list.data())){++listFailures;continue;}
            for(std::uint32_t at=0;at<plan.run[r].listBytes;){
                const unsigned n=(unsigned(list[at+1])<<8)|list[at+2];
                for(unsigned v=0;v<n;++v)merged.push_back(plan.run[r].keys[list[at+3+v*layout.stride+layout.matrixAt]/3]);
                at+=3+n*layout.stride;
            }
        }
        ++plannedShapes;
        bool envelope=false;
        for(unsigned r=0;r<plan.runs;++r)for(unsigned k=0;k<plan.run[r].matrices;++k){
            ++plannedEntries;if(plan.run[r].keys[k]>>16){++envelopeEntries;envelope=true;}}
        envelopeShapes+=envelope;
        comparedVertices+=expected.size();
        if(merged.size()!=expected.size())mismatchedVertices+=expected.size();
        else for(std::size_t i=0;i<expected.size();++i)mismatchedVertices+=merged[i]!=expected[i];
        // Distinct matrices of the whole shape (the palette's most).
        std::vector<std::uint32_t> all;
        for(std::size_t g=0;g<groups.size();++g)for(unsigned k=0;k<sk::slots;++k)if(groups[g].loads[k]!=0xffff){
            const auto key=keyOf(groups[g].loads[k]);bool seen=false;for(auto x:all)seen|=x==key;if(!seen)all.push_back(key);}
        matrices=unsigned(all.size());
    }
    c.rows6+=d6;c.rows3+=d3;c.slim+=dSlim;c.p16+=d16;c.p26+=d26;
    if(lines){
        char b[256];
        std::snprintf(b,sizeof(b),"    shape %2u: groups %2zu matrices %2u  lights %u%s units %u  capacity rows6 %2u rows3 %2u slim %2u  draws rows6 %2u rows3 %2u slim %2u%s%s",
                      index,s.groups.size(),matrices,mat.lights,mat.lit?"":" (unlit)",mat.units,cap6,cap3,capSlim,d6,d3,dSlim,
                      why==sk::Refusal::none?"":"  refused: ",why==sk::Refusal::none?"":sk::refusalName(why));
        lines->push_back(b);
        if(why==sk::Refusal::none)for(unsigned r=0;r<plan.runs;++r){
            const auto& run=plan.run[r];
            std::snprintf(b,sizeof(b),"      run %u: groups %u-%u, %2u matrices = %2u rows, %4u vertices, %5u indices, list %5u bytes",
                          r,run.firstGroup,run.firstGroup+run.groups-1,run.matrices,3u*run.matrices,run.vertices,run.indices,sk::mergedBytes(run));
            lines->push_back(b);
        }
    }
}
bool hasMatrixIndex(const Shape& s){
    for(std::size_t k=0;k+8<=s.vcd.size();k+=8)if(be32(s.vcd.data()+k)==0&&be32(s.vcd.data()+k+4)!=0)return true;
    return false;
}
}

int main(int argc,char** argv){
    std::string discPath;
    std::vector<std::string> detail;
    for(int i=1;i+1<argc;i+=2){
        const std::string a=argv[i];
        if(a=="--disc")discPath=argv[i+1];
        else if(a=="--detail")detail.push_back(argv[i+1]);
        else if(a=="--lights")runtimeLights=unsigned(std::stoul(argv[i+1]));
        else if(a=="--capacity")fixedCapacity=unsigned(std::stoul(argv[i+1]));
    }
    if(discPath.empty()){std::fprintf(stderr,"usage: model_prep_skin --disc zelda.twilight [--detail /res/Object/Kmdl.arc ...]\n");return 2;}
    dusk::native::Disc disc;
    if(disc.open(discPath.c_str())!=dusk::native::DiscStatus::ready){std::fprintf(stderr,"disc not readable\n");return 1;}
    dusk::native::DiscDirectory root;
    if(disc.findDirectory("/",root)!=dusk::native::DiscFileStatus::ready)return 1;
    Count all;
    std::map<std::string,Count> perArchive;
    std::map<std::string,std::vector<std::string>> lines;
    walkAllModels(disc,root,"",[&](const std::string& path,const Bytes& bytes){
        const auto m=parseModel(bytes);
        if(!m.error.empty())return;
        const auto archive=path.substr(0,path.find(':'));
        bool wanted=false;for(const auto& d:detail)wanted|=archive==d;
        Count c;c.models=1;
        std::vector<std::string> shapeLines;
        for(std::size_t s=0;s<m.shapes.size();++s){
            const auto& shape=m.shapes[s];
            if(shape.mtxType!=3||!hasMatrixIndex(shape)||shape.directAttributes)continue;
            surveyShape(bytes,m,shape,unsigned(s),c,wanted?&shapeLines:nullptr);
        }
        all.add(c);perArchive[archive].add(c);
        if(wanted&&c.skinnedShapes){
            char b[200];
            std::snprintf(b,sizeof(b),"  %s: %zu shapes, skinned %llu: groups %llu -> rows6 %llu, rows3 %llu, slim %llu; merged lists %llu bytes",
                          path.c_str(),m.shapes.size(),(unsigned long long)c.skinnedShapes,(unsigned long long)c.groups,(unsigned long long)c.rows6,
                          (unsigned long long)c.rows3,(unsigned long long)c.slim,(unsigned long long)c.mergedBytes);
            auto& l=lines[archive];l.push_back(b);l.insert(l.end(),shapeLines.begin(),shapeLines.end());
        }
    });
    std::printf("lights per lit draw: %s; rows3 capacity: %s\n",runtimeLights?std::to_string(runtimeLights).c_str():"the files' masks",
                fixedCapacity?std::to_string(fixedCapacity).c_str():"each material's");
    std::printf("whole disc: %llu models, %llu skinned shapes in %llu matrix groups (one draw each today)\n",(unsigned long long)all.models,
                (unsigned long long)all.skinnedShapes,(unsigned long long)all.groups);
    std::printf("  draws: rows6 %llu  rows3 %llu  slim %llu  (fixed palette 16: %llu, 26: %llu; one batch per run)\n",
                (unsigned long long)all.rows6,(unsigned long long)all.rows3,(unsigned long long)all.slim,(unsigned long long)all.p16,(unsigned long long)all.p26);
    std::printf("  shapes refused whole: texture matrix per bone %llu, other layout %llu;",(unsigned long long)all.textureMatrixShapes,(unsigned long long)all.layoutShapes);
    for(unsigned r=1;r<unsigned(sk::Refusal::count);++r)std::printf(" %s %llu",sk::refusalName(sk::Refusal(r)),(unsigned long long)all.refused[r]);
    std::printf("\n  merged lists %.1f KiB for %.1f KiB of skinned group lists\n",all.mergedBytes/1024.0,all.listBytes/1024.0);
    std::printf("  planned shapes %llu, with a weighted (envelope) palette entry %llu; entries %llu, weighted %llu\n",
                (unsigned long long)plannedShapes,(unsigned long long)envelopeShapes,(unsigned long long)plannedEntries,(unsigned long long)envelopeEntries);
    std::printf("  checked (rows3 plans): %llu merged lists written (%llu failed), %llu vertices compared with GX's slots, %llu name another matrix\n",
                (unsigned long long)listsWritten,(unsigned long long)listFailures,(unsigned long long)comparedVertices,(unsigned long long)mismatchedVertices);
    for(const auto& d:detail){
        const auto it=perArchive.find(d);
        if(it==perArchive.end()){std::printf("%s: not found\n",d.c_str());continue;}
        const auto& c=it->second;
        std::printf("%s: skinned shapes %llu, groups %llu -> rows6 %llu rows3 %llu slim %llu, merged lists %llu bytes\n",d.c_str(),
                    (unsigned long long)c.skinnedShapes,(unsigned long long)c.groups,(unsigned long long)c.rows6,(unsigned long long)c.rows3,
                    (unsigned long long)c.slim,(unsigned long long)c.mergedBytes);
        for(const auto& l:lines[d])std::printf("%s\n",l.c_str());
    }
    return 0;
}
