// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <compare>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "srf/bytes.hpp"

namespace srf {

/// 128-bit deterministic integrity/identity digest.
///
/// This is an FNV-1a derived non-cryptographic digest. It is used for identity,
/// change detection and integrity trailers. It is NOT a cryptographic hash and
/// this runtime claims no cryptographic authentication of any kind.
struct Digest128 {
    std::uint64_t hi{0};
    std::uint64_t lo{0};

    [[nodiscard]] constexpr bool is_zero() const noexcept { return hi == 0 && lo == 0; }
    [[nodiscard]] std::string hex() const;

    friend constexpr bool operator==(const Digest128&, const Digest128&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const Digest128& a,
                                                      const Digest128& b) noexcept {
        if (a.hi != b.hi) {
            return a.hi <=> b.hi;
        }
        return a.lo <=> b.lo;
    }
};

[[nodiscard]] Digest128 digest128(std::span<const std::byte> data) noexcept;
[[nodiscard]] Digest128 digest128(std::string_view data) noexcept;
[[nodiscard]] std::uint64_t fnv1a64(std::span<const std::byte> data) noexcept;

/// CRC-32C (Castagnoli), used only as a persistence/wire integrity trailer.
[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> data) noexcept;
[[nodiscard]] std::uint32_t crc32c_extend(std::uint32_t seed,
                                          std::span<const std::byte> data) noexcept;

[[nodiscard]] std::string to_hex(std::uint64_t value);
[[nodiscard]] std::string to_hex(std::uint32_t value, int width);

} // namespace srf
