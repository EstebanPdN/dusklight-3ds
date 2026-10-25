#ifndef DUSK_GROUND_CREATE_PROBE_HOOKS_HPP
#define DUSK_GROUND_CREATE_PROBE_HOOKS_HPP
#include "ground_create_probe.hpp"
class cBgS_GndChk;
class dBgS_PolyPassChk;
class dBgS_GrpPassChk;
#define GCP_GATE(value) dusk::native::groundFirstGate(value)
namespace dusk::native {
template<class V> GroundPosition groundPosition(const V& v) {
    return {groundBits(v.x),groundBits(v.y),groundBits(v.z)};
}
template<class V, class A> GroundScope groundLinkBefore(std::uint32_t id,std::uint32_t param,int mode,int point,const V& p,const V& old,A& a) {
    if(!groundProbeEnabled()) return {};
    GroundLinkInput in{id,param,a.m_flags,groundBits(a.field_0x90),groundBits(a.field_0x94),groundBits(a.m_gnd_chk_offset),mode,point,groundPosition(p),groundPosition(old)};
    // static_cast adjusts to the exact cBgS_GndChk base used by the existing dispatch.
    return groundLinkOpen(static_cast<cBgS_GndChk*>(&a.m_gnd),in);
}
template<class V, class A> void groundLinkAfter(GroundScope scope,const V& p,const V& old,A& a) {
    if(!groundProbeEnabled()) return;
    if(scope.selected) groundLinkFinish(scope,groundPosition(p),groundPosition(old),a.m_flags);
    else groundLinkFinish(scope,{},{},0);
}
template<class Q, class P=dBgS_PolyPassChk, class G=dBgS_GrpPassChk> std::uint32_t groundPassMask(Q& q) {
    auto* poly=static_cast<P*>(q.GetPolyPassChk());
    auto* group=static_cast<G*>(q.GetGrpPassChk());
    return (poly ? (1u | (poly->ChkLink() ? 2u : 0u)) : 0u)
        | (group ? (4u | (group->MaskNormalGrp() ? 8u : 0u) | (group->MaskWaterGrp() ? 16u : 0u)) : 0u);
}
}
#endif
