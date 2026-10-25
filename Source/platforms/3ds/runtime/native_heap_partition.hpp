#pragma once
#include <cstdint>
#include <string_view>
namespace dusk::native::heap64 {
inline constexpr std::uint32_t minSystem=16*1024*1024,maxGame=12*1024*1024;
// Explicit private experiment; absent/zero preserves the exact original split.
// Decimal only, bounded before multiply. No whitespace, NUL or trailing data.
inline bool parse(std::string_view text,std::uint32_t& out){
    out=0;constexpr std::string_view prefix="GAME64 game=";
    if(!text.starts_with(prefix)||!text.ends_with('\n'))return false;
    text.remove_prefix(prefix.size());text.remove_suffix(1);
    if(text.empty()||text.size()>8||(text.size()>1&&text.front()=='0'))return false;
    std::uint32_t value=0;
    for(char c:text){if(c<'0'||c>'9'||value>(maxGame-std::uint32_t(c-'0'))/10)return false;value=value*10+std::uint32_t(c-'0');}
    if(value&&(value<4*1024*1024||value%32))return false;
    out=value;return true;
}
struct Split {std::uint32_t system{},game{},transferred{};};
inline bool partition(std::uint32_t system,std::uint32_t game,std::uint32_t requested,Split& out){
    out={};
    if(!requested){out={system,game,0};return true;}
    if(requested<game||requested<4*1024*1024||requested>maxGame||requested%32)return false;
    const auto delta=requested-game;
    if(system<minSystem||delta>system-minSystem)return false;
    out={system-delta,requested,delta};return true;
}
}
