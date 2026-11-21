#pragma once
// Native PICA engine: the per-vertex part of an indirect warp on the GPU
// vertex route (RigidVertexPlan::vertexWarp, planned by planVertexWarp in
// gx_indirect_gpu.hpp). The multi-texture decoder calls
// VertexWarpSampler::store once per vertex: it reads the warp map where
// sampleIndirectImage reads it (same level choice, wrap and filter) and
// stores what the warp adds to its mean. The CPU vertex route spent about
// 10 microseconds a vertex on a warped draw, nearly all of it outside the
// sample; here a vertex costs the sample and the PICA does the rest.
//
// Three forms. The common one (I4, I8, IA4 or IA8 map, sizes that are powers
// of two, repeated on both axes, one level, bilinear: every warp map of the
// water and Lakebed zones measured for Alpha 9) works in integers: texel
// coordinates rounded to 10 fraction bits and the four texels blended with
// integer weights, so its sample is within 255/1024 of the reference's (half
// that along each axis, between texels of 0 and 255). Everything else takes
// the general form, which is sampleIndirectLevel itself.
// When the map's coordinate is made from an indexed texture coordinate
// stored as integers (S16 in every water mesh measured), the common form
// runs the whole pass without a float (runTurns): the coordinate is kept in
// turns, 2^32 to a repeat of the map, so the wrap is the overflow of a 32-bit
// sum.
#include "gx_indirect.hpp"
#include "gx_rigid_vertex.hpp"
#include <bit>
#include <cstring>

