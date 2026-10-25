#include "fused_color_config.hpp"
#include <cstdio>
extern "C" void OSReport(const char*,...);
namespace dusk::native {
bool readFusedColorCopyConfigFromSd(){
    static bool read=false,enabled=false;
    if(read)return enabled;
    read=true;
    const char* reason="missing/unreadable";
    if(auto* file=std::fopen("sdmc:/3ds/dusklight/fused-color-copy.txt","rb")){
        char text[11]; // Exact 10-byte token plus one byte to detect any suffix.
        const auto size=std::fread(text,1,sizeof(text),file);
        bool valid=!std::ferror(file);
        if(std::fclose(file))valid=false;
        if(valid&&parseFusedColorCopyConfig(text,size,enabled))reason="exact SD token";
        else {enabled=false;reason="invalid/unreadable";}
    }
    OSReport("Native fused color copy: %s (%s); private candidate\n",enabled?"enabled":"disabled",reason);
    return enabled;
}
}
