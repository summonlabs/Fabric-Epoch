// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/protocol.hpp"

#include <array>
#include <cstring>
#include <string>

#include "codec.hpp"
#include "fabric_epoch/detail/encoding.hpp"
#include "fabric_epoch/detail/sha256.hpp"
#include "fabric_epoch/error.hpp"
#include "fabric_epoch/version.hpp"

namespace fabric_epoch {

namespace {

using detail::ByteReader;
using detail::ByteWriter;

constexpr std::size_t kMaxParticipantIdLength = ParticipantId::max_length;
constexpr std::size_t kMaxScopeSubjectLength = AuthorityScope::max_subject_length;
constexpr std::size_t kMaxVersionTextLength = 64;
constexpr std::size_t kMaxRoleTextLength = 32;

[[nodiscard]] ParticipantId decode_participant(ByteReader& reader, const Limits& limits) {
  const std::string text = reader.string(limits, kMaxParticipantIdLength);
  const auto parsed = ParticipantId::parse(text);
  if (!parsed.has_value()) {
    throw malformed("malformed participant identifier on the wire");
  }
  return *parsed;
}

void encode_participant(ByteWriter& writer, const ParticipantId& participant) {
  writer.string(participant.value());
}

[[nodiscard]] BoundedText decode_text(ByteReader& reader, const Limits& limits) {
  const std::string text = reader.string(limits, limits.max_string_size);
  if (!BoundedText::is_valid(text, limits.max_string_size)) {
    throw malformed("malformed bounded text on the wire");
  }
  return BoundedText::from_validated(text);
}

void encode_text(ByteWriter& writer, const BoundedText& text, const Limits& limits) {
  if (!BoundedText::is_valid(text.value(), limits.max_string_size)) {
    throw malformed("bounded text exceeds limits.max_string_size");
  }
  writer.string(text.value());
}

[[nodiscard]] AuthorityScope decode_scope(ByteReader& reader, const Limits& limits) {
  const auto kind = scope_kind_from_wire(reader.u8());
  if (!kind.has_value()) {
    throw malformed("unknown scope kind on the wire");
  }
  const std::string subject = reader.string(limits, kMaxScopeSubjectLength);
  const auto scope = AuthorityScope::parse(*kind, subject);
  if (!scope.has_value()) {
    throw malformed("malformed authority scope subject on the wire");
  }
  return *scope;
}

/// Request message types carry caller-supplied input and are additionally
/// bounded by limits.max_request_size. Response types are produced by this
/// implementation and only bounded by the transport frame limit.
[[nodiscard]] bool is_request_message(MessageType type) noexcept {
  switch (type) {
    case MessageType::Hello:
    case MessageType::RegisterParticipant:
    case MessageType::FenceWorker:
    case MessageType::ValidateAuthority:
    case MessageType::AdvanceEpoch:
    case MessageType::EpochStateRequest:
    case MessageType::RevokeAuthority:
    case MessageType::QueryParticipant:
    case MessageType::SnapshotRequest:
    case MessageType::ExplainRequest:
    case MessageType::RetireParticipant:
      return true;
    default:
      return false;
  }
}

void encode_scope(ByteWriter& writer, const AuthorityScope& scope) {
  writer.u8(static_cast<std::uint8_t>(scope.kind()));
  writer.string(scope.subject());
}

void encode_token(ByteWriter& writer, const AuthorityToken& token) {
  writer.u64(token.epoch.value());
  encode_participant(writer, token.participant);
  writer.fixed16(token.boot.bytes());
  writer.u64(token.incarnation.value());
  writer.u64(token.registration_generation.value());
}

[[nodiscard]] AuthorityToken decode_token(ByteReader& reader, const Limits& limits) {
  AuthorityToken token;
  token.epoch = CoordinatorEpoch(reader.u64());
  token.participant = decode_participant(reader, limits);
  token.boot = WorkerBootId::from_bytes(reader.fixed16());
  token.incarnation = IncarnationSequence(reader.u64());
  token.registration_generation = RegistrationGeneration(reader.u64());
  return token;
}

void encode_explanation_payload(ByteWriter& writer, const Explanation& explanation, const Limits& limits) {
  writer.u16(static_cast<std::uint16_t>(explanation.code()));
  writer.u32(static_cast<std::uint32_t>(explanation.details().size()));
  for (const auto& detail : explanation.details()) {
    if (detail.first.size() > limits.max_string_size || detail.second.size() > limits.max_string_size) {
      throw malformed("explanation detail exceeds limits.max_string_size");
    }
    writer.string(detail.first);
    writer.string(detail.second);
  }
}

[[nodiscard]] Explanation decode_explanation_payload(ByteReader& reader, const Limits& limits) {
  const std::uint16_t code = reader.u16();
  if (code < 1 || code > 22) {
    throw malformed("unknown explanation code on the wire");
  }
  const std::uint32_t count = reader.u32();
  if (count > 64) {
    throw resource_limit("explanation detail count exceeds the protocol bound");
  }
  Explanation explanation(static_cast<ExplanationCode>(code));
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string key = reader.string(limits, limits.max_string_size);
    std::string value = reader.string(limits, limits.max_string_size);
    explanation.with(std::move(key), std::move(value));
  }
  return explanation;
}

}  // namespace

