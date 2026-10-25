#pragma once
// DEFAULT OFF, startup-only private diagnostic; no draw or material rewriting.
#include "../gx_command_draw.hpp"
#include "gx_commands.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>
namespace dusk::native::strictmaterial74 {
enum class Config : unsigned { missing, disabled, enabled, invalid };
inline constexpr const char* configPath="sdmc:/3ds/dusklight/strict-material.txt";
inline Config parseConfig(std::string_view text){
    if(text=="enabled=0\n")return Config::disabled;
    if(text=="enabled=1\n")return Config::enabled;
    return Config::invalid;
}
inline const char* configName(Config config){
    switch(config){
    case Config::missing:return "missing";
    case Config::disabled:return "enabled=0";
    case Config::enabled:return "enabled=1";
    case Config::invalid:return "invalid";
    }return "invalid";
}
// Read at most11 bytes to reject any extra data; no existence-only enable.
inline Config readConfig(){
    errno=0;FILE* file=std::fopen(configPath,"rb");
    if(!file)return errno==ENOENT?Config::missing:Config::invalid;
    char text[11];const auto n=std::fread(text,1,sizeof(text),file);
    bool okay=!std::ferror(file);if(std::fclose(file))okay=false;
    if(!okay||n!=10)return Config::invalid;
    return parseConfig({text,n});
}
// Preserve the original nullptr callback's perspective-only admission for every
// remaining reason. In particular, UI/orthographic depth fallback stays strict.
inline bool allowFallback(const gx::CommandDraw& draw,DrawVisualFallback reason){
    return reason!=DrawVisualFallback::tevMaterial&&draw.registers.xfValid[0x1026]&&
           draw.registers.xf[0x1026]==0;
}
}
