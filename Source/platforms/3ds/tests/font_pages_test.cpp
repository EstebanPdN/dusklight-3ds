#include "../runtime/font_pages.hpp"
#include "../runtime/gx_state.hpp"
#include "../runtime/gpu_sync.hpp"
#include "../runtime/texture_gx.hpp"
#include <dolphin/os.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <vector>
namespace {OSThread record{};bool renderOwner=true;unsigned completions{},releases{};}
extern "C" OSThread* OSGetCurrentThread(){return &record;}
namespace dusk::native {
OSThread* platformGpuOwner(){return renderOwner?&record:nullptr;}
GpuCompletion platformCompleteGpu(std::uint64_t){++completions;return renderOwner?GpuCompletion::complete:GpuCompletion::wrongThread;}
bool releaseGxTexture(gx::TextureHandle){++releases;return true;}
}
using namespace dusk::native;
void put32(std::vector<std::uint8_t>& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-i*8);}
int main(){
    std::vector<std::uint8_t> gly(96);
    put32(gly,0,0x474c5931);put32(gly,4,gly.size());put32(gly,16,32);
    gly[27]=gly[29]=8;gly[23]=gly[25]=1;
    std::fill(gly.begin()+32,gly.begin()+64,0x7f);std::fill(gly.begin()+64,gly.end(),0x24);
    int fontA{},fontB{};std::array<unsigned char,64> object{};
    gx::initTextureObject(object.data(),gly.data()+32,8,8,0,0,0,0,false);
    gx::setTextureObjectLod(object.data(),1,1,0,0,0,0,0,0);
    assert(registerFontPage(&fontA,gly,0));assert(fontPageBytes()==64);
    assert(registerFontPage(&fontA,gly,0)&&fontPageBytes()==64);
    assert(registerFontPage(&fontB,gly,0)&&fontPageBytes()==64);
    gx::loadTextureObject(object.data(),0);gx::Texture texture{};
    assert(gx::textureBinding(0,texture)&&texture.width==8&&texture.height==8&&texture.immutablePixels);
    std::uint32_t token{};assert(gx::textureCommandToken(gly.data()+32,token));
    NativeTexturePlan plan{};assert(inspectNativeTexture(texture.bti,plan));
    std::vector<std::uint8_t> pixels(plan.outputBytes);assert(decodeNativeTexture(texture.bti,plan,pixels));
    assert(pixels[0]==0x77&&pixels[1]==0xff);
    assert(registerFontPage(&fontA,gly,1)&&fontPageBytes()==128);
    assert(!registerFontPage(&fontA,gly,2));
    assert(releaseFontPages(&fontA)&&fontPageBytes()==64&&releases==1);
    assert(gx::textureFromCommandToken(token,texture));
    renderOwner=false;assert(!releaseFontPages(&fontB)&&fontPageBytes()==64);
    renderOwner=true;assert(releaseFontPages(&fontB)&&fontPageBytes()==0&&releases==2);
    assert(!gx::textureFromCommandToken(token,texture));assert(!gx::textureBinding(0,texture));
    gx::destroyTextureObject(object.data());
    auto malformed=gly;put32(malformed,16,31);assert(!registerFontPage(&fontA,malformed,0));
    malformed=gly;put32(malformed,4,128);assert(!registerFontPage(&fontA,malformed,0));
    malformed=gly;malformed[21]=8;assert(!registerFontPage(&fontA,malformed,0));
    std::printf("Validated original page alias, tiled pixels, shared owners, bounds and completed-GPU release (%u completions)\n",completions);
}
