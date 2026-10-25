// Private host-only harness. Links only rg_etc1 (zlib license), not the game.
// The upstream codec and its notices remain unmodified in Upstream/.
#include "rg_etc1.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    std::ifstream input(argv[2], std::ios::binary);
    if (!input) return 3;
    std::vector<unsigned char> in((std::istreambuf_iterator<char>(input)), {}), out;
    if (in.size() > 8*1024*1024) return 4;
    const std::string mode = argv[1];
    const auto start = std::chrono::steady_clock::now();
    if (mode == "decode") {
        if (in.size()%8) return 5;
        for (std::size_t i=0; i<in.size(); i+=8) {
            std::array<unsigned,16> pixels{};
            if (!rg_etc1::unpack_etc1_block(in.data()+i,pixels.data())) return 6;
            auto* p=reinterpret_cast<unsigned char*>(pixels.data());
            out.insert(out.end(),p,p+64);
        }
    } else if (mode == "encode") {
        // Input is a list of ordinary row-major 4x4 RGBA blocks. Output is
        // standard big-endian ETC1 blocks. PICA tiling is done by the caller.
        if (in.size()%64) return 5;
        rg_etc1::pack_etc1_block_init();
        rg_etc1::etc1_pack_params params;
        params.m_quality=rg_etc1::cMediumQuality;
        params.m_dithering=false;
        for (std::size_t i=0; i<in.size(); i+=64) {
            std::array<unsigned,16> pixels{};
            std::memcpy(pixels.data(),in.data()+i,64);
            for (unsigned& pixel:pixels) pixel |= 0xff000000u;
            std::array<unsigned char,8> block{};
            rg_etc1::pack_etc1_block(block.data(),pixels.data(),params);
            out.insert(out.end(),block.begin(),block.end());
        }
    } else return 7;
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::ofstream output(argv[3],std::ios::binary);
    output.write(reinterpret_cast<const char*>(out.data()),out.size());
    if (!output) return 8;
    std::printf("{\"codec_ms_including_init\":%.6f,\"blocks\":%zu}\n",ms,in.size()/(mode=="encode"?64:8));
    return 0;
}