namespace dusk::native::gx {
class VertexWarpSampler {
public:
    explicit VertexWarpSampler(const VertexWarpPlan& plan):plan_(plan){
        for(unsigned n=0;n<(plan.blend>0?2u:1u);++n){
            auto& level=levels_[n];const auto& source=plan.levels[n];
            level={};level.image={source.texels,source.bytes};level.width=source.width;level.height=source.height;
            level.format=plan.format;level.wrapS=plan.wrapS;level.wrapT=plan.wrapT;level.magFilter=source.linear;
        }
        const auto& first=plan.levels[0];
        const auto powerOfTwo=[](unsigned v){return v&&!(v&(v-1));};
        if(plan.format<=3&&plan.wrapS==1&&plan.wrapT==1&&!(plan.blend>0)&&first.linear&&powerOfTwo(first.width)&&powerOfTwo(first.height)){
            const auto shape=indirectTile(plan.format);
            texels_=first.texels;maskX_=first.width-1u;maskY_=first.height-1u;
            rowBytes_=((unsigned(first.width)+shape.width-1)/shape.width)*32;
            for(unsigned k=0;k<4;++k){
                fixedRows_[0][k]=plan.rows[0][k]*float(first.width)*1024;
                fixedRows_[1][k]=plan.rows[1][k]*float(first.height)*1024;
            }
            // value[i] = gain[i][0] A + gain[i][1] I + offset[i], A and I
            // being the blended alpha and intensity times 2^20, in the
            // texel's own units (a nibble of I4 and IA4 is 17 bytes' worth).
            for(unsigned i=0;i<2;++i){
                const auto& f=plan.functional[i];
                const float scale=plan.recordScale*(plan.format==0||plan.format==2?17.f:1.f)/1048576.f;
                gain_[i][0]=plan.format<=1?(f[0]+f[1]+f[2])*scale:f[0]*scale;
                gain_[i][1]=plan.format<=1?0.f:(f[1]+f[2])*scale;
                offset_[i]=-(f[0]*plan.mean[0]+f[1]*plan.mean[1]+f[2]*plan.mean[2])*plan.recordScale;
            }
            store_=plan.format==0?&storeFast<0>:plan.format==1?&storeFast<1>:plan.format==2?&storeFast<2>:&storeFast<3>;
        }else store_=&storeGeneral;
    }
    // The two numbers a vertex stores for the source element `in` ((x,y,z)
    // of a position, (s,t,1) of a texture coordinate), at `out` (four bytes).
    // The mean of the warp (zero) where the map cannot be read.
    void store(const float* in,std::byte* out)const{store_(*this,in,out);}
    // The map at (u,v) in images, as sampleIndirectImage reads it.
    bool sample(float u,float v,std::array<float,3>& abg)const{
        if(!sampleIndirectLevel(levels_[0],u,v,abg))return false;
        if(plan_.blend>0){
            std::array<float,3> high;
            if(!sampleIndirectLevel(levels_[1],u,v,high))return false;
            for(unsigned c=0;c<3;++c)abg[c]+=(high[c]-abg[c])*plan_.blend;
        }
        return true;
    }
    bool integerForm()const{return store_!=&storeGeneral;}
    // The whole pass for an indexed texture coordinate of an integer type
    // (`type` a GX component type, `scale` what one unit of it is worth),
    // native byte order: for each of `vertices` vertices the index at `index`
    // (one or two bytes, `packedStride` apart) names an element of `stride`
    // bytes in `data`, and the two shorts go to `out`, `recordBytes` apart.
    // -1: not this form (use store for each vertex); 0 done; 1 an index
    // beyond `limit`.
    int runTurns(unsigned type,float scale,const std::uint8_t* data,std::size_t stride,unsigned limit,bool wideIndex,const std::uint8_t* index,
                 std::size_t packedStride,std::size_t vertices,std::byte* out,std::size_t recordBytes)const{
        if(!integerForm()||plan_.projected||(type!=GX_S16&&type!=GX_U16&&type!=GX_S8&&type!=GX_U8))return -1;
        const auto& first=plan_.levels[0];
        const unsigned widthBits=unsigned(std::countr_zero(unsigned(first.width))),heightBits=unsigned(std::countr_zero(unsigned(first.height)));
        if(widthBits<1||heightBits<1||widthBits>16||heightBits>16)return -1;
        Turns t;
        // A number of repeats (below 2^31) as 32-bit turns: only its
        // fraction matters, since it multiplies an integer. Conversions the
        // processor has, no library call: this runs once per warped draw,
        // 90 times a picture in Morpheel's room.
        auto turns=[](double value){
            value-=double(std::int32_t(value));
            if(value<0)value+=1;
            return std::uint32_t(value*4294967296.0+.5); // saturates one step below a whole turn
        };
        for(unsigned axis=0;axis<2;++axis){
            const auto& row=plan_.rows[axis];
            for(float value:row)if(!(std::abs(value)<1e6f))return -1;
            const double size=axis?first.height:first.width;
            t.row[axis][0]=turns(double(row[0])*scale);t.row[axis][1]=turns(double(row[1])*scale);
            // Half a texel back to the first of the four texels, half a
            // step of the ten fraction bits forward to round.
            t.constant[axis]=turns(double(row[2])+row[3]-.5/size+.5/(1024*size));
        }
        t.shiftX=32-widthBits;t.shiftY=32-heightBits;
        // value = (A gain[0] + I gain[1] + offset) >> 14: A and I the
        // blended alpha and intensity in the texel's own units (a nibble of
        // I4 and IA4 is 17 bytes' worth) with 8 fraction bits. The sums are
        // taken modulo 2^32: the value itself is below 2^15, so what the
        // products overflow cancels.
        const double unit=plan_.format==0||plan_.format==2?17:1;
        for(unsigned i=0;i<2;++i){
            const auto& f=plan_.functional[i];
            const double a=plan_.format<=1?double(f[0])+f[1]+f[2]:double(f[0]),b=plan_.format<=1?0.0:double(f[1])+f[2];
            const double gainA=a*plan_.recordScale*unit*64,gainI=b*plan_.recordScale*unit*64;
            if(!(std::abs(gainA)<2147483648.0&&std::abs(gainI)<2147483648.0))return -1;
            t.gain[i][0]=std::uint32_t(std::int32_t(gainA+(gainA<0?-.5:.5)));t.gain[i][1]=std::uint32_t(std::int32_t(gainI+(gainI<0?-.5:.5)));
            const double mean=(double(f[0])*plan_.mean[0]+double(f[1])*plan_.mean[1]+double(f[2])*plan_.mean[2])*plan_.recordScale;
            if(!(std::abs(mean)<1e15))return -1;
            t.offset[i]=std::uint32_t(std::uint64_t(std::int64_t(-mean*16384+(mean>0?-.5:.5))+8192));
        }
        const bool word=type==GX_S16;
        switch(plan_.format){
        case 0:return word?turnsLoop<0,true>(t,type,data,stride,limit,wideIndex,index,packedStride,vertices,out,recordBytes):
                           turnsLoop<0,false>(t,type,data,stride,limit,wideIndex,index,packedStride,vertices,out,recordBytes);
        case 1:return word?turnsLoop<1,true>(t,type,data,stride,limit,wideIndex,index,packedStride,vertices,out,recordBytes):
                           turnsLoop<1,false>(t,type,data,stride,limit,wideIndex,index,packedStride,vertices,out,recordBytes);
        case 2:return word?turnsLoop<2,true>(t,type,data,stride,limit,wideIndex,index,packedStride,vertices,out,recordBytes):
                           turnsLoop<2,false>(t,type,data,stride,limit,wideIndex,index,packedStride,vertices,out,recordBytes);
        default:return word?turnsLoop<3,true>(t,type,data,stride,limit,wideIndex,index,packedStride,vertices,out,recordBytes):
                            turnsLoop<3,false>(t,type,data,stride,limit,wideIndex,index,packedStride,vertices,out,recordBytes);
        }
    }
private:
    struct Turns {
        std::uint32_t row[2][2]{},constant[2]{};
        unsigned shiftX{},shiftY{};
        std::uint32_t gain[2][2]{},offset[2]{};
    };
    // Alpha and intensity of the texel (x,y) in its own units: a nibble (I4,
    // IA4) or a byte (I8, IA8); indirectTexel gives them as a and b = g, in
    // bytes. `row`: where the tile row holding y starts.
    template<unsigned Format>
    static void rawTexel(const std::uint8_t* row,std::uint32_t x,std::uint32_t y,std::uint32_t& a,std::uint32_t& i){
        if(Format==0){
            const std::uint32_t byte=row[(x>>3)*32+((y&7)<<2)+((x&7)>>1)];
            a=i=(x&1)?byte&15:byte>>4;
        }else if(Format==1){
            a=i=row[(x>>3)*32+(((y&3)<<3)|(x&7))];
        }else if(Format==2){
            const std::uint32_t byte=row[(x>>3)*32+(((y&3)<<3)|(x&7))];
            a=byte>>4;i=byte&15;
        }else{
            const std::uint8_t* at=row+(x>>2)*32+2*(((y&3)<<2)|(x&3));
            a=at[0];i=at[1];
        }
    }
    template<unsigned Format,bool Word>
    int turnsLoop(const Turns& t,unsigned type,const std::uint8_t* data,std::size_t stride,unsigned limit,bool wideIndex,const std::uint8_t* index,
                  std::size_t packedStride,std::size_t vertices,std::byte* out,std::size_t recordBytes)const{
        const std::uint8_t* const texels=texels_;
        const std::uint32_t maskX=maskX_,maskY=maskY_,rowBytes=rowBytes_;
        constexpr unsigned tileShiftY=Format==0?3:2;
        for(std::size_t n=0;n<vertices;++n,index+=packedStride,out+=recordBytes){
            const unsigned element=wideIndex?(unsigned(index[0])<<8)|index[1]:index[0];
            if(element>limit)return 1;
            const std::uint8_t* source=data+std::size_t(element)*stride;
            std::int32_t s,v;
            if(Word){std::int16_t st[2];std::memcpy(st,source,4);s=st[0];v=st[1];}
            else if(type==GX_U16){std::uint16_t st[2];std::memcpy(st,source,4);s=st[0];v=st[1];}
            else if(type==GX_S8){s=std::int8_t(source[0]);v=std::int8_t(source[1]);}
            else{s=source[0];v=source[1];}
            const std::uint32_t ux=std::uint32_t(s)*t.row[0][0]+std::uint32_t(v)*t.row[0][1]+t.constant[0];
            const std::uint32_t uy=std::uint32_t(s)*t.row[1][0]+std::uint32_t(v)*t.row[1][1]+t.constant[1];
            const std::uint32_t x0=ux>>t.shiftX,x1=(x0+1)&maskX,wx=(ux>>(t.shiftX-10))&1023;
            const std::uint32_t y0=uy>>t.shiftY,y1=(y0+1)&maskY,wy=(uy>>(t.shiftY-10))&1023;
            const std::uint8_t* row0=texels+(y0>>tileShiftY)*rowBytes;
            const std::uint8_t* row1=texels+(y1>>tileShiftY)*rowBytes;
            std::uint32_t a00,a10,a01,a11,i00,i10,i01,i11;
            rawTexel<Format>(row0,x0,y0,a00,i00);rawTexel<Format>(row0,x1,y0,a10,i10);
            rawTexel<Format>(row1,x0,y1,a01,i01);rawTexel<Format>(row1,x1,y1,a11,i11);
            const std::uint32_t qx=1024-wx,qy=1024-wy;
            const std::uint32_t alpha=((a00*qx+a10*wx)*qy+(a01*qx+a11*wx)*wy+2048)>>12;
            std::uint32_t first=alpha*t.gain[0][0]+t.offset[0],second=alpha*t.gain[1][0]+t.offset[1];
            if(Format>=2){
                const std::uint32_t intensity=((i00*qx+i10*wx)*qy+(i01*qx+i11*wx)*wy+2048)>>12;
                first+=intensity*t.gain[0][1];second+=intensity*t.gain[1][1];
            }
            // No clamp: the blend of four texels lies between the map's
            // lowest and highest, which the plan's scale keeps below 32000.
            const std::int16_t value[2]{std::int16_t(std::int32_t(first)>>14),std::int16_t(std::int32_t(second)>>14)};
            std::memcpy(out,value,4);
        }
        return 0;
    }
    using Store=void(*)(const VertexWarpSampler&,const float*,std::byte*);
    static std::int16_t fixed(float value){
        if(!(value>-32767.f))return -32767;
        if(!(value<32767.f))return 32767;
        return std::int16_t(std::int32_t(value<0?value-.5f:value+.5f));
    }
    static void zero(std::byte* out){std::memset(out,0,4);}
    static void storeGeneral(const VertexWarpSampler& s,const float* in,std::byte* out){
        const auto& plan=s.plan_;const auto& r=plan.rows;
        float u=r[0][0]*in[0]+r[0][1]*in[1]+r[0][2]*in[2]+r[0][3];
        float v=r[1][0]*in[0]+r[1][1]*in[1]+r[1][2]*in[2]+r[1][3];
        if(plan.projected){
            // Behind the map's projector the warp is undefined (the CPU
            // route refused such a vertex): the mean.
            const float q=r[2][0]*in[0]+r[2][1]*in[1]+r[2][2]*in[2]+r[2][3];
            if(!(q>0))return zero(out);
            u/=q;v/=q;
        }
        std::array<float,3> abg;
        if(!s.sample(u,v,abg))return zero(out);
        for(unsigned c=0;c<3;++c)abg[c]-=plan.mean[c];
        std::int16_t value[2];
        for(unsigned i=0;i<2;++i){
            const auto& f=plan.functional[i];
            value[i]=fixed((f[0]*abg[0]+f[1]*abg[1]+f[2]*abg[2])*plan.recordScale);
        }
        std::memcpy(out,value,4);
    }
    template<unsigned Format>
    static void storeFast(const VertexWarpSampler& s,const float* in,std::byte* out){
        const auto& r=s.fixedRows_;
        float x=r[0][0]*in[0]+r[0][1]*in[1]+r[0][2]*in[2]+r[0][3];
        float y=r[1][0]*in[0]+r[1][1]*in[1]+r[1][2]*in[2]+r[1][3];
        if(s.plan_.projected){
            const auto& d=s.plan_.rows[2];
            const float q=d[0]*in[0]+d[1]*in[1]+d[2]*in[2]+d[3];
            if(!(q>0))return zero(out);
            x/=q;y/=q;
        }
        // The sample point is half a texel before the coordinate (512 of the
        // 1024 steps of a texel); the other half step rounds to the nearest
        // step. Beyond a million texels (or not finite) no fraction is left
        // to read: the mean.
        x-=511.5f;y-=511.5f;
        if(!(x>-1.0e9f&&x<1.0e9f&&y>-1.0e9f&&y<1.0e9f))return zero(out);
        std::int32_t fx=std::int32_t(x),fy=std::int32_t(y);
        fx-=float(fx)>x;fy-=float(fy)>y; // floor
        const std::uint32_t wx=std::uint32_t(fx)&1023,wy=std::uint32_t(fy)&1023;
        const std::uint32_t x0=std::uint32_t(fx>>10)&s.maskX_,x1=(x0+1)&s.maskX_;
        const std::uint32_t y0=std::uint32_t(fy>>10)&s.maskY_,y1=(y0+1)&s.maskY_;
        constexpr unsigned tileShiftY=Format==0?3:2;
        const std::uint8_t* row0=s.texels_+(y0>>tileShiftY)*s.rowBytes_;
        const std::uint8_t* row1=s.texels_+(y1>>tileShiftY)*s.rowBytes_;
        std::uint32_t a00,a10,a01,a11,i00,i10,i01,i11;
        rawTexel<Format>(row0,x0,y0,a00,i00);rawTexel<Format>(row0,x1,y0,a10,i10);
        rawTexel<Format>(row1,x0,y1,a01,i01);rawTexel<Format>(row1,x1,y1,a11,i11);
        const std::uint32_t ux=1024-wx,uy=1024-wy;
        const float alpha=float(std::int32_t((a00*ux+a10*wx)*uy+(a01*ux+a11*wx)*wy));
        std::int16_t value[2];
        if(Format<=1){
            value[0]=fixed(s.gain_[0][0]*alpha+s.offset_[0]);
            value[1]=fixed(s.gain_[1][0]*alpha+s.offset_[1]);
        }else{
            const float intensity=float(std::int32_t((i00*ux+i10*wx)*uy+(i01*ux+i11*wx)*wy));
            value[0]=fixed(s.gain_[0][0]*alpha+s.gain_[0][1]*intensity+s.offset_[0]);
            value[1]=fixed(s.gain_[1][0]*alpha+s.gain_[1][1]*intensity+s.offset_[1]);
        }
        std::memcpy(out,value,4);
    }
    const VertexWarpPlan& plan_;
    std::array<Texture,2> levels_{};
    Store store_{};
    const std::uint8_t* texels_{};
    std::uint32_t maskX_{},maskY_{},rowBytes_{};
    std::array<std::array<float,4>,2> fixedRows_{};
    std::array<std::array<float,2>,2> gain_{};
    std::array<float,2> offset_{};
};
}
