#include "../runtime/boot_stage.hpp"
#include <cassert>
#include <string_view>
int main(){
    using namespace dusk::native;
    BootStage stage;
    assert(parseBootStage("R_SP01 4 0 -1\n",stage));
    assert(std::string_view(stage.name)=="R_SP01"&&stage.room==4&&stage.point==0&&stage.layer==-1);
    assert(parseBootStage("  F_SP103\t63 -32768 15\r\n",stage));
    for(const auto input:{"", "R_SP01", "R_SP01 4 0", "R_SP01 4 0 -1 extra", "../foo 4 0 -1", "TOO_LONG_NAME 0 0 -1",
         "R_SP01 64 0 -1", "R_SP01 -1 0 -1", "R_SP01 4 32768 -1", "R_SP01 4 0 16", "R_SP01 4 0 -2",
         "R_SP01 4 0x10 -1", "R_SP01 4 999999999999999999999 -1"}){
        assert(!parseBootStage(input,stage));assert(stage.name[0]==0);
    }
}
