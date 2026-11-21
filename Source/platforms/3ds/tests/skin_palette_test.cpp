// Merged matrix groups of skinned shapes (runtime/skin_palette.hpp,
// runtime/gx_skin_palette.hpp), host checks:
// - every vertex of a merged list names the matrix its group's slot held
//   under GX's slot rules (a slot a group does not load keeps the previous
//   group's matrix), for hand-made shapes and 20,000 random ones;
// - merged lists keep the groups' primitives in order, byte for byte but the
//   matrix byte; runs keep the batch caps, the group cap and the capacity;
// - refusals: a slot no load of the shape filled, a non-triangle primitive;
// - the palette's rows: entry k at row(k) with the bank's words, nothing
//   written elsewhere, and for every light count and texture unit count the
//   rows of a full palette stay off the projection, colour, fog and light
//   rows and below the texture units' rows (the uniform budget);
// - the normal bound: for blends of rotations (envelopes), scaled and
//   mirrored ones, the angle between A n and A^-T n never exceeds the angle
//   whose sine normalSineSquared bounds.
// Run by Tools/3ds/test_skin_palette.py.
#include "../runtime/gx_skin_palette.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using namespace dusk::native;
namespace {
int failures=0;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x);++failures;}}while(0)

// A shape of the layout: matrix byte, position index16, normal index8.
constexpr skin::VertexLayout layout{4,0};
struct TestGroup {std::array<std::uint16_t,skin::slots> loads;std::vector<std::uint8_t> list;};
void primitive(std::vector<std::uint8_t>& list,std::uint8_t op,const std::vector<unsigned>& slots){
    list.push_back(op);list.push_back(std::uint8_t(slots.size()>>8));list.push_back(std::uint8_t(slots.size()));
    for(unsigned i=0;i<slots.size();++i){
        list.push_back(std::uint8_t(3*slots[i]));list.push_back(std::uint8_t(i>>8));list.push_back(std::uint8_t(i));list.push_back(std::uint8_t(i*7));
    }
}
std::vector<skin::GroupInput> inputs(const std::vector<TestGroup>& groups){
    std::vector<skin::GroupInput> in(groups.size());
    for(std::size_t g=0;g<groups.size();++g){in[g].list=groups[g].list.data();in[g].bytes=std::uint32_t(groups[g].list.size());in[g].loads=groups[g].loads;}
    return in;
}
// GX's view: the matrix (key) every vertex of every group reads.
template<class KeyOf> std::vector<std::uint32_t> gxKeys(const std::vector<TestGroup>& groups,KeyOf keyOf){
    std::vector<std::uint32_t> out;
    std::array<std::uint32_t,skin::slots> slot{};
    for(const auto& g:groups){
        for(unsigned s=0;s<skin::slots;++s)if(g.loads[s]!=0xffff)slot[s]=keyOf(g.loads[s]);
        for(std::size_t at=0;at<g.list.size();){
            if(!g.list[at]){++at;continue;}
            const unsigned count=(unsigned(g.list[at+1])<<8)|g.list[at+2];
            for(unsigned v=0;v<count;++v)out.push_back(slot[g.list[at+3+v*layout.stride]/3]);
            at+=3+count*layout.stride;
        }
    }
    return out;
}
// The merged lists' view: the palette entry every vertex names. Also checks
// that the lists hold the groups' primitives in order, the matrix byte aside.
std::vector<std::uint32_t> mergedKeys(const std::vector<TestGroup>& groups,const skin::ShapePlan& plan,unsigned capacity){
    std::vector<std::uint32_t> out;
    const auto in=inputs(groups);
    for(unsigned r=0;r<plan.runs;++r){
        const auto& run=plan.run[r];
        CHECK(run.matrices<=capacity);CHECK(run.groups>=1&&run.groups<=skin::maxRunGroups);
        CHECK(run.vertices<=skin::batchVertices&&run.indices<=skin::batchIndices&&run.vertices*layout.stride<=skin::batchBytes);
        std::vector<std::uint8_t> list(skin::mergedBytes(run),0xee);
        CHECK(skin::writeMerged(in,layout,plan,r,list.data()));
        for(std::size_t i=run.listBytes;i<list.size();++i)CHECK(list[i]==0);
        std::vector<std::uint8_t> original;
        for(unsigned g=run.firstGroup;g<unsigned(run.firstGroup+run.groups);++g){
            CHECK(plan.runOf[g]==r);
            const auto& l=groups[g].list;
            for(std::size_t at=0;at<l.size();){
                if(!l[at]){++at;continue;}
                const unsigned count=(unsigned(l[at+1])<<8)|l[at+2];
                original.insert(original.end(),l.begin()+long(at),l.begin()+long(at+3+count*layout.stride));
                at+=3+count*layout.stride;
            }
        }
        CHECK(original.size()==run.listBytes);
        for(std::size_t at=0;at<run.listBytes&&at<original.size();){
            const unsigned count=(unsigned(list[at+1])<<8)|list[at+2];
            CHECK(list[at]==original[at]&&count==((unsigned(original[at+1])<<8)|original[at+2]));
            for(unsigned v=0;v<count;++v){
                const std::size_t vertex=at+3+v*layout.stride;
                const unsigned index=list[vertex];
                CHECK(index%3==0&&index/3<run.matrices);
                out.push_back(run.keys[index/3]);
                for(unsigned b=1;b<layout.stride;++b)CHECK(list[vertex+b]==original[vertex+b]);
            }
            at+=3+count*layout.stride;
        }
    }
    return out;
}
std::array<std::uint16_t,skin::slots> loads(std::initializer_list<std::pair<unsigned,unsigned>> set){
    std::array<std::uint16_t,skin::slots> l;l.fill(0xffff);
    for(auto [slot,use]:set)l[slot]=std::uint16_t(use);
    return l;
}

