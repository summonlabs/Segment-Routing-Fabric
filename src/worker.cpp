// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/worker.hpp"

#include <utility>

namespace srf {

WorkerClient::~WorkerClient() {
    close();
}

bool WorkerClient::open(const WorkerConfig& config, ValidationResult& out) {
    config_ = config;
    out.set_limits(&config.limits);
    std::uint16_t port = 0;
    if (!read_port_file(config.port_path, port)) {
        out.add(ReasonCode::PersistIoError, 0, 11);
        return false;
    }
    socket_ = Socket::connect_loopback(port, out);
    return socket_.valid();
}

void WorkerClient::close() {
    socket_.close();
}

MutationAttemptId WorkerClient::next_attempt() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++attempt_counter_;
    const std::uint64_t mixed =
        (config_.boot.value() * 0x9E3779B97F4A7C15ull) ^ (attempt_counter_ * 0x100000001B3ull);
    const std::uint64_t value = mixed == 0 ? attempt_counter_ + 1 : mixed;
    return MutationAttemptId{value};
}

Response WorkerClient::exchange(MessageId message, const ByteBuffer& payload) {
    Response response{};
    if (!socket_.valid()) {
        transport_status_ = StatusCode::Internal;
        response.status = StatusCode::Internal;
        response.reasons.push_back(Reason{ReasonCode::InternalError, 0, 21});
        return response;
    }
    Frame frame{};
    frame.message = message;
    frame.request_id = next_request_++;
    frame.payload = payload;
    ValidationResult result;
    const ByteBuffer bytes = encode_frame(frame, config_.limits, result);
    if (result.failed() || !socket_.send_all(as_bytes(bytes))) {
        transport_status_ = StatusCode::Internal;
        response.status = StatusCode::Internal;
        response.reasons.assign(result.reasons().begin(), result.reasons().end());
        if (response.reasons.empty()) {
            response.reasons.push_back(Reason{ReasonCode::InternalError, 0, 22});
        }
        return response;
    }
    FrameAssembler assembler{};
    std::vector<std::byte> buffer(16384);
    for (;;) {
        Frame incoming{};
        ValidationResult decode_result;
        if (assembler.next(config_.limits, incoming, decode_result)) {
            if (incoming.message != MessageId::Response) {
                response.status = StatusCode::Rejected;
                response.reasons.push_back(Reason{ReasonCode::WireUnknownMessageId, 0,
                                                  static_cast<std::uint64_t>(incoming.message)});
                return response;
            }
            ValidationResult payload_result;
            if (!decode_response(as_bytes(incoming.payload), config_.limits, response,
                                 payload_result)) {
                response.status = StatusCode::Rejected;
                response.reasons.assign(payload_result.reasons().begin(),
                                        payload_result.reasons().end());
                return response;
            }
            transport_status_ = StatusCode::Ok;
            return response;
        }
        if (decode_result.failed()) {
            transport_status_ = StatusCode::Rejected;
            response.status = StatusCode::Rejected;
            response.reasons.assign(decode_result.reasons().begin(), decode_result.reasons().end());
            return response;
        }
        std::size_t received = 0;
        if (!socket_.recv_some(std::span<std::byte>(buffer.data(), buffer.size()), received)) {
            transport_status_ = StatusCode::Internal;
            response.status = StatusCode::Internal;
            response.reasons.push_back(Reason{ReasonCode::InternalError, 0, 23});
            socket_.close();
            return response;
        }
        assembler.feed(std::span<const std::byte>(buffer.data(), received));
    }
}

Response WorkerClient::handshake() {
    HelloRequest hello{};
    hello.boot = config_.boot;
    hello.publisher = config_.publisher;
    hello.scope = config_.scope;
    hello.epoch = config_.expected_epoch;
    Response response = exchange(MessageId::Hello, encode_hello(hello));
    if (response.status == StatusCode::Ok) {
        session_epoch_ = response.epoch;
        session_ = response.session;
    }
    return response;
}

Response WorkerClient::heartbeat() {
    HeartbeatRequest beat{};
    beat.publisher = config_.publisher;
    beat.boot = config_.boot;
    beat.epoch = session_epoch_;
    return exchange(MessageId::Heartbeat, encode_heartbeat(beat));
}

Response WorkerClient::send_mutation(MessageId message, const ListDraft& draft) {
    MutationRequest mutation{};
    mutation.attempt = next_attempt();
    mutation.draft = draft;
    ValidationResult result;
    const ByteBuffer payload = encode_mutation(mutation, config_.limits, result);
    if (result.failed()) {
        Response response{};
        response.status = StatusCode::Rejected;
        response.reasons.assign(result.reasons().begin(), result.reasons().end());
        return response;
    }
    return exchange(message, payload);
}

namespace {

[[nodiscard]] MessageId message_for(LifecycleOp op) noexcept {
    switch (op) {
        case LifecycleOp::Withdraw: return MessageId::Withdraw;
        case LifecycleOp::WithdrawCommit: return MessageId::WithdrawCommit;
        case LifecycleOp::Revoke: return MessageId::Revoke;
        case LifecycleOp::Retire: return MessageId::Retire;
        case LifecycleOp::Revalidate: return MessageId::Revalidate;
    }
    return MessageId::Invalid;
}

} // namespace

Response WorkerClient::send_lifecycle(LifecycleOp op, SegmentListId list,
                                      SegmentListGeneration expected) {
    LifecycleRequest request{};
    request.attempt = next_attempt();
    request.op = op;
    request.list = list;
    request.expected = expected;
    return exchange(message_for(op), encode_lifecycle(request));
}

Response WorkerClient::get(SegmentListId list) {
    GetRequest request{};
    request.list = list;
    return exchange(MessageId::Get, encode_get(request));
}

Response WorkerClient::summaries() {
    return exchange(MessageId::ListSummaries, ByteBuffer{});
}

Response WorkerClient::explain_currentness(SegmentListId list) {
    GetRequest request{};
    request.list = list;
    return exchange(MessageId::ExplainCurrentness, encode_get(request));
}

Response WorkerClient::refresh_currentness() {
    return exchange(MessageId::RefreshCurrentness, ByteBuffer{});
}

Response WorkerClient::goodbye() {
    return exchange(MessageId::Goodbye, ByteBuffer{});
}

} // namespace srf