std::string_view to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::Hello:
      return "HELLO";
    case MessageType::HelloAck:
      return "HELLO_ACK";
    case MessageType::RegisterParticipant:
      return "REGISTER_PARTICIPANT";
    case MessageType::RegisterAck:
      return "REGISTER_ACK";
    case MessageType::FenceWorker:
      return "FENCE_WORKER";
    case MessageType::FenceAck:
      return "FENCE_ACK";
    case MessageType::ValidateAuthority:
      return "VALIDATE_AUTHORITY";
    case MessageType::ValidateResult:
      return "VALIDATE_RESULT";
    case MessageType::AdvanceEpoch:
      return "ADVANCE_EPOCH";
    case MessageType::EpochStateRequest:
      return "EPOCH_STATE_REQUEST";
    case MessageType::EpochStateResponse:
      return "EPOCH_STATE_RESPONSE";
    case MessageType::RevokeAuthority:
      return "REVOKE_AUTHORITY";
    case MessageType::RevokeAck:
      return "REVOKE_ACK";
    case MessageType::QueryParticipant:
      return "QUERY_PARTICIPANT";
    case MessageType::ParticipantState:
      return "PARTICIPANT_STATE";
    case MessageType::SnapshotRequest:
      return "SNAPSHOT_REQUEST";
    case MessageType::SnapshotResponse:
      return "SNAPSHOT_RESPONSE";
    case MessageType::ExplainRequest:
      return "EXPLAIN_REQUEST";
    case MessageType::ExplainResponse:
      return "EXPLAIN_RESPONSE";
    case MessageType::Shutdown:
      return "SHUTDOWN";
    case MessageType::Error:
      return "ERROR";
    case MessageType::AdvanceEpochAck:
      return "ADVANCE_EPOCH_ACK";
    case MessageType::RetireParticipant:
      return "RETIRE_PARTICIPANT";
    case MessageType::RetireAck:
      return "RETIRE_ACK";
  }
  return "UNKNOWN";
}

std::optional<MessageType> message_type_from_wire(std::uint16_t value) noexcept {
  switch (value) {
    case 1:
      return MessageType::Hello;
    case 2:
      return MessageType::HelloAck;
    case 3:
      return MessageType::RegisterParticipant;
    case 4:
      return MessageType::RegisterAck;
    case 5:
      return MessageType::FenceWorker;
    case 6:
      return MessageType::FenceAck;
    case 7:
      return MessageType::ValidateAuthority;
    case 8:
      return MessageType::ValidateResult;
    case 9:
      return MessageType::AdvanceEpoch;
    case 10:
      return MessageType::EpochStateRequest;
    case 11:
      return MessageType::EpochStateResponse;
    case 12:
      return MessageType::RevokeAuthority;
    case 13:
      return MessageType::RevokeAck;
    case 14:
      return MessageType::QueryParticipant;
    case 15:
      return MessageType::ParticipantState;
    case 16:
      return MessageType::SnapshotRequest;
    case 17:
      return MessageType::SnapshotResponse;
    case 18:
      return MessageType::ExplainRequest;
    case 19:
      return MessageType::ExplainResponse;
    case 20:
      return MessageType::Shutdown;
    case 21:
      return MessageType::Error;
    case 22:
      return MessageType::AdvanceEpochAck;
    case 23:
      return MessageType::RetireParticipant;
    case 24:
      return MessageType::RetireAck;
    default:
      return std::nullopt;
  }
}

std::vector<std::byte> encode_frame(MessageType type, std::uint64_t sequence, CoordinatorEpoch epoch,
                                    const MutationAttemptId& request_id,
                                    std::span<const std::byte> payload, const Limits& limits) {
  if (payload.size() > limits.max_frame_payload) {
    throw resource_limit("frame payload exceeds limits.max_frame_payload");
  }
  if (is_request_message(type) && payload.size() > limits.max_request_size) {
    throw resource_limit("request payload exceeds limits.max_request_size");
  }
  ByteWriter writer;
  writer.u32(wire_protocol_magic);
  writer.u16(wire_protocol_version);
  writer.u16(static_cast<std::uint16_t>(type));
  writer.u16(0);
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  writer.u64(sequence);
  writer.u64(epoch.value());
  writer.fixed16(request_id.bytes());
  if (writer.size() != frame_integrity_offset) {
    throw invariant("frame header layout does not match the declared integrity offset");
  }
  detail::Sha256 hasher;
  hasher.update(writer.span());
  hasher.update(payload);
  const auto digest = hasher.finish();
  writer.fixed32(digest);
  writer.raw(payload);
  return writer.take();
}

std::uint32_t peek_payload_length(std::span<const std::byte> header_bytes) {
  if (header_bytes.size() < frame_header_size) {
    throw malformed("frame header is shorter than the fixed header size");
  }
  ByteReader reader(header_bytes.first(frame_header_size));
  (void)reader.u32();
  (void)reader.u16();
  (void)reader.u16();
  (void)reader.u16();
  return reader.u32();
}

