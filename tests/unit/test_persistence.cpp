// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "fixtures.hpp"

#include <cstring>
#include <filesystem>

using srf::test::Harness;
using srf::test::list_id;
using srf::test::node_segment;

namespace {

/// Byte-surgery helper for the adversarial persistence suite. It can repair the
/// integrity trailers so that a targeted corruption reaches the structural checks
/// instead of being caught by the CRC.
class Image {
public:
    explicit Image(std::span<const std::byte> bytes) : bytes_(bytes.begin(), bytes.end()) {}

    [[nodiscard]] const std::vector<std::byte>& bytes() const { return bytes_; }
    [[nodiscard]] std::span<const std::byte> span() const {
        return {bytes_.data(), bytes_.size()};
    }
    [[nodiscard]] std::size_t size() const { return bytes_.size(); }

    [[nodiscard]] bool in_range(std::size_t offset, std::size_t length) const {
        return offset <= bytes_.size() && length <= bytes_.size() - offset;
    }
    void set_u16(std::size_t offset, std::uint16_t value) {
        if (in_range(offset, sizeof(value))) {
            std::memcpy(bytes_.data() + offset, &value, sizeof(value));
        }
    }
    void set_u32(std::size_t offset, std::uint32_t value) {
        if (in_range(offset, sizeof(value))) {
            std::memcpy(bytes_.data() + offset, &value, sizeof(value));
        }
    }
    void set_u64(std::size_t offset, std::uint64_t value) {
        if (in_range(offset, sizeof(value))) {
            std::memcpy(bytes_.data() + offset, &value, sizeof(value));
        }
    }
    void set_byte(std::size_t offset, std::uint8_t value) {
        if (in_range(offset, 1)) {
            bytes_[offset] = static_cast<std::byte>(value);
        }
    }
    /// Recompute the integrity trailers over whatever the image currently is. A
    /// corrupted length field simply means the payload trailer cannot be located;
    /// the header trailer is still repaired so the structural check is reached.
    void repair() {
        if (bytes_.size() < 28) {
            return;
        }
        set_u32(20, srf::crc32c(std::span<const std::byte>(bytes_.data(), 20)));
        std::uint64_t payload_length = 0;
        std::memcpy(&payload_length, bytes_.data() + 8, sizeof(payload_length));
        if (payload_length > bytes_.size() - 28) {
            return;
        }
        const auto length = static_cast<std::size_t>(payload_length);
        const std::uint32_t payload =
            srf::crc32c(std::span<const std::byte>(bytes_.data() + 24, length));
        set_u32(24 + length, payload);
    }

    /// Offset of the body of the first record of the given type.
    [[nodiscard]] std::size_t find_record(std::uint16_t type) const {
        std::uint64_t payload_length = 0;
        std::memcpy(&payload_length, bytes_.data() + 8, sizeof(payload_length));
        std::size_t cursor = 24;
        const std::size_t end = 24 + static_cast<std::size_t>(payload_length);
        while (cursor + 6 <= end) {
            std::uint16_t record_type = 0;
            std::uint32_t length = 0;
            std::memcpy(&record_type, bytes_.data() + cursor, sizeof(record_type));
            std::memcpy(&length, bytes_.data() + cursor + 2, sizeof(length));
            if (record_type == type) {
                return cursor + 6;
            }
            cursor += 6 + length;
        }
        return bytes_.size();
    }

private:
    std::vector<std::byte> bytes_;
};

constexpr std::uint16_t kListRecord = 4;
constexpr std::uint16_t kProfileRecord = 2;

srf::DurableState decode_ok(const Image& image, const srf::Limits& limits) {
    srf::DurableState state{};
    srf::ValidationResult result;
    const bool ok = srf::decode_durable_state(image.span(), limits, state, result);
    (void)ok;
    return state;
}

} // namespace

