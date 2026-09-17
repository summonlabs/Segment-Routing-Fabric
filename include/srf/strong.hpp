// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>

namespace srf {

/// Strongly typed identifier. Distinct Tag types are distinct types: there is no
/// implicit conversion between identifier domains. The zero value is the invalid
/// sentinel and is rejected wherever an identifier is semantically required.
template <class Tag, class T = std::uint64_t>
class StrongId {
public:
    using value_type = T;
    using tag_type = Tag;

    constexpr StrongId() noexcept = default;
    constexpr explicit StrongId(T value) noexcept : value_(value) {}

    [[nodiscard]] constexpr T value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != T{}; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

    friend constexpr bool operator==(StrongId, StrongId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(StrongId, StrongId) noexcept = default;

private:
    T value_{};
};

/// Checked, monotonic, non-wrapping generation counter. Generation 0 is the
/// invalid sentinel. next() returns std::nullopt at the maximum value: the caller
/// must surface exhaustion rather than wrap.
template <class Tag>
class Generation {
public:
    using value_type = std::uint64_t;
    using tag_type = Tag;

    static constexpr std::uint64_t max_value = 0xFFFF'FFFF'FFFF'FFFFull;

    constexpr Generation() noexcept = default;
    constexpr explicit Generation(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] static constexpr Generation initial() noexcept { return Generation(1); }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
    [[nodiscard]] constexpr bool exhausted() const noexcept { return value_ == max_value; }

    [[nodiscard]] constexpr std::optional<Generation> next() const noexcept {
        if (value_ >= max_value) {
            return std::nullopt;
        }
        return Generation(value_ + 1);
    }

    friend constexpr bool operator==(Generation, Generation) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(Generation, Generation) noexcept = default;

private:
    std::uint64_t value_{};
};

/// Explicit conversion helpers. These exist so call sites are forced to spell out
/// the domain they are moving between; nothing converts implicitly.
template <class ToId, class Tag>
[[nodiscard]] constexpr ToId id_cast(StrongId<Tag> from) noexcept {
    return ToId{from.value()};
}

template <class ToGen, class Tag>
[[nodiscard]] constexpr ToGen generation_cast(Generation<Tag> from) noexcept {
    return ToGen{from.value()};
}

} // namespace srf

namespace std {

template <class Tag, class T>
struct hash<srf::StrongId<Tag, T>> {
    [[nodiscard]] size_t operator()(const srf::StrongId<Tag, T>& v) const noexcept {
        return std::hash<T>{}(v.value());
    }
};

template <class Tag>
struct hash<srf::Generation<Tag>> {
    [[nodiscard]] size_t operator()(const srf::Generation<Tag>& v) const noexcept {
        return std::hash<std::uint64_t>{}(v.value());
    }
};

} // namespace std