FrameHeader decode_frame_header(std::span<const std::byte> header_bytes, const Limits& limits) {
  if (header_bytes.size() != frame_header_size) {
    throw malformed("frame header has an unexpected length");
  }
  ByteReader reader(header_bytes);
  FrameHeader header;
  header.magic = reader.u32();
  if (header.magic != wire_protocol_magic) {
    throw malformed("frame magic does not match Fabric Epoch");
  }
  header.protocol_version = reader.u16();
  if (header.protocol_version != wire_protocol_version) {
    throw EpochError(ErrorCategory::UnsupportedVersion, "frame protocol version is not supported");
  }
  const std::uint16_t raw_type = reader.u16();
  const auto type = message_type_from_wire(raw_type);
  if (!type.has_value()) {
    throw malformed("frame carries an unknown message type");
  }
  header.type = *type;
  header.flags = reader.u16();
  if (header.flags != 0) {
    throw malformed("frame carries unsupported flag bits");
  }
  header.payload_length = reader.u32();
  if (header.payload_length > limits.max_frame_payload) {
    throw resource_limit("frame payload length exceeds limits.max_frame_payload");
  }
  if (is_request_message(header.type) && header.payload_length > limits.max_request_size) {
    throw resource_limit("request payload length exceeds limits.max_request_size");
  }
  header.sequence = reader.u64();
  header.epoch = reader.u64();
  header.request_id = reader.fixed16();
  header.integrity = reader.fixed32();
  reader.require_end();
  return header;
}

Frame decode_frame(std::span<const std::byte> header_bytes, std::span<const std::byte> payload,
                   const Limits& limits) {
  const FrameHeader header = decode_frame_header(header_bytes, limits);
  if (header.payload_length != payload.size()) {
    throw malformed("frame payload length does not match the bytes received");
  }
  detail::Sha256 hasher;
  hasher.update(header_bytes.first(frame_integrity_offset));
  hasher.update(payload);
  const auto digest = hasher.finish();
  if (std::memcmp(digest.data(), header.integrity.data(), digest.size()) != 0) {
    throw EpochError(ErrorCategory::IntegrityFailure, "frame integrity digest does not match");
  }
  Frame frame;
  frame.header = header;
  frame.payload.assign(payload.begin(), payload.end());
  return frame;
}

// ---- payload codecs ------------------------------------------------------

std::vector<std::byte> encode_hello(std::string_view product_version, std::string_view role) {
  ByteWriter writer;
  writer.u16(wire_protocol_version);
  writer.string(product_version);
  writer.string(role);
  return writer.take();
}

HelloPayload decode_hello(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  HelloPayload hello;
  hello.wire_version = reader.u16();
  if (hello.wire_version != wire_protocol_version) {
    throw EpochError(ErrorCategory::UnsupportedVersion, "hello protocol version is not supported");
  }
  hello.product_version = reader.string(limits, kMaxVersionTextLength);
  hello.role = reader.string(limits, kMaxRoleTextLength);
  reader.require_end();
  return hello;
}

std::vector<std::byte> encode_hello_ack(CoordinatorEpoch epoch, const CoordinatorBootId& boot,
                                        const SessionId& session, std::string_view product_version) {
  ByteWriter writer;
  writer.u64(epoch.value());
  writer.fixed16(boot.bytes());
  writer.fixed16(session.bytes());
  writer.string(product_version);
  return writer.take();
}

HelloAckPayload decode_hello_ack(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  HelloAckPayload ack;
  ack.epoch = CoordinatorEpoch(reader.u64());
  ack.coordinator_boot = CoordinatorBootId::from_bytes(reader.fixed16());
  ack.session = SessionId::from_bytes(reader.fixed16());
  ack.product_version = reader.string(limits, kMaxVersionTextLength);
  reader.require_end();
  return ack;
}

std::vector<std::byte> encode_register_request(const RegisterParticipantRequest& request,
                                               const SessionId& session, const Limits& limits) {
  ByteWriter writer;
  writer.fixed16(request.attempt.bytes());
  writer.u64(request.expected_epoch.value());
  encode_participant(writer, request.participant);
  writer.fixed16(request.boot.bytes());
  encode_scope_set(writer, request.scopes);
  writer.u8(static_cast<std::uint8_t>(request.policy));
  writer.u8(request.expected_registration_generation.has_value() ? 1u : 0u);
  writer.u64(request.expected_registration_generation.has_value()
                 ? request.expected_registration_generation->value()
                 : 0u);
  encode_text(writer, request.provenance, limits);
  writer.fixed16(session.bytes());
  return writer.take();
}

