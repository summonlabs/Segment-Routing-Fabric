// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "srf/list.hpp"

namespace srf {

/// An immutable point-in-time copy of one list revision. A snapshot is a value: it
/// is never invalidated by later mutations, and it carries the exact digest and
/// authority that were current when it was taken.
struct SegmentListSnapshot {
    SnapshotId id{};
    std::uint64_t revision{0};
    CoordinatorEpoch epoch{};
    SegmentList list{};
    Digest128 digest{};

    [[nodiscard]] bool valid() const noexcept { return id.valid(); }
};

enum class DiffKind : std::uint8_t {
    Invalid = 0,
    Insert = 1,
    Remove = 2,
    Replace = 3,
    Reorder = 4,
    ProfileChange = 5,
    PolicyChange = 6,
    DependencyChange = 7,
    CurrentnessChange = 8,
    LifecycleChange = 9,
    GenerationAdvance = 10,
    StrictnessChange = 11,
    AuthorityChange = 12,
    Created = 13,
    Destroyed = 14,
};

struct DiffEntry {
    DiffKind kind{DiffKind::Invalid};
    std::uint32_t index{0};
    std::uint32_t other_index{0};
    std::uint64_t detail{0};

    friend bool operator==(const DiffEntry&, const DiffEntry&) = default;
};

struct SnapshotDiff {
    SnapshotId from{};
    SnapshotId to{};
    SegmentListId list{};
    std::vector<DiffEntry> entries{};
    bool truncated{false};
    bool order_changed{false};

    [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
    [[nodiscard]] bool contains(DiffKind kind) const noexcept;
};

enum class ExplanationTopic : std::uint8_t {
    Invalid = 0,
    WhyActive = 1,
    WhyNotActive = 2,
    WhySegmentInvalid = 3,
    MissingCapability = 4,
    StaleTopology = 5,
    WhySuperseded = 6,
    WhyRevalidationRequired = 7,
    Currentness = 8,
    Lineage = 9,
};

struct ExplanationEntry {
    ReasonCode code{ReasonCode::Ok};
    std::uint32_t index{0};
    std::uint64_t detail{0};

    friend bool operator==(const ExplanationEntry&, const ExplanationEntry&) = default;
};

struct Explanation {
    ExplanationTopic topic{ExplanationTopic::Invalid};
    SegmentListId list{};
    SegmentListGeneration generation{};
    LifecycleState state{LifecycleState::Invalid};
    Currentness currentness{Currentness::Unknown};
    std::vector<ExplanationEntry> entries{};
    bool truncated{false};

    [[nodiscard]] bool contains(ReasonCode code) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
};

[[nodiscard]] std::string_view diff_kind_name(DiffKind kind) noexcept;
[[nodiscard]] std::string_view explanation_topic_name(ExplanationTopic topic) noexcept;

} // namespace srf