void handMade(){
    // Uses 3 and 13 are the same matrix; group 1 reads slots 3 and 5 kept from group 0.
    auto keyOf=[](std::uint16_t use)->std::uint32_t{return use==13?3u:use;};
    std::vector<TestGroup> groups(4);
    groups[0].loads=loads({{0,0},{1,1},{2,2},{3,3},{4,4},{5,5}});
    primitive(groups[0].list,0x98,{0,1,2,3,4,5,0});
    groups[1].loads=loads({{0,10},{1,11},{2,12}});
    primitive(groups[1].list,0x90,{0,1,3,5,2,3});
    groups[2].loads=loads({{0,13},{6,6},{7,7},{8,8},{9,9}});
    primitive(groups[2].list,0xa0,{0,6,7,8,9});
    groups[2].list.insert(groups[2].list.end(),5,0);  // NOP padding
    groups[3].loads=loads({{1,14},{2,15},{3,16},{4,17}});
    primitive(groups[3].list,0x80,{1,2,3,4,1,2,3,4});
    const auto in=inputs(groups);
    skin::ShapePlan plan;
    CHECK(skin::planShape(in,layout,keyOf,32,plan)==skin::Refusal::none);
    CHECK(plan.runs==1&&plan.run[0].groups==4);
    // Distinct matrices: 0-5, 10-12, 6-9 (13 is 3), 14-17.
    CHECK(plan.run[0].matrices==17);
    CHECK(mergedKeys(groups,plan,32)==gxKeys(groups,keyOf));
    CHECK(skin::planShape(in,layout,keyOf,10,plan)==skin::Refusal::none);
    CHECK(plan.runs==2&&plan.run[0].groups==2&&plan.run[1].groups==2);
    CHECK(mergedKeys(groups,plan,10)==gxKeys(groups,keyOf));
    // A slot no load of the shape filled.
    auto bad=groups;bad[0].loads=loads({{0,0},{1,1},{2,2},{4,4},{5,5}});
    CHECK(skin::planShape(inputs(bad),layout,keyOf,32,plan)==skin::Refusal::unloaded);
    // A line strip.
    bad=groups;bad[3].list.clear();primitive(bad[3].list,0xb0,{1,2,3});
    CHECK(skin::planShape(inputs(bad),layout,keyOf,32,plan)==skin::Refusal::list);
    // The batch caps: 3000 + 1500 vertices are two runs.
    std::vector<TestGroup> big(2);
    big[0].loads=loads({{0,0}});big[1].loads=loads({{1,1}});
    primitive(big[0].list,0x90,std::vector<unsigned>(3000,0));primitive(big[1].list,0x90,std::vector<unsigned>(1500,1));
    CHECK(skin::planShape(inputs(big),layout,keyOf,32,plan)==skin::Refusal::none&&plan.runs==2);
    CHECK(mergedKeys(big,plan,32)==gxKeys(big,keyOf));
}

