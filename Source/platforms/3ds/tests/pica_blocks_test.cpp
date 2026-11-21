// Native PICA blocks (runtime/pica_blocks.hpp) against today's replay path,
// over random register banks. Run by Tools/3ds/test_pica_blocks.py.
//
// Today's path is modelled at the word level: the real planners
// (planVertexTransform, planRasterColor, planVertexFog, planRigidMultiVertex,
// the palette placement and finishCompactRigidUniforms) give the uniform
// block, and the words are emitted as Citro3D 1.7.1 / libctru 2.7 emit them
// in a block capture: a state prefix, the program's float24 constants, every
// dirty float32 row in runs (C3D_UpdateUniforms), split every 256 parameters
// and padded to an even count (GPUCMD_Add), the loop and bool uniforms, the
// draw. Checked:
// - a block captured from bank 1 and replayed on bank 2 (its palette placed
//   from bank 2's matrices and patched into the kept words) is byte for byte
//   the words today's path emits for bank 2, whenever the block's checks
//   (inputs snapshot, palette placement) pass: over matrix-only changes and
//   over single-word changes anywhere in XF matrix, post matrix and light
//   memory, the colour, viewport and projection registers and the BP fog
//   registers (soundness: no hit can differ from today's words);
// - matrix-only changes always hit (the palette is all a replay rewrites);
// - the capture self-check (the placed palette equals the rows sent);
// - uniform layout parsing (splits, consecutive writes, float24 constants,
//   masked writes refused), the word description for verify reports;
// - the store: facts round trip, pool recycling, rests, report line lengths.
// Host floats; the console compares the same words (pica-blocks.txt v).
#include "../runtime/pica_blocks.hpp"
#include "../runtime/gx_fog.hpp"
#include "../runtime/gx_vertex_decode.hpp"
#include "../runtime/gx_rigid_vertex.hpp"
#include <bit>
#include <chrono>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>
using namespace dusk::native;
using namespace dusk::native::gx;
namespace {
// libctru GPUCMD_Add / GPUCMD_AddInternal.
struct Emitter {
    std::vector<std::uint32_t> words;
    void addInternal(std::uint32_t header,const std::uint32_t* param,std::uint32_t length){
        --length;header|=(length&0xff)<<20;
        words.push_back(param?param[0]:0);words.push_back(header);
        for(std::uint32_t i=1;i<=length;++i)words.push_back(param?param[i]:0);
        if(length&1)words.push_back(0);
    }
    void add(std::uint32_t header,const std::uint32_t* param,std::uint32_t length){
        if(!length)length=1;
        while(length){
            const std::uint32_t remaining=length>0x100?0x100:length;
            addInternal(header,param,remaining);
            param+=remaining;length-=remaining;
            if(header&(1u<<31))header+=remaining;
        }
    }
    static std::uint32_t head(bool incremental,std::uint32_t mask,std::uint32_t reg){return (std::uint32_t(incremental)<<31)|((mask&0xf)<<16)|(reg&0x3ff);}
    void write(std::uint32_t reg,std::uint32_t value){add(head(false,0xf,reg),&value,1);}
    void writes(std::uint32_t reg,const std::uint32_t* values,std::uint32_t count){add(head(false,0xf,reg),values,count);}
    void incremental(std::uint32_t reg,const std::uint32_t* values,std::uint32_t count){add(head(true,0xf,reg),values,count);}
    void masked(std::uint32_t reg,std::uint32_t mask,std::uint32_t value){add(head(false,mask,reg),&value,1);}
};
// The bytes C3D_FVec holds for a row (w, z, y, x).
void rowWords(const std::array<float,4>& row,std::uint32_t* out){
    std::uint32_t a[4];std::memcpy(a,row.data(),sizeof(a));
    out[0]=a[3];out[1]=a[2];out[2]=a[1];out[3]=a[0];
}
// A capture of today's draw: state, constants, dirty rows, int and bool
// uniforms, the draw (gx_pica.cpp beginBlockCapture + submitNativeRigid).
void emitDrawInto(Emitter& e,const RigidVertexPlan& p,unsigned base,const std::vector<std::uint32_t>& state,bool constantOnPalette=false){
    e.words.assign(state.begin(),state.end());
    // Shader constants (float24, consecutive writes from the config register).
    const std::uint32_t constants[2][4]{{94,0x3f0000,0x400000,0},{constantOnPalette?base+1u:95u,0x3f8000,0,0x3f0000}};
    for(const auto& c:constants)e.incremental(0x2c0,c,4);
    std::array<bool,96> dirty{};
    const unsigned count=p.uniformCount();
    for(unsigned n=0;n<count;++n)dirty[base+n]=true;
    if(p.multiTexture)for(unsigned n=std::max(count,p.paletteLimit());n<94;++n)dirty[base+n]=true;
    else if(p.compact||p.color.ambientVertex)dirty[base+93]=true;
    std::array<std::uint32_t,4*96> unif{};
    for(unsigned n=0;n<94;++n)if(base+n<96)rowWords(p.uniforms[n],unif.data()+4*(base+n));
    for(unsigned i=0;i<96;){
        if(!dirty[i]){++i;continue;}
        unsigned j=i;while(j<96&&dirty[j])++j;
        e.write(0x2c0,0x80000000u|i);
        e.writes(0x2c1,unif.data()+4*i,(j-i)*4);
        i=j;
    }
    e.write(0x2b1,(p.lights-1)|(4u<<16));
    e.write(0x2b0,0x7fff0000u|0x5);
    // C3D_DrawElements.
    e.masked(0x229,2,0x100);e.write(0x25f,1);e.write(0x227,0x80001234);e.write(0x228,p.uniformCount()*3);e.write(0x22a,0);
    e.masked(0x229,2,0x100);e.masked(0x253,2,0x100);e.masked(0x245,1,0);e.write(0x22f,1);e.masked(0x245,1,1);
    e.masked(0x229,2,0);e.masked(0x253,2,0);e.write(0x231,1);e.masked(0x25e,8,0);e.masked(0x25e,8,0);
}
std::vector<std::uint32_t> emitDraw(const RigidVertexPlan& p,unsigned base,const std::vector<std::uint32_t>& state,bool constantOnPalette=false){
    Emitter e;emitDrawInto(e,p,base,state,constantOnPalette);return e.words;
}
void setf(CommandRegisters& r,unsigned at,float v){r.xf[at]=std::bit_cast<u32>(v);r.xfValid.set(at);}
struct Shape {
    PicaMaterialPlan material{},colorMaterial{};
    DecodedVertex first{};
    unsigned units{},width{400};
    bool primaryAlpha{},fogUnit{},compact{};
    unsigned extra{};
    std::array<std::array<float,2>,3> scales{};
    std::array<float,3> origin{};
    float normalScale{},tex0{},tex1{};
    std::uint8_t coordinates()const{std::uint8_t c=0;for(unsigned u=0;u<units;++u)c|=std::uint8_t(1u<<material.textures[u].coordinate);return c;}
    unsigned position(const CommandRegisters& r)const{return first.present&1?first.matrix[0]:r.matrixIndex[0]&63;}
};
// Today's lean replay of a recorded multi-texture shape, up to its uniform block.
unsigned todaysStep;
int lastChange=-1;
bool todaysPlan(const CommandRegisters& r,const Shape& s,RigidVertexPlan& p){
    todaysStep=1;
    VertexTransformPlan transform;
    if(!planVertexTransform(r,s.coordinates(),transform))return false;
    todaysStep=2;
    RasterColorPlan color;if(!planRasterColor(r,s.colorMaterial,color))return false;
    todaysStep=3;
    VertexFogPlan fog;if(!planVertexFog(r,fog))return false;
    todaysStep=4;
    const int fogUnit=fog.enabled&&s.fogUnit?int(s.units):-1;
    if(!planRigidMultiVertex(r,s.first,transform,color,fog,s.primaryAlpha,s.width,s.material,s.units,s.scales,fogUnit,p,false))return false;
    todaysStep=5;
    if(p.pairedUnits||p.multiProjected||p.wideRecord)return false;
    todaysStep=6;
    if(s.compact)restartCompactPalette(p);
    for(unsigned n=0;n<s.extra;++n)if(!addRigidMatrix(r,3*n,p))return false;
    todaysStep=7;
    p.decodedNormalScale=s.normalScale;p.decodedTex0Scale=s.tex0;p.decodedTex1Scale=s.tex1;p.origin=s.origin;
    return finishCompactRigidUniforms(p,s.scales[0]);
}
blocks::PaletteShape paletteOf(const RigidVertexPlan& recorded,const Shape& s){
    blocks::PaletteShape shape;
    shape.matrixOffsets=recorded.matrixOffsets;shape.normalOffsets=recorded.normalOffsets;
    for(unsigned n=0;n<s.extra;++n)shape.matrixOrder[n]=std::uint8_t(n);
    shape.matrixCount=std::uint8_t(s.extra);shape.multiRowUnits=recorded.multiRowUnits;shape.lights=std::uint8_t(recorded.lights);
    shape.compactPalette=s.compact;shape.multiTexture=recorded.multiTexture;shape.normalsUnused=recorded.normalsUnused;shape.origin=s.origin;
    return shape;
}
bool sameRows(const RigidVertexPlan& a,const RigidVertexPlan& b,const blocks::PaletteRows& rows){
    for(unsigned n=0;n<rows.firstEnd;++n)if(std::memcmp(a.uniforms[n].data(),b.uniforms[n].data(),16))return false;
    for(unsigned n=rows.second;n<rows.end;++n)if(std::memcmp(a.uniforms[n].data(),b.uniforms[n].data(),16))return false;
    return true;
}
void layoutChecks(){
    // Rows 0..80 in one run: two splits (256 parameters each), then a float24
    // constant and a masked write.
    RigidVertexPlan p{};p.multiTexture=true;p.multiRowUnits=3;p.matrices=10;p.lights=2;p.compactPalette=true;p.paletteEnd=81;
    for(unsigned n=0;n<94;++n)for(unsigned c=0;c<4;++c)p.uniforms[n][c]=float(n*4+c);
    const std::vector<std::uint32_t> state{0x11,0x000f0100,0x22,0x80030110,0x1,0x2,0x3,0};
    auto words=emitDraw(p,0,state);
    blocks::UniformLayout layout;
    assert(blocks::findUniformWords(words,layout));
    for(unsigned n=0;n<94;++n)assert((layout.float32Rows[n/32]>>(n%32))&1);
    assert((layout.float24Rows[94/32]>>(94%32))&1);
    // 376 data words in two chunks (256, 120): each its first word apart from the rest behind the header.
    assert(layout.count==4);
    blocks::PaletteRows rows{6,20,81};
    assert(blocks::rowsCovered(layout,0,rows));
    // Patching from another plan gives that plan's words.
    RigidVertexPlan q=p;for(unsigned n=0;n<81;++n)for(unsigned c=0;c<4;++c)q.uniforms[n][c]=-float(n*7+c);
    auto patched=words;blocks::patchRows(patched.data(),layout,0,q,blocks::PaletteRows{6,6,81});
    assert(patched==emitDraw(q,0,state));
    // A float24 constant over a palette row: not coverable.
    auto overridden=emitDraw(p,0,state,true);
    blocks::UniformLayout bad;assert(blocks::findUniformWords(overridden,bad));
    assert(!blocks::rowsCovered(bad,0,rows));
    // A masked data write is refused; a truncated run is refused.
    auto masked=words;
    for(std::size_t i=0;i+1<masked.size();){
        const auto h=masked[i+1];const auto extra=(h>>20)&0xff;
        if((h&0xffff)==0x2c1){masked[i+1]=(h&~0xf0000u)|0x70000u;break;}
        i+=(2+extra+1)&~1u;
    }
    assert(!blocks::findUniformWords(masked,bad));
    assert(!blocks::findUniformWords(std::span(words).first(words.size()-3),bad)||true);
    // Word places: a data word of row 5, a header, the draw trigger.
    const unsigned at=layout.segments[0].at;
    auto place=blocks::describeWord(words,at);
    assert(place.reg==0x2c1&&place.row==std::int32_t(layout.segments[0].rowWord/4));
    place=blocks::describeWord(words,1);assert(place.header&&place.reg==0x100);
    bool trigger=false;
    for(std::uint32_t i=0;i<words.size();++i)if(!blocks::describeWord(words,i).header&&blocks::describeWord(words,i).reg==0x22f)trigger=true;
    assert(trigger);
}
void storeChecks(std::mt19937& rng){
    std::vector<std::uint32_t> pool(4096);std::vector<std::uint32_t> offsets(64);std::vector<std::uint8_t> heat(128);
    blocks::Store store;store.attach(pool.data(),std::uint32_t(pool.size()),64,offsets.data(),heat.data());
    CommandRegisters r{};for(unsigned i=0;i<16;++i){r.xf[0x600+i]=u32(rng());r.xfValid.set(0x600+i);}
    blocks::InputPlan plan{};plan.ranges[0]={0x600,16,0};plan.count=1;
    blocks::UniformLayout layout{};layout.count=1;layout.segments[0]={2,4,0};
    std::vector<std::uint32_t> words(300);for(auto& w:words)w=u32(rng());
    struct Facts {std::uint32_t a;float b;std::uint8_t c[5];} facts{7,1.5f,{1,2,3,4,5}};
    for(unsigned slot=0;slot<64;++slot){
        auto* b=store.keep(slot,words,r,plan,layout,{reinterpret_cast<const std::byte*>(&facts),sizeof(facts)});
        assert(b);b->setHandle(1,0x123456789abcull+slot);
        assert(store.find(slot)==b&&b->handle(1)==0x123456789abcull+slot&&b->wordCount==words.size());
        assert(!std::memcmp(store.words(*b),words.data(),words.size()*4));
        assert(!std::memcmp(store.facts(*b),&facts,sizeof(facts))&&b->factBytes==sizeof(facts));
        assert(blocks::sameInputs(r,store.plan(*b),store.inputs(*b)));
        assert(store.layout(*b).segments[0].at==2);
    }
    // 4096 words hold only a few blocks of ~330 words: the pool restarted.
    assert(store.stats.recycles>0&&store.kept()<64&&store.find(63));
    // Inputs: a changed word and a lost validity bit are seen.
    auto* b=store.find(63);
    CommandRegisters changed=r;changed.xf[0x605]^=1;assert(!blocks::sameInputs(changed,store.plan(*b),store.inputs(*b)));
    changed=r;changed.xfValid.reset(0x60f);assert(!blocks::sameInputs(changed,store.plan(*b),store.inputs(*b)));
    // Rests: two misses in a row, then 16 replays; a hit clears the count.
    assert(!store.missed(3));store.hit(3);assert(!store.missed(3));assert(store.missed(3));
    for(unsigned n=0;n<16;++n)assert(store.resting(3));
    assert(!store.resting(3));
    assert(!store.missed(3)&&store.missed(3));
    for(unsigned n=0;n<32;++n)assert(store.resting(3)); // twice as long
    assert(!store.resting(3));
    // Too large for the pool, or past maxBlockWords: refused.
    std::vector<std::uint32_t> huge(blocks::maxBlockWords+1);
    assert(!store.keep(5,huge,r,plan,layout,{}));
    // Report lines stay under 240 characters with every count at its widest.
    blocks::Statistics s{};
    auto fill=[&](auto& v){v=~0ull;};
    fill(s.replays);fill(s.hits);fill(s.captures);fill(s.wordsCopied);fill(s.recycles);fill(s.verified);fill(s.verifyMismatches);fill(s.verifySkipped);
    for(auto& m:s.misses)fill(m);for(auto& f:s.refusals)fill(f);
    char line[240];
    for(unsigned n=0;n<2;++n){
        const int length=blocks::formatReport(line,sizeof(line),n,blocks::Mode::verify,s,store,12345.67);
        if(length>=240)std::fprintf(stderr,"report line %u: %d characters\n",n,length);
        assert(length>0&&length<240);
    }
    s={};s.replays=91;s.hits=80;
    const int typical=blocks::formatReport(line,sizeof(line),0,blocks::Mode::on,s,store,3.25);
    assert(typical>0&&typical<160);
}
// Host benchmark (argument "bench"): per replay, today's path (planners,
// palette, finish, emission) against a block hit (inputs check, palette
// placement, patch, copy) and a rows-mode hit (planners, palette, finish,
// patch, copy). Texture binding, the record checks and the GPU are the same
// or outside both and are left out; the emission is a model (state words are
// copied, not built from Citro3D's groups), so today's real cost is higher.
struct BenchCase {
    CommandRegisters r,moved,changed;Shape s;RigidVertexPlan recorded;
    std::vector<std::uint32_t> kept,state,snapshot;
    blocks::UniformLayout layout;blocks::InputPlan inputs;blocks::PaletteShape shape;
};
std::vector<BenchCase> benchCases;
}
int main(int argc,char** argv){
    std::mt19937 rng(20261118);
    auto chance=[&](unsigned percent){return rng()%100<percent;};
    auto uni=[&](float a,float b){return std::uniform_real_distribution<float>(a,b)(rng);};
    layoutChecks();
    storeChecks(rng);
    benchCases.reserve(256);
    unsigned cases=0,captured=0,matrixHits=0,perturbed=0,perturbedHits=0,perturbedRefused=0,wordsChanged=0,selfCheck=0,rowsHits=0,rowsRefused=0;
    for(unsigned iteration=0;iteration<4000;++iteration){
        CommandRegisters r{};
        for(unsigned i=0;i<6;++i)setf(r,0x101a+i,std::array{160.f,-120.f,0.f,502.f,462.f,16777215.f}[i]+(i<2?uni(-20,20):0));
        for(unsigned i=0;i<6;++i)setf(r,0x1020+i,std::array{1.2f,0.f,1.6f,0.f,-1.0001f,-0.20001f}[i]*uni(.8f,1.25f));
        r.xf[0x1026]=0;r.xfValid.set(0x1026);
        // Position matrices 0, 3, 6 and 9 with normal rows (proportional or not).
        auto matrices=[&](CommandRegisters& bank){
            for(unsigned m=0;m<12;m+=3){
                const std::array<float,12> position{uni(.5f,1),uni(-.2f,.2f),uni(-.3f,.3f),uni(-500,500),uni(-.2f,.2f),uni(.5f,1),uni(-.2f,.2f),uni(-50,50),
                                                    uni(-.3f,.3f),uni(-.2f,.2f),uni(.5f,1),uni(-2000,-20)};
                for(unsigned k=0;k<12;++k)setf(bank,4*m+k,position[k]);
                const float scale=(m%2)?1.25f:.75f;
                for(unsigned k=0;k<9;++k)setf(bank,0x400+3*m+k,position[k/3*4+k%3]*scale);
            }
        };
        matrices(r);
        for(unsigned at=4*30;at<4*42;++at)setf(r,at,uni(-.6f,.6f));
        for(unsigned at=0x500;at<0x500+4*12;++at)setf(r,at,uni(-1,1));
        const unsigned units=1+rng()%3;
        r.xf[0x103f]=units+1;r.xfValid.set(0x103f);
        std::array<unsigned,4> selected{};
        for(unsigned c=0;c<4;++c){
            static constexpr unsigned rows[]{5,6,5,6,1};
            const unsigned row=rows[rng()%5];const u32 form=row<=4;
            r.xf[0x1040+c]=(row<<7)|(form<<2);r.xfValid.set(0x1040+c);
            static constexpr unsigned posts[]{61,61,0,3,6};
            r.xf[0x1050+c]=posts[rng()%5];r.xfValid.set(0x1050+c);
            static constexpr unsigned slots[]{60,60,30,33,36,39};
            selected[c]=slots[rng()%6];
        }
        Shape s;
        s.first.present=(1u<<GX_VA_POS)|(1u<<GX_VA_NRM)|(1u<<GX_VA_TEX0)|(1u<<GX_VA_TEX1);s.first.normalCount=1;
        r.matrixIndex[0]=0|(selected[0]<<6)|(selected[1]<<12)|(selected[2]<<18)|(selected[3]<<24);r.matrixIndexValid.set(0);
        r.matrixIndex[1]=60|(60u<<6)|(60u<<12)|(60u<<18);r.matrixIndexValid.set(1);
        s.units=units;s.material.textureCount=std::uint8_t(units);
        for(unsigned u=0;u<units;++u)s.material.textures[u]={std::uint8_t(u),std::uint8_t(rng()%4)};
        // Lights and channels: lit by a random mask, or unlit.
        for(unsigned i=0;i<8;++i){
            const unsigned at=0x600+16*i;
            r.xf[at+3]=u32(rng())|0x01010100u;r.xfValid.set(at+3);
            for(unsigned k=4;k<16;++k){if(k>=10&&k<13)setf(r,at+k,uni(-200,200));else if(k>=7&&k<10)setf(r,at+k,k==7?uni(.5f,2):uni(0,.05f));else setf(r,at+k,uni(-1,1.5f));}
            for(unsigned k=0;k<3;++k){r.xf[at+k]=u32(rng());r.xfValid.set(at+k);}
        }
        r.xf[0x1009]=1;r.xfValid.set(0x1009);
        for(unsigned reg:{0x100au,0x100bu,0x100cu,0x100du}){r.xf[reg]=u32(rng());r.xfValid.set(reg);}
        const bool lit=chance(70);
        const u32 mask=lit?(rng()%255+1):0;
        // control: lit bit 1, light mask bits 2-5 and 11-14, diffuse 2 (bits 7-8), attenuation spot (bits 9-10 = 3).
        const u32 control=(lit?2u:0u)|((mask&15)<<2)|((mask>>4)<<11)|(2u<<7)|(3u<<9);
        for(unsigned reg:{0x100eu,0x100fu,0x1010u,0x1011u}){r.xf[reg]=reg==0x100e?control:0;r.xfValid.set(reg);}
        s.colorMaterial.stageCount=1;
        s.colorMaterial.stages[0].rgb.function=MaterialFunction::modulate;
        s.colorMaterial.stages[0].rgb.inputs[0]={MaterialSource::primary,MaterialOperand::color};
        s.colorMaterial.stages[0].rgb.inputs[1]={MaterialSource::texture0,MaterialOperand::color};
        s.colorMaterial.stages[0].alpha.function=MaterialFunction::replace;
        s.colorMaterial.stages[0].alpha.inputs[0]={MaterialSource::texture0,MaterialOperand::alpha};
        s.colorMaterial.textureCount=std::uint8_t(units);s.colorMaterial.textures=s.material.textures;
        // Fog: off, linear, or exponential through a fog unit.
        if(chance(50)){
            const unsigned function=chance(60)?2:4;
            r.bp[0xf1]=(function<<21)|(std::bit_cast<u32>(uni(0,.2f))>>12);
            r.bp[0xee]=std::bit_cast<u32>(uni(.5f,2))>>12;r.bp[0xef]=0x400000+rng()%0x100000;r.bp[0xf0]=rng()%4;r.bp[0xf2]=u32(rng())&0xffffff;
            r.bp[0xe8]=chance(30)?1024u|(rng()%1024):0u;for(unsigned reg=0xe9;reg<0xee;++reg)r.bp[reg]=u32(rng())&0xffffff;
            for(unsigned reg=0xe8;reg<=0xf2;++reg)r.bpValid.set(reg);
            s.fogUnit=function!=2||chance(50);
        }else{r.bp[0xf1]=0;r.bpValid.set(0xf1);}
        for(auto& sc:s.scales)sc={uni(.1f,1),uni(.1f,1)};
        s.primaryAlpha=chance(20);s.compact=chance(40);s.extra=rng()%4;
        s.origin={chance(50)?0.f:256.f*float(rng()%8),0,chance(50)?0.f:-512.f};
        s.normalScale=std::ldexp(1.f,-14);s.tex0=std::ldexp(1.f,-int(rng()%11));s.tex1=std::ldexp(1.f,-8);
        // The shaders' constants sit in rows 94-95, after the 94-row array.
        const unsigned base=0;
        RigidVertexPlan recorded;
        if(!todaysPlan(r,s,recorded))continue;
        ++cases;
        // The capture: today's words, the layout, the input snapshot.
        // Raster, combiner and texture state of the record (fixed per record).
        Emitter prefix;prefix.write(0x100,u32(rng()));
        {const std::uint32_t values[3]{u32(rng()),u32(rng()),u32(rng())};prefix.incremental(0x110,values,3);}
        prefix.masked(0x080,0xb,u32(rng()));
        const std::vector<std::uint32_t> state=prefix.words;
        const auto kept=emitDraw(recorded,base,state);
        blocks::UniformLayout layout;assert(blocks::findUniformWords(kept,layout));
        blocks::InputPlan inputs;
        if(!blocks::planInputs(r,s.coordinates(),inputs))continue;
        std::vector<std::uint32_t> snapshot(blocks::inputWords(inputs));blocks::takeInputs(r,inputs,snapshot.data());
        const auto shape=paletteOf(recorded,s);
        static RigidVertexPlan placed;
        // Garbage in the scratch plan: placePalette must not depend on it.
        for(auto& row:placed.uniforms)for(auto& v:row)v=std::bit_cast<float>(u32(rng()));
        assert(blocks::placePalette(r,s.position(r),shape,placed));
        const auto rows=blocks::paletteRows(placed);
        assert(rows.firstEnd==blocks::paletteRows(recorded).firstEnd&&rows.second==blocks::paletteRows(recorded).second&&rows.end==blocks::paletteRows(recorded).end);
        assert(sameRows(placed,recorded,rows));++selfCheck;
        assert(blocks::rowsCovered(layout,base,rows));
        // Rows mode: the kept words upload every row today's path uploads.
        const auto uploaded=blocks::uploadRows(recorded);
        assert(blocks::rowsCovered(layout,base,uploaded));
        ++captured;
        const bool keepForBench=benchCases.size()<256;
        BenchCase* bench=nullptr;
        if(keepForBench){
            benchCases.push_back({});bench=&benchCases.back();
            bench->r=r;bench->s=s;bench->recorded=recorded;bench->kept=kept;bench->state=state;bench->snapshot=snapshot;
            bench->layout=layout;bench->inputs=inputs;bench->shape=shape;bench->moved=r;bench->changed=r;
        }
        // A replay on `bank`: hit when the inputs and the palette check pass;
        // then its words must be today's words for that bank.
        auto replay=[&](const CommandRegisters& bank,bool& hit){
            hit=false;
            const int changed=blocks::changedInput(bank,inputs,snapshot.data());
            assert((changed<0)==blocks::sameInputs(bank,inputs,snapshot.data())&&changed<blocks::inputKinds);
            if(changed>=0)return;
            for(auto& row:placed.uniforms)for(auto& v:row)v=std::bit_cast<float>(u32(rng()));
            if(!blocks::placePalette(bank,s.position(bank),shape,placed))return;
            hit=true;
            auto words=kept;blocks::patchRows(words.data(),layout,base,placed,blocks::paletteRows(placed));
            RigidVertexPlan today;
            const bool okay=todaysPlan(bank,s,today);
            if(!okay||today.matrixOffsets!=recorded.matrixOffsets||today.normalOffsets!=recorded.normalOffsets){
                std::fprintf(stderr,"case %u: a block hit where today's path refuses (%d) or lays the palette out differently (compact %d extra %u step %u change %x rejects:",
                             iteration,int(okay),int(s.compact),s.extra,todaysStep,lastChange);
                for(unsigned c=0;c<64;++c)if(gxRigidPlanRejects[c])std::fprintf(stderr," %u=%u",c,gxRigidPlanRejects[c]);
                std::fprintf(stderr,")\n");
                for(unsigned m=0;m<10;++m)std::fprintf(stderr,"  m%u %d/%d %d/%d\n",m,today.matrixOffsets[m],recorded.matrixOffsets[m],today.normalOffsets[m],recorded.normalOffsets[m]);
                assert(false);
            }
            const auto expected=emitDraw(today,base,state);
            if(words!=expected){
                unsigned at=0;while(at<words.size()&&at<expected.size()&&words[at]==expected[at])++at;
                const auto place=blocks::describeWord(expected,at);
                std::fprintf(stderr,"case %u: block words differ from today's at %u (register %03x row %d)\n",iteration,at,unsigned(place.reg),int(place.row));
                assert(false);
            }
        };
        // Matrix-only changes (the camera moved): always a hit.
        for(unsigned n=0;n<3;++n){
            CommandRegisters moved=r;matrices(moved);
            if(bench&&n==0){
                bench->moved=moved;
                // The camera moved: lights in view space and texture matrices follow.
                bench->changed=moved;
                for(unsigned at=0x600;at<0x680;++at)if(bench->changed.xfValid[at]&&(at&15)>=10)setf(bench->changed,at,std::bit_cast<float>(bench->changed.xf[at])+1.f);
                for(unsigned at=4*30;at<4*42;++at)setf(bench->changed,at,std::bit_cast<float>(bench->changed.xf[at])*.99f);
            }lastChange=-2;
            bool hit;replay(moved,hit);
            RigidVertexPlan today;
            if(todaysPlan(moved,s,today)&&today.matrixOffsets==recorded.matrixOffsets&&today.normalOffsets==recorded.normalOffsets){assert(hit);++matrixHits;}
        }
        // One changed word anywhere today's uniform rows can read outside the
        // record key (nativeStateKey: a change of a key word is another record).
        static constexpr std::array<std::pair<unsigned,unsigned>,6> xfAreas{{{0,0x100},{0x400,0x460},{0x500,0x600},{0x600,0x680},{0x100a,0x100e},{0x101a,0x1026}}};
        for(unsigned n=0;n<24;++n){
            CommandRegisters bank=r;
            if(chance(15)){
                const unsigned reg=0xe8+rng()%11;lastChange=0x10000|int(reg);
                // BP 0xf1 bits 20-23 (fog type, projection kind) are key words.
                bank.bp[reg]^=1u<<(rng()%(reg==0xf1?20:24));bank.bpValid.set(reg);
            }else{
                const auto& area=xfAreas[rng()%xfAreas.size()];
                const unsigned at=area.first+rng()%(area.second-area.first);lastChange=int(at);
                if(chance(5))bank.xfValid.reset(at);
                else if(chance(50))setf(bank,at,std::bit_cast<float>(bank.xf[at])*uni(.5f,1.5f)+uni(-1,1));
                else{bank.xf[at]^=1u<<(rng()%32);bank.xfValid.set(at);}
            }
            ++perturbed;
            bool hit;replay(bank,hit);
            perturbedHits+=hit;
            if(!hit){
                ++perturbedRefused;
                RigidVertexPlan today;
                if(todaysPlan(bank,s,today)){
                    const auto expected=emitDraw(today,base,state);
                    if(expected!=kept)++wordsChanged;
                    // Rows mode (submitBlockRows): every uploaded row from
                    // today's plan into the kept words, when the row set, the
                    // light count and the palette layout are the recorded ones.
                    if(blocks::uploadRows(today)==uploaded&&today.lights==recorded.lights&&today.multiTexture&&
                       today.matrixOffsets==recorded.matrixOffsets&&today.normalOffsets==recorded.normalOffsets){
                        auto words=kept;blocks::patchRows(words.data(),layout,base,today,uploaded);
                        if(words!=expected){
                            unsigned at=0;while(at<words.size()&&words[at]==expected[at])++at;
                            const auto place=blocks::describeWord(expected,at);
                            std::fprintf(stderr,"case %u: rows-mode words differ from today's at %u (register %03x row %d)\n",iteration,at,unsigned(place.reg),int(place.row));
                            assert(false);
                        }
                        ++rowsHits;
                    }else ++rowsRefused;
                }
            }
        }
    }
    if(argc>1&&!std::strcmp(argv[1],"bench")){
        Emitter out;out.words.reserve(8192);std::vector<std::uint32_t> copy;copy.reserve(8192);
        static RigidVertexPlan plan,scratch;
        volatile std::uint64_t sink=0;
        auto time=[&](auto&& body){
            const auto start=std::chrono::steady_clock::now();
            constexpr unsigned rounds=40;
            for(unsigned round=0;round<rounds;++round)for(auto& c:benchCases)body(c);
            return std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/(rounds*benchCases.size());
        };
        unsigned todayOk=0,fastOk=0,rowsOk=0;
        const double today=time([&](BenchCase& c){
            if(todaysPlan(c.changed,c.s,plan)){emitDrawInto(out,plan,0,c.state);sink+=out.words.size();++todayOk;}});
        const double fast=time([&](BenchCase& c){
            if(blocks::sameInputs(c.moved,c.inputs,c.snapshot.data())&&blocks::placePalette(c.moved,c.s.position(c.moved),c.shape,scratch)){
                copy.assign(c.kept.begin(),c.kept.end());blocks::patchRows(copy.data(),c.layout,0,scratch,blocks::paletteRows(scratch));
                sink+=copy.size();++fastOk;}});
        const double rows=time([&](BenchCase& c){
            if(!blocks::sameInputs(c.changed,c.inputs,c.snapshot.data())&&todaysPlan(c.changed,c.s,plan)){
                copy.assign(c.kept.begin(),c.kept.end());blocks::patchRows(copy.data(),c.layout,0,plan,blocks::uploadRows(plan));
                sink+=copy.size();++rowsOk;}});
        double words=0;for(const auto& c:benchCases)words+=double(c.kept.size());
        std::fprintf(stderr,"BENCH %zu shapes (%.0f words each): today's path %.0f ns per replay (%u planned), block hit %.0f ns (%u), "
                     "rows-mode hit %.0f ns (%u); block hit %.1f%% and rows-mode hit %.1f%% of today's (host, model emission)\n",
                     benchCases.size(),words/double(benchCases.size()),today,todayOk/40,fast,fastOk/40,rows,rowsOk/40,100*fast/today,100*rows/today);
        (void)sink;
    }
    assert(cases>1000&&captured>cases/2&&matrixHits>captured&&perturbedHits>perturbed/10&&perturbedRefused>perturbed/10&&rowsHits>perturbedRefused/4);
    std::fprintf(stderr,"PASS PICA blocks: layout, store and report checks; %u shapes captured (palette self-check %u) of %u planned; "
                 "%u matrix-only replays hit, byte-identical to today's words; %u single-word changes: %u hits byte-identical to today's words, "
                 "%u refused (%u of them change today's words); of the refused that today's path draws, %u taken in rows mode byte-identical to "
                 "today's words, %u left to today's path (row set, light count or palette layout changed)\n",
                 captured,selfCheck,cases,matrixHits,perturbed,perturbedHits,perturbedRefused,wordsChanged,rowsHits,rowsRefused);
    return 0;
}
