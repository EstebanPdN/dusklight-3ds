#!/usr/bin/env python3
"""Host-only actual alpha backend + framebuffer owner integration and regressions."""
import hashlib
import json
import os
from pathlib import Path
import resource
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'Source/platforms/3ds'
OUT = ROOT / 'Diagnostics/implementation-44/alpha-ownership/host'
SDK = ROOT / 'Upstream/extracted'

HARNESS = r'''
namespace {
struct OwnedSurface {
    std::vector<u32> color,depth;C3D_RenderTarget target{};
    OwnedSurface(unsigned w,unsigned h):color(w*h),depth(w*h){
        auto& f=target.frameBuf;f.width=h;f.height=w;f.colorBuf=color.data();f.depthBuf=depth.data();
        f.colorFmt=GPU_RB_RGBA8;f.depthFmt=GPU_RB_DEPTH24_STENCIL8;f.colorMask=15;f.depthMask=3;
        target.ownsColor=target.ownsDepth=true;
    }
};
std::vector<std::unique_ptr<OwnedSurface>> ownedSurfaces;
OSThread* caller=reinterpret_cast<OSThread*>(0x100);
OSThread* gpuOwner=caller;
unsigned binds{},clears{},splits{},copies{},waits{},invalidates{},getters{},creates{},outputs{};
unsigned failWait{},loseOwnerAt{};
bool failBind{},failCreate{},probeActiveGuards{},failInvalidation{};
C3D_RenderTarget* ownerLossTarget{};
std::vector<char> order;
struct Pending {void* dest{};const void* source{};std::size_t bytes{};} pending;
std::array<std::byte,320*240*2> bottom;
struct Phys {const void* p{};u32 address{};};std::array<Phys,128> physicals;
unsigned physicalCount{};
void guardProbe();
unsigned effects(){return binds+clears+splits+copies+waits+invalidates+creates+outputs;}
bool owns(const AlphaIntervalPolicy& p,C3D_RenderTarget& t){
    const auto& f=t.frameBuf;return p.ownsTarget({f.colorBuf,f.depthBuf,f.width,f.height},p.user);
}
template<class F>void mustThrow(F fn){bool threw=false;try{fn();}catch(const std::runtime_error&){threw=true;}assert(threw);}
}
extern "C" {
OSThread* OSGetCurrentThread(){return caller;}
u32 osConvertVirtToPhys(const void* p){
    for(unsigned i=0;i<physicalCount;++i)if(physicals[i].p==p)return physicals[i].address;
    assert(physicalCount<physicals.size());const u32 addr=0x10000000+(physicalCount+1)*0x800000;
    physicals[physicalCount++]={p,addr};return addr;
}
u64 svcGetSystemTick(){return 1;}
u32* gpuCmdBuf{};u32 gpuCmdBufSize{},gpuCmdBufOffset{};
void* linearAlloc(size_t n){return std::malloc(n);}
Result GSPGPU_InvalidateDataCache(const void*,u32){++invalidates;order.push_back('I');assert(!pending.bytes);return failInvalidation?-1:0;}
u8* gfxGetFramebuffer(gfxScreen_t,gfx3dSide_t,u16* w,u16* h){if(w)*w=240;if(h)*h=320;return reinterpret_cast<u8*>(bottom.data());}
void gfxScreenSwapBuffers(gfxScreen_t,bool){++outputs;}
C3D_RenderTarget* C3D_RenderTargetCreate(int h,int w,GPU_COLORBUF,C3D_DEPTHTYPE){
    ++creates;if(failCreate)return nullptr;
    ownedSurfaces.push_back(std::make_unique<OwnedSurface>(w,h));return &ownedSurfaces.back()->target;
}
bool C3D_FrameDrawOn(C3D_RenderTarget* t){++binds;if(failBind)return false;frame=t->frameBuf;return true;}
void C3D_FrameSplit(u8 flags){assert(flags==GX_CMDLIST_FLUSH);++splits;order.push_back('S');}
void C3D_FrameBufClear(C3D_FrameBuf* f,C3D_ClearBits bits,u32 c,u32 d){
    assert(bits==C3D_CLEAR_ALL);++clears;order.push_back('C');
    std::fill_n(static_cast<u32*>(f->colorBuf),std::size_t(f->width)*f->height,c);
    std::fill_n(static_cast<u32*>(f->depthBuf),std::size_t(f->width)*f->height,d);
}
void C3D_RenderTargetSetOutput(C3D_RenderTarget*,gfxScreen_t,gfx3dSide_t,u32){++outputs;}
void C3D_SyncDisplayTransfer(u32* src,u32 dim,u32* dst,u32,u32){
    assert(!pending.bytes);++copies;order.push_back('T');pending={dst,src,std::size_t(dim>>16)*(dim&0xffff)*4};
}
void C3D_SyncTextureCopy(u32* src,u32,u32* dst,u32,u32 bytes,u32){
    assert(!pending.bytes);++copies;order.push_back('T');pending={dst,src,bytes};
}
}
namespace dusk::native {
OSThread* platformGpuOwner(){return gpuOwner;}
[[noreturn]] void fatal(const char* text){throw std::runtime_error(text);}
}
namespace {
void guardProbe(){
    const auto n=effects();auto handle=mainFramebuffer();std::span<const std::byte> out;
    assert(!alphaIntervalCopySafe());
    assert(!platformClearFramebuffer(handle,0,0)&&!platformBindFramebuffer(handle));
    assert(!platformCreateFramebuffer(16,8)&&!beginOffscreenFramebuffer(16,8));
    assert(!configureMainFramebuffer(608,448)&&!beginFramebufferRender()&&!clearFramebufferForRender(0,0));
    assert(!deferMainFramebufferClear(0,0)&&!endFramebufferFrame()&&!restoreMainFramebuffer());
    assert(!platformReadFramebufferColor(handle,out)&&out.empty());
    assert(!platformReadFramebufferDepthImage(handle,out)&&out.empty());
    u32 z=0xbadbad;assert(!platformReadFramebufferDepth(handle,1,1,z)&&z==0xbadbad);
    RetainedDepthProbe probe;DepthProbeCopy copy;
    assert(!platformRetainDepthReadback(handle,out,probe)&&!platformCaptureRetainedDepthB(probe,copy));
    assert(platformInvalidateRetainedDepthA()==-1&&!platformReleaseRetainedDepth());
    mustThrow([]{updateVideoPresentation();});assert(effects()==n);
}
void ledgerUnit(){
    bool on=true;assert(parseAlphaIntervalConfig("enabled=0\n",on)&&!on);
    assert(parseAlphaIntervalConfig("enabled=1\n",on)&&on);
    assert(parseAlphaIntervalConfig("enabled=1\r\n",on)&&on);
    for(auto text:{"", "1", "enabled=1", "enabled=2\n", "enabled=1\n ", " enabled=1\n", "enabled=1\nenabled=0\n"})assert(!parseAlphaIntervalConfig(text,on)&&!on);
    const std::string nul("enabled=1\n\0",11);assert(!parseAlphaIntervalConfig(nul,on));
    AlphaIntervalTargetLedger l;
    AlphaIntervalStorage s{{reinterpret_cast<void*>(0x1000),reinterpret_cast<void*>(0x2000),16,8},0x3000,0x4000};
    assert(l.add(reinterpret_cast<void*>(1),s));l.owner=caller;l.enabled=true;
    assert(!l.owns(reinterpret_cast<void*>(1),s,caller));l.didBind(reinterpret_cast<void*>(1));
    assert(!l.owns(reinterpret_cast<void*>(1),s,caller));l.didClear(reinterpret_cast<void*>(1),0xaa123456);
    assert(l.owns(reinterpret_cast<void*>(1),s,caller));
    auto alias=s;alias.target.color=reinterpret_cast<void*>(0x5000);alias.target.depth=reinterpret_cast<void*>(0x6000);
    assert(!l.add(reinterpret_cast<void*>(2),alias)); // physical aliases refused
    const auto generation=l.find(reinterpret_cast<void*>(1))->generation;
    l.release();assert(!l.owns(reinterpret_cast<void*>(1),s,caller));
    assert(l.add(reinterpret_cast<void*>(1),s)&&l.find(reinterpret_cast<void*>(1))->generation>generation);
    l.owner=caller;l.enabled=true;l.didBind(reinterpret_cast<void*>(1));assert(!l.owns(reinterpret_cast<void*>(1),s,caller));
    l.didClear(reinterpret_cast<void*>(1),0xab123456);assert(!l.owns(reinterpret_cast<void*>(1),s,caller));
    l.didClear(reinterpret_cast<void*>(1),0xaa123456);assert(l.owns(reinterpret_cast<void*>(1),s,caller));
    assert(!l.owns(reinterpret_cast<void*>(1),s,reinterpret_cast<void*>(2)));
    auto changed=s;changed.depthPhysical+=8;assert(!l.owns(reinterpret_cast<void*>(1),changed,caller));
    for(unsigned i=1;i<9;++i){auto v=s;const auto k=i*0x10000;
        v.target.color=reinterpret_cast<void*>(0x1000+k);v.target.depth=reinterpret_cast<void*>(0x2000+k);
        v.colorPhysical+=k;v.depthPhysical+=k;assert(l.add(reinterpret_cast<void*>(i+1),v));}
    assert(!l.canRegister());
    std::puts("PASS exact config grammar, bounded nine-slot ledger, virtual/physical aliases and monotonic reuse generations");
}
void ownerIntegration(){
    reset();releaseFramebufferRecords();OwnedSurface main(400,240);auto& target=main.target;
    assert(initializeFramebuffers(&target,400,240));AlphaIntervalPolicy policy{true,ownership,nullptr};
    assert(configureFramebufferAlphaIntervals(false,policy)&&!policy.enabled&&!policy.ownsTarget&&!alphaIntervalTargets.enabled);
    // Rejected setup must not confer evidence or install a callback.
    target.ownsDepth=false;assert(!configureFramebufferAlphaIntervals(true,policy)&&!policy.enabled);target.ownsDepth=true;
    const auto savedOwner=gpuOwner;gpuOwner=nullptr;assert(!configureFramebufferAlphaIntervals(true,policy));gpuOwner=savedOwner;
    assert(configureFramebufferAlphaIntervals(true,policy)&&policy.enabled&&policy.ownsTarget);
    assert(!owns(policy,target)&&beginFramebufferFrame()&&!owns(policy,target));
    const auto generation=alphaIntervalTargets.find(&target)->generation;
    assert(deferMainFramebufferClear(0x12345678,0xaa654321)&&!owns(policy,target));
    // A failed real geometry flush cannot consume the pending clear or certify zero.
    auto b=batch(1);samples={{150,.6f,.8f,true}};assert(submitGxGeometry(b));failFlush=true;
    const auto before=clears;assert(!beginFramebufferRender()&&clears==before&&!owns(policy,target));failFlush=false;
    assert(beginFramebufferRender()&&clears==before+1&&owns(policy,target));
    assert(main.depth.front()==0xaa654321&&main.depth.back()==0xaa654321);
    const auto record=*alphaIntervalTargets.find(&target);const auto effectsBefore=effects(),gettersBefore=getters,physBefore=physicalCount;
    const auto stateBefore=effect;
    for(unsigned i=0;i<1024;++i)assert(owns(policy,target));
    const auto after=*alphaIntervalTargets.find(&target);
    assert(record.generation==after.generation&&record.clearSerial==after.clearSerial&&record.wholeZero==after.wholeZero);
    assert(effectsBefore==effects()&&gettersBefore==getters&&physBefore==physicalCount&&std::memcmp(&stateBefore,&effect,sizeof(effect))==0);
    auto r=raster();r.alphaIntervalPolicy=policy;PicaMaterialPlan material;
    b=batch(1);samples={{150,.6f,.8f,true}};probeActiveGuards=true;
    assert(submitCommand(r,material,b)&&alphaIntervalCopySafe()&&owns(policy,target));probeActiveGuards=false;
    assert(configureMainFramebuffer(608,448)&&owns(policy,target));
    auto refuse=[&](auto mutate){auto old=target;mutate();const auto n=effects();assert(!owns(policy,target));
        assert(!platformBindFramebuffer(&target)&&!platformClearFramebuffer(&target,0,0));
        std::span<const std::byte> out;assert(!platformReadFramebufferColor(&target,out)&&!platformReadFramebufferDepthImage(&target,out));assert(effects()==n);target=old;};
    refuse([&]{target.ownsColor=false;});refuse([&]{target.ownsDepth=false;});
    refuse([&]{target.frameBuf.depthFmt=GPU_RB_DEPTH24;});refuse([&]{target.frameBuf.colorFmt=GPU_RB_RGB8;});
    refuse([&]{target.frameBuf.colorMask=7;});refuse([&]{target.frameBuf.depthMask=2;});refuse([&]{target.frameBuf.block32=true;});
    refuse([&]{target.frameBuf.depthBuf=target.frameBuf.colorBuf;});refuse([&]{target.frameBuf.width=16;});
    caller=reinterpret_cast<OSThread*>(0x200);assert(!owns(policy,target));caller=gpuOwner;
    // Existing offscreen initialization, failed bind, reuse, restoration and logical viewport extent.
    failCreate=true;assert(!beginOffscreenFramebuffer(31,17));failCreate=false;
    failBind=true;assert(!beginOffscreenFramebuffer(31,17)&&activeFramebuffer()==&target&&owns(policy,target));failBind=false;
    auto* off=&ownedSurfaces.back()->target;assert(alphaIntervalTargets.find(off)->wholeZero&&!owns(policy,*off));
    assert(beginOffscreenFramebuffer(31,17)&&activeFramebuffer()==off&&owns(policy,*off)&&!owns(policy,target));
    assert(currentTargetExtent.logicalWidth==31&&currentTargetExtent.logicalHeight==17&&currentTargetExtent.pixelWidth==32&&currentTargetExtent.pixelHeight==24);
    const auto offGeneration=alphaIntervalTargets.find(off)->generation,offClear=alphaIntervalTargets.find(off)->clearSerial;
    assert(restoreMainFramebuffer()&&owns(policy,target)&&currentTargetExtent.logicalWidth==608);
    assert(beginOffscreenFramebuffer(31,17)&&alphaIntervalTargets.find(off)->generation==offGeneration&&alphaIntervalTargets.find(off)->clearSerial==offClear+1);
    assert(restoreMainFramebuffer()&&owns(policy,target));
    // Clear bit zero evidence uses HIGH stencil byte; low depth bits are unrelated.
    assert(platformClearFramebuffer(&target,0,0xabffffff)&&!owns(policy,target)&&main.depth.front()==0xabffffff);
    assert(platformClearFramebuffer(&target,0,0xaaffffff)&&owns(policy,target)&&main.depth.front()==0xaaffffff);
    std::span<const std::byte> image;order.clear();assert(platformReadFramebufferDepthImage(&target,image));
    assert((order==std::vector<char>{'W','T','W','I'}));u32 word{};std::memcpy(&word,image.data(),4);assert(word==0xaaffffff);
    u32 z=99;assert(sampleFramebufferDepthImage(currentTargetExtent,image,0,0,z)&&z==0);
    order.clear();std::span<const std::byte> color;assert(platformReadFramebufferColor(&target,color));
    assert((order==std::vector<char>{'W','T','W','I'}));
    // The post-transfer fence must still be required before publishing CPU data.
    failWait=waits+2;image={};assert(!platformReadFramebufferDepthImage(&target,image)&&image.empty()&&pending.bytes);
    failWait=0;assert(platformCompleteGpu(1)==GpuCompletion::complete);
    failInvalidation=true;assert(!platformReadFramebufferDepthImage(&target,image)&&image.empty());failInvalidation=false;
    assert(platformReadFramebufferDepthImage(&target,image));RetainedDepthProbe retained;DepthProbeCopy copy;
    assert(platformRetainDepthReadback(&target,image,retained));
    const auto noCopy=copies;assert(!platformReadFramebufferDepthImage(&target,image)&&copies==noCopy);
    order.clear();assert(platformCaptureRetainedDepthB(retained,copy));
    assert((order==std::vector<char>{'W','T','W','I'})&&copy.priorCompletion==0&&copy.copyCompletion==0);
    assert(retained.imageA.size()==retainedDepthProbeBytes&&retained.imageB.size()==retainedDepthProbeBytes);
    assert(platformInvalidateRetainedDepthA()==0&&platformReleaseRetainedDepth());
    assert(deferMainFramebufferClear(0,0)&&endFramebufferFrame());assert(!owns(policy,target));
    const auto lastClears=clears;failBind=true;assert(!beginFramebufferFrame()&&clears==lastClears);failBind=false;
    assert(beginFramebufferFrame()&&clears==lastClears&&owns(policy,target));assert(beginFramebufferRender()&&clears==lastClears+1);
    assert(endFramebufferFrame());releaseFramebufferRecords();
    assert(!owns(policy,target));assert(initializeFramebuffers(&target,400,240));
    assert(configureFramebufferAlphaIntervals(true,policy)&&alphaIntervalTargets.find(&target)->generation>generation);
    assert(beginFramebufferFrame()&&!owns(policy,target));assert(clearFramebufferForRender(0,0)&&owns(policy,target));
    assert(platformReadFramebufferDepthImage(&target,image));RetainedDepthProbe fresh;
    assert(platformRetainDepthReadback(&target,image,fresh)&&fresh.targetGeneration!=retained.targetGeneration);
    const auto staleEffects=effects();assert(!platformCaptureRetainedDepthB(retained,copy)&&effects()==staleEffects);
    assert(platformCaptureRetainedDepthB(fresh,copy)&&platformReleaseRetainedDepth());
    // Real backend ownership loss after lower mask poisons. No operation may then
    // clear, rebind, allocate/copy, present, or erase the evidence of failure.
    reset();frame=target.frameBuf;r=raster();r.alphaIntervalPolicy=policy;b=batch(1);samples={{150,.6f,.8f,true}};
    ownerLossTarget=&target;loseOwnerAt=3;assert(!submitCommand(r,material,b)&&!alphaIntervalCopySafe());
    assert(emitted.size()==2);guardProbe();target.ownsDepth=true;loseOwnerAt=0;
    releaseFramebufferRecords();assert(!alphaIntervalCopySafe()&&!owns(policy,target));
    assert(!initializeFramebuffers(&target,400,240));destroyGxPica();assert(!alphaIntervalCopySafe());
    releaseFramebufferReadback();
    std::puts("PASS real owner callback + actual four-pass backend, initial/pending/failed clears, offscreen restore/reuse, retained D24S8, unchanged transfer fences, poison containment");
}
}
int main(){truthTests();plannerTests();sequenceTests();failures();ledgerUnit();ownerIntegration();destroyGxGeometry();}
'''


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    stub = OUT/'stubs';stub.mkdir(exist_ok=True)
    (stub/'3ds.h').write_text((NATIVE/'tests/alpha_interval_stubs/3ds.h').read_text()+r'''
extern "C" {
#include <3ds/gpu/gx.h>
#include <3ds/gpu/gpu.h>
u32 osConvertVirtToPhys(const void*);
u64 svcGetSystemTick();
Result GSPGPU_FlushDataCache(const void*,u32);
Result GSPGPU_InvalidateDataCache(const void*,u32);
}
''')
    (stub/'citro3d.h').write_text((NATIVE/'tests/alpha_interval_stubs/citro3d.h').read_text()+'\nextern "C" {\n#include <c3d/renderqueue.h>\n}\n')
    original = (NATIVE/'tests/gx_alpha_interval_test.cpp').read_text()
    original = original.replace('#include "../gx_pica.cpp"','#include "'+str(NATIVE/'gx_pica.cpp')+'"\n#include "runtime/alpha_interval_target.hpp"\n#include "runtime/gx_framebuffer.hpp"\n#include "runtime/video_mode.hpp"\n#include <memory>\n#include <stdexcept>')
    # Declarations for the extra SDK mocks must precede the inherited mock bodies.
    marker='// Mock SDK: official declarations, enum values, and inline helpers are used.'
    declarations='''namespace {extern unsigned getters,waits,failWait,loseOwnerAt;extern bool probeActiveGuards;extern C3D_RenderTarget* ownerLossTarget;void guardProbe();void completePending();void waitEvent();}\n'''
    original=original.replace(marker,declarations+marker)
    original=original.replace('C3D_FrameBuf* C3D_GetFrameBuf(){return &frame;}','C3D_FrameBuf* C3D_GetFrameBuf(){++getters;return &frame;}')
    original=original.replace('emitted.push_back(tri);','emitted.push_back(tri);if(probeActiveGuards)guardProbe();')
    original=original.replace('GpuCompletion platformCompleteGpu(std::uint64_t){assert(flushGxGeometry());return GpuCompletion::complete;}', '''GpuCompletion platformCompleteGpu(std::uint64_t){
        ++waits;waitEvent();if(waits==failWait)return GpuCompletion::timedOut;
        assert(flushGxGeometry());completePending();return GpuCompletion::complete;
    }''')
    original=original.replace('++prepareCalls;if(callbackBatch)', '++prepareCalls;if(prepareCalls==loseOwnerAt&&ownerLossTarget)ownerLossTarget->ownsDepth=false;if(callbackBatch)')
    original=original[:original.index('int main(){truthTests();')]
    harness=OUT/'ownership.cpp';harness.write_text(original+HARNESS+r'''
namespace {void completePending(){if(pending.bytes){std::memcpy(pending.dest,pending.source,pending.bytes);pending={};}}void waitEvent(){order.push_back('W');}}
''')
    common=['clang++','-std=c++20','-DTARGET_PC=1','-DDUSK_TARGET_3DS=1',
            '-I'+str(ROOT/'Source/extern/aurora/include'),'-I'+str(NATIVE),'-I'+str(NATIVE/'runtime'),
            '-Wall','-Wextra','-Werror','-Wno-switch','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
    nativeflags=['-I'+str(stub),'-I'+str(SDK/'libctru-2.7.0/libctru/include'),'-I'+str(SDK/'citro3d-1.7.1/include'),'-ffunction-sections','-fdata-sections','-Wl,-dead_strip']
    sources=[harness]+[NATIVE/p for p in ('gx_geometry_pica.cpp','runtime/gx_gpu_geometry.cpp','runtime/gx_raster_plan.cpp','runtime/gx_pica_math.cpp','runtime/gx_framebuffer.cpp','runtime/gx_framebuffer_ctru.cpp')]
    commands=[]
    env={**os.environ,'ASAN_OPTIONS':'halt_on_error=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    inputs={Path(__file__).resolve(),NATIVE/'tests/gx_alpha_interval_test.cpp',NATIVE/'runtime/game_startup.cpp'}
    def run(name,cmd,negative=None):
        if name.endswith('-build'):
            command=list(map(str,cmd));at=command.index('-o')
            depcmd=[v for i,v in enumerate(command) if i not in (at,at+1) and not v.startswith('-Wl,')]+['-MM']
            output=run(name+'-dependencies',depcmd)
            for token in shlex.split(output.replace('\\\n',' ')):
                if not token.endswith(':'):inputs.add(Path(token).resolve())
            for path,digest in hashes().items():
                prior=receipt.setdefault('input_sha256',{}).setdefault(path,digest)
                assert prior==digest,'Compiler input drift: '+path
        r=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=90,env=env)
        output=r.stdout+r.stderr;(OUT/(name+'.log')).write_text(output)
        commands.append({'name':name,'argv':list(map(str,cmd)),'returncode':r.returncode})
        if negative:
            assert r.returncode!=0 and 'Assertion failed' in output and negative in output,output
            assert 'ERROR: AddressSanitizer' not in output and 'runtime error:' not in output,output
        elif r.returncode:
            print(output[-9000:]);r.check_returncode()
        print(name+': '+('expected assertion' if negative else 'PASS'),flush=True)
        return output
    # Compile negatives from isolated copies, leaving live source untouched.
    ledger=NATIVE/'runtime/alpha_interval_target.hpp'
    before=ledger.read_text()
    controls=OUT/'controls';controls.mkdir(exist_ok=True)
    control_specs=[('no-zero','r->wholeZero&&r->clearSerial&&r->canonical','r->canonical','!l.owns'),
                   ('reset-generation','records={};bound=nullptr;','nextGeneration=0;records={};bound=nullptr;','generation>generation')]
    cases=[]
    for name,old,new,assertion in control_specs:
        dest=controls/name;dest.mkdir(exist_ok=True)
        assert before.count(old)==1
        (dest/'alpha_interval_target.hpp').write_text(before.replace(old,new))
        modified=[]
        for src in sources:
            if src==harness or src.name in ('gx_framebuffer.cpp','gx_framebuffer_ctru.cpp'):
                copy=dest/src.name
                text=src.read_text().replace('"runtime/alpha_interval_target.hpp"','"'+str(dest/'alpha_interval_target.hpp')+'"').replace('"alpha_interval_target.hpp"','"'+str(dest/'alpha_interval_target.hpp')+'"')
                # Local relative native includes retain their exact original target.
                if src!=harness:text=text.replace('"../gx_pica.hpp"','"'+str(NATIVE/'gx_pica.hpp')+'"').replace('"../gx_geometry_pica.hpp"','"'+str(NATIVE/'gx_geometry_pica.hpp')+'"')
                copy.write_text(text);modified.append(copy)
            else:modified.append(src)
        cases.append((name,modified,assertion))
    ctr=NATIVE/'runtime/gx_framebuffer_ctru.cpp'
    unguarded=controls/'unguarded-clear.cpp';text=ctr.read_text()
    start=text.index('bool platformClearFramebuffer(');end=text.index('bool platformReadFramebufferColor(',start)
    text=text[:start]+text[start:end].replace('!alphaIntervalCopySafe()||','').replace('||!alphaIntervalCopySafe()','')+text[end:]
    text=text.replace('"../gx_pica.hpp"','"'+str(NATIVE/'gx_pica.hpp')+'"').replace('"../gx_geometry_pica.hpp"','"'+str(NATIVE/'gx_geometry_pica.hpp')+'"')
    unguarded.write_text(text)
    cases.append(('unguarded-clear',[unguarded if p==ctr else p for p in sources],'!platformClearFramebuffer'))
    receipt={'status':'running','commands':commands,'scope':'Host ASan/UBSan; real gx_pica, geometry, framebuffer C++ and C3D owner callback. SDK operations mocked with official SDK declarations. No native execution.'}
    def hashes():return {str(p.relative_to(ROOT) if p.is_relative_to(ROOT) else p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(inputs)}
    try:
        deps=run('dependencies',[*[x for x in common+nativeflags if not x.startswith('-Wl,')],'-MM',*sources])
        for token in shlex.split(deps.replace('\\\n',' ')):
            if not token.endswith(':'):inputs.add(Path(token).resolve())
        receipt['input_sha256']=hashes()
        run('ownership-build',[*common,*nativeflags,*sources,'-o',OUT/'ownership'])
        run('ownership-test',[OUT/'ownership'])
        for name,units,assertion in cases:
            run(name+'-build',[*common,*nativeflags,*units,'-o',OUT/name]);run(name+'-test',[OUT/name],assertion)
        startup=NATIVE/'runtime/game_startup.cpp'
        text=startup.read_text();start=text.index('bool readAlphaIntervalConfig(){');end=text.index('\nstd::size_t readTextureHalfPolicy',start)
        config=OUT/'config.cpp';config.write_text('#include "runtime/alpha_interval_target.hpp"\n#include <cstdio>\n#include <cerrno>\n#include <cassert>\n#include <stdexcept>\nnamespace dusk::native {[[noreturn]] void fatal(const char* s){throw std::runtime_error(s);}\n'+text[start:end]+r"""
}
int main(int argc,char** argv){assert(argc==2);bool threw=false,on=false;try{on=dusk::native::readAlphaIntervalConfig();}catch(const std::runtime_error&){threw=true;}
assert(threw==(argv[1][0]=='E'));if(!threw)assert(on==(argv[1][0]=='1'));}
""")
        run('config-build',[*common,config,'-o',OUT/'config'])
        for i,(payload,result) in enumerate([(None,'0'),(b'enabled=0\n','0'),(b'enabled=1\n','1'),(b'enabled=1\r\n','1'),
                (b'','E'),(b'enabled=1','E'),(b'enabled=1\x00\n','E'),(b'enabled=1\n\n','E'),(b'enabled=1\n'+b' '*8192,'E')]):
            case=OUT/'config-cases'/str(i);sd=case/'sdmc:/3ds/dusklight';sd.mkdir(parents=True,exist_ok=True)
            flag=sd/'alpha-interval.txt'
            if payload is None:
                if flag.exists():flag.unlink()
            else:flag.write_bytes(payload)
            # Only the child working directory changes; no live SD path is used.
            executable=case/'run.py';executable.write_text('import os\nos.chdir('+repr(str(case))+')\nos.execv('+repr(str(OUT/'config'))+',["config",'+repr(result)+'])\n')
            run('config-case-'+str(i),['python3',executable])
        # Compare calls, exact arguments and ordering, including both original
        # post-copy waits and invalidations. Guards add no queue operations.
        baseline=ROOT/'Diagnostics/implementation-44/alpha-ownership/before'
        fence_names='platformCompleteGpu|C3D_FrameSplit|C3D_RenderTargetClear|C3D_SyncDisplayTransfer|C3D_SyncTextureCopy|GSPGPU_InvalidateDataCache|C3D_FrameEnd'
        def extract(source):
            calls=[]
            for match in re.finditer(r'\b(?:'+fence_names+r')\(',source):
                depth=1;end=match.end()
                while depth:
                    depth+=(source[end]=='(')-(source[end]==')');end+=1
                calls.append(source[match.start():end])
            return calls
        fence_receipt={}
        for path in (NATIVE/'runtime/gx_framebuffer_ctru.cpp',startup):
            old=(baseline/path.relative_to(ROOT)).read_text();now=path.read_text()
            assert extract(old)==extract(now),'Fence/transfer call drift: '+str(path)
            fence_receipt[str(path.relative_to(ROOT))]=extract(now)
        receipt['unchanged_fence_transfer_calls']=fence_receipt
        # Preserve the owner's existing broad adapter/no-skip regression suite;
        # reuse its recorded build recipe, with all outputs in this task folder.
        prior=json.loads((ROOT/'Diagnostics/implementation-44/alpha-interval/host/results.json').read_text())
        adapter=next(c['argv'] for c in prior['commands'] if c['name']=='adapter-build')
        adapter=list(adapter);adapter[adapter.index('-o')+1]=str(OUT/'adapter')
        run('adapter-build',adapter);run('adapter-test',[OUT/'adapter'])
        # Existing viewport/copy tests have no PICA backend. Supply only their
        # explicitly safe non-interval mock query; real poison was tested above.
        mock=OUT/'legacy_guard.cpp';mock.write_text('namespace dusk::native {bool legacySafe=true;bool alphaIntervalCopySafe(){return legacySafe;}}\n')
        viewport=[NATIVE/'runtime'/x for x in ('gx_framebuffer.cpp','gx_framebuffer_api.cpp','gx_stream.cpp','gx_commands.cpp','gx_vertex_decode.cpp','gx_vertex_transform.cpp','gx_raster_plan.cpp','gx_pica_math.cpp')]
        viewport += [NATIVE/'gx_command_api.cpp',ROOT/'Source/extern/aurora/lib/gx/dl.cpp',ROOT/'Source/extern/aurora/lib/gx/attr_fmt.cpp',NATIVE/'tests/framebuffer_viewport_test.cpp',mock]
        run('viewport-build',[*common,*viewport,'-o',OUT/'viewport']);run('viewport-test',[OUT/'viewport'])
        pixels=[NATIVE/'runtime'/x for x in ('gx_framebuffer.cpp','gx_copy_pixels.cpp','texture_gx.cpp','gx_pica_math.cpp')]+[NATIVE/'tests/framebuffer_copy_test.cpp',mock]
        run('pixels-build',[*common,*pixels,'-o',OUT/'pixels']);run('pixels-test',[OUT/'pixels'])
        assert receipt['input_sha256']==hashes(),'Compiler input drift during tests'
        receipt.update(status='pass',input_unchanged=True)
    except (OSError,subprocess.SubprocessError,AssertionError) as error:
        receipt.update(status='fail',error=str(error))
    (OUT/'results.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps({k:v for k,v in receipt.items() if k not in ('commands','input_sha256')},indent=2))
    return int(receipt['status']!='pass')
if __name__=='__main__':raise SystemExit(main())
