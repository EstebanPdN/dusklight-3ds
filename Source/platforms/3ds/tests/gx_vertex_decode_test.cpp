#include "../runtime/gx_vertex_decode.hpp"
#include "../runtime/gx_rigid_vertex.hpp"
#include "gx_rigid_reference.hpp"
#include "../runtime/gx_compact_cache.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace dusk::native::gx;
using aurora::gx::dl::VtxLayout;
using Bytes=std::vector<u8>;
static void near(float a,float b){assert(std::fabs(a-b)<0.000001f);}
static VertexDecodePlan plan(GXAttrType descriptor,GXCompType type,GXCompCnt cnt,u8 fraction,unsigned size) {
    VtxLayout lo{};lo.attrs[GX_VA_POS]={0,static_cast<u8>(size),descriptor};lo.stride=size;
    GXVtxAttrFmtList fmt{GX_VA_POS,cnt,type,fraction};VertexDecodePlan p;
    assert(planVertexDecode(lo,{&fmt,1},p));return p;
}
static void rigidBatches(){
    unsigned checked=0,compactChecked=0,positionsChecked=0;
    unsigned gatherBoundaries=0;
    std::vector<u8> cacheStorage(128*1024);CompactVertexCache compactCache;assert(compactCache.attach(cacheStorage));
    CommandRegisters registers;registers.matrixIndexValid.set(0);
    auto xf=[&](unsigned index,float value){registers.xf[index]=std::bit_cast<u32>(value);registers.xfValid.set(index);};
    const float projection[]{1,.2f,2,.3f,-.5f,1};
    const float viewport[]{304,-224,8388607,644,564,16777215};
    for(unsigned i=0;i<6;++i){xf(0x1020+i,projection[i]);xf(0x101a+i,viewport[i]);}
    registers.xf[0x1026]=1;registers.xfValid.set(0x1026);
    for(unsigned m=0;m<10;++m)for(unsigned row=0;row<3;++row)for(unsigned c=0;c<4;++c)
        xf(12*m+4*row+c,(row==c?1.f:0.f)+.03f*(m+1)+.05f*(c+1));
    VertexTransformPlan transform;assert(planVertexTransform(registers,0,transform));
    for(unsigned matrixSelectors=0;matrixSelectors<3;++matrixSelectors)
    for(unsigned posType=0;posType<3;++posType)for(unsigned normalType=0;normalType<3;++normalType)
    for(unsigned uvType=0;uvType<3;++uvType)
    for(unsigned wide=0;wide<2;++wide)for(unsigned withColor=0;withColor<2;++withColor)
    for(unsigned sources=0;sources<4;++sources)for(unsigned alphaLit=0;alphaLit<2;++alphaLit){
        if(!withColor&&sources)continue;
        const unsigned count=wide?311:97,width=wide?2:1;
        VtxLayout layout{};std::vector<GXVtxAttrFmtList> formats;unsigned cursor=0;
        if(matrixSelectors){layout.attrs[GX_VA_PNMTXIDX]={u8(cursor++),1,GX_DIRECT};
            if(matrixSelectors==2)layout.attrs[GX_VA_TEX1MTXIDX]={u8(cursor++),1,GX_DIRECT};}
        const GXAttr attrs[]{GX_VA_POS,GX_VA_NRM,GX_VA_CLR0,GX_VA_TEX0};
        const unsigned components[]{3,3,1,2};
        const GXCompType types[]{GX_F32,GX_S16,GX_S8};
        const GXCompType type[]{types[posType],types[normalType],GX_RGBA8,types[uvType]};
        unsigned scalarSize[4]{},strides[4]{};
        for(unsigned i=0;i<4;++i){scalarSize[i]=type[i]==GX_F32?4:type[i]==GX_S16?2:type[i]==GX_RGBA8?4:1;strides[i]=components[i]*scalarSize[i]+1;}
        std::array<Bytes,4> storage;VertexArrays arrays;
        for(unsigned slot=0;slot<4;++slot){
            if(slot==2&&!withColor)continue;
            const auto attr=attrs[slot];layout.attrs[attr]={u8(cursor),u8(width),wide?GX_INDEX16:GX_INDEX8};cursor+=width;
            formats.push_back({attr,slot==0?GX_POS_XYZ:slot==1?GX_NRM_XYZ:slot==2?GX_CLR_RGBA:GX_TEX_ST,type[slot],u8(slot==0?4:slot==3?14:0)});
            storage[slot].resize(1+count*strides[slot]);
            for(unsigned index=0;index<count;++index){auto* dest=storage[slot].data()+1+index*strides[slot];
                if(slot==2)for(unsigned c=0;c<4;++c)dest[c]=u8(index*31+c*53);
                else for(unsigned c=0;c<components[slot];++c){
                    if(type[slot]==GX_F32){const float value=float(int(index%71)-35)*.07f+.3f*c;std::memcpy(dest+4*c,&value,4);}
                    else if(type[slot]==GX_S16){const std::int16_t value=std::int16_t(index*193+c*8193);std::memcpy(dest+2*c,&value,2);}
                    else {const std::int8_t value=std::int8_t(index*13+c*65);std::memcpy(dest+c,&value,1);}}}
            arrays[attr]={{storage[slot].data()+1,count*strides[slot]},strides[slot],true};
        }
        layout.stride=cursor;VertexDecodePlan plan;assert(planVertexDecode(layout,formats,plan));
        Bytes packed(513*cursor);
        for(unsigned n=0;n<513;++n){unsigned at=n*cursor;
            if(matrixSelectors){const unsigned indices[]{0,3,6,27};packed[at++]=indices[n%4];if(matrixSelectors==2)packed[at++]=u8(n%10*3);}
            for(unsigned slot=0;slot<4;++slot){if(slot==2&&!withColor)continue;
            const unsigned index=(n*(17+slot*6)+slot*19)%count;if(wide)packed[at++]=index>>8;packed[at++]=u8(index);}}
        RigidVertexPlan rigid;rigid.color.material={.2f,.4f,.6f,.8f};rigid.color.vertex=sources;
        rigid.lights=5;rigid.color.channels[0].mask=31;rigid.indexedMatrices=matrixSelectors!=0;rigid.matrixOffsets.fill(-1);
        rigid.matrixOffsets[0]=0;rigid.matrixOffsets[1]=39;rigid.matrixOffsets[2]=45;rigid.matrixOffsets[9]=87;
        const auto matrixMap=matrixSelectors?std::span<const std::int8_t>(rigid.matrixOffsets):std::span<const std::int8_t>{};
        rigid.color.lit=alphaLit?3:1;rigid.color.ambient[3]=.37f;
        const std::array<float,2> scale{.7f,.3f};std::vector<GpuVertex> output(513);
        assert(decodeRigidBatch(plan,packed,arrays,output,rigid.color,scale,matrixMap));
        for(unsigned n=0;n<513;++n){DecodedVertex decoded;GpuVertex expected;
            assert(decodeVertex(plan,std::span(packed).subspan(n*cursor,cursor),arrays,decoded));
            assert(packRigidVertex(rigid,decoded,scale,expected));
            assert(output[n].position==expected.position&&output[n].color==expected.color&&output[n].texcoord==expected.texcoord&&output[n].textureQ==expected.textureQ);++checked;}
        std::vector<GpuVertex> positionOutput(513);
        assert(decodePositionTransformBatch(plan,packed,arrays,positionOutput,registers,transform.clipProjection()));
        for(unsigned n=0;n<513;++n){
            DecodedVertex decoded;TransformedVertex transformed;
            assert(decodeVertex(plan,std::span(packed).subspan(n*cursor,cursor),arrays,decoded));
            assert(transformVertex(transform,decoded,transformed,nullptr,false));
            assert(positionOutput[n].position==transformed.clip);
            assert((positionOutput[n].color==std::array<float,4>{})&&positionOutput[n].textureQ==1);
            ++positionsChecked;
        }
        auto invalidPositionArrays=arrays;invalidPositionArrays[GX_VA_NRM].bytes=invalidPositionArrays[GX_VA_NRM].bytes.first(2);
        assert(!decodePositionTransformBatch(plan,packed,invalidPositionArrays,positionOutput,registers,transform.clipProjection()));
        assert(!decodePositionTransformBatch(plan,std::span(packed).first(packed.size()-1),arrays,positionOutput,registers,transform.clipProjection()));
        if(matrixSelectors){
            auto badPacked=packed;badPacked[0]=1;
            assert(!decodePositionTransformBatch(plan,badPacked,arrays,positionOutput,registers,transform.clipProjection()));
        }
        std::vector<GpuVertex> compact(513);
        if(decodeCompactRigidBatch(plan,packed,arrays,compact,scale,rigid)){
            assert(rigid.compact);compactChecked+=513;
            if(!matrixSelectors&&!sources&&!alphaLit&&posType==0&&!wide&&!withColor){
                // Check the integer magnitude guard through the public decoder:
                // both signs, boundary neighbors, subnormals and NaN payloads,
                // at every source alignment used by a native position record.
                const std::uint32_t bits[]{0,1,0x007fffff,0x00800000,
                    std::bit_cast<std::uint32_t>(1e7f)-1,
                    std::bit_cast<std::uint32_t>(1e7f),
                    std::bit_cast<std::uint32_t>(1e7f)+1,
                    0x7f7fffff,0x7f800000,0x7f800001,0x7fc00000,0x7fffffff};
                for(unsigned alignment=0;alignment<4;++alignment)
                for(auto magnitude:bits)for(auto sign:{0u,0x80000000u}){
                    auto inputs=arrays;Bytes position(12+alignment,0);
                    const auto raw=magnitude|sign;
                    std::memcpy(position.data()+alignment,&raw,4);
                    inputs[GX_VA_POS]={{position.data()+alignment,12},0,true};
                    std::vector<GpuVertex> actual(1),expected(1);
                    const auto one=std::span(packed).first(cursor);
                    const bool reference=decodeRigidBatch(plan,one,inputs,expected,rigid.color,scale);
                    const bool accepted=decodeCompactRigidBatch(plan,one,inputs,actual,scale,rigid);
                    assert(accepted==reference);
                    if(accepted){
                        const auto expanded=expandCompactRigid(rigid,std::as_bytes(std::span(actual)),0);
                        assert(expanded.position==expected[0].position);
                    }
                }
                assert(decodeCompactRigidBatch(plan,packed,arrays,compact,scale,rigid));
            }
            // Array limits must be recomputed from each live span. Exercise
            // zero stride, the full index domain, a huge stride at index zero,
            // and an element whose last byte is exactly the end of the array.
            if(!matrixSelectors&&!sources&&!alphaLit)for(unsigned slot=0;slot<4;++slot){
                if(slot==2&&!withColor)continue;
                auto edgeArrays=arrays;
                auto one=Bytes(packed.begin(),packed.begin()+cursor);
                const unsigned at=layout.attrs[attrs[slot]].offset;
                auto setIndex=[&](unsigned index){if(wide)one[at]=index>>8;one[at+width-1]=u8(index);};
                auto& array=edgeArrays[attrs[slot]];
                const unsigned bytes=components[slot]*scalarSize[slot];
                array.bytes=array.bytes.first(bytes);array.stride=0;setIndex(wide?65535:255);
                std::vector<GpuVertex> edge(1),reference(1);
                assert(decodeCompactRigidBatch(plan,one,edgeArrays,edge,scale,rigid));
                assert(decodeRigidBatch(plan,one,edgeArrays,reference,rigid.color,scale));
                const auto expanded=expandCompactRigid(rigid,std::as_bytes(std::span(edge)),0);
                assert(expanded.position==reference[0].position&&expanded.texcoord==reference[0].texcoord);
                DecodedVertex decoded;TransformedVertex transformed;
                assert(decodeVertex(plan,one,edgeArrays,decoded));
                assert(transformVertex(transform,decoded,transformed));
                assert(decodePositionTransformBatch(plan,one,edgeArrays,reference,registers,transform.clipProjection()));
                assert(reference[0].position==transformed.clip);
                array.stride=0xffffffffu;setIndex(0);
                assert(decodeCompactRigidBatch(plan,one,edgeArrays,edge,scale,rigid));
                assert(decodeRigidBatch(plan,one,edgeArrays,reference,rigid.color,scale));
                assert(decodePositionTransformBatch(plan,one,edgeArrays,reference,registers,transform.clipProjection()));
                setIndex(1);assert(!decodeCompactRigidBatch(plan,one,edgeArrays,edge,scale,rigid));
                assert(!decodeRigidBatch(plan,one,edgeArrays,reference,rigid.color,scale));
                assert(!decodePositionTransformBatch(plan,one,edgeArrays,reference,registers,transform.clipProjection()));
                array.stride=0;setIndex(0);array.bytes=array.bytes.first(bytes-1);
                assert(!decodeCompactRigidBatch(plan,one,edgeArrays,edge,scale,rigid));
                assert(!decodeRigidBatch(plan,one,edgeArrays,reference,rigid.color,scale));
                assert(!decodePositionTransformBatch(plan,one,edgeArrays,reference,registers,transform.clipProjection()));
                ++gatherBoundaries;
            }
            compactCache.clear();
            assert(decodeCompactRigidBatch(plan,packed,arrays,compact,scale,rigid,&compactCache));
            assert(compactCache.misses==1&&compactCache.hits==0);
            const auto beforeCache=compact;
            assert(decodeCompactRigidBatch(plan,packed,arrays,compact,scale,rigid,&compactCache));
            assert(compactCache.hits==1&&!std::memcmp(compact.data(),beforeCache.data(),513*sizeof(CompactRigidVertex)));
            // Same pointer with changed live position bytes must not hit.
            storage[0][1]^=1;
            assert(decodeCompactRigidBatch(plan,packed,arrays,compact,scale,rigid,&compactCache));
            assert(compactCache.misses==2);
            std::vector<GpuVertex> changed(513);
            assert(decodeCompactRigidBatch(plan,packed,arrays,changed,scale,rigid));
            assert(!std::memcmp(compact.data(),changed.data(),513*sizeof(CompactRigidVertex)));
            storage[0][1]^=1;
            assert(decodeCompactRigidBatch(plan,packed,arrays,compact,scale,rigid,&compactCache));
            assert(!std::memcmp(compact.data(),beforeCache.data(),513*sizeof(CompactRigidVertex)));
            const auto priorHits=compactCache.hits;
            const auto material=rigid.color.material;rigid.color.material[0]=.75f;
            const std::array<float,2> newScale{.5f,.75f};
            assert(decodeCompactRigidBatch(plan,packed,arrays,compact,newScale,rigid,&compactCache));
            assert(compactCache.hits==priorHits+1&&rigid.uniforms[93][0]==.75f);
            assert(rigid.uniforms[13][2]==std::ldexp(.5f,-14));
            rigid.color.material=material;
            assert(decodeCompactRigidBatch(plan,packed,arrays,compact,scale,rigid,&compactCache));
            for(unsigned n=0;n<513;++n){
                const auto actual=expandCompactRigid(rigid,std::as_bytes(std::span(compact)),n);
                assert(actual.position==output[n].position&&actual.texcoord==output[n].texcoord);
                for(unsigned c=0;c<4;++c)near(actual.color[c],output[n].color[c]);
            }
            assert(!decodeCompactRigidBatch(plan,std::span(packed).first(packed.size()-1),arrays,compact,scale,rigid));
            assert(!rigid.compact&&rigid.uniforms[12][3]==0);
        }
        if(matrixSelectors){
            assert(!decodeRigidBatch(plan,packed,arrays,output,rigid.color,scale));
            for(int offset:{-2,1,38,40,89,94}){
                auto badMap=rigid.matrixOffsets;badMap[1]=offset;
                assert(!decodeRigidBatch(plan,packed,arrays,output,rigid.color,scale,badMap));
            }
            auto invalidMatrix=packed;invalidMatrix[0]=1;assert(!decodeRigidBatch(plan,invalidMatrix,arrays,output,rigid.color,scale,matrixMap));
        }
        auto broken=arrays;broken[GX_VA_NRM].bytes=broken[GX_VA_NRM].bytes.first(2);assert(!decodeRigidBatch(plan,packed,broken,output,rigid.color,scale,matrixMap));
        broken=arrays;broken[GX_VA_POS].stride=0xffffffffu;assert(!decodeRigidBatch(plan,packed,broken,output,rigid.color,scale,matrixMap));
        broken=arrays;broken[GX_VA_TEX0].littleEndian=false;assert(!decodeRigidBatch(plan,packed,broken,output,rigid.color,scale,matrixMap));
        assert(!decodeRigidBatch(plan,std::span(packed).first(packed.size()-1),arrays,output,rigid.color,scale,matrixMap));
        auto invalid=packed;for(unsigned b=0;b<width;++b)invalid[b]=255;assert(!decodeRigidBatch(plan,invalid,arrays,output,rigid.color,scale,matrixMap));
        if(type[0]==GX_F32){const unsigned nanBits=0x7fc00000;std::memcpy(storage[0].data()+1,&nanBits,4);
            assert(!decodeRigidBatch(plan,packed,arrays,output,rigid.color,scale,matrixMap));}
    }
    assert(positionsChecked==831060);
    std::printf("PASS %u position-only vertices exactly match generic decode/transform\n",positionsChecked);
    assert(gatherBoundaries==28);
    std::printf("PASS %u gather limit cases across all three decoders: zero/huge stride, full index range, exact and short live spans\n",gatherBoundaries);
    assert(compactChecked==61560);
    std::printf("PASS %u compact vertices match generic decode/pack (color tolerance 1e-6)\n",compactChecked);
    std::printf("PASS %u directly packed rigid vertices exactly match generic decode/pack; unaligned native F32/S16/S8 with independent fractions, mixed indices, color/alpha sources and bounded refusal\n",checked);
}
static void mixedDirectRigid(){
    unsigned checked=0;
    const GXAttr attrs[]{GX_VA_POS,GX_VA_NRM,GX_VA_CLR0,GX_VA_TEX0};
    for(unsigned mask=0;mask<16;++mask)for(unsigned numeric=0;numeric<3;++numeric)
    for(unsigned sources=0;sources<4;++sources)for(unsigned ambient=0;ambient<2;++ambient){
        const GXCompType types[]{GX_F32,GX_S16,GX_S8};
        VtxLayout layout{};std::vector<GXVtxAttrFmtList> formats;
        std::array<Bytes,4> storage;VertexArrays arrays;unsigned cursor=0;
        for(unsigned slot=0;slot<4;++slot){
            const unsigned scalar=slot==2?4:numeric==0?4:numeric==1?2:1;
            const unsigned components=slot==2?1:slot==3?2:3;
            const bool direct=mask&(1u<<slot);
            layout.attrs[attrs[slot]]={u8(cursor),u8(direct?scalar*components:1),direct?GX_DIRECT:GX_INDEX8};
            cursor+=direct?scalar*components:1;
            formats.push_back({attrs[slot],slot==0?GX_POS_XYZ:slot==1?GX_NRM_XYZ:slot==2?GX_CLR_RGBA:GX_TEX_ST,slot==2?GX_RGBA8:types[numeric],u8(slot==3?7:slot==0?3:0)});
            for(unsigned i=0;i<17;++i)for(unsigned c=0;c<components;++c){
                if(slot==2){for(unsigned k=0;k<4;++k)storage[slot].push_back(u8(i*17+k*71));}
                else if(numeric==0){float value=.13f*(int(i)-8)+.31f*c;const auto* b=reinterpret_cast<const u8*>(&value);storage[slot].insert(storage[slot].end(),b,b+4);}
                else if(numeric==1){std::int16_t value=std::int16_t(i*1903+c*4701-23000);const auto* b=reinterpret_cast<const u8*>(&value);storage[slot].insert(storage[slot].end(),b,b+2);}
                else storage[slot].push_back(u8(i*13+c*61));
            }
            arrays[attrs[slot]]={storage[slot],scalar*components,true};
        }
        layout.stride=cursor;VertexDecodePlan plan;assert(planVertexDecode(layout,formats,plan));Bytes packed;
        for(unsigned n=0;n<129;++n)for(unsigned slot=0;slot<4;++slot){
            const unsigned index=(n*(slot+3)+slot)%17;
            const unsigned scalar=slot==2?4:numeric==0?4:numeric==1?2:1;
            const unsigned components=slot==2?1:slot==3?2:3;
            if(!(mask&(1u<<slot))){packed.push_back(u8(index));continue;}
            const unsigned size=scalar*components;
            auto record=Bytes(storage[slot].begin()+index*size,storage[slot].begin()+(index+1)*size);
            if(slot!=2&&std::endian::native==std::endian::little)
                for(unsigned c=0;c<components;++c)std::reverse(record.begin()+c*scalar,record.begin()+(c+1)*scalar);
            packed.insert(packed.end(),record.begin(),record.end());
        }
        RigidVertexPlan rigid;rigid.color.material={.2f,.4f,.6f,.8f};rigid.color.vertex=sources;
        rigid.color.ambientVertex=ambient;rigid.color.ambient[3]=.37f;rigid.color.lit=3;
        std::vector<GpuVertex> result(129);
        assert(decodeRigidBatch(plan,packed,arrays,result,rigid.color,{.75f,.5f}));
        for(unsigned n=0;n<129;++n){
            DecodedVertex original;GpuVertex expected;
            assert(decodeVertex(plan,std::span(packed).subspan(n*cursor,cursor),arrays,original));
            assert(packRigidVertex(rigid,original,{.75f,.5f},expected));
            assert(result[n].position==expected.position&&result[n].color==expected.color&&result[n].texcoord==expected.texcoord);
            ++checked;
        }
        assert(!decodeRigidBatch(plan,std::span(packed).first(packed.size()-1),arrays,result,rigid.color,{1,1}));
        if(!(mask&2)){auto broken=arrays;broken[GX_VA_NRM].bytes={};assert(!decodeRigidBatch(plan,packed,broken,result,rigid.color,{1,1}));}
        else if(!numeric){auto bad=packed;const auto offset=layout.attrs[GX_VA_NRM].offset;bad[offset]=0x7f;bad[offset+1]=0x80;bad[offset+2]=bad[offset+3]=0;assert(!decodeRigidBatch(plan,bad,arrays,result,rigid.color,{1,1}));}
    }
    std::printf("PASS %u mixed direct/indexed rigid vertices exactly match generic decode/pack, BE F32/S16/S8, ambient/material color sources and bounds\n",checked);
}
static void cacheProtocol(){
    std::array<u8,256> memory{};CompactVertexCache cache;assert(cache.attach(memory));
    std::array<u8,4> packed{1,2,3,4};std::array<u8,32> source{};
    for(unsigned i=0;i<source.size();++i)source[i]=u8(i*13);
    std::array<std::byte,64> encoded{},target{};encoded.fill(std::byte{0x57});
    CompactVertexCache::Arrays arrays{};arrays[0]=source;
    CompactVertexCache::Ranges ranges{};ranges[0]={3,12};
    CompactVertexCache::Key key{};
    assert(cache.store(key,packed,arrays,ranges,encoded));
    assert(cache.find(key,packed,arrays,target)&&target==encoded);
    packed[0]^=1;assert(!cache.find(key,packed,arrays,target));packed[0]^=1;
    source[7]^=1;assert(!cache.find(key,packed,arrays,target));source[7]^=1;
    auto shortArray=arrays;shortArray[0]=shortArray[0].first(14);assert(!cache.find(key,packed,shortArray,target));
    auto badRange=ranges;badRange[0].bytes=100;assert(!cache.store(key,packed,arrays,badRange,encoded));
    auto alias=arrays;alias[0]=memory;assert(!cache.find(key,packed,alias,target));
    assert(!cache.store(key,packed,alias,ranges,encoded));
    assert(!cache.find(key,packed,arrays,std::as_writable_bytes(std::span(memory)).first(64)));
    assert(!cache.store(key,packed,arrays,ranges,std::as_bytes(std::span(memory)).first(64)));
    assert(cache.find(key,packed,arrays,target)&&target==encoded);
    // Repeated output allocations wrap the output half of the bounded pool.
    for(unsigned i=1;i<=4;++i){key[0]=i;encoded[0]=std::byte(i);assert(cache.store(key,packed,arrays,ranges,encoded));}
    key[0]=0;assert(!cache.find(key,packed,arrays,target));key[0]=4;
    assert(cache.find(key,packed,arrays,target)&&target==encoded);
    cache.clear();assert(cache.hits==0&&cache.misses==0&&!cache.find(key,packed,arrays,target));
    std::puts("PASS compact cache: exact live input mutation, fresh material/scales, alias/extent refusal, ring eviction and reset");
}
static void byteEqualityProtocol(){
    unsigned cases=0;
    for(unsigned left=0;left<8;++left)for(unsigned right=0;right<8;++right)
    for(unsigned length=0;length<=129;++length){
        Bytes a(length+left),b(length+right);
        for(unsigned i=0;i<length;++i)a[left+i]=b[right+i]=u8(i*71+length);
        assert(equalBytes(a.data()+left,b.data()+right,length));++cases;
        for(unsigned changed=0;changed<length;++changed){
            b[right+changed]^=0x80;assert(!equalBytes(a.data()+left,b.data()+right,length));
            b[right+changed]^=0x80;++cases;
        }
    }
    std::printf("PASS %u bounded byte-equality cases across all alignments, tails and mismatch positions\n",cases);
}
static void sharedSnapshotProtocol(){
    std::array<u8,8192> memory{};CompactVertexCache cache;assert(!cache.attach(memory,memory.size()));assert(cache.attach(memory,2048));
    std::array<u8,512> source{};
    for(unsigned i=0;i<source.size();++i)source[i]=u8(i*17);
    std::array<std::array<u8,4>,8> packets{};
    std::array<std::byte,64> output{},target{};
    CompactVertexCache::Arrays arrays{};arrays[0]=source;
    CompactVertexCache::Ranges ranges{};ranges[0]={16,32};
    CompactVertexCache::Key key{};
    for(unsigned i=0;i<packets.size();++i){
        key[0]=i;packets[i][0]=i;output[0]=std::byte(i);
        assert(cache.store(key,packets[i],arrays,ranges,output));
    }
    assert(cache.arrayStores==1&&cache.arrayBytes==source.size());
    for(unsigned i=0;i<packets.size();++i){key[0]=i;
        assert(cache.find(key,packets[i],arrays,target)&&target[0]==std::byte(i));
    }
    // Several layouts using the same packet deliberately collide in the lookup
    // hint. Alternating layouts must still return their own complete output.
    for(unsigned i=0;i<8;++i){key[0]=100+i;output[0]=std::byte(100+i);
        assert(cache.store(key,packets[0],arrays,ranges,output));
    }
    for(unsigned turn=0;turn<4;++turn)for(unsigned i=0;i<8;++i){key[0]=100+i;
        assert(cache.find(key,packets[0],arrays,target)&&target[0]==std::byte(100+i));
    }
    source[100]^=1;key[0]=100;
    assert(cache.find(key,packets[0],arrays,target)&&target[0]==std::byte(100));
    source[100]^=1;
    // Changed consumed bytes cannot reuse any output sharing those bytes.
    // Pure lookup does not overwrite a shared snapshot or advance its token.
    source[20]^=1;
    for(unsigned i=0;i<packets.size();++i){key[0]=i;assert(!cache.find(key,packets[i],arrays,target));}
    assert(cache.arrayStores==1);
    key[0]=0;output[0]=std::byte(41);assert(cache.store(key,packets[0],arrays,ranges,output));
    assert(cache.arrayStores==2);
    source[20]^=1;assert(!cache.find(key,packets[0],arrays,target));
    output[0]=std::byte(42);assert(cache.store(key,packets[0],arrays,ranges,output));
    assert(cache.find(key,packets[0],arrays,target)&&target==output);
    key[0]=1;assert(!cache.find(key,packets[1],arrays,target));
    // Evict only input snapshots: the original output record remains resident,
    // but its token must not match a newly observed allocation at that address.
    std::array<u8,4096> smallMemory{};CompactVertexCache small;assert(small.attach(smallMemory));
    std::array<std::array<u8,512>,5> distinct{};
    for(unsigned i=0;i<distinct.size();++i){
        distinct[i].fill(u8(i));arrays[0]=distinct[i];key[0]=i;
        assert(small.store(key,packets[i],arrays,ranges,output));
    }
    assert(small.arrayEvictions>0&&small.evictions==0);
    arrays[0]=distinct[0];key[0]=0;assert(!small.find(key,packets[0],arrays,target));
    assert(small.store(key,packets[0],arrays,ranges,output));
    assert(small.find(key,packets[0],arrays,target)&&target==output);
    // A single array larger than its pool is a bounded miss, never a partial
    // snapshot that could falsely accept later bytes from the same address.
    arrays[0]=source;std::array<u8,3000> large{};arrays[0]=large;
    assert(!small.store(key,packets[0],arrays,ranges,output));
    assert(!small.find(key,packets[0],arrays,target));
    small.clear();assert(!small.find(key,packets[0],arrays,target));
    std::puts("PASS shared array snapshots: one copy for eight shapes, live mutation and restoration, input-only eviction, oversized spans and reset");
}
int main() {
    cacheProtocol();
    sharedSnapshotProtocol();
    byteEqualityProtocol();
    mixedDirectRigid();
    rigidBatches();
    VertexArrays arrays{};DecodedVertex out;VertexDecodeError error;
    auto direct=plan(GX_DIRECT,GX_F32,GX_POS_XYZ,0,12);
    const Bytes pos{0x3f,0x80,0,0,0xc0,0,0,0,0x40,0x60,0,0};
    assert(decodeVertex(direct,pos,arrays,out));near(out.position[0],1);near(out.position[1],-2);near(out.position[2],3.5);
    for(size_t n=0;n<pos.size();++n){out.present=99;assert(!decodeVertex(direct,{pos.data(),n},arrays,out,&error));assert(error==VertexDecodeError::packedBounds && out.present==0);}
    Bytes bad=pos;bad[0]=0x7f;bad[1]=0x80;assert(!decodeVertex(direct,bad,arrays,out,&error));assert(error==VertexDecodeError::nonfinite);
    // Index stream is BE regardless of host-endian array flag, including unaligned floats.
    auto indexed=plan(GX_INDEX16,GX_F32,GX_POS_XYZ,0,2);
    Bytes big(13,0xee);big.insert(big.end(),pos.begin(),pos.end());
    arrays[GX_VA_POS]={big,13,false};const u8 index[]{0,1};
    assert(decodeVertex(indexed,index,arrays,out));near(out.position[2],3.5);
    Bytes little=big;for(unsigned i=13;i<25;i+=4){std::swap(little[i],little[i+3]);std::swap(little[i+1],little[i+2]);}
    arrays[GX_VA_POS]={little,13,true};assert(decodeVertex(indexed,index,arrays,out));near(out.position[1],-2);
    arrays[GX_VA_POS].bytes={little.data(),little.size()-1};assert(!decodeVertex(indexed,index,arrays,out,&error));assert(error==VertexDecodeError::arrayBounds);
    arrays[GX_VA_POS]={little,0xffffffffu,true};assert(!decodeVertex(indexed,index,arrays,out));
    // Constant arrays with zero stride are legal, even for a large index.
    arrays[GX_VA_POS]={pos,0,false};const u8 high[]{0xff,0xff};assert(decodeVertex(indexed,high,arrays,out));near(out.position[0],1);
    for(auto type:{GX_U8,GX_S8,GX_U16,GX_S16}) {
        const bool wide=type==GX_U16||type==GX_S16;const unsigned bytes=wide?4:2;
        auto p=plan(GX_DIRECT,type,GX_POS_XY,2,bytes);Bytes packed=wide?Bytes{0xff,0xfc,0,8}:Bytes{0xfc,8};
        assert(decodeVertex(p,packed,arrays,out));near(out.position[0],type==GX_S8||type==GX_S16?-1:wide?16383:63);near(out.position[1],2);near(out.position[2],0);
    }
    // Two colors, both matrix selectors, normal and the last texture coordinate.
    VtxLayout lo{};unsigned offset=0;
    auto add=[&](GXAttr attr,GXAttrType type,unsigned size){lo.attrs[attr]={static_cast<u8>(offset),static_cast<u8>(size),type};offset+=size;};
    add(GX_VA_PNMTXIDX,GX_DIRECT,1);add(GX_VA_TEX7MTXIDX,GX_DIRECT,1);add(GX_VA_POS,GX_INDEX8,1);
    add(GX_VA_NRM,GX_DIRECT,3);add(GX_VA_CLR0,GX_DIRECT,2);add(GX_VA_CLR1,GX_DIRECT,3);add(GX_VA_TEX7,GX_DIRECT,4);lo.stride=offset;
    GXVtxAttrFmtList formats[]{{GX_VA_POS,GX_POS_XYZ,GX_F32,0},{GX_VA_NRM,GX_NRM_XYZ,GX_S8,0},
        {GX_VA_CLR0,GX_CLR_RGB,GX_RGB565,0},{GX_VA_CLR1,GX_CLR_RGBA,GX_RGBA6,0},{GX_VA_TEX7,GX_TEX_ST,GX_S16,8}};
    VertexDecodePlan p;assert(planVertexDecode(lo,formats,p));
    const Bytes packed{9,60,0,64,0xc0,32,0xf8,0x1f,0xff,0xff,0xff,0xff,0x80,1,0};
    assert(decodeVertex(p,packed,arrays,out));assert(out.matrix[0]==9&&out.matrix[8]==60);
    near(out.normal[0][0],1);near(out.normal[0][1],-1);near(out.normal[0][2],0.5);
    near(out.color[0][0],1);near(out.color[0][1],0);near(out.color[0][2],1);near(out.color[1][3],1);
    near(out.texcoord[7][0],-0.5);near(out.texcoord[7][1],1);
    // NBT3: separate indices plus the pinned Aurora per-slice offset in each record.
    VtxLayout nbt{};nbt.attrs[GX_VA_POS]={0,1,GX_INDEX8};nbt.attrs[GX_VA_NRM]={1,3,GX_INDEX8};nbt.stride=4;
    GXVtxAttrFmtList nf[]{{GX_VA_POS,GX_POS_XYZ,GX_F32,0},{GX_VA_NRM,GX_NRM_NBT3,GX_S16,0}};
    assert(planVertexDecode(nbt,nf,p));Bytes normals(54,0);
    normals[0]=0x40;normals[18+6+2]=0x40;normals[36+12+4]=0x40;
    arrays[GX_VA_NRM]={normals,18,false};const u8 ni[]{0,0,1,2};
    assert(decodeVertex(p,ni,arrays,out));near(out.normal[0][0],1);near(out.normal[1][1],1);near(out.normal[2][2],1);
    arrays[GX_VA_NRM].bytes={normals.data(),53};assert(!decodeVertex(p,ni,arrays,out));
    // Decode all packed color types, including byte-order-sensitive formats.
    for(auto type:{GX_RGB565,GX_RGB8,GX_RGBX8,GX_RGBA4,GX_RGBA6,GX_RGBA8}) {
        const unsigned size=type==GX_RGB565||type==GX_RGBA4?2:type==GX_RGB8||type==GX_RGBA6?3:4;
        VtxLayout c{};c.attrs[GX_VA_POS]={0,1,GX_INDEX8};c.attrs[GX_VA_CLR0]={1,1,GX_INDEX8};c.stride=2;
        GXVtxAttrFmtList cf[]{{GX_VA_POS,GX_POS_XYZ,GX_F32,0},{GX_VA_CLR0,GX_CLR_RGBA,type,0}};
        assert(planVertexDecode(c,cf,p));Bytes white(size,0xff);arrays[GX_VA_CLR0]={white,size,false};const u8 ci[]{0,0};
        assert(decodeVertex(p,ci,arrays,out));for(float v:out.color[0])near(v,1);
        white.assign(size,0);arrays[GX_VA_CLR0]={white,size,true};assert(decodeVertex(p,ci,arrays,out));
        for(unsigned j=0;j<3;++j)near(out.color[0][j],0);near(out.color[0][3],type<=GX_RGBX8?1:0);
        Bytes distinct;float expected[4]{};
        if(type==GX_RGB565){distinct={0x43,0x08};expected[0]=1/31.0f;expected[1]=2/63.0f;expected[2]=3/31.0f;expected[3]=1;}
        else if(type==GX_RGBA4){distinct={0x34,0x12};for(unsigned j=0;j<4;++j)expected[j]=(j+1)/15.0f;}
        else if(type==GX_RGBA6){distinct={0xc4,0x20,0x04};for(unsigned j=0;j<4;++j)expected[j]=(j+1)/63.0f;}
        else {distinct={0x11,0x22,0x33,0x44};distinct.resize(size);for(unsigned j=0;j<3;++j)expected[j]=(17*(j+1))/255.0f;expected[3]=type==GX_RGBA8?68/255.0f:1;}
        arrays[GX_VA_CLR0]={distinct,size,true};assert(decodeVertex(p,ci,arrays,out));
        for(unsigned j=0;j<4;++j)near(out.color[0][j],expected[j]);
    }
    auto broken=lo;broken.attrs[GX_VA_CLR1].offset=0;assert(!planVertexDecode(broken,formats,p,&error));assert(error==VertexDecodeError::layout && p.packedStride()==0);
    assert(!decodeVertex(p,packed,arrays,out));
    formats[0].type=GX_RGBA8;assert(!planVertexDecode(lo,formats,p,&error));assert(error==VertexDecodeError::format);
    auto fractional=plan(GX_DIRECT,GX_U16,GX_POS_XY,31,4);const u8 tiny[]{0,1,0,2};
    assert(decodeVertex(fractional,tiny,arrays,out));assert(out.position[0]==std::ldexp(1.0f,-31) && out.position[1]==std::ldexp(1.0f,-30));
    // Reusing a prepared format across many vertices must not retain prior values.
    auto signed8=plan(GX_DIRECT,GX_S8,GX_POS_XY,0,2);
    for(int v=-128;v<128;++v){const u8 bytes[]{static_cast<u8>(v),static_cast<u8>(127-v)};
      assert(decodeVertex(signed8,bytes,arrays,out));assert(out.position[0]==v && out.position[2]==0);}
    puts("PASS: vertex formats, direct/indexed bounds, signed fractions, unaligned mixed endian arrays, NBT3, packed colors and failure clearing");
}
