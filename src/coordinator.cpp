// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#include "srf/coordinator.hpp"

#include <algorithm>
#include <utility>

namespace srf {

CoordinatorServer::CoordinatorServer(CoordinatorConfig config) : config_(std::move(config)) {}

CoordinatorServer::~CoordinatorServer() {
    stop();
}

bool CoordinatorServer::start(ValidationResult& out) {
    out.set_limits(&config_.limits);
    StoreConfig store_config{};
    store_config.limits = config_.limits;
    // Every piece of fabric evidence this coordinator serves is SYNTHETIC.
    store_config.evidence = make_synthetic_fabric();
    if (!config_.state_path.empty()) {
        store_config.persistence = std::make_shared<FilePersistence>(config_.state_path);
    }
    store_ = std::make_unique<SegmentListStore>(std::move(store_config));

    // Recovery happens BEFORE any local declaration, because declaring a profile is
    // itself a durable mutation: doing it first would overwrite the recovered state
    // with an empty one.
    if (config_.load_persisted && !config_.state_path.empty()) {
        load_report_ = store_->load();
        if (!load_report_.loaded && !load_report_.empty) {
            out.absorb(load_report_.result);
            return false;
        }
    }

    // The coordinator declares the abstract profiles it can actually implement. No
    // physical profile exists in 1.0.0. A profile that was restored from durable
    // state keeps its persisted generation.
    const SegmentProfile builtins[3] = {abstract_governance_profile(),
                                        abstract_ordered_profile(),
                                        abstract_nullable_profile()};
    for (const SegmentProfile& profile : builtins) {
        SegmentProfile existing{};
        if (store_->registry().find_profile(profile.id, existing)) {
            continue;
        }
        const ValidationResult registered = store_->register_profile(profile);
        if (registered.failed()) {
            out.absorb(registered);
            return false;
        }
    }

    if (config_.advance_epoch) {
        if (!store_->authority().advance_epoch()) {
            out.add(ReasonCode::EpochExhausted);
            return false;
        }
    }
    // The new epoch must be durable before the coordinator serves anything, so that
    // a crash cannot let a later incarnation reuse it.
    const ValidationResult flushed = store_->persist_now();
    if (flushed.failed()) {
        out.absorb(flushed);
        return false;
    }

    listener_ = Socket::listen_loopback(config_.port, out);
    if (!listener_.valid()) {
        return false;
    }
    port_ = listener_.local_port();
    if (!config_.port_path.empty()) {
        if (!write_port_file(config_.port_path, port_)) {
            out.add(ReasonCode::PersistIoError, 0, port_);
            return false;
        }
    }
    stopping_.store(false);
    monitor_ = std::thread([this] { monitor_loop(); });
    return true;
}

std::size_t CoordinatorServer::registered_publishers() const {
    return store_->authority().publisher_count();
}

void CoordinatorServer::stop() {
    const bool was_stopping = stopping_.exchange(true);
    listener_.close();
    if (monitor_.joinable()) {
        monitor_.join();
    }
    std::vector<std::thread> pending;
    std::vector<std::shared_ptr<Socket>> peers;
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        pending.swap(workers_);
        peers = active_peers_;
    }
    // Unblock any connection thread that is waiting on a live peer socket.
    for (const std::shared_ptr<Socket>& peer : peers) {
        if (peer) {
            peer->close();
        }
    }
    for (std::thread& worker : pending) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    (void)was_stopping;
    store_.reset();
}

void CoordinatorServer::serve() {
    while (!stopping_.load()) {
        ValidationResult ignored;
        Socket peer = listener_.accept(ignored);
        if (!peer.valid()) {
            if (stopping_.load()) {
                break;
            }
            continue;
        }
        const std::uint32_t served = connections_served_.fetch_add(1) + 1;
        auto shared = std::make_shared<Socket>(std::move(peer));
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            active_peers_.push_back(shared);
            workers_.emplace_back([this, shared]() { handle_connection(shared); });
        }
        if (config_.max_connections != 0 && served >= config_.max_connections) {
            break;
        }
    }
}

void CoordinatorServer::touch(PublisherId publisher) {
    std::lock_guard<std::mutex> lock(lease_mutex_);
    last_seen_[publisher.value()] = std::chrono::steady_clock::now();
}

std::size_t CoordinatorServer::expire_leases() {
    std::vector<PublisherId> expired;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(lease_mutex_);
        for (auto it = last_seen_.begin(); it != last_seen_.end();) {
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - it->second)
                                 .count();
            if (age > static_cast<long long>(config_.lease_milliseconds)) {
                expired.push_back(PublisherId{it->first});
                it = last_seen_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const PublisherId publisher : expired) {
        const ValidationResult fenced = store_->authority().mark_dead(publisher);
        (void)fenced;
    }
    if (!expired.empty()) {
        // Fencing is only observable once currentness has been re-evaluated.
        (void)store_->refresh_currentness();
    }
    return expired.size();
}

