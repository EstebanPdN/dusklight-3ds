#pragma once
#include <array>
#include <cstdint>

namespace dusk::native {
// Scoped to one original font parse. Positions, UVs and all four colors are
// copied at emission time; only adjacent glyphs with the same page are grouped.
// The caller flushes before any texture mutation or graphics-state change.
struct FontQuad {
    float left,top,right,bottom;
    std::array<std::uint32_t,4> colors;
    std::array<std::uint16_t,4> uv; // left, top, right, bottom
};
class FontBatch {
public:
    FontBatch()=default;
    FontBatch(const FontBatch&)=delete;
    FontBatch& operator=(const FontBatch&)=delete;
    ~FontBatch(){flush();}
    bool selectTexture(const void* image){
        if(image==texture&&texture)return false;
        flush();texture=image;return true;
    }
    void boundary(){flush();texture=nullptr;}
    void append(const FontQuad& quad){
        if(count==quads.size())flush();
        quads[count++]=quad;
    }
    void flush();
private:
    std::array<FontQuad,32> quads;
    unsigned count{};
    const void* texture{};
};
}