RegisterWireRequest decode_register_request(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  RegisterWireRequest wire;
  wire.request.attempt = MutationAttemptId::from_bytes(reader.fixed16());
  wire.request.expected_epoch = CoordinatorEpoch(reader.u64());
  wire.request.participant = decode_participant(reader, limits);
  wire.request.boot = WorkerBootId::from_bytes(reader.fixed16());
  wire.request.scopes = decode_scope_set(reader, limits);
  const auto policy = incarnation_policy_from_wire(reader.u8());
  if (!policy.has_value()) {
    throw malformed("unknown incarnation policy on the wire");
  }
  wire.request.policy = *policy;
  const std::uint8_t has_generation = reader.u8();
  if (has_generation > 1) {
    throw malformed("register request carries an invalid generation presence flag");
  }
  const std::uint64_t generation = reader.u64();
  if (has_generation == 1) {
    wire.request.expected_registration_generation = RegistrationGeneration(generation);
  } else if (generation != 0) {
    throw malformed("register request carries a generation value without its presence flag");
  }
  wire.request.provenance = decode_text(reader, limits);
  wire.session = SessionId::from_bytes(reader.fixed16());
  reader.require_end();
  return wire;
}

std::vector<std::byte> encode_register_result(const RegistrationResult& result, const Limits& limits) {
  ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(result.outcome));
  writer.u64(result.epoch.value());
  writer.fixed16(result.coordinator_boot.bytes());
  writer.u8(result.registration_generation.has_value() ? 1u : 0u);
  writer.u64(result.registration_generation.has_value() ? result.registration_generation->value() : 0u);
  writer.u8(result.incarnation.has_value() ? 1u : 0u);
  writer.u64(result.incarnation.has_value() ? result.incarnation->value() : 0u);
  writer.u8(result.replayed ? 1u : 0u);
  encode_explanation_payload(writer, result.explanation, limits);
  return writer.take();
}

RegistrationResult decode_register_result(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  RegistrationResult result;
  const std::uint8_t outcome = reader.u8();
  if (outcome > static_cast<std::uint8_t>(RegistrationOutcome::RevalidationRequired)) {
    throw malformed("register result carries an unknown outcome code");
  }
  result.outcome = static_cast<RegistrationOutcome>(outcome);
  result.epoch = CoordinatorEpoch(reader.u64());
  result.coordinator_boot = CoordinatorBootId::from_bytes(reader.fixed16());
  const std::uint8_t has_generation = reader.u8();
  const std::uint64_t generation = reader.u64();
  if (has_generation == 1) {
    result.registration_generation = RegistrationGeneration(generation);
  } else if (has_generation != 0) {
    throw malformed("register result carries an invalid generation presence flag");
  }
  const std::uint8_t has_incarnation = reader.u8();
  const std::uint64_t incarnation = reader.u64();
  if (has_incarnation == 1) {
    result.incarnation = IncarnationSequence(incarnation);
  } else if (has_incarnation != 0) {
    throw malformed("register result carries an invalid incarnation presence flag");
  }
  const std::uint8_t replayed = reader.u8();
  if (replayed > 1) {
    throw malformed("register result carries an invalid replay flag");
  }
  result.replayed = replayed == 1;
  result.explanation = decode_explanation_payload(reader, limits);
  reader.require_end();
  return result;
}

std::vector<std::byte> encode_validate_request(const ValidateAuthorityRequest& request,
                                               const Limits& limits) {
  ByteWriter writer;
  encode_token(writer, request.token);
  encode_scope(writer, request.scope);
  writer.u8(request.expected_generation.has_value() ? 1u : 0u);
  writer.u64(request.expected_generation.has_value() ? request.expected_generation->value() : 0u);
  writer.u8(request.grant.has_value() ? 1u : 0u);
  writer.fixed16(request.grant.has_value() ? request.grant->bytes() : std::array<std::byte, 16>{});
  (void)limits;
  return writer.take();
}

ValidateAuthorityRequest decode_validate_request(std::span<const std::byte> payload,
                                                const Limits& limits) {
  ByteReader reader(payload);
  ValidateAuthorityRequest request;
  request.token = decode_token(reader, limits);
  request.scope = decode_scope(reader, limits);
  const std::uint8_t has_generation = reader.u8();
  const std::uint64_t generation = reader.u64();
  if (has_generation == 1) {
    request.expected_generation = RegistrationGeneration(generation);
  } else if (has_generation != 0) {
    throw malformed("validate request carries an invalid generation presence flag");
  }
  const std::uint8_t has_grant = reader.u8();
  const auto grant_bytes = reader.fixed16();
  if (has_grant == 1) {
    request.grant = AuthorityGrantId::from_bytes(grant_bytes);
  } else if (has_grant != 0) {
    throw malformed("validate request carries an invalid grant presence flag");
  }
  reader.require_end();
  return request;
}

std::vector<std::byte> encode_validation(const AuthorityValidation& validation, const Limits& limits) {
  ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(validation.code));
  writer.u8(static_cast<std::uint8_t>(validation.currentness));
  writer.u64(validation.epoch.value());
  writer.fixed16(validation.coordinator_boot.bytes());
  writer.u8(validation.registration_generation.has_value() ? 1u : 0u);
  writer.u64(validation.registration_generation.has_value() ? validation.registration_generation->value() : 0u);
  writer.u8(validation.incarnation.has_value() ? 1u : 0u);
  writer.u64(validation.incarnation.has_value() ? validation.incarnation->value() : 0u);
  writer.u8(validation.replaced_by_epoch.has_value() ? 1u : 0u);
  writer.u64(validation.replaced_by_epoch.has_value() ? validation.replaced_by_epoch->value() : 0u);
  writer.u8(validation.superseded_by.has_value() ? 1u : 0u);
  writer.fixed16(validation.superseded_by.has_value() ? validation.superseded_by->bytes()
                                                      : std::array<std::byte, 16>{});
  encode_explanation_payload(writer, validation.explanation, limits);
  return writer.take();
}

