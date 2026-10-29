// Host-only native pose/model-plan binding inspection. No frames or draws.
#include "native/pose.hpp"
#include "native/model_plan.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
using namespace dusk::pica;
std::vector<char> read(const char* p){std::ifstream f(p,std::ios::binary);return {(std::istreambuf_iterator<char>(f)),{}};}
int main(int argc,char** argv){
    if(argc!=2&&argc!=3)return 2;
    auto bytes=read(argv[1]);PosePack pack;if(PosePack::open(std::as_bytes(std::span(bytes)),pack)!=PackError::none)return 3;
    std::vector<Affine> world(pack.nodeCount());std::vector<PoseMatrix> matrices(pack.matrixCount());PoseView pose;
    if(!pack.evaluate(PosePack::identity(),{},world,matrices,pose))return 4;
    unsigned slots=0;
    if(argc==3){auto data=read(argv[2]);ModelPlan plan;if(ModelPlan::open(std::as_bytes(std::span(data)),plan)!=PackError::none)return 5;
        for(unsigned n=0;n<plan.uniformCount();++n){auto u=plan.uniform(n);Palette p;
            if(!u||!pack.anchorNode(u->anchor)||!pose.palette(std::span(u->palette).first(u->paletteCount),p))return 6;
            DrawUniforms check;check.palette=p;for(unsigned r=0;r<4;++r)check.viewProjection[r][r]=1;
            if(!validateUniforms(check,p.count-1))return 7;++slots;
        }
    }
    std::printf("{\"nodes\":%u,\"matrices\":%u,\"native_plan_slots\":%u,\"frames\":0,\"draws\":0}\n",pack.nodeCount(),pack.matrixCount(),slots);
}
