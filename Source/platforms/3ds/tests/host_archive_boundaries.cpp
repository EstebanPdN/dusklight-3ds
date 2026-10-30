// Host-only execution boundaries for the real memory archive integration.
// Archives are mounted and validated, but this test never uploads/draws images.
#include "../runtime/gpu_sync.hpp"
#include "../gx_texture_pica.hpp"
#include <dolphin/os.h>
#include "JSystem/JUtility/JUTConsole.h"
#include <cassert>
#include <mutex>
namespace {
std::recursive_mutex exclusion;
thread_local bool enabled=true;
}
extern "C" BOOL OSDisableInterrupts() {
    const bool before=enabled;exclusion.lock();enabled=false;return before;
}
extern "C" BOOL OSRestoreInterrupts(BOOL before) {
    const bool old=enabled;enabled=before;exclusion.unlock();return old;
}
namespace dusk::native {
OSThread* platformGpuOwner(){return nullptr;}
GpuCompletion platformCompleteGpu(std::uint64_t){assert(false);return GpuCompletion::notReady;}
bool releaseGxTexture(gx::TextureHandle){assert(false);return false;}
}
// Sanitizer RTTI retains the worker debug-console methods on ELF hosts.
// This archive suite must never enter the console-rendering path.
void JUTConsole::clear(){assert(false && "archive test entered debug console");}
void JUTConsole::print_f(const char*,...){assert(false && "archive test entered debug console");}
void JUTConsole::print(const char*){assert(false && "archive test entered debug console");}