AuthorityValidation decode_validation(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  AuthorityValidation validation;
  const std::uint8_t code = reader.u8();
  if (code > static_cast<std::uint8_t>(AuthorityValidationCode::Revoked)) {
    throw malformed("validation carries an unknown result code");
  }
  validation.code = static_cast<AuthorityValidationCode>(code);
  const std::uint8_t currentness = reader.u8();
  if (currentness > static_cast<std::uint8_t>(AuthorityCurrentness::Revoked)) {
    throw malformed("validation carries an unknown currentness code");
  }
  validation.currentness = static_cast<AuthorityCurrentness>(currentness);
  validation.epoch = CoordinatorEpoch(reader.u64());
  validation.coordinator_boot = CoordinatorBootId::from_bytes(reader.fixed16());
  const std::uint8_t has_generation = reader.u8();
  const std::uint64_t generation = reader.u64();
  if (has_generation == 1) {
    validation.registration_generation = RegistrationGeneration(generation);
  } else if (has_generation != 0) {
    throw malformed("validation carries an invalid generation presence flag");
  }
  const std::uint8_t has_incarnation = reader.u8();
  const std::uint64_t incarnation = reader.u64();
  if (has_incarnation == 1) {
    validation.incarnation = IncarnationSequence(incarnation);
  } else if (has_incarnation != 0) {
    throw malformed("validation carries an invalid incarnation presence flag");
  }
  const std::uint8_t has_replaced = reader.u8();
  const std::uint64_t replaced = reader.u64();
  if (has_replaced == 1) {
    validation.replaced_by_epoch = CoordinatorEpoch(replaced);
  } else if (has_replaced != 0) {
    throw malformed("validation carries an invalid replaced-epoch presence flag");
  }
  const std::uint8_t has_superseded = reader.u8();
  const auto superseded = reader.fixed16();
  if (has_superseded == 1) {
    validation.superseded_by = WorkerBootId::from_bytes(superseded);
  } else if (has_superseded != 0) {
    throw malformed("validation carries an invalid superseded-boot presence flag");
  }
  validation.explanation = decode_explanation_payload(reader, limits);
  reader.require_end();
  return validation;
}

std::vector<std::byte> encode_fence_request(const FenceWorkerRequest& request, const Limits& limits) {
  ByteWriter writer;
  writer.fixed16(request.attempt.bytes());
  writer.u64(request.expected_epoch.value());
  encode_participant(writer, request.participant);
  writer.fixed16(request.boot.bytes());
  writer.u8(static_cast<std::uint8_t>(request.reason));
  encode_text(writer, request.note, limits);
  return writer.take();
}

FenceWorkerRequest decode_fence_request(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  FenceWorkerRequest request;
  request.attempt = MutationAttemptId::from_bytes(reader.fixed16());
  request.expected_epoch = CoordinatorEpoch(reader.u64());
  request.participant = decode_participant(reader, limits);
  request.boot = WorkerBootId::from_bytes(reader.fixed16());
  const auto reason = fence_reason_from_wire(reader.u8());
  if (!reason.has_value()) {
    throw malformed("fence request carries an unknown reason code");
  }
  request.reason = *reason;
  request.note = decode_text(reader, limits);
  reader.require_end();
  return request;
}

std::vector<std::byte> encode_fence_result(const FenceResult& result, const Limits& limits) {
  ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(result.outcome));
  writer.u64(result.epoch.value());
  writer.u8(result.fence_generation.has_value() ? 1u : 0u);
  writer.u64(result.fence_generation.has_value() ? result.fence_generation->value() : 0u);
  writer.u8(result.incarnation.has_value() ? 1u : 0u);
  writer.u64(result.incarnation.has_value() ? result.incarnation->value() : 0u);
  encode_explanation_payload(writer, result.explanation, limits);
  return writer.take();
}

FenceResult decode_fence_result(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  FenceResult result;
  const std::uint8_t outcome = reader.u8();
  if (outcome > static_cast<std::uint8_t>(FenceOutcome::NoFenceRequired)) {
    throw malformed("fence result carries an unknown outcome code");
  }
  result.outcome = static_cast<FenceOutcome>(outcome);
  result.epoch = CoordinatorEpoch(reader.u64());
  const std::uint8_t has_generation = reader.u8();
  const std::uint64_t generation = reader.u64();
  if (has_generation == 1) {
    result.fence_generation = FencingGeneration(generation);
  } else if (has_generation != 0) {
    throw malformed("fence result carries an invalid generation presence flag");
  }
  const std::uint8_t has_incarnation = reader.u8();
  const std::uint64_t incarnation = reader.u64();
  if (has_incarnation == 1) {
    result.incarnation = IncarnationSequence(incarnation);
  } else if (has_incarnation != 0) {
    throw malformed("fence result carries an invalid incarnation presence flag");
  }
  result.explanation = decode_explanation_payload(reader, limits);
  reader.require_end();
  return result;
}

