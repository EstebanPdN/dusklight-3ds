#include "camera.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
namespace dusk::pica {
namespace {
bool scalar(float value,double limit){const auto bits=std::bit_cast<std::uint32_t>(value)&0x7fffffffu;
    return bits<0x7f800000u&&(!bits||bits>=0x20800000u)&&std::abs(double(value))<=limit;}
bool publish(double value,double limit,float& out){
    if(!std::isfinite(value)||std::abs(value)>limit||(value!=0&&std::abs(value)<0x1p-62))return false;
    out=float(value);return scalar(out,limit);
}
}
bool prepareCamera(const Matrix& projection,const CameraRegion& r,unsigned physicalWidth,unsigned physicalHeight,CameraProjection& out){
    out={};
    if(!physicalWidth||!physicalHeight||physicalWidth>1024||physicalHeight>1024)return false;
    const float fields[]{r.canvasWidth,r.canvasHeight,r.x,r.y,r.width,r.height,r.nearDepth,r.farDepth};
    for(float x:fields)if(!scalar(x,32768))return false;
    if(r.canvasWidth<1||r.canvasHeight<1||r.width<=0||r.height<=0||r.nearDepth<0||r.nearDepth>1||r.farDepth<0||r.farDepth>1)return false;
    for(const auto& row:projection)for(unsigned c=0;c<4;++c)if(!scalar(row[c],c==3?0x1p20:32768))return false;
    if(projection[3]==Row{})return false;
    const double cw=r.canvasWidth,ch=r.canvasHeight,x=r.x,y=r.y,w=r.width,h=r.height;
    const double left=std::clamp(x,0.0,cw),right=std::clamp(x+w,0.0,cw),top=std::clamp(y,0.0,ch),bottom=std::clamp(y+h,0.0,ch);
    if(left>=right||top>=bottom)return false;
    CameraProjection result;
    // Quarter-turn display orientation agrees with installed Mtx_PerspTilt:
    // native X = logical clip Y; native Y = -logical clip X.
    const double sx=w/cw,sy=h/ch,tx=1-(2*x+w)/cw,ty=1-(2*y+h)/ch;
    const double sz=double(r.farDepth)-r.nearDepth,tz=double(r.farDepth)-1;
    for(unsigned c=0;c<4;++c){const double limit=c==3?0x1p20:32768;
        if(!publish(sy*projection[1][c]+ty*projection[3][c],limit,result.rows[0][c])||
           !publish(-sx*projection[0][c]+tx*projection[3][c],limit,result.rows[1][c])||
           !publish(sz*projection[2][c]+tz*projection[3][c],limit,result.rows[2][c]))return false;
        result.rows[3][c]=projection[3][c];
    }
    result.left=unsigned(std::floor((ch-bottom)*physicalWidth/ch));
    result.right=unsigned(std::ceil((ch-top)*physicalWidth/ch));
    result.top=unsigned(std::floor((cw-right)*physicalHeight/cw));
    result.bottom=unsigned(std::ceil((cw-left)*physicalHeight/cw));
    if(result.left>=result.right||result.top>=result.bottom||result.right>physicalWidth||result.bottom>physicalHeight)return false;
    out=result;return true;
}
bool restrictCameraScissor(CameraProjection& camera,const CameraRegion& r,const CameraScissor& s,
                           unsigned physicalWidth,unsigned physicalHeight){
    if(!physicalWidth||!physicalHeight||physicalWidth>1024||physicalHeight>1024||
       !scalar(r.canvasWidth,32768)||!scalar(r.canvasHeight,32768)||
       r.canvasWidth<1||r.canvasHeight<1||!s.width||!s.height||
       camera.left>=camera.right||camera.top>=camera.bottom||
       camera.right>physicalWidth||camera.bottom>physicalHeight)return false;
    const double cw=r.canvasWidth,ch=r.canvasHeight;
    const double left=std::min(double(s.x),cw),top=std::min(double(s.y),ch);
    const double right=std::min(double(std::uint64_t(s.x)+s.width),cw);
    const double bottom=std::min(double(std::uint64_t(s.y)+s.height),ch);
    if(left>=right||top>=bottom)return false;
    const unsigned x0=unsigned(std::floor((ch-bottom)*physicalWidth/ch));
    const unsigned x1=unsigned(std::ceil((ch-top)*physicalWidth/ch));
    const unsigned y0=unsigned(std::floor((cw-right)*physicalHeight/cw));
    const unsigned y1=unsigned(std::ceil((cw-left)*physicalHeight/cw));
    const unsigned clippedLeft=std::max(camera.left,x0),clippedRight=std::min(camera.right,x1);
    const unsigned clippedTop=std::max(camera.top,y0),clippedBottom=std::min(camera.bottom,y1);
    if(clippedLeft>=clippedRight||clippedTop>=clippedBottom)return false;
    camera.left=clippedLeft;camera.right=clippedRight;camera.top=clippedTop;camera.bottom=clippedBottom;
    return true;
}
}
