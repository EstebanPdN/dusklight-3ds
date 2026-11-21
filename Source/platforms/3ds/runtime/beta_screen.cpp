#include "beta_screen.hpp"
#include "application_dump_ctru.hpp"
#include "disc.hpp"
#include "resource_archive.hpp"
#include <3ds.h>
#include <zstd.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

extern const unsigned char beta_face[];
extern const unsigned int beta_face_size;
extern "C" void OSReport(const char*,...);
namespace dusk::native {
namespace {
constexpr int W=320,H=240, bx=19,by=185,bw=282,bh=42;
constexpr char alphabet[]="0123456789.-% msDUMPO NEFAICL";
// The embedded face contains only the port wordmark, interface labels and numeric font.
// Menu textures are composed from the player's own game bundle during startup.
struct Artwork {
    u16 pixels[W*H];
    u8 glyphs[2][sizeof(alphabet)-1][26][16];
};
Artwork art{};
bool enabled=true;
bool hidden=false;
float frameMs=0;
int speedPercent=0,progress=-1;
u64 resultUntil=0,lastPaint=0;
bool success=false;
u16 pack(int r,int g,int b){return ((r>>3)<<11)|((g>>2)<<5)|(b>>3);}
u16 mix(u16 a,u16 b,int opacity){
    const int t=255-opacity;
    return ((((a>>11)*t+(b>>11)*opacity)/255)<<11)|
           (((((a>>5)&63)*t+((b>>5)&63)*opacity)/255)<<5)|
           (((a&31)*t+(b&31)*opacity)/255);
}
// Original save-menu pieces are GX I4 tiles. Borrow them only during boot.
struct Tile {
    BtiInfo info{};
    bool load(const RarcArchive& arc,const char* name){
        RarcEntry e;
        return arc.findFile(name,e)==ResourceStatus::ready &&
            inspectBti(e.contents,256,info)==ResourceStatus::ready && info.format==0;
    }
    int at(int x,int y) const {
        const auto idx=((y/8)*((info.width+7)/8)+x/8)*32+(y%8)*4+(x%8)/2;
        return ((info.image.data[idx]>>(x%2?0:4))&15)*17;
    }
};
void stamp(const Tile& t,int x,int y,int w,int h,bool flipX,bool flipY,bool button){
    for(int j=0;j<h;++j)for(int i=0;i<w;++i){
        const int a=t.at((flipX?w-1-i:i)*t.info.width/w,(flipY?h-1-j:j)*t.info.height/h);
        const u16 tint=button?pack(66+175*a/255,50+154*a/255,30+105*a/255):
                                pack(48+165*a/255,42+147*a/255,32+108*a/255);
        auto& pixel=art.pixels[(y+j)*W+x+i];pixel=mix(pixel,tint,a);
    }
}
bool loadTrim(){
    Disc disc;DiscFile file;
    if(disc.open(gameImagePath)!=DiscStatus::ready||disc.findFile("/res/Layout/saveres.arc",file)!=DiscFileStatus::ready||
       !file.size()||file.size()>1024*1024)return false;
    std::vector<u8> stored(file.size()),expanded;
    if(!disc.readFile(file,0,stored.data(),stored.size()))return false;
    const u8* bytes=stored.data();size_t length=stored.size();
    if(length>=4&&!std::memcmp(bytes,"Yaz0",4)){
        u32 size=0;
        if(yaz0ExpandedSize(bytes,length,2*1024*1024,size)!=ResourceStatus::ready)return false;
        expanded.resize(size);size_t written=0;
        if(decodeYaz0(bytes,length,expanded.data(),size,size,written)!=ResourceStatus::ready)return false;
        bytes=expanded.data();length=written;
    }
    RarcArchive arc;
    if(arc.open(bytes,length,{2*1024*1024,256,8192,256*1024})!=ResourceStatus::ready)return false;
    Tile corner,top,side,cap,center,orn;
    if(!corner.load(arc,"tt_horiwaku_lu.bti")||!top.load(arc,"tt_horiwaku_top_rr.bti")||
       !side.load(arc,"tt_horiwaku_side_l_rr.bti")||!cap.load(arc,"tt_button_base0_side.bti")||
       !center.load(arc,"tt_button_base0_center_tate.bti")||!orn.load(arc,"tt_kazari_kani_00.bti"))return false;
    for(bool flip:{false,true}){
        for(bool other:{false,true})stamp(corner,flip?W-19:0,other?H-19:0,19,19,flip,other,false);
        for(int x=19;x<W-19;x+=24)stamp(top,x,flip?H-10:0,std::min(24,W-19-x),10,false,flip,false);
        for(int y=19;y<H-19;y+=12)stamp(side,flip?W-19:0,y,19,std::min(12,H-19-y),flip,false,false);
        stamp(cap,flip?bx+bw-17:bx,by,17,bh,flip,false,true);
    }
    for(int x=bx+17;x<bx+bw-17;x+=17)stamp(center,x,by,std::min(17,bx+bw-17-x),bh,false,false,true);
    if(orn.info.width>=92&&orn.info.height>=18)
        for(int left:{61,228})for(int y=0;y<8;++y)for(int x=0;x<31;++x){
            auto& p=art.pixels[(14+y)*W+left+x];
            p=mix(p,pack(196,174,132),std::clamp((221-orn.at(36+x*56/31,5+y*13/8))*3,0,255));
        }
    return true;
}
struct Surface {
    u16* fb;
    u16& at(int x,int y){return fb[x*H+(H-1-y)];}
    void text(int center,int y,const char* s,bool small=false){
        const int advance=small?9:14;
        int x=center-int(std::strlen(s))*advance/2;
        for(;*s;++s,x+=advance){
            const char* found=std::strchr(alphabet,*s);
            if(!found)continue;
            const auto& mask=art.glyphs[small][found-alphabet];
            for(int j=0;j<26;++j)for(int i=0;i<16;++i)if(mask[j][i]&&x+i>=0&&x+i<W){
                auto& p=at(x+i,y+j);p=mix(p,0xffde,mask[j][i]);
            }
        }
    }
};
void paint(int brightness=255){
    auto* fb=reinterpret_cast<u16*>(gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,nullptr,nullptr));
    if(!fb)return;
    Surface s{fb};
    if(hidden){
        std::memset(fb,0,W*H*2);
        for(int y=8;y<32;++y)for(int x=288;x<312;++x)s.at(x,y)=art.pixels[y*W+x];
        for(int i=0;i<15;++i)s.at(293+i,13+i)=pack(228,194,134);
    }else{
        for(int x=0;x<W;++x)for(int y=0;y<H;++y)s.at(x,y)=art.pixels[y*W+x];
        char str[32];
        if(frameMs>0)std::snprintf(str,sizeof(str),"%.1f",1000.0f/frameMs);else std::strcpy(str,"--");
        s.text(59,145,str);
        if(frameMs>0)std::snprintf(str,sizeof(str),"%d%%",speedPercent);else std::strcpy(str,"--");
        s.text(151,145,str);
        if(frameMs>0)std::snprintf(str,sizeof(str),"%.1f",frameMs);else std::strcpy(str,"--");
        s.text(241,145,str);s.text(288,151,"ms",true);
        const bool finished=resultUntil!=0;
        int filled=finished&&success?100:std::max(0,progress);
        for(int x=bx+4;x<bx+4+(bw-8)*filled/100;++x)for(int y=by+4;y<by+bh-4;++y)s.at(x,y)=pack(39,105,66);
        if(progress>=0)std::snprintf(str,sizeof(str),"%d%%",progress);
        else std::strcpy(str,finished?(success?"DONE":"FAILED"):"DUMP");
        s.text(160,200,str,true);
    }
    if(brightness<255)for(int i=0;i<W*H;++i)fb[i]=mix(0,fb[i],brightness);
    GSPGPU_FlushDataCache(fb,W*H*2);
}
}
bool betaScreenEnabled(){return enabled;}
void startBetaScreen(){
    if(auto* flag=std::fopen("sdmc:/3ds/DuskLight/dev-console.txt","rb")){std::fclose(flag);enabled=false;return;}
    paint(0);
    if(ZSTD_decompress(&art,sizeof(art),beta_face,beta_face_size)!=sizeof(art))return;
    const bool trim=loadTrim();
    OSReport("Public Beta 01: menu art %s, bottom fade 2000 ms\n",trim?"ready":"unavailable");
    const u64 start=svcGetSystemTick();
    for(;;){
        const int level=std::min<u64>(255,(svcGetSystemTick()-start)*255/(2ULL*SYSCLOCK_ARM11));
        paint(level);if(level==255)break;
        gspWaitForVBlank();
    }
    OSReport("Public Beta 01: bottom fade complete in %.3f s\n",
             double(svcGetSystemTick()-start)/SYSCLOCK_ARM11);
}
void updateBetaScreen(float ms,int speed){
    frameMs=ms;speedPercent=speed;
    const u64 now=svcGetSystemTick();
    if(resultUntil&&now>=resultUntil){resultUntil=0;lastPaint=0;}
    if(!hidden&&now-lastPaint>=SYSCLOCK_ARM11/4){paint();lastPaint=now;}
}
void touchBetaScreen(unsigned x,unsigned y,bool allowDump){
    if(x>=282&&x<318&&y>=4&&y<36){hidden=!hidden;paint();}
    else if(allowDump&&!hidden&&progress<0&&!resultUntil&&x>=bx&&x<bx+bw&&y>=by&&y<by+bh){
        requestApplicationDump(false);
    }
}
void betaDumpProgress(std::uint64_t done,std::uint64_t total){
    progress=total?int(std::min<std::uint64_t>(99,done*100/total)):0;
    if(!hidden)paint();
}
void betaDumpFinished(bool ok){
    progress=-1;success=ok;resultUntil=svcGetSystemTick()+SYSCLOCK_ARM11;paint();
}
}
