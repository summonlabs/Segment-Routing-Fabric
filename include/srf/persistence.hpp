// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

#include "srf/authority.hpp"
#include "srf/list.hpp"
#include "srf/policy.hpp"
#include "srf/profile.hpp"
#include "srf/reason.hpp"
#include "srf/version.hpp"

namespace srf {

/// One retained idempotency record. Exact replay of a committed attempt must be
/// idempotent across a restart, and reuse of the same attempt id with different
/// content must be rejected even after the writer is gone.
struct DurableAttemptRecord {
    MutationAttemptId attempt{};
    SegmentListId list{};
    Digest128 payload{};
    SegmentListGeneration generation{};
    bool committed{false};

    friend bool operator==(const DurableAttemptRecord&, const DurableAttemptRecord&) = default;
};

/// Bounded, append-only lineage history.
struct DurableHistoryEntry {
    HistoryId id{};
    SegmentListId list{};
    SegmentListGeneration generation{};
    LifecycleState state{LifecycleState::Invalid};
    Currentness currentness{Currentness::Unknown};
    Digest128 digest{};
    std::uint64_t revision{0};

    friend bool operator==(const DurableHistoryEntry&, const DurableHistoryEntry&) = default;
};

/// The complete durable state of one coordinator. Everything the coordinator must
/// survive a hard restart with lives here.
struct DurableState {
    std::uint16_t format_version{kPersistenceFormatVersion};
    CoordinatorEpoch epoch{};
    std::uint64_t revision{0};

    std::vector<SegmentProfile> profiles{};
    std::vector<SegmentPolicy> policies{};
    std::vector<SegmentList> lists{};
    std::vector<DurableHistoryEntry> history{};
    std::vector<DurableAttemptRecord> attempts{};
    std::vector<PublisherState> publishers{};
    std::vector<WorkerBootId> fenced_boots{};
};

struct LoadReport {
    bool loaded{false};
    bool empty{false};
    ValidationResult result{};
    DurableState state{};
    std::size_t lists_recovered{0};
    std::size_t lists_requiring_revalidation{0};
    std::size_t publishers_fenced{0};
    std::size_t history_entries{0};
};

/// Durable storage. save() must not return true before the bytes are durable.
class IPersistence {
public:
    IPersistence() = default;
    virtual ~IPersistence() = default;
    IPersistence(const IPersistence&) = delete;
    IPersistence& operator=(const IPersistence&) = delete;

    [[nodiscard]] virtual bool save(const DurableState& state, const Limits& limits,
                                    ValidationResult& out) = 0;
    [[nodiscard]] virtual bool load(LoadReport& report, const Limits& limits) = 0;
    [[nodiscard]] virtual bool erase() = 0;
};

/// Atomic file-backed persistence: magic, version, checked counts, deterministic
/// encoding, integrity trailer and replace-by-rename.
class FilePersistence final : public IPersistence {
public:
    explicit FilePersistence(std::filesystem::path path);
    ~FilePersistence() override = default;

    [[nodiscard]] bool save(const DurableState& state, const Limits& limits,
                            ValidationResult& out) override;
    [[nodiscard]] bool load(LoadReport& report, const Limits& limits) override;
    [[nodiscard]] bool erase() override;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

/// In-memory persistence with fault injection. Used by the proofs that a mutation
/// is never acknowledged before it is durable.
class MemoryPersistence final : public IPersistence {
public:
    MemoryPersistence() = default;
    ~MemoryPersistence() override = default;

    [[nodiscard]] bool save(const DurableState& state, const Limits& limits,
                            ValidationResult& out) override;
    [[nodiscard]] bool load(LoadReport& report, const Limits& limits) override;
    [[nodiscard]] bool erase() override;

    /// Fail the next save call. The store must not acknowledge the mutation.
    void fail_next_save();
    /// Corrupt one byte of the stored image.
    [[nodiscard]] bool corrupt_byte(std::size_t offset, std::uint8_t xor_mask);
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {bytes_.data(), bytes_.size()};
    }
    [[nodiscard]] std::size_t save_count() const noexcept { return save_count_; }
    [[nodiscard]] bool has_content() const noexcept { return !bytes_.empty(); }
    void set_bytes(std::span<const std::byte> bytes);

private:
    std::vector<std::byte> bytes_{};
    std::size_t save_count_{0};
    bool fail_next_{false};
};

/// Deterministic encoding. Returns an empty buffer and a failed result when a
/// record exceeds the configured maximum persistence record size.
[[nodiscard]] ByteBuffer encode_durable_state(const DurableState& state, const Limits& limits,
                                             ValidationResult& result);

[[nodiscard]] bool decode_durable_state(std::span<const std::byte> bytes, const Limits& limits,
                                        DurableState& out, ValidationResult& result);

} // namespace srf
