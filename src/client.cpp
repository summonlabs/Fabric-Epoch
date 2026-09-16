// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/client.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "fabric_epoch/error.hpp"
#include "fabric_epoch/version.hpp"
#include "socket.hpp"

namespace fabric_epoch {
namespace {

[[nodiscard]] MutationAttemptId request_identifier() { return MutationAttemptId::generate(); }

}  // namespace

struct CoordinatorClient::Impl {
  ClientOptions options;
  net::Socket socket;
  SessionId session;
  CoordinatorEpoch epoch;
  std::uint64_t sequence = 1;
  bool hello_done = false;
  /// Set by close() and request_shutdown() so a blocked wait_for_disconnect()
  /// is released deterministically instead of relying on socket teardown.
  bool disconnect_wait_cancelled = false;

  explicit Impl(ClientOptions client_options) : options(std::move(client_options)) {}

  void ensure_connected() const {
    if (!socket.valid()) {
      throw io_failure("client is not connected to a coordinator");
    }
  }

  void ensure_hello() const {
    if (!hello_done) {
      throw protocol_violation("HELLO must complete before any other request");
    }
  }

  void send(MessageType type, const MutationAttemptId& request_id, std::span<const std::byte> payload) {
    ensure_connected();
    const std::vector<std::byte> bytes =
        encode_frame(type, sequence++, epoch, request_id, payload, options.limits);
    socket.send_all(std::span<const std::byte>(bytes));
  }

  /// Reads one response frame. Returns false when the peer closed the
  /// connection before a frame started arriving.
  [[nodiscard]] bool receive(Frame& frame) {
    std::array<std::byte, frame_header_size> header{};
    if (!socket.recv_exact(header)) {
      return false;
    }
    const std::uint32_t length = peek_payload_length(header);
    if (length > options.limits.max_frame_payload) {
      throw resource_limit("declared response payload exceeds limits.max_frame_payload");
    }
    std::vector<std::byte> payload(length);
    if (length != 0 && !socket.recv_exact(payload)) {
      throw io_failure("coordinator closed the connection in the middle of a frame payload");
    }
    frame = decode_frame(header, payload, options.limits);
    return true;
  }

  [[nodiscard]] Frame exchange(MessageType request_type, MessageType expected_type,
                               const MutationAttemptId& request_id,
                               std::span<const std::byte> payload) {
    send(request_type, request_id, payload);
    Frame frame;
    if (!receive(frame)) {
      throw io_failure("coordinator closed the connection before responding");
    }
    if (frame.header.type == MessageType::Error) {
      const ErrorPayload error = decode_error(frame.payload, options.limits);
      throw EpochError(ErrorCategory::ProtocolViolation,
                       "coordinator rejected the request: " + error.message);
    }
    if (frame.header.type != expected_type) {
      throw protocol_violation(std::string("unexpected response type ") +
                               std::string(to_string(frame.header.type)));
    }
    return frame;
  }
};

CoordinatorClient::CoordinatorClient(ClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

CoordinatorClient::~CoordinatorClient() = default;

void CoordinatorClient::connect() {
  std::lock_guard<std::mutex> lock(mutex_);
  net::ensure_initialized();
  impl_->socket = net::connect_to(impl_->options.host, impl_->options.port,
                                  impl_->options.connect_timeout_milliseconds);
  impl_->hello_done = false;
  impl_->sequence = 1;
  impl_->disconnect_wait_cancelled = false;
}

void CoordinatorClient::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  impl_->disconnect_wait_cancelled = true;
  impl_->socket.close();
  impl_->hello_done = false;
}

bool CoordinatorClient::connected() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return impl_->socket.valid();
}

HelloAckPayload CoordinatorClient::hello(std::string_view role) {
  std::lock_guard<std::mutex> lock(mutex_);
  impl_->ensure_connected();
  const std::vector<std::byte> payload = encode_hello(version_string, role);
  impl_->send(MessageType::Hello, request_identifier(), std::span<const std::byte>(payload));
  Frame frame;
  if (!impl_->receive(frame)) {
    throw io_failure("coordinator closed the connection during HELLO");
  }
  if (frame.header.type == MessageType::Error) {
    const ErrorPayload error = decode_error(frame.payload, impl_->options.limits);
    throw EpochError(ErrorCategory::ProtocolViolation, "HELLO rejected: " + error.message);
  }
  if (frame.header.type != MessageType::HelloAck) {
    throw protocol_violation("coordinator did not answer HELLO with HELLO_ACK");
  }
  HelloAckPayload ack = decode_hello_ack(frame.payload, impl_->options.limits);
  impl_->session = ack.session;
  impl_->epoch = ack.epoch;
  impl_->hello_done = true;
  return ack;
}

RegistrationResult CoordinatorClient::register_participant(const RegisterParticipantRequest& request,
                                                           const SessionId& session) {
  std::lock_guard<std::mutex> lock(mutex_);
  impl_->ensure_hello();
  const std::vector<std::byte> payload =
      encode_register_request(request, session, impl_->options.limits);
  const Frame frame =
      impl_->exchange(MessageType::RegisterParticipant, MessageType::RegisterAck,
                      request.attempt, std::span<const std::byte>(payload));
  return decode_register_result(frame.payload, impl_->options.limits);
}

AuthorityValidation CoordinatorClient::validate_authority(const ValidateAuthorityRequest& request) {
  std::lock_guard<std::mutex> lock(mutex_);
  impl_->ensure_hello();
  const std::vector<std::byte> payload = encode_validate_request(request, impl_->options.limits);
  const Frame frame = impl_->exchange(MessageType::ValidateAuthority, MessageType::ValidateResult,
                                      request_identifier(), std::span<const std::byte>(payload));
  return decode_validation(frame.payload, impl_->options.limits);
}

