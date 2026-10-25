#pragma once
// Existing backend-only mock fixtures have no registry or GPU queue. Keep the
// real fused kernel/planner linked, but forbid entering its platform boundary.
#if defined(__3DS__)
#error Host-only OFF fixture must never be used in native code
#endif
#include "../gx_texture_pica.hpp"
#include "../runtime/gpu_sync.hpp"
#include <3ds/types.h>
#include <cassert>
namespace dusk::native {
static void unexpectedFusedOffService(){
    assert(!fusedColorCopyEnabled && "backend-only fixture requires fused copy OFF");
    assert(false && "OFF must not call a fused-only platform/registry service");
}
OSThread* platformGpuOwner(){unexpectedFusedOffService();return nullptr;}
GpuCompletion platformQueryGpu(){unexpectedFusedOffService();return GpuCompletion::notReady;}
namespace gx {
bool textureCommandToken(const void*,std::uint32_t&){unexpectedFusedOffService();return false;}
bool textureFromCommandToken(std::uint32_t,Texture&){unexpectedFusedOffService();return false;}
}
}
extern "C" OSThread* OSGetCurrentThread(){dusk::native::unexpectedFusedOffService();return nullptr;}
extern "C" Result GSPGPU_FlushDataCache(const void*,u32){dusk::native::unexpectedFusedOffService();return -1;}
