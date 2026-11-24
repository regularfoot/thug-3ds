#include "rg_etc1.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>

// Input is a stream of 256x128 RGBA frames from FFmpeg. PICA samples the four
// ETC1 blocks in each 8x8 tile in row-major order with reversed block bytes.
int main() {
    constexpr unsigned width = 256, height = 128;
    std::array<std::uint32_t, width * height> rgba;
    std::array<unsigned char, width * height / 2> encoded;
    rg_etc1::pack_etc1_block_init();
    rg_etc1::etc1_pack_params params;
    params.clear();
    params.m_quality = rg_etc1::cLowQuality;
    for (;;) {
        const auto n = std::fread(rgba.data(), 1, sizeof(rgba), stdin);
        if (!n && std::feof(stdin)) return std::fflush(stdout) == 0 ? 0 : 1;
        if (n != sizeof(rgba)) return 1;
        auto dest = encoded.begin();
        for (unsigned y = 0; y < height; y += 8) {
            for (unsigned x = 0; x < width; x += 8) {
                for (unsigned block = 0; block < 4; ++block) {
                    unsigned texels[16];
                    for (unsigned row = 0; row < 4; ++row) {
                        auto src = rgba.data() + (y + block / 2 * 4 + row) * width + x + block % 2 * 4;
                        std::copy_n(src, 4, texels + row * 4);
                    }
                    unsigned char compressed[8];
                    rg_etc1::pack_etc1_block(compressed, texels, params);
                    dest = std::reverse_copy(compressed, compressed + 8, dest);
                }
            }
        }
        if (std::fwrite(encoded.data(), 1, encoded.size(), stdout) != encoded.size()) return 1;
    }
}