void CoordinatorServer::monitor_loop() {
    while (!stopping_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (stopping_.load()) {
            break;
        }
        (void)expire_leases();
    }
}

void CoordinatorServer::handle_connection(const std::shared_ptr<Socket>& peer) {
    Session session{};
    FrameAssembler assembler{};
    std::vector<std::byte> buffer(16384);
    bool open = true;
    while (open && !stopping_.load()) {
        std::size_t received = 0;
        if (!peer->recv_some(std::span<std::byte>(buffer.data(), buffer.size()), received)) {
            break;  // orderly close or transport error: the publisher is gone
        }
        assembler.feed(std::span<const std::byte>(buffer.data(), received));
        for (;;) {
            Frame frame{};
            ValidationResult result;
            if (!assembler.next(config_.limits, frame, result)) {
                break;
            }
            Response response = dispatch(frame, session);
            ValidationResult encode_result;
            const ByteBuffer encoded = encode_response(response, config_.limits, encode_result);
            if (encode_result.failed()) {
                open = false;
                break;
            }
            Frame out{};
            out.message = MessageId::Response;
            out.request_id = frame.request_id;
            out.payload = encoded;
            ValidationResult frame_result;
            const ByteBuffer bytes = encode_frame(out, config_.limits, frame_result);
            if (frame_result.failed() || !peer->send_all(as_bytes(bytes))) {
                open = false;
                break;
            }
            if (frame.message == MessageId::Goodbye) {
                open = false;
                break;
            }
        }
        if (assembler.overflowed()) {
            break;
        }
    }
    // Real loss detection: an ended connection permanently fences that boot.
    if (session.registered) {
        // Real loss detection: the writer's connection ended, so its boot is
        // permanently fenced and every list it published loses currentness.
        const ValidationResult fenced = store_->authority().mark_dead(session.publisher);
        (void)fenced;
        {
            std::lock_guard<std::mutex> lock(lease_mutex_);
            last_seen_.erase(session.publisher.value());
        }
        (void)store_->refresh_currentness();
    }
    peer->close();
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        active_peers_.erase(std::remove(active_peers_.begin(), active_peers_.end(), peer),
                            active_peers_.end());
    }
}

