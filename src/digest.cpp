// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/digest.hpp"

#include <array>
#include <cstdio>

namespace srf {
namespace {

constexpr std::uint64_t kFnvOffsetA = 0xCBF29CE484222325ull;
constexpr std::uint64_t kFnvPrime = 0x00000100000001B3ull;
constexpr std::uint64_t kFnvOffsetB = 0x9E3779B97F4A7C15ull;
constexpr std::uint64_t kFnvOffsetC = 0x2545F4914F6CDD1Dull;

[[nodiscard]] std::uint64_t fnv1a_seeded(std::uint64_t seed,
                                         std::span<const std::byte> data) noexcept {
    std::uint64_t h = seed;
    for (const std::byte b : data) {
        h ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(b));
        h *= kFnvPrime;
    }
    return h;
}

constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
    std::array<std::uint32_t, 256> table{};
    constexpr std::uint32_t poly = 0x82F63B78u;
    // Iterated by reference rather than by subscript so that the bound is carried
    // by the container itself.
    std::uint32_t index = 0;
    for (std::uint32_t& slot : table) {
        std::uint32_t c = index;
        ++index;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) != 0u ? (poly ^ (c >> 1)) : (c >> 1);
        }
        slot = c;
    }
    return table;
}

constexpr auto kCrc32cTable = make_crc32c_table();

} // namespace

Digest128 digest128(std::span<const std::byte> data) noexcept {
    const std::uint64_t a = fnv1a_seeded(kFnvOffsetA, data);
    // Domain-separated second lane: length-prefixed so that padding or extension
    // is always detectable, then folded twice with a distinct offset basis.
    std::array<std::byte, 8> len{};
    const std::uint64_t n = static_cast<std::uint64_t>(data.size());
    for (int i = 0; i < 8; ++i) {
        len[static_cast<std::size_t>(i)] = static_cast<std::byte>((n >> (8 * i)) & 0xFFu);
    }
    std::uint64_t b = fnv1a_seeded(kFnvOffsetB, data);
    b = fnv1a_seeded(b, std::span<const std::byte>(len.data(), len.size()));
    b = fnv1a_seeded(b ^ kFnvOffsetC, data);

    Digest128 out{};
    out.hi = a;
    out.lo = b;
    if (out.hi == 0 && out.lo == 0) {
        out.lo = 1;  // keep the digest distinguishable from the absent sentinel
    }
    return out;
}

Digest128 digest128(std::string_view data) noexcept {
    return digest128(as_bytes(data));
}

std::uint64_t fnv1a64(std::span<const std::byte> data) noexcept {
    const std::uint64_t h = fnv1a_seeded(kFnvOffsetA, data);
    return h == 0 ? 1 : h;
}

std::uint32_t crc32c_extend(std::uint32_t seed, std::span<const std::byte> data) noexcept {
    std::uint32_t c = ~seed;
    for (const std::byte b : data) {
        c = kCrc32cTable[(c ^ static_cast<std::uint32_t>(static_cast<std::uint8_t>(b))) & 0xFFu] ^
            (c >> 8);
    }
    return ~c;
}

std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
    return crc32c_extend(0, data);
}

std::string to_hex(std::uint64_t value) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
    return std::string(buf);
}

std::string to_hex(std::uint32_t value, int width) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%0*x", width, static_cast<unsigned int>(value));
    return std::string(buf);
}

std::string Digest128::hex() const {
    return to_hex(hi) + to_hex(lo);
}

} // namespace srf
