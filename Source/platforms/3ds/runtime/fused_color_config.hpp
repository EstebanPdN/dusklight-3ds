#pragma once
#include <cstddef>
#include <cstring>
namespace dusk::native {
// Exact LF token only. Every invalid input explicitly clears the output.
inline bool parseFusedColorCopyConfig(const char* text,std::size_t size,bool& enabled){
    enabled=false;
    if(!text||size!=10)return false;
    if(std::memcmp(text,"enabled=0\n",10)==0)return true;
    if(std::memcmp(text,"enabled=1\n",10)==0){enabled=true;return true;}
    return false;
}
// Original game owner startup only: one bounded read and one report per process.
// Missing, invalid, read-error and close-error inputs remain disabled; no rearm.
bool readFusedColorCopyConfigFromSd();
}
