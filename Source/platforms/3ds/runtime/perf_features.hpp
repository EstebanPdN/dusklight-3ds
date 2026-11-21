#pragma once
// Switches of the performance improvements added from Alpha 21 on.
//
// Every improvement that changes what is drawn or computed asks `on(Id)` at
// the moment it decides (never once at load), so that it can be turned off
// and on between two pictures: the automatic test measures the same spot of a
// zone with the improvements, as a reference without them, and without one of
// them alone (measured_dump.hpp: what that one is worth in that zone), and
// sdmc:/3ds/DuskLight/features.txt selects them for a session ("none", "all",
// or words like "lod=0 mips=1"; read once at boot).
//
// Rules for a new improvement:
// - add its Id and name here, default on only once its pictures were compared
//   with the reference in several zones;
// - with the switch off the code must take exactly the path it took before
//   the improvement existed;
// - the switch changes only at a picture boundary, on the game thread, with
//   the second-core worker idle: a reader on either thread sees one value for
//   a whole picture, and anything cached under one value (a record, a list, a
//   sampler) must be keyed by what it was built from, not assumed.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dusk::native::features {
enum class Id : unsigned {
    modelLod,       // far models drawn with the converter's reduced index lists
    shadowLod,      // model shadows by size on screen, with the reduced lists
    skinPalette,    // fewer draw groups for skinned models
    vertexVariants, // vertex program that runs only what the material needs
    worldMips,      // mip levels for minified world textures authored without them
    farLogic,       // cheaper logic for actors far away and out of view
    packetPaint,    // cheaper painting of a material packet on the game thread
    count
};
inline constexpr const char* names[unsigned(Id::count)]{"lod","shadow","skin","vertex","mips","logic","paint"};
// The session's choice (features.txt) and the overrides: the two a
// measurement holds for its pictures (every improvement off, or the ones of a
// mask only), and the one of the bottom-screen button (STATS page), which
// stays until it is touched again: with an emulator save state it gives the
// same game frame with and without the improvements, pixel for pixel.
// On since Alpha 21, each after its pictures were compared with the reference
// in the built-in emulator (the game's own capture of one picture number from
// a save state, with and without; Build/alpha21/gate and the agents' evidence
// folders):
// - modelLod, shadowLod: 0 to 171 of 96,000 pixels in three places, indices at
//   or under the reference in the ten slow zones measured; they do nothing
//   without sdmc:/3ds/DuskLight/zelda.lod;
// - skinPalette: exact for unweighted bones, a normal within 4 degrees of
//   J3D's for weighted ones (at most 19 pixels by 4 levels in three places);
// - vertexVariants, packetPaint: the same picture byte for byte;
// - farLogic: fish out of view and over 3000 units away rest.
// Off: worldMips. Its levels reach the draws and the picture changes where it
// can be seen (30 to 39 % of the pixels, 1.3 to 2.3 % by more than 16 levels;
// the lettering of the Hidden Village sign goes soft), and what the PICA
// saves by it has not been measured on a console. `mips=1` in features.txt
// turns it on for a session; the automatic test then measures it like the
// others.
// The automatic test measures every zone with all of the chosen ones off and
// with one of them off: a console run says what each is worth
// (measure_report.py).
inline std::array<bool,unsigned(Id::count)> chosen{true,true,true,true,false,true,true};
static_assert(unsigned(Id::worldMips)==4,"the defaults above are in the order of Id");
static_assert(unsigned(Id::count)==7,"a new improvement needs its default and its evidence above");
inline bool reference{},manualReference{};
inline unsigned without{}; // bit n: improvement n is off
inline bool on(Id id){return chosen[unsigned(id)]&&!reference&&!manualReference&&!(without>>unsigned(id)&1u);}
// Between two pictures, game thread: every improvement off (true) or as chosen.
inline void setReference(bool value){reference=value;}
inline void setManualReference(bool value){manualReference=value;}
// Between two pictures, game thread: the improvements of `mask` off (0: none).
inline void setWithout(unsigned mask){without=mask;}
// The mask of one improvement by its name; 0 for a name that is none.
inline unsigned maskOf(const char* name){
    for(unsigned n=0;n<unsigned(Id::count);++n)if(!std::strcmp(name,names[n]))return 1u<<n;
    return 0;
}
// The chosen improvements as a mask, and the n-th of them (n counted round).
inline unsigned chosenMask(){
    unsigned mask=0;
    for(unsigned n=0;n<unsigned(Id::count);++n)if(chosen[n])mask|=1u<<n;
    return mask;
}
inline unsigned chosenOne(unsigned n){
    const unsigned mask=chosenMask();
    if(!mask)return 0;
    unsigned count=0;
    for(unsigned f=0;f<unsigned(Id::count);++f)count+=mask>>f&1u;
    n%=count;
    for(unsigned f=0;f<unsigned(Id::count);++f)if(mask>>f&1u&&!n--)return 1u<<f;
    return 0;
}
// "lod+shadow" for a mask, "-" for none: for measurement labels.
inline const char* maskText(unsigned mask){
    static char line[64];
    int at=0;line[0]=0;
    for(unsigned n=0;n<unsigned(Id::count);++n)if(mask>>n&1u)
        at+=std::snprintf(line+at,sizeof(line)-std::size_t(at),"%s%s",at?"+":"",names[n]);
    return at?line:"-";
}
// "lod=1 shadow=0 ...": for logs and measurement labels.
inline const char* text(){
    static char line[112];
    int at=0;
    for(unsigned n=0;n<unsigned(Id::count);++n)
        at+=std::snprintf(line+at,sizeof(line)-std::size_t(at),"%s%s=%d",n?" ":"",names[n],int(on(Id(n))));
    return line;
}
// Applies the words of features.txt; returns the number not understood.
inline int parse(const char* words){
    int refused=0;
    char word[32];
    const char* at=words;
    while(*at){
        while(*at==' '||*at=='\t'||*at=='\r'||*at=='\n'||*at==';')++at;
        std::size_t n=0;
        while(at[n]&&at[n]!=' '&&at[n]!='\t'&&at[n]!='\r'&&at[n]!='\n'&&at[n]!=';')++n;
        if(!n)break;
        const std::size_t kept=n<sizeof(word)-1?n:sizeof(word)-1;
        std::memcpy(word,at,kept);word[kept]=0;at+=n;
        if(!std::strcmp(word,"none")||!std::strcmp(word,"off")){chosen.fill(false);continue;}
        if(!std::strcmp(word,"all")||!std::strcmp(word,"on")){chosen.fill(true);continue;}
        char* value=std::strchr(word,'=');
        bool known=false;
        if(value&&(value[1]=='0'||value[1]=='1')&&!value[2]){
            *value=0;
            for(unsigned f=0;f<unsigned(Id::count);++f)if(!std::strcmp(word,names[f])){chosen[f]=value[1]=='1';known=true;}
        }
        refused+=!known;
    }
    return refused;
}
}
