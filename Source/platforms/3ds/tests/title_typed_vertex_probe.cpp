// Private, read-only compatibility probe. Saves counts only, never model bytes.
#include "../runtime/disc.hpp"
#include "../runtime/resource_archive.hpp"
#include <aurora/dl.hpp>
#include "../runtime/gx_vertex_decode.hpp"
#include "../runtime/gx_stream.hpp"
#include <dolphin/gd.h>
#include <dolphin/os.h>
#include <stdexcept>
#include <bit>
#include <cmath>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>
extern "C" void OSPanic(const char*,int,const char* msg,...){throw std::runtime_error(msg);}
extern "C" void DCFlushRange(void*,u32){}
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
  size_t at=32; Span shapes, vertex;
  for(u32 i=0;i<blocks;++i) {
    const u32 size=word(bmd,at+4); assert(size>=8); const auto block=range(bmd,at,size);
    if(!memcmp(block.data(),"SHP1",4)) { assert(shapes.empty()); shapes=block; }
    if(!memcmp(block.data(),"VTX1",4)) { assert(vertex.empty()); vertex=block; }
    at+=size;
  }
  assert(!shapes.empty()); range(shapes,0,44);
  const size_t count=half(shapes,8), init=word(shapes,12), remap=word(shapes,16),
    descriptors=word(shapes,24), lists=word(shapes,32), draws=word(shapes,40);
  using namespace dusk::native::gx;
  assert(vertex.size()>=64);std::vector<GXVtxAttrFmtList> formats;
  size_t fmtAt=word(vertex,8);bool fmtEnd=false;
  for(unsigned i=0;i<32;++i) {
    const auto f=range(vertex,fmtAt+16*i,16);const auto attr=word(f,0);
    if(attr==GX_VA_NULL){fmtEnd=true;break;}
    assert(attr>=GX_VA_POS && attr<=GX_VA_TEX7 && word(f,4)<=2 && word(f,8)<=5);
    formats.push_back({static_cast<GXAttr>(attr),static_cast<GXCompCnt>(word(f,4)),static_cast<GXCompType>(word(f,8)),f[12]});
  }
  assert(fmtEnd);
  VertexArrays arrays{};
  const unsigned attributes[]{GX_VA_POS,GX_VA_NRM,GX_VA_NBT,GX_VA_CLR0,GX_VA_CLR1,
    GX_VA_TEX0,GX_VA_TEX1,GX_VA_TEX2,GX_VA_TEX3,GX_VA_TEX4,GX_VA_TEX5,GX_VA_TEX6,GX_VA_TEX7};
  for(unsigned i=0;i<13;++i) {
    const size_t offset=word(vertex,12+4*i);if(!offset)continue;
    assert(attributes[i]!=GX_VA_NBT); // This measured title has no separate NBT array.
    const GXVtxAttrFmtList* fmt=nullptr;for(const auto& f:formats)if(f.attr==attributes[i])fmt=&f;assert(fmt);
    size_t end=vertex.size();for(unsigned j=i+1;j<13;++j){const auto n=word(vertex,12+4*j);if(n){end=n;break;}}
    assert(end>=offset);const auto bytes=range(vertex,offset,end-offset);
    unsigned stride=0;
    if(fmt->attr==GX_VA_CLR0||fmt->attr==GX_VA_CLR1)
      stride=fmt->type==GX_RGB565||fmt->type==GX_RGBA4?2:fmt->type==GX_RGB8||fmt->type==GX_RGBA6?3:4;
    else {
      const unsigned count=fmt->attr==GX_VA_POS?(fmt->cnt==GX_POS_XY?2:3):fmt->attr==GX_VA_NRM?(fmt->cnt==GX_NRM_XYZ?3:9):(fmt->cnt==GX_TEX_S?1:2);
      const unsigned size=fmt->type==GX_F32?4:fmt->type==GX_U16||fmt->type==GX_S16?2:1;
      stride=count*size;
    }
    arrays[attributes[i]]={bytes,stride,false};
  }
  struct Check {Span record;size_t decoded=0;u32 seen=0;} check;
  CommandMemory memory;MemoryHandle modelHandle;assert(memory.add(bmd,modelHandle));
  CommandProcessor processor(memory);
  u8 scratch[256];CommandStream stream(processor,scratch);assert(setCommandStream(&stream));
  processor.setSink([](const CommandDraw& draw,void* pointer){
    auto& check=*static_cast<Check*>(pointer);
    for(u32 v=0;v<draw.vertexCount;++v){DecodedVertex value;if(!draw.vertex(v,value))return false;
      for(unsigned axis=0;axis<3;++axis){const float minimum=std::bit_cast<float>(word(check.record,16+axis*4));const float maximum=std::bit_cast<float>(word(check.record,28+axis*4));
        if(!std::isfinite(value.position[axis])||value.position[axis]<minimum-0.001f||value.position[axis]>maximum+0.001f)return false;}
      ++check.decoded;check.seen|=value.present;
    }
    return true;
  },&check);
  formats.push_back({GX_VA_NULL,GX_POS_XY,GX_U8,0});


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
      check.record=record;
      GXClearVtxDesc();
      for(const auto* entry=desc;entry->attr!=GX_VA_NULL;++entry)GXSetVtxDesc(entry->attr,entry->type);
      for(const auto fmt:formats)if(fmt.attr!=GX_VA_NULL)GXSetVtxAttrFmt(GX_VTXFMT0,fmt.attr,fmt.cnt,fmt.type,fmt.frac);
      for(unsigned attr=GX_VA_POS;attr<=GX_VA_TEX7;++attr)if(!arrays[attr].bytes.empty())
        GXSetArray(static_cast<GXAttr>(attr),arrays[attr].bytes.data(),arrays[attr].bytes.size(),arrays[attr].stride,false);
      assert(stream.finish());
      GXCallDisplayList(input.data(),input.size());GXCallDisplayList(output->data(),output->size());
    }
  }
  printf("{\"decoded_vertices_both_streams\":%zu,\"present_attribute_mask\":%u,\"positions_within_original_shape_bounds\":true,\"commands\":%llu,\"draws\":%llu,\"prepared_plans\":%llu,\"registered_model_regions\":1}\n",check.decoded,check.seen,static_cast<unsigned long long>(processor.statistics().commands),static_cast<unsigned long long>(processor.statistics().draws),static_cast<unsigned long long>(processor.statistics().plans));
  assert(stream.finish()&&setCommandStream(nullptr));
  assert(memory.remove(modelHandle));
  printf("{\"resource\":\"Title.arc/titlelogo_r.bmd\",\"shape_count\":%zu,\"matrix_groups\":%zu,"
         "\"draws_before\":%zu,\"draws_after\":%zu,\"vertices_before\":%zu,\"vertices_after\":%zu,"
         "\"triangles\":%zu,\"prebuilt_indices\":%zu,\"bytes_before\":%zu,\"bytes_after\":%zu,"
         "\"exact_expanded_vertex_stream_match\":true,\"payload_saved\":false}\n",
         count,groups,original.draws,optimized.draws,original.vertices,optimized.vertices,
         original.triangles,optimized.indices,oldBytes,newBytes);
}
