#include "scene.hpp"
#include <algorithm>
namespace dusk::pica {
Submission Scene::draw(Renderer& r,std::span<const SceneItem> items){
    if(!r.owner())return {0,r.error()};
    if(!r.open)return {0,Error::protocol};
    if(items.size()>capacity||items.size()>r.limits.drawsPerFrame-r.stats.frameDraws)return {0,Error::budget};
    // Complete CPU contract validation before the first backend state change.
    // No user callback runs between validation and native submission.
    for(const auto& item:items){
        if(!item.model||!item.parameters)return {0,Error::invalidAsset};
        if(item.model->renderer!=&r)return {0,Error::protocol};
        const auto error=item.model->preflightPart(item.part,*item.parameters);
        if(error!=Error::none)return {0,error};
    }
    std::array<std::uint16_t,capacity> sequence;
    for(unsigned n=0;n<items.size();++n)sequence[n]=std::uint16_t(n);
    std::sort(sequence.begin(),sequence.begin()+items.size(),[&](unsigned a,unsigned b){
        const auto& left=items[a];const auto& right=items[b];
        if(left.pass!=right.pass)return left.pass<right.pass;
        if(left.order!=right.order)return left.order<right.order;
        return a<b;
    });
    unsigned submitted=0;
    for(unsigned n=0;n<items.size();++n){const auto& item=items[sequence[n]];
        if(!item.model->submitPart(item.part,*item.parameters))return {submitted,item.model->error()};
        ++submitted;
    }
    return {submitted,Error::none};
}
}
