#pragma once
#include <cstdint>
#include <span>

namespace dusk::native {
// The original GLY1 block remains caller-owned. This bounded render-owner
// adapter copies one validated page into a BTI and aliases its original image.
bool registerFontPage(const void* font,std::span<const std::uint8_t> gly1,unsigned page);
bool releaseFontPages(const void* font);
std::size_t fontPageBytes();
}
