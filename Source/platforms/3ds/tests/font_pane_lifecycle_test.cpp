// Compile the production font owner against a bounded host heap and picture/asset stand-ins.
#include <array>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using s16 = std::int16_t;
using f32 = float;
#define DUSK_IF_ELSE(native, original) native
#define IF_DUSK_BLOCK(condition) if (condition) {
#define IF_DUSK_BLOCK_END }
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

class JKRHeap {
public:
    long size;
    long largest;
    long other;
    long getSize() const { return size; }
    long getTotalFreeSize() const { return largest + other; }
};
JKRHeap* currentHeap;
JKRHeap* sceneHeap;
JKRHeap* JKRGetCurrentHeap() { return currentHeap; }
JKRHeap* JKRSetCurrentHeap(JKRHeap* heap) {
    auto* previous = currentHeap;
    currentHeap = heap;
    return previous;
}
JKRHeap* dComIfGp_getExpHeap2D() { return sceneHeap; }

struct Allocation {
    JKRHeap* heap;
    long charge;
};
std::map<void*, Allocation> allocations;
struct HeapTag {};
void* allocate(std::size_t bytes, long charge) {
    if (charge > currentHeap->largest) {
        return nullptr;
    }
    void* ptr = std::malloc(bytes);
    assert(ptr);
    currentHeap->largest -= charge;
    allocations.emplace(ptr, Allocation{currentHeap, charge});
    return ptr;
}
void* operator new(std::size_t bytes, HeapTag) noexcept {
    return allocate(bytes, bytes);
}
template <typename T>
void testDelete(T* ptr) {
    if (ptr != nullptr) {
        ptr->~T();
        const auto allocation = allocations.at(ptr);
        allocation.heap->largest += allocation.charge;
        allocations.erase(ptr);
        std::free(ptr);
    }
}
#define JKR_NEW new (HeapTag{})
#define JKR_DELETE(ptr) testDelete(ptr)
[[noreturn]] void OSPanic(const char*, int, const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::exit(1);
}

namespace JUtility {
struct TColor {
    u32 rgba;
    TColor(u32 value) : rgba(value) {}
    TColor(u8 r, u8 g, u8 b, u8 a) : rgba(u32(r) << 24 | u32(g) << 16 | u32(b) << 8 | a) {}
};
}