std::vector<std::byte> encode_revoke_request(const RevokeAuthorityRequest& request, const Limits& limits) {
  ByteWriter writer;
  writer.fixed16(request.attempt.bytes());
  writer.u64(request.expected_epoch.value());
  writer.fixed16(request.grant.bytes());
  writer.u8(static_cast<std::uint8_t>(request.reason));
  encode_text(writer, request.note, limits);
  return writer.take();
}

RevokeAuthorityRequest decode_revoke_request(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  RevokeAuthorityRequest request;
  request.attempt = MutationAttemptId::from_bytes(reader.fixed16());
  request.expected_epoch = CoordinatorEpoch(reader.u64());
  request.grant = AuthorityGrantId::from_bytes(reader.fixed16());
  const auto reason = fence_reason_from_wire(reader.u8());
  if (!reason.has_value()) {
    throw malformed("revoke request carries an unknown reason code");
  }
  request.reason = *reason;
  request.note = decode_text(reader, limits);
  reader.require_end();
  return request;
}

std::vector<std::byte> encode_revoke_result(const RevokeResult& result, const Limits& limits) {
  ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(result.outcome));
  writer.u64(result.epoch.value());
  writer.u8(result.generation.has_value() ? 1u : 0u);
  writer.u64(result.generation.has_value() ? result.generation->value() : 0u);
  writer.u64(result.derived_grants_revoked);
  encode_explanation_payload(writer, result.explanation, limits);
  return writer.take();
}

RevokeResult decode_revoke_result(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  RevokeResult result;
  const std::uint8_t outcome = reader.u8();
  if (outcome > static_cast<std::uint8_t>(RevokeOutcome::ConflictingAttempt)) {
    throw malformed("revoke result carries an unknown outcome code");
  }
  result.outcome = static_cast<RevokeOutcome>(outcome);
  result.epoch = CoordinatorEpoch(reader.u64());
  const std::uint8_t has_generation = reader.u8();
  const std::uint64_t generation = reader.u64();
  if (has_generation == 1) {
    result.generation = GrantGeneration(generation);
  } else if (has_generation != 0) {
    throw malformed("revoke result carries an invalid generation presence flag");
  }
  result.derived_grants_revoked = static_cast<std::size_t>(reader.u64());
  result.explanation = decode_explanation_payload(reader, limits);
  reader.require_end();
  return result;
}

std::vector<std::byte> encode_advance_request(const EpochAdvanceRequest& request, const Limits& limits) {
  ByteWriter writer;
  writer.fixed16(request.attempt.bytes());
  writer.u64(request.expected_epoch.value());
  writer.u8(static_cast<std::uint8_t>(request.cause));
  encode_text(writer, request.provenance, limits);
  return writer.take();
}

EpochAdvanceRequest decode_advance_request(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  EpochAdvanceRequest request;
  request.attempt = MutationAttemptId::from_bytes(reader.fixed16());
  request.expected_epoch = CoordinatorEpoch(reader.u64());
  const auto cause = epoch_transition_cause_from_wire(reader.u8());
  if (!cause.has_value()) {
    throw malformed("advance request carries an unknown transition cause");
  }
  request.cause = *cause;
  request.provenance = decode_text(reader, limits);
  reader.require_end();
  return request;
}

std::vector<std::byte> encode_advance_result(const EpochAdvanceResult& result, const Limits& limits) {
  ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(result.outcome));
  writer.u64(result.epoch.value());
  writer.fixed16(result.coordinator_boot.bytes());
  writer.u8(result.transition.has_value() ? 1u : 0u);
  writer.fixed16(result.transition.has_value() ? result.transition->bytes() : std::array<std::byte, 16>{});
  writer.u8(result.replayed ? 1u : 0u);
  encode_explanation_payload(writer, result.explanation, limits);
  return writer.take();
}

EpochAdvanceResult decode_advance_result(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  EpochAdvanceResult result;
  const std::uint8_t outcome = reader.u8();
  if (outcome > static_cast<std::uint8_t>(EpochAdvanceOutcome::ConflictingAttempt)) {
    throw malformed("advance result carries an unknown outcome code");
  }
  result.outcome = static_cast<EpochAdvanceOutcome>(outcome);
  result.epoch = CoordinatorEpoch(reader.u64());
  result.coordinator_boot = CoordinatorBootId::from_bytes(reader.fixed16());
  const std::uint8_t has_transition = reader.u8();
  const auto transition = reader.fixed16();
  if (has_transition == 1) {
    result.transition = EpochTransitionId::from_bytes(transition);
  } else if (has_transition != 0) {
    throw malformed("advance result carries an invalid transition presence flag");
  }
  const std::uint8_t replayed = reader.u8();
  if (replayed > 1) {
    throw malformed("advance result carries an invalid replay flag");
  }
  result.replayed = replayed == 1;
  result.explanation = decode_explanation_payload(reader, limits);
  reader.require_end();
  return result;
}

