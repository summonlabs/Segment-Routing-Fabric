// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <string_view>

namespace srf {

inline constexpr std::uint16_t kVersionMajor = 1;
inline constexpr std::uint16_t kVersionMinor = 0;
inline constexpr std::uint16_t kVersionPatch = 1;

/// Wire-compatible protocol revision. Bumped only when the wire encoding changes.
inline constexpr std::uint16_t kWireProtocolVersion = 1;

/// On-disk persistence format revision.
inline constexpr std::uint16_t kPersistenceFormatVersion = 1;

[[nodiscard]] constexpr std::string_view version_string() noexcept { return "1.0.1"; }

/// Classification of what this build can actually prove. This runtime deliberately
/// implements no physical segment-routing programming; every profile it ships is
/// ABSTRACT/SYNTHETIC. See README.md for the exact boundary.
[[nodiscard]] constexpr std::string_view support_classification() noexcept {
    return "ABSTRACT/SYNTHETIC";
}

} // namespace srf