SRF_TEST(persistence, engine_round_trips_through_memory_and_file) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1), node_segment(2)}).ok());
    SRF_EXPECT(harness.create(2, {srf::test::adjacency_segment(1)}).ok());
    const srf::DurableState original = harness.store().export_state();

    srf::ValidationResult encode_result;
    const srf::ByteBuffer bytes = srf::encode_durable_state(original, harness.store().limits(),
                                                            encode_result);
    SRF_EXPECT(encode_result.ok());
    SRF_EXPECT(!bytes.empty());

    srf::DurableState decoded{};
    srf::ValidationResult decode_result;
    SRF_EXPECT(srf::decode_durable_state(srf::as_bytes(bytes), harness.store().limits(), decoded,
                                         decode_result));
    SRF_EXPECT(decode_result.ok());
    SRF_EXPECT_EQ(decoded.lists.size(), original.lists.size());
    SRF_EXPECT_EQ(decoded.lists.size(), static_cast<std::size_t>(2));
    for (std::size_t i = 0; i < decoded.lists.size(); ++i) {
        SRF_EXPECT(decoded.lists[i].content.id == original.lists[i].content.id);
        SRF_EXPECT(decoded.lists[i].generation == original.lists[i].generation);
        SRF_EXPECT(decoded.lists[i].content_digest == original.lists[i].content_digest);
        SRF_EXPECT(decoded.lists[i].sequence_digest == original.lists[i].sequence_digest);
        SRF_EXPECT_EQ(decoded.lists[i].content.segments.size(),
                      original.lists[i].content.segments.size());
        for (std::size_t s = 0; s < decoded.lists[i].content.segments.size(); ++s) {
            SRF_EXPECT(decoded.lists[i].content.segments[s] ==
                       original.lists[i].content.segments[s]);
        }
    }
    SRF_EXPECT_EQ(decoded.history.size(), original.history.size());
    SRF_EXPECT_EQ(decoded.attempts.size(), original.attempts.size());

    // Encoding is deterministic.
    srf::ValidationResult second_result;
    const srf::ByteBuffer again = srf::encode_durable_state(original, harness.store().limits(),
                                                           second_result);
    SRF_EXPECT(again == bytes);
}

SRF_TEST(persistence, file_backend_replaces_atomically_and_reads_back) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "srf_persistence_test";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    const std::filesystem::path path = directory / "state.srfstate";

    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    srf::FilePersistence persistence(path);
    srf::ValidationResult result;
    SRF_EXPECT(persistence.save(harness.store().export_state(), harness.store().limits(), result));
    SRF_EXPECT(std::filesystem::exists(path));
    SRF_EXPECT(!std::filesystem::exists(directory / "state.srfstate.tmp"));

    srf::LoadReport report{};
    SRF_EXPECT(persistence.load(report, harness.store().limits()));
    SRF_EXPECT(report.loaded);
    SRF_EXPECT_EQ(report.lists_recovered, static_cast<std::size_t>(1));

    SRF_EXPECT(persistence.erase());
    srf::LoadReport empty_report{};
    SRF_EXPECT(persistence.load(empty_report, harness.store().limits()));
    SRF_EXPECT(!empty_report.loaded);
    SRF_EXPECT(empty_report.empty);
    std::filesystem::remove_all(directory, ec);
}

SRF_TEST(persistence, empty_image_is_rejected) {
    srf::DurableState state{};
    srf::ValidationResult result;
    SRF_EXPECT(!srf::decode_durable_state(std::span<const std::byte>{}, srf::Limits{}, state,
                                          result));
    SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistEmpty);
}