void randomShapes(){
    std::mt19937 rng(20261120);
    auto pick=[&](unsigned n){return unsigned(rng()%n);};
    unsigned planned=0,vertices=0;
    for(unsigned trial=0;trial<20000;++trial){
        const unsigned groupCount=2+pick(40),capacity=10+pick(15),uses=12+pick(60);
        std::vector<std::uint32_t> key(uses);
        for(auto& k:key)k=pick(2)<<16|pick(uses);  // joint or envelope table, with repeats
        auto keyOf=[&](std::uint16_t use){return key[use];};
        std::vector<TestGroup> groups(groupCount);
        std::uint16_t filled=0;
        for(auto& g:groups){
            g.loads.fill(0xffff);
            for(unsigned s=0;s<skin::slots;++s)if(!filled||pick(3)==0){g.loads[s]=std::uint16_t(pick(uses));filled|=std::uint16_t(1u<<s);}
            const unsigned prims=1+pick(4);
            for(unsigned p=0;p<prims;++p){
                const std::uint8_t op=std::array<std::uint8_t,4>{0x90,0x98,0xa0,0x80}[pick(4)];
                unsigned count=op==0x90?3*(1+pick(20)):op==0x80?4*(1+pick(10)):3+pick(40);
                std::vector<unsigned> slots;
                for(unsigned v=0;v<count;++v){unsigned s;do s=pick(skin::slots);while(!(filled&(1u<<s)));slots.push_back(s);}
                primitive(g.list,op,slots);
                if(pick(4)==0)g.list.insert(g.list.end(),pick(8),0);
            }
        }
        skin::ShapePlan plan;
        const auto why=skin::planShape(inputs(groups),layout,keyOf,capacity,plan);
        CHECK(why==skin::Refusal::none);
        if(why!=skin::Refusal::none)continue;
        unsigned covered=0;for(unsigned r=0;r<plan.runs;++r)covered+=plan.run[r].groups;
        CHECK(covered==groupCount);
        const auto merged=mergedKeys(groups,plan,capacity);
        CHECK(merged==gxKeys(groups,keyOf));
        ++planned;vertices+=unsigned(merged.size());
    }
    std::fprintf(stderr,"random shapes planned %u, vertices compared %u\n",planned,vertices);
}

