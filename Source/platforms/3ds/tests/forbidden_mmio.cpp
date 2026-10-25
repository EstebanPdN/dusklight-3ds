// Negative compile fixture: these GameCube hardware accesses must not compile.
#include <dolphin/gx.h>
#include <dolphin/hw_regs.h>
void forbidden_access() {
#if defined(DUSK_TEST_FIFO)
    GXWGFifo.u32 = 0;
#else
    __VIRegs[0] = 0;
#endif
}
