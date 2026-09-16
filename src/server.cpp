// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/server.hpp"

#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "fabric_epoch/error.hpp"
#include "fabric_epoch/protocol.hpp"
#include "fabric_epoch/version.hpp"
#include "socket.hpp"

namespace fabric_epoch {
namespace {

[[nodiscard]] SessionClosureReason classify_close(bool protocol_violation, bool server_stop) {
  if (protocol_violation) {
    return SessionClosureReason::ProtocolViolation;
  }
  if (server_stop) {
    return SessionClosureReason::ServerShutdown;
  }
  return SessionClosureReason::PeerDisconnect;
}

}  // namespace

struct CoordinatorServer::Impl {
  struct SessionThread {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished;
  };

  EpochRuntime* runtime = nullptr;
  CoordinatorServerOptions options;
  net::Listener listener;

  std::atomic<bool> stop_now{false};
  std::atomic<bool> stopped{false};
  std::atomic<bool> shutdown_flag{false};
  std::atomic<std::size_t> active_sessions{0};
  std::atomic<std::uint64_t> sequence{1};

  std::thread accept_thread;
  mutable std::mutex mutex;
  std::condition_variable shutdown_cv;
  std::vector<SessionThread> session_threads;

  Impl(EpochRuntime& epoch_runtime, CoordinatorServerOptions server_options)
      : runtime(&epoch_runtime), options(std::move(server_options)) {
    if (options.product_version.empty()) {
      options.product_version = std::string(version_string);
    }
  }

  ~Impl() { stop(); }

  [[nodiscard]] std::uint64_t next_sequence() noexcept { return sequence.fetch_add(1); }

  void start() {
    listener.bind_and_listen(options.bind_address, options.port, options.limits.listen_backlog);
    accept_thread = std::thread([this]() { accept_loop(); });
  }

  void accept_loop() {
    while (!stop_now.load()) {
      net::Socket socket = listener.accept_one(options.io_wait_milliseconds);
      if (!socket.valid()) {
        reap_finished_sessions();
        continue;
      }
      if (stop_now.load()) {
        socket.close();
        break;
      }
      auto finished = std::make_shared<std::atomic<bool>>(false);
      std::lock_guard<std::mutex> lock(mutex);
      session_threads.push_back(SessionThread{
          std::thread([this, handle = socket.release(), finished]() mutable {
            net::Socket owned(handle);
            try {
              serve(owned);
            } catch (...) {
              owned.close();
            }
            finished->store(true);
          }),
          finished});
      reap_finished_sessions_locked();
    }
  }

  void reap_finished_sessions_locked() {
    for (auto it = session_threads.begin(); it != session_threads.end();) {
      if (it->finished->load()) {
        if (it->thread.joinable()) {
          it->thread.join();
        }
        it = session_threads.erase(it);
      } else {
        ++it;
      }
    }
  }

  void reap_finished_sessions() {
    std::lock_guard<std::mutex> lock(mutex);
    reap_finished_sessions_locked();
  }

  void stop() {
    if (stopped.exchange(true)) {
      return;
    }
    stop_now.store(true);
    if (accept_thread.joinable()) {
      accept_thread.join();
    }
    listener.close();
    std::vector<SessionThread> threads;
    {
      std::lock_guard<std::mutex> lock(mutex);
      threads.swap(session_threads);
    }
    for (auto& entry : threads) {
      if (entry.thread.joinable()) {
        entry.thread.join();
      }
    }
  }

  void request_shutdown() {
    {
      // The flag is updated under the same mutex the waiter uses so that a
      // request arriving between the predicate check and the wait cannot be
      // lost.
      std::lock_guard<std::mutex> lock(mutex);
      shutdown_flag.store(true);
    }
    shutdown_cv.notify_all();
  }

  // ---- frame IO ----------------------------------------------------------

  [[nodiscard]] bool read_frame(net::Socket& socket, Frame& frame) {
    std::array<std::byte, frame_header_size> header{};
    if (!socket.recv_exact(header)) {
      return false;
    }
    const std::uint32_t length = peek_payload_length(header);
    if (length > options.limits.max_frame_payload) {
      throw resource_limit("declared frame payload exceeds limits.max_frame_payload");
    }
    std::vector<std::byte> payload(length);
    if (length != 0 && !socket.recv_exact(payload)) {
      throw malformed("peer closed the connection in the middle of a frame payload");
    }
    frame = decode_frame(header, payload, options.limits);
    return true;
  }

  void write_frame(net::Socket& socket, MessageType type, std::uint64_t epoch,
                   const MutationAttemptId& request_id, std::span<const std::byte> payload) {
    const std::vector<std::byte> bytes =
        encode_frame(type, next_sequence(), CoordinatorEpoch(epoch), request_id, payload, options.limits);
    socket.send_all(std::span<const std::byte>(bytes));
  }