Response CoordinatorServer::dispatch(const Frame& frame, Session& session) {
    Response response{};
    response.request_id = frame.request_id;
    response.epoch = store_->authority().epoch();
    ValidationResult result;

    switch (frame.message) {
        case MessageId::Hello: {
            HelloRequest hello{};
            if (!decode_hello(as_bytes(frame.payload), config_.limits, hello, result)) {
                response.status = status_for(result);
                response.reasons.assign(result.reasons().begin(), result.reasons().end());
                response.truncated = result.truncated();
                return response;
            }
            // A worker that has never spoken to this coordinator cannot know the
            // current epoch. An unset epoch is resolved here, once; every later
            // mutation must carry the resolved epoch.
            const CoordinatorEpoch requested_epoch =
                hello.epoch.valid() ? hello.epoch : store_->authority().epoch();
            ValidationResult registered = store_->authority().register_publisher(
                hello.publisher, hello.boot, hello.scope, requested_epoch, config_.limits);
            if (registered.failed()) {
                response.status = status_for(registered);
                response.reasons.assign(registered.reasons().begin(), registered.reasons().end());
                response.truncated = registered.truncated();
                return response;
            }
            session.publisher = hello.publisher;
            session.boot = hello.boot;
            session.scope = hello.scope;
            session.epoch = store_->authority().epoch();
            session.registered = true;
            touch(hello.publisher);
            response.status = StatusCode::Ok;
            response.epoch = store_->authority().epoch();
            response.session = WorkerSessionId{hello.publisher.value()};
            return response;
        }
        case MessageId::Heartbeat: {
            HeartbeatRequest beat{};
            if (!decode_heartbeat(as_bytes(frame.payload), config_.limits, beat, result)) {
                response.status = status_for(result);
                response.reasons.assign(result.reasons().begin(), result.reasons().end());
                return response;
            }
            ValidationResult beat_result = store_->authority().heartbeat(
                beat.publisher, beat.boot, beat.epoch);
            if (beat_result.ok()) {
                touch(beat.publisher);
            }
            response.status = status_for(beat_result);
            response.reasons.assign(beat_result.reasons().begin(), beat_result.reasons().end());
            response.truncated = beat_result.truncated();
            return response;
        }
        case MessageId::CreateList:
        case MessageId::ReplaceList: {
            MutationRequest mutation{};
            if (!decode_mutation(as_bytes(frame.payload), config_.limits, mutation, result)) {
                response.status = status_for(result);
                response.reasons.assign(result.reasons().begin(), result.reasons().end());
                response.truncated = result.truncated();
                return response;
            }
            CallerIdentity caller{};
            caller.epoch = session.epoch;
            caller.publisher = session.publisher;
            caller.boot = session.boot;
            caller.scope = session.scope;
            caller.attempt = mutation.attempt;
            if (!session.registered) {
                response.status = StatusCode::Rejected;
                response.reasons.push_back(Reason{ReasonCode::CallerUnknownPublisher, 0,
                                                  session.publisher.value()});
                return response;
            }
            MutationOutcome outcome = store_->create_list(caller, mutation.draft);
            response.status = outcome.status;
            response.reasons.assign(outcome.result.reasons().begin(), outcome.result.reasons().end());
            response.truncated = outcome.result.truncated();
            response.list = outcome.list;
            response.generation = outcome.generation;
            response.digest = outcome.digest;
            response.idempotent_replay = outcome.idempotent_replay;
            response.epoch = store_->authority().epoch();
            return response;
        }
        case MessageId::Withdraw:
        case MessageId::WithdrawCommit:
        case MessageId::Revoke:
        case MessageId::Retire:
        case MessageId::Revalidate: {
            LifecycleRequest request{};
            if (!decode_lifecycle(as_bytes(frame.payload), config_.limits, request, result)) {
                response.status = status_for(result);
                response.reasons.assign(result.reasons().begin(), result.reasons().end());
                return response;
            }
            CallerIdentity caller{};
            caller.epoch = session.epoch;
            caller.publisher = session.publisher;
            caller.boot = session.boot;
            caller.scope = session.scope;
            caller.attempt = request.attempt;
            MutationOutcome outcome{};
            switch (request.op) {
                case LifecycleOp::Withdraw:
                    outcome = store_->withdraw(caller, request.list, request.expected);
                    break;
                case LifecycleOp::WithdrawCommit:
                    outcome = store_->withdraw_commit(caller, request.list, request.expected);
                    break;
                case LifecycleOp::Revoke:
                    outcome = store_->revoke(caller, request.list, request.expected);
                    break;
                case LifecycleOp::Retire:
                    outcome = store_->retire(caller, request.list, request.expected);
                    break;
                case LifecycleOp::Revalidate:
                    outcome = store_->revalidate(caller, request.list, request.expected);
                    break;
            }
            response.status = outcome.status;
            response.reasons.assign(outcome.result.reasons().begin(), outcome.result.reasons().end());
            response.truncated = outcome.result.truncated();
            response.list = outcome.list;
            response.generation = outcome.generation;
            response.digest = outcome.digest;
            response.idempotent_replay = outcome.idempotent_replay;
            response.epoch = store_->authority().epoch();
            return response;
        }
        case MessageId::Get:
        case MessageId::ExplainCurrentness: {
            GetRequest request{};
            if (!decode_get(as_bytes(frame.payload), config_.limits, request, result)) {
                response.status = status_for(result);
                response.reasons.assign(result.reasons().begin(), result.reasons().end());
                return response;
            }
            if (frame.message == MessageId::ExplainCurrentness) {
                const Explanation explanation = store_->explain_currentness(request.list);
                response.list = explanation.list;
                response.generation = explanation.generation;
                for (const ExplanationEntry& entry : explanation.entries) {
                    response.reasons.push_back(Reason{entry.code, entry.index, entry.detail});
                }
                response.status = StatusCode::Ok;
                return response;
            }
            const std::optional<SegmentList> list = store_->get(request.list);
            if (!list.has_value()) {
                response.status = StatusCode::NotFound;
                response.reasons.push_back(Reason{ReasonCode::ListNotFound, 0, request.list.value()});
                return response;
            }
            response.status = StatusCode::Ok;
            response.list = list->content.id;
            response.generation = list->generation;
            response.digest = list->content_digest;
            response.summaries.push_back(summarize(*list, store_->authority().epoch()));
            return response;
        }
        case MessageId::ListSummaries: {
            for (const SegmentList& list : store_->lists()) {
                response.summaries.push_back(summarize(list, store_->authority().epoch()));
            }
            response.status = StatusCode::Ok;
            return response;
        }
        case MessageId::RefreshCurrentness: {
            const CurrentnessReport report = store_->refresh_currentness();
            response.status = StatusCode::Ok;
            response.generation = SegmentListGeneration{report.invalidated};
            response.digest = Digest128{report.examined, 0};
            return response;
        }
        case MessageId::Goodbye: {
            if (session.registered) {
                const ValidationResult fenced = store_->authority().mark_dead(session.publisher);
                (void)fenced;
                session.registered = false;
            }
            response.status = StatusCode::Ok;
            return response;
        }
        case MessageId::HelloAck:
        case MessageId::Response:
        case MessageId::Invalid:
        default: {
            response.status = StatusCode::Rejected;
            response.reasons.push_back(
                Reason{ReasonCode::WireUnknownMessageId, 0,
                       static_cast<std::uint64_t>(frame.message)});
            return response;
        }
    }
}

} // namespace srf
