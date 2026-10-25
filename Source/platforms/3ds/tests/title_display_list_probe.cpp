// Private, read-only compatibility probe. Saves counts only, never model bytes.
#include "../runtime/disc.hpp"
#include "../runtime/resource_archive.hpp"
#include <aurora/dl.hpp>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>
using namespace dusk::native;
using namespace aurora::gx::dl;
using Bytes = std::vector<u8>;
using Span = std::span<const u8>;
static Span range(Span b, size_t at, size_t n) {
  assert(at <= b.size() && n <= b.size() - at); return b.subspan(at,n);
}
static u32 word(Span b, size_t at) {
  auto s=range(b,at,4); return (u32(s[0])<<24)|(u32(s[1])<<16)|(u32(s[2])<<8)|s[3];
}
static u16 half(Span b, size_t at) { auto s=range(b,at,2); return (u16(s[0])<<8)|s[1]; }
struct Metrics { size_t draws=0,vertices=0,indices=0,triangles=0; };
static Bytes flatten(Span b, const GXVtxDescList* desc, Metrics& stats) {
  Reader reader(b.data(),b.size(),desc); Bytes out;
  while(auto c=reader.next()) {
    if(c->kind==Command::Kind::Passthrough) {
      // This selected shape probe does not interpret changing format/state.
      assert(c->size==1 && c->data[0]==0); continue;
    }
    ++stats.draws; const auto& d=c->draw; stats.vertices+=d.vtxCount;
    auto emit=[&](u16 i){ const auto* v=d.vtx(i); out.insert(out.end(),v,v+d.layout->stride); };
    if(c->kind==Command::Kind::DrawIndexed) {
      assert(d.prim==GX_TRIANGLES && d.indexCount%3==0);
      stats.indices+=d.indexCount; stats.triangles+=d.indexCount/3;
      for(u32 i=0;i<d.indexCount;++i) emit(d.index(i));
    } else {
      assert(expand_triangles(d.prim,d.vtxCount,[&](u16 a,u16 b,u16 c){
        emit(a);emit(b);emit(c);++stats.triangles;
      }));
    }
  }
  assert(!reader.failed()); return out;
}
int main(int argc,char** argv) {
  assert(argc==2); Disc disc; assert(disc.open(argv[1])==DiscStatus::ready);
  DiscFile file; assert(disc.findFile("/res/Object/Title.arc",file)==DiscFileStatus::ready);
  constexpr size_t budget=32*1024*1024;
  assert(file.size()<=budget); Bytes encoded(file.size());
  assert(disc.readFile(file,0,encoded.data(),encoded.size()));
  u32 expanded=0; assert(yaz0ExpandedSize(encoded.data(),encoded.size(),budget,expanded)==ResourceStatus::ready);
  Bytes archiveBytes(expanded); size_t written=0;
  assert(decodeYaz0(encoded.data(),encoded.size(),archiveBytes.data(),archiveBytes.size(),budget,written)==ResourceStatus::ready);
  RarcArchive archive; assert(archive.open(archiveBytes.data(),written,{budget,4096,65536,255})==ResourceStatus::ready);
  RarcEntry model; assert(archive.findFile("titlelogo_r.bmd",model)==ResourceStatus::ready);
  Span bmd(model.contents.data,model.contents.size); assert(bmd.size()>=32 && !memcmp(bmd.data(),"J3D2bmd3",8));
  assert(word(bmd,8)==bmd.size()); const u32 blocks=word(bmd,12); assert(blocks<=64);
  size_t at=32; Span shapes;
  for(u32 i=0;i<blocks;++i) {
    const u32 size=word(bmd,at+4); assert(size>=8); const auto block=range(bmd,at,size);
    if(!memcmp(block.data(),"SHP1",4)) { assert(shapes.empty()); shapes=block; }
    at+=size;
  }
  assert(!shapes.empty()); range(shapes,0,44);
  const size_t count=half(shapes,8), init=word(shapes,12), remap=word(shapes,16),
    descriptors=word(shapes,24), lists=word(shapes,32), draws=word(shapes,40);
  Metrics original,optimized; size_t groups=0,oldBytes=0,newBytes=0;
  for(size_t shape=0;shape<count;++shape) {
    const size_t index=half(shapes,remap+2*shape); assert(index<count);
    const auto record=range(shapes,init+40*index,40);
    const size_t num=half(record,2), descOffset=descriptors+half(record,4), first=half(record,8);
    GXVtxDescList desc[GX_VA_MAX_ATTR+1]{}; bool terminated=false;
    for(size_t d=0;d<GX_VA_MAX_ATTR+1;++d) {
      const u32 attr=word(shapes,descOffset+8*d), type=word(shapes,descOffset+8*d+4);
      assert((attr<=GX_VA_TEX7 || attr==GX_VA_NULL) && type<=GX_INDEX16);
      desc[d]={static_cast<GXAttr>(attr),static_cast<GXAttrType>(type)};
      if(attr==GX_VA_NULL) { terminated=true; break; }
    }
    assert(terminated);
    for(size_t group=0;group<num;++group) {
      const size_t drawAt=draws+8*(first+group);
      auto input=range(shapes,lists+word(shapes,drawAt+4),word(shapes,drawAt));
      auto output=optimize(input.data(),input.size(),desc); assert(output);
      const auto before=flatten(input,desc,original), after=flatten(*output,desc,optimized);
      assert(before==after); ++groups; oldBytes+=input.size(); newBytes+=output->size();
    }
  }
  printf("{\"resource\":\"Title.arc/titlelogo_r.bmd\",\"shape_count\":%zu,\"matrix_groups\":%zu,"
         "\"draws_before\":%zu,\"draws_after\":%zu,\"vertices_before\":%zu,\"vertices_after\":%zu,"
         "\"triangles\":%zu,\"prebuilt_indices\":%zu,\"bytes_before\":%zu,\"bytes_after\":%zu,"
         "\"exact_expanded_vertex_stream_match\":true,\"payload_saved\":false}\n",
         count,groups,original.draws,optimized.draws,original.vertices,optimized.vertices,
         original.triangles,optimized.indices,oldBytes,newBytes);
}