  void write_error(net::Socket& socket, std::uint64_t epoch, const MutationAttemptId& request_id,
                   std::uint16_t code, std::string_view message) noexcept {
    try {
      const std::vector<std::byte> payload = encode_error(code, message, options.limits);
      write_frame(socket, MessageType::Error, epoch, request_id, std::span<const std::byte>(payload));
    } catch (const EpochError&) {
      // The peer is already gone; the session is torn down by the caller.
    }
  }

  // ---- session -----------------------------------------------------------

  void serve(net::Socket& socket) {
    socket.set_receive_timeout(options.frame_receive_budget_milliseconds);
    active_sessions.fetch_add(1);
    const SessionId session = SessionId::generate();
    bool protocol_violation = false;
    bool bound = false;
    SessionBinding binding;
    bool hello_seen = false;

    try {
      while (!stop_now.load()) {
        if (!socket.wait_readable(options.io_wait_milliseconds)) {
          continue;
        }
        Frame frame;
        if (!read_frame(socket, frame)) {
          break;
        }
        const MutationAttemptId request_id = MutationAttemptId::from_bytes(frame.header.request_id);
        const CoordinatorEpoch epoch = runtime->current_epoch();

        if (!hello_seen && frame.header.type != MessageType::Hello) {
          protocol_violation = true;
          write_error(socket, epoch.value(), request_id, 1, "first message must be HELLO");
          break;
        }

        if (frame.header.type == MessageType::Hello) {
          const HelloPayload hello = decode_hello(frame.payload, options.limits);
          if (hello.role.empty()) {
            throw malformed("HELLO carries an empty role");
          }
          hello_seen = true;
          const std::vector<std::byte> payload =
              encode_hello_ack(runtime->current_epoch(), runtime->coordinator_boot(), session,
                               options.product_version);
          write_frame(socket, MessageType::HelloAck, runtime->current_epoch().value(), request_id,
                      std::span<const std::byte>(payload));
          continue;
        }

        if (frame.header.type == MessageType::Shutdown) {
          request_shutdown();
          stop_now.store(true);
          break;
        }

        if (frame.header.type == MessageType::RegisterParticipant) {
          if (bound) {
            protocol_violation = true;
            write_error(socket, epoch.value(), request_id, 4,
                        "session is already bound to a worker incarnation");
            break;
          }
          const RegisterWireRequest wire = decode_register_request(frame.payload, options.limits);
          const RegistrationResult result = runtime->register_participant(wire.request);
          if (result.outcome == RegistrationOutcome::Registered ||
              result.outcome == RegistrationOutcome::Idempotent) {
            SessionBinding candidate;
            candidate.id = session;
            candidate.participant = wire.request.participant;
            candidate.boot = wire.request.boot;
            candidate.incarnation = result.incarnation.value_or(IncarnationSequence(0));
            candidate.epoch = result.epoch;
            candidate.registration_generation =
                result.registration_generation.value_or(RegistrationGeneration(0));
            if (candidate.incarnation.is_zero()) {
              protocol_violation = true;
              write_error(socket, epoch.value(), request_id, 5,
                          "registration did not produce an incarnation sequence");
              break;
            }
            if (!runtime->bind_session(candidate)) {
              protocol_violation = true;
              write_error(socket, epoch.value(), request_id, 6,
                          "session could not be bound to the new incarnation");
              break;
            }
            binding = candidate;
            bound = true;
          }
          const std::vector<std::byte> payload = encode_register_result(result, options.limits);
          write_frame(socket, MessageType::RegisterAck, result.epoch.value(), request_id,
                      std::span<const std::byte>(payload));
          continue;
        }

        switch (frame.header.type) {
          case MessageType::ValidateAuthority: {
            const ValidateAuthorityRequest request =
                decode_validate_request(frame.payload, options.limits);
            const AuthorityValidation validation = runtime->validate_authority(request);
            const std::vector<std::byte> payload = encode_validation(validation, options.limits);
            write_frame(socket, MessageType::ValidateResult, epoch.value(), request_id,
                        std::span<const std::byte>(payload));
            break;
          }
          case MessageType::FenceWorker: {
            const FenceWorkerRequest request = decode_fence_request(frame.payload, options.limits);
            const FenceResult result = runtime->fence_worker(request);
            const std::vector<std::byte> payload = encode_fence_result(result, options.limits);
            write_frame(socket, MessageType::FenceAck, result.epoch.value(), request_id,
                        std::span<const std::byte>(payload));
            break;
          }
          case MessageType::RevokeAuthority: {
            const RevokeAuthorityRequest request = decode_revoke_request(frame.payload, options.limits);
            const RevokeResult result = runtime->revoke_authority(request);
            const std::vector<std::byte> payload = encode_revoke_result(result, options.limits);
            write_frame(socket, MessageType::RevokeAck, result.epoch.value(), request_id,
                        std::span<const std::byte>(payload));
            break;
          }
          case MessageType::AdvanceEpoch: {
            const EpochAdvanceRequest request = decode_advance_request(frame.payload, options.limits);
            const EpochAdvanceResult result = runtime->advance_epoch(request);
            const std::vector<std::byte> payload = encode_advance_result(result, options.limits);
            write_frame(socket, MessageType::AdvanceEpochAck, result.epoch.value(), request_id,
                        std::span<const std::byte>(payload));
            break;
          }
          case MessageType::RetireParticipant: {
            const RetireParticipantRequest request =
                decode_retire_request(frame.payload, options.limits);
            const RetireResult result = runtime->retire_participant(request);
            const std::vector<std::byte> payload = encode_retire_result(result, options.limits);
            write_frame(socket, MessageType::RetireAck, result.epoch.value(), request_id,
                        std::span<const std::byte>(payload));
            break;
          }
          case MessageType::EpochStateRequest: {
            const std::uint32_t reserved = decode_snapshot_request_stub(frame.payload);
            if (reserved != 0) {
              throw malformed("epoch state request carries unsupported reserved bits");
            }
            const EpochState state = runtime->state();
            const std::vector<std::byte> payload = encode_epoch_state(state, options.limits);
            write_frame(socket, MessageType::EpochStateResponse, state.epoch.value(), request_id,
                        std::span<const std::byte>(payload));
            break;
          }
          case MessageType::QueryParticipant: {
            const ParticipantId participant = decode_query_participant(frame.payload, options.limits);
            const std::optional<ParticipantView> view = runtime->query_participant(participant);
            if (!view.has_value()) {
              write_error(socket, epoch.value(), request_id, 2, "unknown participant");
              break;
            }
            const std::vector<std::byte> payload = encode_participant_view(*view, options.limits);
            write_frame(socket, MessageType::ParticipantState, epoch.value(), request_id,
                        std::span<const std::byte>(payload));
            break;
          }
          case MessageType::SnapshotRequest: {
            const std::uint32_t reserved = decode_snapshot_request_stub(frame.payload);
            if (reserved != 0) {
              throw malformed("snapshot request carries unsupported reserved bits");
            }
            const AuthoritySnapshot snapshot = runtime->snapshot();
            const std::vector<std::byte> payload =
                encode_snapshot_response(snapshot, options.limits);
            write_frame(socket, MessageType::SnapshotResponse, snapshot.epoch.value(), request_id,
                        std::span<const std::byte>(payload));
            break;
          }
          case MessageType::ExplainRequest: {
            const ExplainRequestPayload request =
                decode_explain_request(frame.payload, options.limits);
            const Explanation explanation =
                runtime->explain_authority(request.participant, request.boot);
            const std::vector<std::byte> payload = encode_explanation(explanation, options.limits);
            write_frame(socket, MessageType::ExplainResponse, epoch.value(), request_id,
                        std::span<const std::byte>(payload));
            break;
          }
          default: {
            protocol_violation = true;
            write_error(socket, epoch.value(), request_id, 3, "message type is not accepted here");
            break;
          }
        }
        if (protocol_violation || stop_now.load()) {
          break;
        }
      }
    } catch (const EpochError& error) {
      protocol_violation = true;
      write_error(socket, runtime->current_epoch().value(), MutationAttemptId{},
                  static_cast<std::uint16_t>(error.category()), error.what());
    } catch (const std::exception&) {
      protocol_violation = true;
    }

    if (bound) {
      // The closure result is deliberately not propagated to the peer: the
      // connection is already ending. It is retained for the duration of the
      // call so the fencing decision is complete before the socket is closed.
      const FenceResult closure =
          runtime->close_session(binding.id, classify_close(protocol_violation, stop_now.load()));
      (void)closure;
    }
    socket.close();
    active_sessions.fetch_sub(1);
  }

  [[nodiscard]] std::uint32_t decode_snapshot_request_stub(std::span<const std::byte> payload) const {
    detail::ByteReader reader(payload);
    const std::uint32_t reserved = reader.u32();
    reader.require_end();
    return reserved;
  }
};

CoordinatorServer::CoordinatorServer(EpochRuntime& runtime, CoordinatorServerOptions options)
    : impl_(std::make_unique<Impl>(runtime, std::move(options))) {}

CoordinatorServer::~CoordinatorServer() = default;

void CoordinatorServer::start() { impl_->start(); }

void CoordinatorServer::stop() { impl_->stop(); }

std::uint16_t CoordinatorServer::port() const { return impl_->listener.bound_port(); }

std::size_t CoordinatorServer::active_session_count() const { return impl_->active_sessions.load(); }

bool CoordinatorServer::shutdown_requested() const { return impl_->shutdown_flag.load(); }

void CoordinatorServer::wait_for_shutdown_request() {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  impl_->shutdown_cv.wait(lock, [this]() { return impl_->shutdown_flag.load(); });
}

}  // namespace fabric_epoch
