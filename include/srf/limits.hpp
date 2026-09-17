// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>

namespace srf {

/// Every bound the runtime enforces. Each field is consulted on at least one code
/// path; tests/unit/test_limits.cpp proves that each bound is observable.
struct Limits {
    /// Maximum number of live segment-list lineages retained in the store.
    std::uint32_t max_lists = 4096;
    /// Maximum segments in one ordered segment list (create/replace/batch/load/wire).
    std::uint32_t max_segments_per_list = 32;
    /// Maximum raw payload bytes carried by one segment before canonical padding.
    std::uint32_t max_segment_payload_bytes = 16;
    /// Maximum registered segment profiles.
    std::uint32_t max_profiles = 64;
    /// Maximum registered segment policies.
    std::uint32_t max_policies = 256;
    /// Maximum mutations in one batch request.
    std::uint32_t max_batch_size = 64;
    /// Maximum registered publishers (live worker sessions) per coordinator.
    std::uint32_t max_publishers = 32;
    /// Maximum encoded frame size accepted or produced by the wire codec.
    std::uint32_t max_frame_bytes = 262144;
    /// Maximum retained durable history entries.
    std::uint32_t max_history = 256;
    /// Maximum entries in one explanation.
    std::uint32_t max_explanation_entries = 64;
    /// Maximum bytes in one persistence record body.
    std::uint32_t max_persistence_record_bytes = 1 << 20;
    /// Maximum reasons retained in one complete validation reason list.
    std::uint32_t max_reasons = 32;
    /// Maximum diff entries produced by one snapshot comparison.
    std::uint32_t max_diff_entries = 512;
    /// Maximum total segments retained across all live lists.
    std::uint32_t max_total_segments = 65536;
    /// Maximum retained idempotency (mutation attempt) records.
    std::uint32_t max_attempt_records = 4096;
    /// Maximum evidence records accepted in one evidence snapshot.
    std::uint32_t max_evidence_records = 8192;
};

} // namespace srf