void setWord(gx::CommandRegisters& r,unsigned at,std::uint32_t v){r.xf[at]=v;r.xfValid.set(at);}
gx::CommandRegisters bankWith(std::uint32_t generation,unsigned entries){
    gx::CommandRegisters r{};
    setWord(r,skin::xfHeader,skin::headerMagic<<16|entries);setWord(r,skin::xfList,generation);
    for(unsigned k=0;k<entries;++k)for(unsigned i=0;i<12;++i)setWord(r,skin::xfMatrices+12*k+i,std::bit_cast<std::uint32_t>(float(100*k+i)+0.25f));
    return r;
}
gx::RigidVertexPlan planWith(unsigned lights,unsigned units){
    gx::RigidVertexPlan p{};
    p.multiTexture=true;p.lights=lights;p.multiRowUnits=std::uint8_t(units);
    for(auto& row:p.uniforms)row={-7,-7,-7,-7};
    return p;
}
void palettePlacement(){
    const std::uint32_t generation=skin::generationBit|5;
    for(unsigned units=1;units<=3;++units)for(unsigned lights=1;lights<=skin::maxLights;++lights){
        const unsigned capacity=skin::capacityForUnits(units);
        for(unsigned n=1;n<=capacity;++n){
            auto p=planWith(lights,units);auto r=bankWith(generation,n);
            gx::SkinRefusal why{};
            CHECK(gx::placeSkinPalette(r,generation,p,&why));
            CHECK(p.skinEntries==n&&p.matrices==n&&p.compactPalette);
            CHECK(p.uniformCount()<=p.paletteLimit()&&p.paletteLimit()<=94);
            std::vector<bool> written(94);
            for(unsigned k=0;k<n;++k){
                const unsigned row=skin::row(k);
                // Never the projection, colour, fog or light rows; never a unit row.
                CHECK(row+3<=6||(row>=19+4*lights&&row>=skin::paletteRow));
                CHECK(row+3<=p.paletteLimit());
                for(unsigned i=0;i<12;++i){
                    CHECK(p.uniforms[row+i/4][i%4]==float(100*k+i)+0.25f);
                    written[row+i/4]=true;
                }
                CHECK(gx::skinRecordRow(p,3*k)==int(row));
            }
            CHECK(gx::skinRecordRow(p,3*n)==-1&&gx::skinRecordRow(p,1)==-1);
            for(unsigned row=0;row<94;++row)if(!written[row])for(float v:p.uniforms[row])CHECK(v==-7);
        }
        // One entry more than fits.
        auto p=planWith(lights,units);auto r=bankWith(generation,capacity+1);
        gx::SkinRefusal why{};
        CHECK(!gx::placeSkinPalette(r,generation,p,&why)&&why==gx::SkinRefusal::rows);
    }
    gx::SkinRefusal why{};
    auto p=planWith(skin::maxLights+1,1);auto r=bankWith(generation,4);
    CHECK(!gx::placeSkinPalette(r,generation,p,&why)&&why==gx::SkinRefusal::lights);
    p=planWith(2,1);
    CHECK(!gx::placeSkinPalette(r,generation+1,p,&why)&&why==gx::SkinRefusal::header);
    p=planWith(2,1);p.multiProjected=true;
    CHECK(!gx::placeSkinPalette(r,generation,p,&why)&&why==gx::SkinRefusal::program);
    p=planWith(2,1);r.xfValid.reset(skin::xfMatrices+12*2+5);
    CHECK(!gx::placeSkinPalette(r,generation,p,&why)&&why==gx::SkinRefusal::matrix);
    p=planWith(2,1);r=bankWith(generation,3);r.xf[skin::xfMatrices+7]=std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity());
    CHECK(!gx::placeSkinPalette(r,generation,p,&why)&&why==gx::SkinRefusal::matrix);
    // Merged generations never meet vertex data or instance-batch lists.
    CHECK(skin::mergedGeneration(skin::generationBit|1)&&!skin::mergedGeneration(1)&&!skin::mergedGeneration(0x80000001u)&&
          !skin::mergedGeneration(0xc0000001u));
    CHECK(skin::inPaletteXf(skin::xfHeader,4+12*skin::maxMatrices)&&!skin::inPaletteXf(0xf0,0x20)&&!skin::inPaletteXf(0x3ff,2));
}
}
// A^-T (double) by cofactors and determinant.
bool inverseTranspose(const double a[3][3],double out[3][3]){
    const double det=a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])-a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])+a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]);
    if(std::abs(det)<1e-12)return false;
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j){
        const unsigned i1=(i+1)%3,i2=(i+2)%3,j1=(j+1)%3,j2=(j+2)%3;
        out[i][j]=(a[i1][j1]*a[i2][j2]-a[i1][j2]*a[i2][j1])/det; // cofactor C_ij / det = (A^-T)_ij
    }
    return true;
}
void rotation(std::mt19937& rng,double r[3][3],double maxAngle){
    std::normal_distribution<double> g;std::uniform_real_distribution<double> u(-maxAngle,maxAngle);
    double ax[3]={g(rng),g(rng),g(rng)};const double l=std::sqrt(ax[0]*ax[0]+ax[1]*ax[1]+ax[2]*ax[2]);
    for(double& x:ax)x/=l;
    const double t=u(rng),c=std::cos(t),sn=std::sin(t),k=1-c;
    const double x=ax[0],y=ax[1],z=ax[2];
    const double m[3][3]={{c+x*x*k,x*y*k-z*sn,x*z*k+y*sn},{y*x*k+z*sn,c+y*y*k,y*z*k-x*sn},{z*x*k-y*sn,z*y*k+x*sn,c+z*z*k}};
    std::memcpy(r,m,sizeof(m));
}
void normalBound(){
    std::mt19937 rng(42614);
    std::uniform_real_distribution<double> unit(0,1);
    std::normal_distribution<double> g;
    unsigned cases=0,bounded=0;double tightest=0,worstExcess=-1;
    for(unsigned trial=0;trial<40000;++trial){
        // A blend of 2-4 bone rotations (bent up to 120 degrees apart), then
        // the view and model: a rotation, a scale, sometimes non-uniform or
        // mirrored.
        double a[3][3]{};double sum=0;const unsigned bones=2+trial%3;double w[4];
        for(unsigned b=0;b<bones;++b){w[b]=unit(rng)+0.01;sum+=w[b];}
        for(unsigned b=0;b<bones;++b){double r[3][3];rotation(rng,r,trial%4==0?0.1:1.05);
            for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)a[i][j]+=w[b]/sum*r[i][j];}
        double v[3][3];rotation(rng,v,3.14159);
        double scale[3];const double s0=0.01+5*unit(rng);
        for(unsigned i=0;i<3;++i)scale[i]=s0*(trial%5==0?1+0.1*g(rng):1);
        if(trial%7==0)scale[0]=-scale[0];
        double m[3][3]{};
        for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)for(unsigned k=0;k<3;++k)m[i][j]+=v[i][k]*scale[k]*a[k][j];
        float f[3][4]{};for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)f[i][j]=float(m[i][j]);
        const float e2=skin::normalSineSquared(f);
        double it[3][3];
        if(!inverseTranspose(m,it)){CHECK(e2>=1);continue;}
        ++cases;
        if(e2>=1)continue;
        ++bounded;
        const double bound=std::sqrt(double(e2));
        double worst=0;
        for(unsigned n=0;n<64;++n){
            double d[3]={g(rng),g(rng),g(rng)};
            double p[3]{},q[3]{};
            for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j){p[i]+=m[i][j]*d[j];q[i]+=it[i][j]*d[j];}
            const double cr[3]={p[1]*q[2]-p[2]*q[1],p[2]*q[0]-p[0]*q[2],p[0]*q[1]-p[1]*q[0]};
            const double sine=std::sqrt(cr[0]*cr[0]+cr[1]*cr[1]+cr[2]*cr[2])/
                              (std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2])*std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]));
            const double dot=p[0]*q[0]+p[1]*q[1]+p[2]*q[2];
            CHECK(dot>0);  // never the opposite side (mirrored matrices too)
            worst=std::max(worst,sine);
        }
        // float rounding of e2 against the double geometry
        CHECK(worst<=bound*(1+1e-4)+1e-6);
        worstExcess=std::max(worstExcess,worst-bound);
        if(bound>0.01)tightest=std::max(tightest,worst/bound);
    }
    // The comment's example: a 50/50 blend bent 36.45 degrees sits at the limit.
    {
        const double h=36.45/2*3.14159265358979/180;
        float f[3][4]{{1,0,0,0},{0,float(std::cos(h)*std::cos(h)),0,0},{0,0,float(std::cos(h)*std::cos(h)),0}};
        // 0.5 (Rx(h) + Rx(-h)) = diag(1, cos h, cos h)
        f[1][1]=f[2][2]=float(std::cos(h));
        CHECK(std::abs(std::sqrt(skin::normalSineSquared(f))-skin::normalSineLimit)<2e-4f);
    }
    std::fprintf(stderr,"normal bound: %u invertible blends, %u bounded, worst angle at most %.3f of its bound (sines), never over it (largest excess %.2e)\n",
                 cases,bounded,tightest,worstExcess);
    CHECK(bounded>cases/4);
}
int main(){
    handMade();
    normalBound();
    randomShapes();
    palettePlacement();
    if(failures){std::fprintf(stderr,"%d failures\n",failures);return 1;}
    std::fprintf(stderr,"PASS skin palette\n");
    return 0;
}
