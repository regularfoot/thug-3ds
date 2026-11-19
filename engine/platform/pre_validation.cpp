#include <source_port/files.h>
#include <cstdint>
#include <cstring>
#include <climits>
#include <cerrno>
namespace {
std::uint32_t le32(const unsigned char* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
bool bad() { errno = EILSEQ; return false; }
// Validate output extent without replacing the original LZSS decoder. An
// incomplete back-reference used to read beyond the archive in DecodeLZSS.
bool compressed_extent(const unsigned char* p, std::size_t n, std::size_t expected) {
    std::size_t in = 0, out = 0;
    unsigned flags = 0;
    while (in < n) {
        if (((flags >>= 1) & 256) == 0) flags = p[in++] | 0xff00;
        if (in == n) return false;
        std::size_t count;
        if (flags & 1) { ++in; count = 1; }
        else {
            if (n - in < 2) return false;
            count = (p[in + 1] & 15) + 3;
            in += 2;
        }
        if (count > expected - out) return false;
        out += count;
    }
    return out == expected;
}
}
namespace SourcePort {
bool ValidatePre(const unsigned char* p, std::size_t n) {
    if (!p || n < 12 || n > INT_MAX || le32(p) != n || le32(p + 4) != 0xabcd0003) return bad();
    auto entries = le32(p + 8);
    if (entries > (n - 12) / 20) return bad();
    std::size_t at = 12;
    for (std::uint32_t i = 0; i < entries; ++i) {
        if (n - at < 16) return bad();
        std::size_t size = le32(p + at), compressed = le32(p + at + 4);
        unsigned name_size = unsigned(p[at + 8]) | (unsigned(p[at + 9]) << 8);
        if (size > INT_MAX || compressed >= 0x2000000 || name_size < 4 || name_size > 32767 || name_size % 4) return bad();
        at += 16;
        if (name_size > n - at) return bad();
        const auto* end = static_cast<const unsigned char*>(std::memchr(p + at, 0, name_size));
        if (!end || end == p + at || end - (p + at) >= 128) return bad();
        at += name_size;
        auto actual = compressed ? compressed : size;
        auto padded = (actual + 3) & ~std::size_t(3);
        if (padded > n - at) return bad();
        if (compressed && !compressed_extent(p + at, compressed, size)) return bad();
        at += padded;
    }
    return at == n ? true : bad();
}
}
