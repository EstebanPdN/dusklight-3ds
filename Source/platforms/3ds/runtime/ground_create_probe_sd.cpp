#include "ground_create_probe.hpp"
#if defined(DUSK_TARGET_3DS) && DUSK_GROUND_CREATE_PROBE
#include <cstdio>
struct OSThread;
extern "C" OSThread* OSGetCurrentThread();
namespace dusk::native {
std::uintptr_t groundProbeThreadToken() { return reinterpret_cast<std::uintptr_t>(OSGetCurrentThread()); }
void initializeGroundProbeFromSd() {
    // Sole original game owner, once before game creation. No hook performs file I/O.
    static bool read=false;
    if(read)return;
    read=true;
    auto* file=std::fopen("sdmc:/3ds/dusklight/ground-create-probe.txt","rb");
    if(!file){initializeGroundProbe(false);return;}
    char text[23]; // Exact 22-byte LF token plus one byte detects every suffix.
    auto n=std::fread(text,1,sizeof(text),file);
    bool okay=!std::ferror(file);
    if(std::fclose(file))okay=false;
    const bool successDetails=okay && parseGroundProbeSuccessConfig(text,n);
    initializeGroundProbe((okay && parseGroundProbeConfig(text,n)) || successDetails,successDetails);
}
}
#endif
