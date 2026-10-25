#include "../runtime/texture_half_policy.hpp"
#include <cassert>
#include <cstdio>
#include <string>
using namespace dusk::native;
int main(){
    std::array<NativeTextureFingerprint,64> policy{};std::size_t count=7;
    assert(parseTextureHalfPolicy(" \n",policy,count)&&count==0);
    const std::string token="0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789ABCDEF";
    assert(parseTextureHalfPolicy("\t"+token+"\r\n",policy,count)&&count==1);
    assert(policy[0][0]==1&&policy[0][31]==239);
    const auto original=policy;
    for(const auto& invalid:{token+token,token+"\n"+token,std::string(8193,' '),token.substr(1),token+"#",std::string(64,'x')}){
        assert(!parseTextureHalfPolicy(invalid,policy,count)&&count==1&&policy==original);
    }
    std::string many;
    for(unsigned n=0;n<64;++n){char digits[3];std::snprintf(digits,sizeof(digits),"%02x",n);many+=std::string(62,'0')+digits+"\n";}
    assert(parseTextureHalfPolicy(many,policy,count)&&count==64);
    const auto full=policy;
    assert(!parseTextureHalfPolicy(many+std::string(62,'0')+"40\n",policy,count)&&count==64&&policy==full);
    std::puts("PASS bounded unique hex policy, normalization, invalid/truncated/overflow input and atomic refusal");
}