std::vector<std::byte> encode_epoch_state(const EpochState& state, const Limits& limits) {
  ByteWriter writer;
  writer.u64(state.epoch.value());
  writer.fixed16(state.coordinator_boot.bytes());
  writer.u64(state.generation.value());
  writer.u64(state.registration_floor.value());
  writer.u64(state.fence_floor.value());
  writer.u64(state.participant_count);
  writer.u64(state.active_worker_count);
  writer.u64(state.retained_fence_count);
  writer.u64(state.live_grant_count);
  writer.u64(state.transition_history_count);
  (void)limits;
  return writer.take();
}

EpochState decode_epoch_state(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  EpochState state;
  state.epoch = CoordinatorEpoch(reader.u64());
  state.coordinator_boot = CoordinatorBootId::from_bytes(reader.fixed16());
  state.generation = EpochGeneration(reader.u64());
  state.registration_floor = RegistrationGeneration(reader.u64());
  state.fence_floor = FencingGeneration(reader.u64());
  state.participant_count = static_cast<std::size_t>(reader.u64());
  state.active_worker_count = static_cast<std::size_t>(reader.u64());
  state.retained_fence_count = static_cast<std::size_t>(reader.u64());
  state.live_grant_count = static_cast<std::size_t>(reader.u64());
  state.transition_history_count = static_cast<std::size_t>(reader.u64());
  reader.require_end();
  (void)limits;
  return state;
}

std::vector<std::byte> encode_query_participant(const ParticipantId& id, const Limits& limits) {
  ByteWriter writer;
  encode_participant(writer, id);
  (void)limits;
  return writer.take();
}

ParticipantId decode_query_participant(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  const ParticipantId id = decode_participant(reader, limits);
  reader.require_end();
  return id;
}

std::vector<std::byte> encode_participant_view(const ParticipantView& view, const Limits& limits) {
  ByteWriter writer;
  detail::encode_participant_view(writer, view, limits);
  return writer.take();
}

ParticipantView decode_participant_view(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  ParticipantView view = detail::decode_participant_view(reader, limits);
  reader.require_end();
  return view;
}

std::vector<std::byte> encode_explain_request(const ParticipantId& participant,
                                              const WorkerBootId& boot, const Limits& limits) {
  ByteWriter writer;
  encode_participant(writer, participant);
  writer.fixed16(boot.bytes());
  (void)limits;
  return writer.take();
}

ExplainRequestPayload decode_explain_request(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  ExplainRequestPayload request;
  request.participant = decode_participant(reader, limits);
  request.boot = WorkerBootId::from_bytes(reader.fixed16());
  reader.require_end();
  return request;
}

std::vector<std::byte> encode_explanation(const Explanation& explanation, const Limits& limits) {
  ByteWriter writer;
  encode_explanation_payload(writer, explanation, limits);
  return writer.take();
}

Explanation decode_explanation(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  Explanation explanation = decode_explanation_payload(reader, limits);
  reader.require_end();
  return explanation;
}

std::vector<std::byte> encode_snapshot_request(const Limits& limits) {
  (void)limits;
  ByteWriter writer;
  writer.u32(0);
  return writer.take();
}

std::vector<std::byte> encode_snapshot_response(const AuthoritySnapshot& snapshot, const Limits& limits) {
  ByteWriter writer;
  writer.fixed16(snapshot.id.bytes());
  writer.u64(snapshot.epoch.value());
  writer.fixed16(snapshot.coordinator_boot.bytes());
  writer.u64(snapshot.generation.value());
  writer.u64(snapshot.registration_floor.value());
  writer.u64(snapshot.fence_floor.value());

  writer.u32(static_cast<std::uint32_t>(snapshot.participants.size()));
  for (const auto& participant : snapshot.participants) {
    detail::encode_participant_view(writer, participant, limits);
  }

  writer.u32(static_cast<std::uint32_t>(snapshot.fences.size()));
  for (const auto& fence : snapshot.fences) {
    writer.fixed16(fence.boot.bytes());
    encode_participant(writer, fence.participant);
    writer.u8(static_cast<std::uint8_t>(fence.reason));
    writer.u64(fence.generation.value());
    writer.u64(fence.epoch.value());
    writer.u64(fence.incarnation.value());
    encode_text(writer, fence.note, limits);
  }

  writer.u32(static_cast<std::uint32_t>(snapshot.grants.size()));
  for (const auto& grant : snapshot.grants) {
    writer.fixed16(grant.id.bytes());
    writer.u64(grant.epoch.value());
    encode_participant(writer, grant.participant);
    writer.fixed16(grant.boot.bytes());
    writer.u64(grant.generation.value());
    writer.u8(grant.revoked ? 1u : 0u);
    writer.u8(grant.delegated ? 1u : 0u);
    encode_scope_set(writer, grant.scopes);
  }

  writer.fixed32(snapshot.digest.bytes());
  return writer.take();
}

