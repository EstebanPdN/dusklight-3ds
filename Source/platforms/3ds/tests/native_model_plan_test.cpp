#include "native/model_plan.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>
using namespace dusk::pica;
namespace {
unsigned word(const std::vector<std::byte>& d,unsigned at){unsigned v=0;for(unsigned n=0;n<4;++n)v|=std::to_integer<unsigned>(d[at+n])<<(8*n);return v;}
void put(std::vector<std::byte>& d,unsigned at,unsigned v){for(unsigned n=0;n<4;++n)d[at+n]=std::byte(v>>(8*n));}
void repair(std::vector<std::byte>& d){put(d,44,crc32(std::span(d).first(44),std::span(d).subspan(48)));}
}
int main(int argc,char** argv){
    assert(argc==3);std::ifstream file(argv[1],std::ios::binary);assert(file.good());std::vector<char> raw((std::istreambuf_iterator<char>(file)),{});std::vector<std::byte> bytes(raw.size());std::memcpy(bytes.data(),raw.data(),raw.size());auto plan=std::make_unique<ModelPlan>();
    assert(ModelPlan::open(bytes,*plan)==PackError::none&&plan->size()==2&&plan->uniformCount()==2);
    assert(plan->bindings(0).empty()&&plan->bindings(64).empty()&&plan->bindings(63).size()==2);
    for(unsigned bit:{1u,2u,4u,16u,32u})assert(plan->bindings(63^bit).empty());
    PartBinding part;unsigned pass;assert(plan->entry(0,part,pass)&&part.mesh==1&&part.material==1&&part.samplerMask==3&&part.samplers[1].lodBias==-152&&pass==0);
    assert(plan->uniform(0)->paletteCount==2&&plan->uniform(0)->palette[1]==2&&plan->uniform(1)->palette[1]==3&&plan->uniform(1)->anchor==1);
    assert(!plan->entry(2,part,pass)&&part.mesh==0&&!plan->uniform(2));
    // Every decoded binding is owned, including after unaligned file input and
    // mutation/release of the source metadata bytes.
    auto unaligned=std::vector<std::byte>(bytes.size()+1);std::memcpy(unaligned.data()+1,bytes.data(),bytes.size());assert(ModelPlan::open(std::span(unaligned).subspan(1),*plan)==PackError::none);
    std::fill(unaligned.begin(),unaligned.end(),std::byte{});assert(plan->entry(0,part,pass)&&part.material==1);
    unsigned rejected=0;
    for(unsigned size=0;size<bytes.size();++size){assert(ModelPlan::open(std::span(bytes).first(size),*plan)!=PackError::none&&plan->size()==0&&plan->bindings(63).empty());++rejected;}
    for(unsigned at=0;at<bytes.size();++at){auto bad=bytes;bad[at]^=std::byte{1};assert(ModelPlan::open(bad,*plan)!=PackError::none&&plan->size()==0);++rejected;}
    const auto slots=word(bytes,32);
    for(auto [at,value]:std::vector<std::pair<unsigned,unsigned>>{{24,64},{48,0},{52,0},{56,2},{60,8},{64,2},{68,1},{108,1},{slots,99},{slots+4,0},{slots+8,0},{slots+12,0},{slots+20,1},{slots+44,1}}){auto bad=bytes;put(bad,at,value);repair(bad);assert(ModelPlan::open(bad,*plan)!=PackError::none&&plan->size()==0);++rejected;}
    for(unsigned at:{72u,73u,74u,75u,76u,77u,78u,79u,82u,83u}){auto bad=bytes;bad[at]=std::byte{255};repair(bad);assert(ModelPlan::open(bad,*plan)!=PackError::none&&plan->size()==0);++rejected;}
    std::ifstream biases(argv[2]);assert(biases.good());unsigned cases=0;
    for(int rawBias=-32768;rawBias<32768;++rawBias){int expected;assert(biases>>expected);volatile float hundredths=1.0f/100.0f;float bias=float(rawBias)*hundredths;bias=bias<-4?-4:bias>=4?3.99f:bias;const int quantized=int(std::trunc(bias*32.0f));assert(quantized*8==expected);++cases;}
    std::printf("PASS owned native model plan, explicit producer gates, hierarchy bindings, %u malformed/truncated refusals and %u independent source-bias values\n",rejected,cases);
}
