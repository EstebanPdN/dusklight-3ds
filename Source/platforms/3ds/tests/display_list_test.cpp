#include <aurora/dl.hpp>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXCommandList.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <limits>
using namespace aurora::gx::dl;
using Bytes = std::vector<u8>;
static const GXVtxDescList desc[] = {{GX_VA_POS, GX_INDEX16}, {GX_VA_NULL, GX_NONE}};
static void be16(Bytes& b, u16 v) { b.push_back(v >> 8); b.push_back(v); }
static void be32(Bytes& b, u32 v) { be16(b, v >> 16); be16(b, v); }
static void draw(Bytes& b, u8 prim, u16 count, u16 base = 0) {
  b.push_back(prim); be16(b, count);
  for (u32 i = 0; i < count; ++i) be16(b, base + i);
}
static std::vector<u16> triangles(const Bytes& b) {
  Reader reader(b.data(), b.size(), desc);
  std::vector<u16> result;
  while (auto c = reader.next()) {
    if (c->kind == Command::Kind::Passthrough) continue;
    const auto& d = c->draw;
    if (c->kind == Command::Kind::DrawIndexed) {
      assert(d.prim == GX_TRIANGLES && d.indexCount % 3 == 0);
      for (u32 i = 0; i < d.indexCount; ++i) result.push_back(d.attr_idx(d.index(i), GX_VA_POS));
    } else {
      assert(expand_triangles(d.prim, d.vtxCount, [&](u16 a, u16 v, u16 c) {
        result.push_back(d.attr_idx(a, GX_VA_POS)); result.push_back(d.attr_idx(v, GX_VA_POS));
        result.push_back(d.attr_idx(c, GX_VA_POS));
      }));
    }
  }
  assert(!reader.failed()); return result;
}
static void rejected(const Bytes& b) {
  Reader reader(b.data(), b.size(), desc);
  while (reader.next()) {}
  assert(reader.failed()); assert(!reader.next());
  assert(!optimize(b.data(), b.size(), desc));
}
int main() {
  Bytes input;
  draw(input, GX_TRIANGLESTRIP, 5, 10); draw(input, GX_TRIANGLEFAN, 4, 20);
  draw(input, GX_QUADS, 4, 30); draw(input, GX_TRIANGLES, 3, 40);
  auto out = optimize(input.data(), input.size(), desc); assert(out);
  const std::vector<u16> expected{10,11,12,12,11,13,12,13,14,20,21,22,20,22,23,
                                30,31,32,32,33,30,40,41,42};
  assert(triangles(input) == expected && triangles(*out) == expected);
  Reader merged(out->data(), out->size(), desc); auto c = merged.next();
  assert(c && c->kind == Command::Kind::DrawIndexed && c->draw.vtxCount == 16 && c->draw.indexCount == 24);
  assert(!merged.next() && !merged.failed());
  assert(optimize(out->data(), out->size(), desc) == out);
  // Lazy indices must also preserve triangles preceding the first strip.
  Bytes lazy; draw(lazy, GX_TRIANGLES, 6, 50); draw(lazy, GX_TRIANGLESTRIP, 4, 60);
  draw(lazy, GX_TRIANGLES, 3, 70);
  auto lazyOut = optimize(lazy.data(), lazy.size(), desc); assert(lazyOut);
  assert(triangles(lazy) == triangles(*lazyOut));
  // A NOP before an optimized command makes its host-endian indices unaligned.
  Bytes unaligned{GX_NOP}; unaligned.insert(unaligned.end(), out->begin(), out->end());
  assert(triangles(unaligned) == expected);
  for (size_t n = 1; n < out->size(); ++n) rejected(Bytes(out->begin(), out->begin() + n));
  Bytes bad = *out; bad[10] = 0xff; bad[11] = 0xff; rejected(bad);
  bad = *out; bad[3] = GX_LOAD_CP_REG; rejected(bad);
  bad = *out; for (int i = 6; i < 10; ++i) bad[i] = 0xff; rejected(bad);
  bad = {GX_AURORA}; be16(bad, GX_AURORA_DRAW_INDEXED); bad.push_back(GX_TRIANGLES);
  be16(bad, 0); be32(bad, 0x80000000u); rejected(bad); // 32-bit byte-count wrap.
  // State commands are ordering barriers, copied byte for byte.
  Bytes state; draw(state, GX_TRIANGLES, 3); state.insert(state.end(), {GX_LOAD_BP_REG,1,2,3,4});
  draw(state, GX_TRIANGLES, 3, 3); state.push_back(GX_NOP); draw(state, GX_TRIANGLES, 3, 6);
  auto barrier = optimize(state.data(), state.size(), desc); assert(barrier);
  Reader br(barrier->data(), barrier->size(), desc);
  assert(br.next()->draw.vtxCount == 3); c = br.next();
  const u8 bp[]{GX_LOAD_BP_REG,1,2,3,4}; assert(c && c->size == sizeof(bp) && !memcmp(c->data,bp,sizeof(bp)));
  assert(br.next()->draw.vtxCount == 6 && !br.next() && !br.failed());
  assert(triangles(state) == triangles(*barrier));
  // Different VATs and the 16-bit vertex limit split batches.
  Bytes formats; draw(formats, GX_TRIANGLES, 3); draw(formats, GX_TRIANGLES | 1, 3);
  auto split = optimize(formats.data(), formats.size(), desc); assert(split);
  Reader fr(split->data(), split->size(), desc);
  assert(fr.next()->draw.fmt == GX_VTXFMT0); assert(fr.next()->draw.fmt == GX_VTXFMT1); assert(!fr.next());
  Bytes large; draw(large, GX_TRIANGLES, 65535); draw(large, GX_TRIANGLES, 3);
  auto limits = optimize(large.data(), large.size(), desc); assert(limits);
  Reader lr(limits->data(), limits->size(), desc);
  assert(lr.next()->draw.vtxCount == 65535); assert(lr.next()->draw.vtxCount == 3); assert(!lr.next());
  assert(triangles(large) == triangles(*limits));
  // Non-triangulable commands remain exact; invalid expansion emits no partial output.
  for (auto prim : {GX_LINES, GX_LINESTRIP, GX_POINTS, GX_QUADS, GX_TRIANGLES}) {
    Bytes b; draw(b, prim, 2); auto r = optimize(b.data(), b.size(), desc); assert(r && *r == b);
  }
  int emitted = 0; assert(!expand_triangles(GX_QUADS, 5, [&](u16,u16,u16){++emitted;})); assert(!emitted);
  Reader nil(nullptr, 10, desc); assert(nil.failed() && !nil.next());
  assert(!optimize(nullptr, 10, desc)); assert(optimize(nullptr, 0, desc)->empty());
  Reader zero(input.data(), input.size(), u8(0)); assert(zero.failed() && !zero.next());
  // Ordinary big-endian attributes, direct scalar sizes and NBT3 index triplets.
  const GXVtxDescList detailed[]{{GX_VA_PNMTXIDX,GX_DIRECT},{GX_VA_POS,GX_DIRECT},
    {GX_VA_NRM,GX_INDEX16},{GX_VA_CLR0,GX_DIRECT},{GX_VA_TEX0,GX_DIRECT},{GX_VA_NULL,GX_NONE}};
  GXVtxAttrFmtList fmts[]{{GX_VA_POS,GX_POS_XYZ,GX_F32,0},{GX_VA_NRM,GX_NRM_NBT3,GX_S16,0},
    {GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0},{GX_VA_TEX0,GX_TEX_ST,GX_S16,8},{GX_VA_NULL,GX_POS_XY,GX_U8,0}};
  VtxFmtLists lists{}; lists[0] = fmts;
  Bytes direct{GX_TRIANGLES,0,3}; direct.resize(3 + 3 * 27); direct[3] = 6;
  Reader dr(direct.data(), direct.size(), detailed, &lists); c = dr.next();
  assert(c && c->draw.layout->stride == 27 && c->draw.attr_idx(0,GX_VA_PNMTXIDX) == 6);
  assert(c->draw.layout->attrs[GX_VA_NRM].size == 6);
  fmts[0].cnt = GX_NRM_NBT3; Reader invalid(direct.data(),direct.size(),detailed,&lists);
  assert(!invalid.next() && invalid.failed());
  // Every prefix of state commands must either complete the command or fail.
  for (const Bytes& b : {Bytes{GX_LOAD_CP_REG,0,0,0,0,0}, Bytes{GX_LOAD_XF_REG,0,0,0,0,0,0,0,0},
       Bytes{GX_CMD_CALL_DL,0,0,0,0,0,0,0,0}, Bytes{GX_LOAD_INDX_A,0,0,0,0}}) {
    for (size_t n = 1; n < b.size(); ++n) rejected(Bytes(b.begin(),b.begin()+n));
    Reader r(b.data(), b.size(), desc); c=r.next(); assert(c && c->size==b.size() && !r.next() && !r.failed());
  }
  puts("PASS: original Aurora DL batching, exact topology/order, formats, 65535 boundary, truncation and invalid indices");
}