AuthoritySnapshot decode_snapshot_response(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  AuthoritySnapshot snapshot;
  snapshot.id = EpochSnapshotId::from_bytes(reader.fixed16());
  snapshot.epoch = CoordinatorEpoch(reader.u64());
  snapshot.coordinator_boot = CoordinatorBootId::from_bytes(reader.fixed16());
  snapshot.generation = EpochGeneration(reader.u64());
  snapshot.registration_floor = RegistrationGeneration(reader.u64());
  snapshot.fence_floor = FencingGeneration(reader.u64());

  const std::uint32_t participant_count = reader.u32();
  if (participant_count > limits.max_participants) {
    throw resource_limit("snapshot participant count exceeds limits.max_participants");
  }
  for (std::uint32_t i = 0; i < participant_count; ++i) {
    snapshot.participants.push_back(detail::decode_participant_view(reader, limits));
  }

  const std::uint32_t fence_count = reader.u32();
  if (fence_count > limits.max_recent_fences) {
    throw resource_limit("snapshot fence count exceeds limits.max_recent_fences");
  }
  for (std::uint32_t i = 0; i < fence_count; ++i) {
    FenceRecord fence;
    fence.boot = WorkerBootId::from_bytes(reader.fixed16());
    fence.participant = decode_participant(reader, limits);
    const auto reason = fence_reason_from_wire(reader.u8());
    if (!reason.has_value()) {
      throw malformed("snapshot carries an unknown fence reason");
    }
    fence.reason = *reason;
    fence.generation = FencingGeneration(reader.u64());
    fence.epoch = CoordinatorEpoch(reader.u64());
    fence.incarnation = IncarnationSequence(reader.u64());
    fence.note = decode_text(reader, limits);
    snapshot.fences.push_back(std::move(fence));
  }

  const std::uint32_t grant_count = reader.u32();
  if (grant_count > limits.max_authority_grants) {
    throw resource_limit("snapshot grant count exceeds limits.max_authority_grants");
  }
  for (std::uint32_t i = 0; i < grant_count; ++i) {
    GrantView grant;
    grant.id = AuthorityGrantId::from_bytes(reader.fixed16());
    grant.epoch = CoordinatorEpoch(reader.u64());
    grant.participant = decode_participant(reader, limits);
    grant.boot = WorkerBootId::from_bytes(reader.fixed16());
    grant.generation = GrantGeneration(reader.u64());
    const std::uint8_t revoked = reader.u8();
    const std::uint8_t delegated = reader.u8();
    if (revoked > 1 || delegated > 1) {
      throw malformed("snapshot carries an invalid grant flag");
    }
    grant.revoked = revoked == 1;
    grant.delegated = delegated == 1;
    grant.scopes = decode_scope_set(reader, limits);
    snapshot.grants.push_back(std::move(grant));
  }

  snapshot.digest = SnapshotDigest::from_bytes(reader.fixed32());
  reader.require_end();
  return snapshot;
}

std::vector<std::byte> encode_retire_request(const RetireParticipantRequest& request,
                                             const Limits& limits) {
  ByteWriter writer;
  writer.fixed16(request.attempt.bytes());
  writer.u64(request.expected_epoch.value());
  encode_participant(writer, request.participant);
  encode_text(writer, request.note, limits);
  return writer.take();
}

RetireParticipantRequest decode_retire_request(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  RetireParticipantRequest request;
  request.attempt = MutationAttemptId::from_bytes(reader.fixed16());
  request.expected_epoch = CoordinatorEpoch(reader.u64());
  request.participant = decode_participant(reader, limits);
  request.note = decode_text(reader, limits);
  reader.require_end();
  return request;
}

std::vector<std::byte> encode_retire_result(const RetireResult& result, const Limits& limits) {
  ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(result.outcome));
  writer.u64(result.epoch.value());
  writer.u64(result.incarnations_fenced);
  encode_explanation_payload(writer, result.explanation, limits);
  return writer.take();
}

RetireResult decode_retire_result(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  RetireResult result;
  const std::uint8_t outcome = reader.u8();
  if (outcome > static_cast<std::uint8_t>(RetireOutcome::ConflictingAttempt)) {
    throw malformed("retire result carries an unknown outcome code");
  }
  result.outcome = static_cast<RetireOutcome>(outcome);
  result.epoch = CoordinatorEpoch(reader.u64());
  result.incarnations_fenced = static_cast<std::size_t>(reader.u64());
  result.explanation = decode_explanation_payload(reader, limits);
  reader.require_end();
  return result;
}

std::vector<std::byte> encode_error(std::uint16_t code, std::string_view message, const Limits& limits) {
  if (message.size() > limits.max_string_size) {
    message = message.substr(0, limits.max_string_size);
  }
  ByteWriter writer;
  writer.u16(code);
  writer.string(message);
  return writer.take();
}

ErrorPayload decode_error(std::span<const std::byte> payload, const Limits& limits) {
  ByteReader reader(payload);
  ErrorPayload error;
  error.code = reader.u16();
  error.message = reader.string(limits, limits.max_string_size);
  reader.require_end();
  return error;
}

}  // namespace fabric_epoch