struct ResTIMG {
    std::string name;
    int width;
    int height;
};
struct Archive {
    std::map<std::string, ResTIMG> resources;
    std::vector<std::string> lookups;
    void* getResource(u32, const char* name) {
        lookups.emplace_back(name);
        return &resources.try_emplace(name, ResTIMG{name, 64, 32}).first->second;
    }
    void removeResourceAll() { resources.clear(); }
};
Archive mainArchive, itemArchive;
Archive* dComIfGp_getMain2DArchive() { return &mainArchive; }
Archive* dComIfGp_getItemIconArchive() { return &itemArchive; }
const char* dMeter2Info_getNumberTextureName(int index) {
    static const char* names[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
    return names[index];
}

struct J2DPane {
    J2DPane* getParentPane() { return nullptr; }
    f32 getScaleX() const { return 1; }
    f32 getScaleY() const { return 1; }
};
class J2DTextBox : public J2DPane {};
enum { ROTATE_Z };
struct Draw {
    const ResTIMG* image;
    u32 white;
    f32 blend;
    f32 width;
    bool mirrorY;
};
std::vector<Draw> draws;

class J2DPicture {
public:
    // Alpha 17's actual ARM picture request. Texture and allocator metadata
    // costs are excluded, so this fixture models the pane-specific failure.
    static constexpr long localCharge = 336;
    static void* operator new(std::size_t bytes, HeapTag) noexcept {
        return allocate(bytes, localCharge);
    }
    explicit J2DPicture(const ResTIMG* image = nullptr) { append(image, 1); }
    void append(const ResTIMG* image, f32) {
        if (image != nullptr) {
            textures.at(textureCount++) = image;
        }
    }
    const ResTIMG* getTexture(u8 index) const {
        return index < textures.size() ? textures[index] : nullptr;
    }
    u8 getTextureCount() const { return textureCount; }
    void setBlackWhite(JUtility::TColor dark, JUtility::TColor light) { black = dark; white = light; }
    JUtility::TColor getBlack() const { return black; }
    JUtility::TColor getWhite() const { return white; }
    void setBlendRatio(f32 weight, f32) { blend = weight; }
    void setAlpha(u8) {}
    void rotate(f32, f32, int, f32) {}
    void draw(f32, f32, f32 width, f32, bool, bool mirrorY, bool) {
        draws.push_back({textures[0], white.rgba, blend, width, mirrorY});
    }

    std::array<const ResTIMG*, 2> textures{};
    u8 textureCount{};
    JUtility::TColor black{0}, white{0xffffffff};
    f32 blend{1};
};

struct {
    f32 mPortalIconScale = 2.0f;
} g_MsgObject_HIO_c;
namespace dusk::version {
bool isRegionJpn() { return false; }
}

#ifndef DUSK_TEST_OUT_FONT_SOURCE
#define DUSK_TEST_OUT_FONT_SOURCE "../../../src/d/d_msg_out_font.cpp"
#endif
#include DUSK_TEST_OUT_FONT_SOURCE

void queue(COutFont_c& font, u8 glyph, J2DTextBox* text = nullptr) {
    font.drawFont(text, glyph, 1, 2, 3, 4, 0xff00ffff, 255);
}

int main() {
    constexpr long queueBytes = 35 * sizeof(COutFontSet_c);
    constexpr long fixtureLargest = queueBytes + 41 * J2DPicture::localCharge + 256;
    JKRHeap scene{767792, fixtureLargest, 104};
    JKRHeap ambient{4096, 0, 0};
    sceneHeap = &scene;
    JKRSetCurrentHeap(&scene);
    {
        COutFont_c font(1);
        font.createPane(); // The eager Alpha 17 path fails here at index 41.
        assert(mainArchive.lookups.empty() && itemArchive.lookups.empty());
        assert(scene.largest == fixtureLargest - queueBytes);

        JKRSetCurrentHeap(&ambient);
        queue(font, 41);
        queue(font, 25);
        queue(font, 9);
        queue(font, 3);
        queue(font, 30);
        assert(JKRGetCurrentHeap() == &ambient && ambient.largest == 0);
        assert(scene.largest == fixtureLargest - queueBytes - 6 * J2DPicture::localCharge);
        assert(itemArchive.lookups == std::vector<std::string>{"st_bompoach_lv1.bti"});
        assert(font.mpPane[9]->getTextureCount() == 2);
        assert(font.mpPane[9]->getTexture(1)->name == "font_07_02.bti");
        assert(font.mpPane[26]->getTexture(0)->name == "font_16_backlight.bti");
        assert(font.mpPane[42] == nullptr);

        font.setAlphaRatio(1.0f);
        font.draw(nullptr, 0, 0, 1);
        assert(draws.size() == 7);
        assert(draws[0].image->name == "st_bompoach_lv1.bti");
        assert(draws[1].image->name == "font_16_backlight.bti" && draws[1].width == 6);
        assert(draws[2].image->name == "im_map_icon_portal_4ia_40_05.bti");
        assert(draws[3].blend < 1 && draws[3].blend > 0);
        assert(draws[4].white == 0xff && draws[5].white == 0x787878ff);
        assert(draws[6].width == 6); // Rupee aspect derived from its original image.

        font.initialize();
        for (int i = 0; i < 35; ++i) {
            queue(font, 9);
        }
        queue(font, 42); // A full queue should not allocate another pane.
        font.createPane(); // Repeated setup must keep the selected owner and loaded pages.
        assert(font.mpPane[42] == nullptr);
        assert(scene.largest == fixtureLargest - queueBytes - 6 * J2DPicture::localCharge);

        std::array<J2DPicture, 70> borrowed;
        font.setPane(borrowed.data());
        assert(scene.largest == fixtureLargest - queueBytes);
        font.initialize();
        queue(font, 41);
        assert(font.mpPane[41] == &borrowed[41]);
    }
    assert(allocations.empty() && scene.largest == fixtureLargest);

    JKRHeap roomy{65536, 65536, 0};
    JKRSetCurrentHeap(&roomy);
    {
        COutFont_c font(0);
        font.createPane();
        J2DTextBox text;
        for (u8 glyph = 0; glyph < 70; ++glyph) {
            draws.clear();
            queue(font, glyph, &text);
            font.draw(&text, 0, 0, 1);
            assert(!draws.empty());
            assert(font.mpPane[glyph]->getTexture(0)->name == font.getBtiName(glyph));
            font.reset(&text);
        }
    }
    assert(allocations.empty() && roomy.largest == 65536);
    std::puts("font panes: Alpha 17 heap fixture, all 70 glyphs, routing and teardown passed");
}
