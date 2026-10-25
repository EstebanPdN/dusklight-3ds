#pragma once
#include <charconv>
#include <cstring>
#include <string_view>
namespace dusk::native {
struct BootStage {char name[9]{};int room{},point{},layer{-1};};
inline bool parseBootStage(std::string_view text,BootStage& result){
    result={};BootStage parsed;
    auto token=[&](){
        const auto begin=text.find_first_not_of(" \t\r\n");
        if(begin==text.npos){text={};return std::string_view{};}
        text.remove_prefix(begin);const auto end=text.find_first_of(" \t\r\n");
        const auto word=text.substr(0,end);
        text.remove_prefix(end==text.npos?text.size():end);return word;
    };
    const auto name=token();if(name.empty()||name.size()>8)return false;
    for(char c:name)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'))return false;
    std::memcpy(parsed.name,name.data(),name.size());
    for(int* value:{&parsed.room,&parsed.point,&parsed.layer}){
        const auto word=token();if(word.empty())return false;
        const auto converted=std::from_chars(word.data(),word.data()+word.size(),*value);
        if(converted.ec!=std::errc{}||converted.ptr!=word.data()+word.size())return false;
    }
    if(!token().empty()||parsed.room<0||parsed.room>63||parsed.point<-32768||parsed.point>32767||parsed.layer<-1||parsed.layer>15)return false;
    result=parsed;return true;
}
}