SRF_TEST(persistence, structural_corruptions_are_rejected_with_specific_reasons) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    const std::span<const std::byte> valid = harness.memory()->bytes();
    const srf::Limits limits{};

    {
        Image image(valid);
        image.set_byte(0, 'X');
        image.repair();
        srf::DurableState state{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(image.span(), limits, state, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistBadMagic);
    }
    {
        Image image(valid);
        image.set_u16(4, 99);
        image.repair();
        srf::DurableState state{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(image.span(), limits, state, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistBadVersion);
    }
    {
        Image image(valid);
        image.set_u16(6, 4);  // unsupported flags
        image.repair();
        srf::DurableState state{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(image.span(), limits, state, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistBadVersion);
    }
    {
        Image image(valid);
        image.set_u64(8, 0xFFFF'FFFF'FFFFull);
        image.repair();
        srf::DurableState state{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(image.span(), limits, state, result));
        SRF_EXPECT(result.contains(srf::ReasonCode::PersistOverflow) ||
                   result.contains(srf::ReasonCode::PersistTruncated));
    }
    {
        Image image(valid);
        image.set_u32(16, 0xFFFF);
        image.repair();
        srf::DurableState state{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(image.span(), limits, state, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistAbsurdCount);
    }
    {
        std::vector<std::byte> extended(valid.begin(), valid.end());
        extended.push_back(std::byte{0});
        srf::DurableState state{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(
            std::span<const std::byte>(extended.data(), extended.size()), limits, state, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistTrailingBytes);
    }
    {
        std::vector<std::byte> truncated(valid.begin(), valid.end() - 2);
        srf::DurableState state{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(
            std::span<const std::byte>(truncated.data(), truncated.size()), limits, state, result));
        SRF_EXPECT(result.contains(srf::ReasonCode::PersistTruncated) ||
                   result.contains(srf::ReasonCode::PersistBadIntegrity));
    }
    {
        Image image(valid);
        image.set_byte(3, static_cast<std::uint8_t>(image.bytes()[3]) ^ 0x01);
        srf::DurableState state{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(image.span(), limits, state, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistBadIntegrity);
    }
    {
        // A corrupted record length far beyond the configured bound.
        Image image(valid);
        const std::size_t record = image.find_record(kListRecord);
        SRF_EXPECT(record < image.size());
        image.set_u32(record - 4, 0x7FFF'FFFF);
        image.repair();
        srf::DurableState state{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(image.span(), limits, state, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistAbsurdCount);
    }
    {
        // A corrupted record type.
        Image image(valid);
        const std::size_t record = image.find_record(kListRecord);
        image.set_u16(record - 6, 0x00FF);
        image.repair();
        srf::DurableState state{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(image.span(), limits, state, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistBadRecord);
    }
    {
        // Invalid segment kind inside a list record.
        Image image(valid);
        const std::size_t record = image.find_record(kListRecord);
        // Byte offset of the first segment inside a list record body:
        // 4+2 u64 identity, u8+u16 strictness/width, 4+4 u64 dependency
        // generations, 1 u64 revision generation, 3 u64 lineage, 4 u64 authority,
        // 11+1 u64 provenance, u8+u8 lifecycle, 4 u64 digests, u64 revision,
        // u32 segment count.
        constexpr std::size_t kListBodyPrefix = 321;
        const std::size_t segment_offset = record + kListBodyPrefix;
        image.set_u16(segment_offset, 0);
        image.repair();
        srf::DurableState state{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(image.span(), limits, state, result));
        SRF_EXPECT(result.contains(srf::ReasonCode::PersistKindInvalid) ||
                   result.contains(srf::ReasonCode::PersistCorrupt) ||
                   result.contains(srf::ReasonCode::PersistDigestMismatch));
    }
}

SRF_TEST(persistence, structural_state_defects_are_rejected) {
    const srf::Limits limits{};
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1)}).ok());
    const srf::DurableState valid = harness.store().export_state();

    {
        // Duplicate list identity.
        srf::DurableState state = valid;
        state.lists.push_back(state.lists.front());
        srf::ValidationResult encode_result;
        const srf::ByteBuffer bytes = srf::encode_durable_state(state, limits, encode_result);
        SRF_EXPECT(encode_result.ok());
        srf::DurableState decoded{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(srf::as_bytes(bytes), limits, decoded, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistDuplicateIdentity);
    }
    {
        // Impossible generation.
        srf::DurableState state = valid;
        state.lists.front().generation = srf::SegmentListGeneration{};
        srf::ValidationResult encode_result;
        const srf::ByteBuffer bytes = srf::encode_durable_state(state, limits, encode_result);
        srf::DurableState decoded{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(srf::as_bytes(bytes), limits, decoded, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistImpossibleGeneration);
    }
    {
        // Dangling profile reference.
        srf::DurableState state = valid;
        state.lists.front().content.profile = srf::SegmentProfileId{0xDEAD};
        srf::ValidationResult encode_result;
        const srf::ByteBuffer bytes = srf::encode_durable_state(state, limits, encode_result);
        srf::DurableState decoded{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(srf::as_bytes(bytes), limits, decoded, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistDanglingReference);
    }
    {
        // Invalid segment kind.
        srf::DurableState state = valid;
        state.lists.front().content.segments.front().kind = srf::SegmentKind::Invalid;
        srf::ValidationResult encode_result;
        const srf::ByteBuffer bytes = srf::encode_durable_state(state, limits, encode_result);
        srf::DurableState decoded{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(srf::as_bytes(bytes), limits, decoded, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistKindInvalid);
    }
    {
        // Invalid encoding identity.
        srf::DurableState state = valid;
        state.lists.front().content.segments.front().encoding = srf::SegmentEncodingId::Invalid;
        srf::ValidationResult encode_result;
        const srf::ByteBuffer bytes = srf::encode_durable_state(state, limits, encode_result);
        srf::DurableState decoded{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(srf::as_bytes(bytes), limits, decoded, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistEncodingInvalid);
    }
    {
        // Depth beyond the configured bound.
        srf::DurableState state = valid;
        srf::Segment extra = srf::test::node_segment(2);
        state.lists.front().content.segments.push_back(extra);
        state.lists.front().content.segments.push_back(extra);
        srf::Limits tight = limits;
        tight.max_segments_per_list = 2;
        srf::ValidationResult encode_result;
        const srf::ByteBuffer bytes = srf::encode_durable_state(state, tight, encode_result);
        srf::DurableState decoded{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(srf::as_bytes(bytes), tight, decoded, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistDepthInvalid);
    }
    {
        // Corrupted digest with repaired integrity trailers.
        srf::DurableState state = valid;
        state.lists.front().content_digest = srf::Digest128{1, 2};
        srf::ValidationResult encode_result;
        const srf::ByteBuffer bytes = srf::encode_durable_state(state, limits, encode_result);
        srf::DurableState decoded{};
        srf::ValidationResult result;
        SRF_EXPECT(!srf::decode_durable_state(srf::as_bytes(bytes), limits, decoded, result));
        SRF_EXPECT_PRIMARY(result, srf::ReasonCode::PersistDigestMismatch);
    }
    {
        // A list record larger than the configured record bound.
        srf::Limits tight = limits;
        tight.max_persistence_record_bytes = 32;
        srf::ValidationResult encode_result;
        const srf::ByteBuffer bytes = srf::encode_durable_state(valid, tight, encode_result);
        SRF_EXPECT(bytes.empty());
        SRF_EXPECT_REASON(encode_result, srf::ReasonCode::LimitMaxPersistenceRecordBytes);
    }
}

SRF_TEST(persistence, every_single_byte_corruption_is_detected) {
    Harness harness;
    SRF_EXPECT(harness.create(1, {node_segment(1), node_segment(2)}).ok());
    SRF_EXPECT(harness.create(2, {srf::test::adjacency_segment(1)}).ok());
    srf::MemoryPersistence pristine{};
    srf::ValidationResult save_result;
    SRF_EXPECT(pristine.save(harness.store().export_state(), harness.store().limits(),
                             save_result));
    const std::vector<std::byte> original(pristine.bytes().begin(), pristine.bytes().end());
    SRF_EXPECT(original.size() > 200);

    const srf::Limits limits{};
    std::size_t undetected = 0;
    for (std::size_t offset = 0; offset < original.size(); ++offset) {
        srf::MemoryPersistence corrupted{};
        corrupted.set_bytes(std::span<const std::byte>(original.data(), original.size()));
        SRF_EXPECT(corrupted.corrupt_byte(offset, 0x01));
        srf::LoadReport report{};
        const bool loaded = corrupted.load(report, limits);
        if (loaded) {
            ++undetected;
            std::printf("      undetected corruption at offset %zu\n", offset);
        }
    }
    SRF_EXPECT_EQ(undetected, static_cast<std::size_t>(0));
}
