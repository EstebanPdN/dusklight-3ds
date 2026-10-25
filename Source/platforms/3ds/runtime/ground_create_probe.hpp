#ifndef DUSK_GROUND_CREATE_PROBE_HPP
#define DUSK_GROUND_CREATE_PROBE_HPP
#ifndef DUSK_GROUND_CREATE_PROBE
#define DUSK_GROUND_CREATE_PROBE 0
#endif
#include <cstddef>
#include <cstdint>
namespace dusk::native {
enum class GroundFailure : unsigned { none, btkAlloc, btkCreate, brkAlloc, brkCreate, model, tevstr, dzbAlloc, dzbSet, kcolAlloc };
// Initialization alone belongs to the original game owner, once before fapGm_Create.
// Other entries reject non-owners before accessing mutable state. No new TLS.
bool parseGroundProbeConfig(const char*, std::size_t); // exact legacy mode 1
bool parseGroundProbeSuccessConfig(const char*, std::size_t); // exact opt-in mode 2
void initializeGroundProbeFromSd();
void initializeGroundProbe(bool); // legacy: success details remain OFF
void initializeGroundProbe(bool, bool retainSuccessDetails);
bool groundProbeEnabled();
void groundProbeBegin(std::uint32_t);
void groundProbeEnd();
// Closes capture without publishing; only groundProbeEnd emits. No rearming.
void groundProbeClose();
std::uintptr_t groundProbeThreadToken(); // native adapter: existing OSGetCurrentThread identity
std::uint32_t groundBits(float);
void groundBgBegin(std::uint32_t, int, std::uint32_t, const void*, const void*);
void groundBgHeapBegin(std::uint32_t, const void*);
void groundBgHeapResult(std::uint32_t, int, const void*);
void groundBgModel(std::uint32_t, int, bool, bool);
void groundBgFailure(std::uint32_t, GroundFailure, int);
void groundBgResource(std::uint32_t, unsigned, const void*);
void groundBgCollision(std::uint32_t, unsigned, const void*);
void groundBgRegist(std::uint32_t, unsigned, const void*, bool, int, bool);
void groundBgOuter(std::uint32_t, int);
void groundReleased(const void*, int, std::uint32_t);
struct GroundPosition { std::uint32_t x,y,z; };
struct GroundLinkInput {
    std::uint32_t id,param,flags,f90,f94,offset;
    int mode,point;
    GroundPosition pos,old;
};
// Cookie is automatic scalar state, not TLS or retained engine memory.
struct GroundScope { std::uintptr_t previous; bool selected; };
GroundScope groundLinkOpen(const void*, const GroundLinkInput&);
void groundLinkFinish(GroundScope, GroundPosition, GroundPosition, std::uint32_t);
bool groundFirstGate(bool); // identity return; never evaluates an expression again
bool groundQuerySelected(const void*);
void groundQueryBefore(const void*, float,float,float,std::uint32_t,float,float,float,bool,std::uint32_t,std::uint32_t);
void groundQueryAfter(const void*, float,int,int);
bool groundDispatchBegin(const void*);
void groundDispatchCount(const void*, unsigned);
void groundDispatch(const void*, int,std::uint32_t,const void*,float,float,bool);
}
#endif
