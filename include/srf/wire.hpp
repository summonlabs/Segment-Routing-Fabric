// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include "srf/bytes.hpp"
#include "srf/limits.hpp"
#include "srf/list.hpp"
#include "srf/reason.hpp"
#include "srf/store.hpp"
#include "srf/version.hpp"

namespace srf {

/// Explicit wire message identifiers. Stable, bounded and strictly validated:
/// an unknown identifier is a decode rejection, never a silent skip.
enum class MessageId : std::uint16_t {
    Invalid = 0,
    Hello = 1,
    HelloAck = 2,
    Heartbeat = 3,
    Goodbye = 4,
    CreateList = 10,
    ReplaceList = 11,
    Withdraw = 12,
    WithdrawCommit = 13,
    Revoke = 14,
    Retire = 15,
    Revalidate = 16,
    Get = 20,
    ListSummaries = 21,
    ExplainCurrentness = 22,
    RefreshCurrentness = 23,
    Response = 40,
};

[[nodiscard]] std::string_view message_id_name(MessageId id) noexcept;

/// Wire frame. The header is fixed-width and fully explicit; nothing is ever a
/// raw struct dump, and every frame carries an integrity trailer over the
/// semantic header and payload.
struct Frame {
    std::uint16_t version{kWireProtocolVersion};
    MessageId message{MessageId::Invalid};
    std::uint16_t flags{0};
    std::uint64_t request_id{0};
    ByteBuffer payload{};
};

inline constexpr std::size_t kFrameHeaderBytes = 28;
inline constexpr std::size_t kFrameTrailerBytes = 4;

[[nodiscard]] ByteBuffer encode_frame(const Frame& frame, const Limits& limits,
                                      ValidationResult& result);
[[nodiscard]] bool decode_frame(std::span<const std::byte> bytes, const Limits& limits,
                                Frame& frame, ValidationResult& result);

/// Bounded partial-frame assembly. A peer that announces an oversized frame, or
/// that trickles a frame forever, is rejected rather than buffered without bound.
class FrameAssembler {
public:
    FrameAssembler() = default;

    void feed(std::span<const std::byte> bytes);
    [[nodiscard]] bool next(const Limits& limits, Frame& frame, ValidationResult& result);
    [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size(); }
    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
    void clear();

private:
    std::vector<std::byte> buffer_{};
    bool overflowed_{false};
};

// ------------------------------------------------------------------- payloads
struct HelloRequest {
    WorkerBootId boot{};
    PublisherId publisher{};
    ScopeId scope{};
    CoordinatorEpoch epoch{};
};

struct HelloAck {
    CoordinatorEpoch epoch{};
    WorkerSessionId session{};
};

struct HeartbeatRequest {
    PublisherId publisher{};
    WorkerBootId boot{};
    CoordinatorEpoch epoch{};
};

struct MutationRequest {
    MutationAttemptId attempt{};
    ListDraft draft{};
};

enum class LifecycleOp : std::uint8_t {
    Withdraw = 1,
    WithdrawCommit = 2,
    Revoke = 3,
    Retire = 4,
    Revalidate = 5,
};

struct LifecycleRequest {
    MutationAttemptId attempt{};
    LifecycleOp op{LifecycleOp::Withdraw};
    SegmentListId list{};
    SegmentListGeneration expected{};
};

struct GetRequest {
    SegmentListId list{};
};

struct ListSummary {
    SegmentListId list{};
    SegmentListGeneration generation{};
    LifecycleState state{LifecycleState::Invalid};
    Currentness currentness{Currentness::Unknown};
    SegmentProfileId profile{};
    SegmentProfileGeneration profile_generation{};
    Digest128 digest{};
    std::uint32_t segment_count{0};
    CoordinatorEpoch epoch{};
};

struct Response {
    std::uint64_t request_id{0};
    StatusCode status{StatusCode::Ok};
    std::vector<Reason> reasons{};
    bool truncated{false};
    SegmentListId list{};
    SegmentListGeneration generation{};
    Digest128 digest{};
    bool idempotent_replay{false};
    CoordinatorEpoch epoch{};
    /// Assigned by the coordinator on Hello and echoed thereafter.
    WorkerSessionId session{};
    std::vector<ListSummary> summaries{};
};

[[nodiscard]] ByteBuffer encode_hello(const HelloRequest& hello);
[[nodiscard]] bool decode_hello(std::span<const std::byte> bytes, const Limits& limits,
                                HelloRequest& hello, ValidationResult& result);
[[nodiscard]] ByteBuffer encode_hello_ack(const HelloAck& ack);
[[nodiscard]] bool decode_hello_ack(std::span<const std::byte> bytes, const Limits& limits,
                                    HelloAck& ack, ValidationResult& result);
[[nodiscard]] ByteBuffer encode_heartbeat(const HeartbeatRequest& beat);
[[nodiscard]] bool decode_heartbeat(std::span<const std::byte> bytes, const Limits& limits,
                                    HeartbeatRequest& beat, ValidationResult& result);
[[nodiscard]] ByteBuffer encode_mutation(const MutationRequest& mutation, const Limits& limits,
                                         ValidationResult& result);
[[nodiscard]] bool decode_mutation(std::span<const std::byte> bytes, const Limits& limits,
                                   MutationRequest& mutation, ValidationResult& result);
[[nodiscard]] ByteBuffer encode_lifecycle(const LifecycleRequest& request);
[[nodiscard]] bool decode_lifecycle(std::span<const std::byte> bytes, const Limits& limits,
                                    LifecycleRequest& request, ValidationResult& result);
[[nodiscard]] ByteBuffer encode_get(const GetRequest& request);
[[nodiscard]] bool decode_get(std::span<const std::byte> bytes, const Limits& limits,
                              GetRequest& request, ValidationResult& result);
[[nodiscard]] ByteBuffer encode_response(const Response& response, const Limits& limits,
                                         ValidationResult& result);
[[nodiscard]] bool decode_response(std::span<const std::byte> bytes, const Limits& limits,
                                   Response& response, ValidationResult& result);

[[nodiscard]] ListSummary summarize(const SegmentList& list, CoordinatorEpoch epoch);

} // namespace srf
