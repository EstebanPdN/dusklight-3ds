#pragma once
#include "texture_gx.hpp"
#include <string_view>
namespace dusk::native {
// Startup-only experiment config: <=64 unique SHA-256 hex tokens. No wildcard.
// On malformed/oversized/duplicate input, caller's policy is unchanged.
inline bool parseTextureHalfPolicy(std::string_view text,
        std::array<NativeTextureFingerprint,64>& output,std::size_t& count){
    if(text.size()>8192)return false;
    std::array<NativeTextureFingerprint,64> pending{};std::size_t used=0,at=0;
    auto space=[](char c){return c==' '||c=='\n'||c=='\r'||c=='\t';};
    auto hex=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;};
    while(at<text.size()){
        while(at<text.size()&&space(text[at]))++at;
        if(at==text.size())break;
        if(used==pending.size()||text.size()-at<64)return false;
        auto& digest=pending[used];
        for(unsigned n=0;n<32;++n){const int hi=hex(text[at++]),lo=hex(text[at++]);
            if(hi<0||lo<0)return false;digest[n]=std::uint8_t(hi*16+lo);}
        if(at<text.size()&&!space(text[at]))return false;
        for(std::size_t n=0;n<used;++n)if(pending[n]==digest)return false;
        ++used;
    }
    output=pending;count=used;return true;
}
}