FenceResult CoordinatorClient::fence_worker(const FenceWorkerRequest& request) {
  std::lock_guard<std::mutex> lock(mutex_);
  impl_->ensure_hello();
  const std::vector<std::byte> payload = encode_fence_request(request, impl_->options.limits);
  const Frame frame = impl_->exchange(MessageType::FenceWorker, MessageType::FenceAck,
                                      request.attempt, std::span<const std::byte>(payload));
  return decode_fence_result(frame.payload, impl_->options.limits);
}

RevokeResult CoordinatorClient::revoke_authority(const RevokeAuthorityRequest& request) {
  std::lock_guard<std::mutex> lock(mutex_);
  impl_->ensure_hello();
  const std::vector<std::byte> payload = encode_revoke_request(request, impl_->options.limits);
  const Frame frame = impl_->exchange(MessageType::RevokeAuthority, MessageType::RevokeAck,
                                      request.attempt, std::span<const std::byte>(payload));
  return decode_revoke_result(frame.payload, impl_->options.limits);
}

RetireResult CoordinatorClient::retire_participant(const RetireParticipantRequest& request) {
  std::lock_guard<std::mutex> lock(mutex_);
  impl_->ensure_hello();
  const std::vector<std::byte> payload = encode_retire_request(request, impl_->options.limits);
  const Frame frame = impl_->exchange(MessageType::RetireParticipant, MessageType::RetireAck,
                                      request.attempt, std::span<const std::byte>(payload));
  return decode_retire_result(frame.payload, impl_->options.limits);
}

EpochAdvanceResult CoordinatorClient::advance_epoch(const EpochAdvanceRequest& request) {
  std::lock_guard<std::mutex> lock(mutex_);
  impl_->ensure_hello();
  const std::vector<std::byte> payload = encode_advance_request(request, impl_->options.limits);
  const Frame frame = impl_->exchange(MessageType::AdvanceEpoch, MessageType::AdvanceEpochAck,
                                      request.attempt, std::span<const std::byte>(payload));
  EpochAdvanceResult result = decode_advance_result(frame.payload, impl_->options.limits);
  impl_->epoch = result.epoch;
  return result;
}

EpochState CoordinatorClient::epoch_state() {
  std::lock_guard<std::mutex> lock(mutex_);
  impl_->ensure_hello();
  const std::vector<std::byte> payload = encode_snapshot_request(impl_->options.limits);
  const Frame frame = impl_->exchange(MessageType::EpochStateRequest, MessageType::EpochStateResponse,
                                      request_identifier(), std::span<const std::byte>(payload));
  EpochState state = decode_epoch_state(frame.payload, impl_->options.limits);
  impl_->epoch = state.epoch;
  return state;
}

std::optional<ParticipantView> CoordinatorClient::query_participant(const ParticipantId& participant) {
  std::lock_guard<std::mutex> lock(mutex_);
  impl_->ensure_hello();
  const std::vector<std::byte> payload = encode_query_participant(participant, impl_->options.limits);
  impl_->send(MessageType::QueryParticipant, request_identifier(), std::span<const std::byte>(payload));
  Frame frame;
  if (!impl_->receive(frame)) {
    throw io_failure("coordinator closed the connection before responding");
  }
  if (frame.header.type == MessageType::Error) {
    return std::nullopt;
  }
  if (frame.header.type != MessageType::ParticipantState) {
    throw protocol_violation("unexpected response type for a participant query");
  }
  return decode_participant_view(frame.payload, impl_->options.limits);
}

AuthoritySnapshot CoordinatorClient::snapshot() {
  std::lock_guard<std::mutex> lock(mutex_);
  impl_->ensure_hello();
  const std::vector<std::byte> payload = encode_snapshot_request(impl_->options.limits);
  const Frame frame = impl_->exchange(MessageType::SnapshotRequest, MessageType::SnapshotResponse,
                                      request_identifier(), std::span<const std::byte>(payload));
  return decode_snapshot_response(frame.payload, impl_->options.limits);
}

Explanation CoordinatorClient::explain(const ParticipantId& participant, const WorkerBootId& boot) {
  std::lock_guard<std::mutex> lock(mutex_);
  impl_->ensure_hello();
  const std::vector<std::byte> payload =
      encode_explain_request(participant, boot, impl_->options.limits);
  const Frame frame = impl_->exchange(MessageType::ExplainRequest, MessageType::ExplainResponse,
                                      request_identifier(), std::span<const std::byte>(payload));
  return decode_explanation(frame.payload, impl_->options.limits);
}

void CoordinatorClient::request_shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  impl_->ensure_hello();
  const std::vector<std::byte> payload = encode_snapshot_request(impl_->options.limits);
  impl_->send(MessageType::Shutdown, request_identifier(), std::span<const std::byte>(payload));
  impl_->disconnect_wait_cancelled = true;
  impl_->socket.close();
}

void CoordinatorClient::wait_for_disconnect() {
  // A short wait budget keeps close() able to release this call; the loop
  // itself never gives up on its own, so an idle but healthy session is not
  // mistaken for a disconnect.
  for (;;) {
    bool finished = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!impl_->socket.valid() || impl_->disconnect_wait_cancelled) {
        finished = true;
      } else if (impl_->socket.wait_readable(200)) {
        Frame frame;
        if (!impl_->receive(frame)) {
          finished = true;
        }
      }
    }
    if (finished) {
      break;
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  impl_->disconnect_wait_cancelled = true;
  impl_->socket.close();
}

}  // namespace fabric_epoch
