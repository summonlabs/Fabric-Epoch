// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fabric_epoch/authority.hpp"
#include "fabric_epoch/detail/encoding.hpp"
#include "fabric_epoch/epoch.hpp"
#include "fabric_epoch/fence.hpp"
#include "fabric_epoch/identity.hpp"
#include "fabric_epoch/limits.hpp"
#include "fabric_epoch/participant.hpp"
#include "fabric_epoch/snapshot.hpp"
#include "fabric_epoch/version.hpp"

namespace fabric_epoch {

/// Stable wire message identifiers. These values are part of the protocol
/// contract and are never derived from C++ enumerator ordinals.
enum class MessageType : std::uint16_t {
  Hello = 1,
  HelloAck = 2,
  RegisterParticipant = 3,
  RegisterAck = 4,
  FenceWorker = 5,
  FenceAck = 6,
  ValidateAuthority = 7,
  ValidateResult = 8,
  AdvanceEpoch = 9,
  EpochStateRequest = 10,
  EpochStateResponse = 11,
  RevokeAuthority = 12,
  RevokeAck = 13,
  QueryParticipant = 14,
  ParticipantState = 15,
  SnapshotRequest = 16,
  SnapshotResponse = 17,
  ExplainRequest = 18,
  ExplainResponse = 19,
  Shutdown = 20,
  Error = 21,
  AdvanceEpochAck = 22,
  RetireParticipant = 23,
  RetireAck = 24,
};

[[nodiscard]] std::string_view to_string(MessageType type) noexcept;
[[nodiscard]] std::optional<MessageType> message_type_from_wire(std::uint16_t value) noexcept;

/// Fixed protocol header. The integrity field covers every other header field
/// and the whole payload.
struct FrameHeader {
  std::uint32_t magic = wire_protocol_magic;
  std::uint16_t protocol_version = wire_protocol_version;
  MessageType type = MessageType::Error;
  std::uint16_t flags = 0;
  std::uint32_t payload_length = 0;
  std::uint64_t sequence = 0;
  std::uint64_t epoch = 0;
  std::array<std::byte, 16> request_id{};
  std::array<std::byte, 32> integrity{};
};

/// magic(4) version(2) type(2) flags(2) payload_length(4) sequence(8)
/// epoch(8) request_id(16) integrity(32)
inline constexpr std::size_t frame_header_size = 4 + 2 + 2 + 2 + 4 + 8 + 8 + 16 + 32;
/// Bytes preceding the integrity field; the digest covers these plus payload.
inline constexpr std::size_t frame_integrity_offset = 4 + 2 + 2 + 2 + 4 + 8 + 8 + 16;

/// A decoded, integrity-verified frame.
struct Frame {
  FrameHeader header;
  std::vector<std::byte> payload;
};

/// Encodes a frame: header fields plus payload, with the integrity digest
/// computed over the header fields that precede the digest and the payload.
[[nodiscard]] std::vector<std::byte> encode_frame(MessageType type, std::uint64_t sequence,
                                                  CoordinatorEpoch epoch,
                                                  const MutationAttemptId& request_id,
                                                  std::span<const std::byte> payload,
                                                  const Limits& limits);

/// Decodes and integrity-verifies a frame. The caller must first read the
/// fixed-size header and learn payload_length; this function takes the header
/// bytes and the payload bytes separately. Trailing bytes in the header buffer
/// are rejected.
[[nodiscard]] Frame decode_frame(std::span<const std::byte> header_bytes,
                                 std::span<const std::byte> payload, const Limits& limits);

/// Payload codecs. Every decoder consumes its input exactly; trailing bytes are
/// rejected. A malformed enum value is rejected rather than coerced.
[[nodiscard]] std::vector<std::byte> encode_hello(std::string_view product_version,
                                                  std::string_view role);
struct HelloPayload {
  std::string product_version;
  std::string role;
  std::uint16_t wire_version = 0;
};
[[nodiscard]] HelloPayload decode_hello(std::span<const std::byte> payload, const Limits& limits);
/// Decodes and validates only the fixed header. The caller reads exactly
/// frame_header_size bytes, asks for the payload length, bounds it against the
/// configured maximum, and only then allocates and reads the payload.
[[nodiscard]] FrameHeader decode_frame_header(std::span<const std::byte> header_bytes,
                                              const Limits& limits);
[[nodiscard]] std::uint32_t peek_payload_length(std::span<const std::byte> header_bytes);

[[nodiscard]] std::vector<std::byte> encode_hello_ack(CoordinatorEpoch epoch,
                                                      const CoordinatorBootId& boot,
                                                      const SessionId& session,
                                                      std::string_view product_version);
struct HelloAckPayload {
  CoordinatorEpoch epoch;
  CoordinatorBootId coordinator_boot;
  SessionId session;
  std::string product_version;
};
[[nodiscard]] HelloAckPayload decode_hello_ack(std::span<const std::byte> payload, const Limits& limits);

[[nodiscard]] std::vector<std::byte> encode_register_request(const RegisterParticipantRequest& request,
                                                             const SessionId& session,
                                                             const Limits& limits);
struct RegisterWireRequest {
  RegisterParticipantRequest request;
  SessionId session;
};
[[nodiscard]] RegisterWireRequest decode_register_request(std::span<const std::byte> payload,
                                                          const Limits& limits);
[[nodiscard]] std::vector<std::byte> encode_register_result(const RegistrationResult& result,
                                                            const Limits& limits);
[[nodiscard]] RegistrationResult decode_register_result(std::span<const std::byte> payload,
                                                        const Limits& limits);

[[nodiscard]] std::vector<std::byte> encode_validate_request(const ValidateAuthorityRequest& request,
                                                             const Limits& limits);
[[nodiscard]] ValidateAuthorityRequest decode_validate_request(std::span<const std::byte> payload,
                                                               const Limits& limits);
[[nodiscard]] std::vector<std::byte> encode_validation(const AuthorityValidation& validation,
                                                       const Limits& limits);
[[nodiscard]] AuthorityValidation decode_validation(std::span<const std::byte> payload,
                                                    const Limits& limits);

[[nodiscard]] std::vector<std::byte> encode_fence_request(const FenceWorkerRequest& request,
                                                          const Limits& limits);
[[nodiscard]] FenceWorkerRequest decode_fence_request(std::span<const std::byte> payload,
                                                      const Limits& limits);
[[nodiscard]] std::vector<std::byte> encode_fence_result(const FenceResult& result, const Limits& limits);
[[nodiscard]] FenceResult decode_fence_result(std::span<const std::byte> payload, const Limits& limits);

[[nodiscard]] std::vector<std::byte> encode_revoke_request(const RevokeAuthorityRequest& request,
                                                           const Limits& limits);
[[nodiscard]] RevokeAuthorityRequest decode_revoke_request(std::span<const std::byte> payload,
                                                           const Limits& limits);
[[nodiscard]] std::vector<std::byte> encode_revoke_result(const RevokeResult& result, const Limits& limits);
[[nodiscard]] RevokeResult decode_revoke_result(std::span<const std::byte> payload, const Limits& limits);

[[nodiscard]] std::vector<std::byte> encode_advance_request(const EpochAdvanceRequest& request,
                                                            const Limits& limits);
[[nodiscard]] EpochAdvanceRequest decode_advance_request(std::span<const std::byte> payload,
                                                         const Limits& limits);
[[nodiscard]] std::vector<std::byte> encode_advance_result(const EpochAdvanceResult& result,
                                                           const Limits& limits);
[[nodiscard]] EpochAdvanceResult decode_advance_result(std::span<const std::byte> payload,
                                                       const Limits& limits);

[[nodiscard]] std::vector<std::byte> encode_epoch_state(const EpochState& state, const Limits& limits);
[[nodiscard]] EpochState decode_epoch_state(std::span<const std::byte> payload, const Limits& limits);

[[nodiscard]] std::vector<std::byte> encode_query_participant(const ParticipantId& id, const Limits& limits);
[[nodiscard]] ParticipantId decode_query_participant(std::span<const std::byte> payload, const Limits& limits);
[[nodiscard]] std::vector<std::byte> encode_participant_view(const ParticipantView& view, const Limits& limits);
[[nodiscard]] ParticipantView decode_participant_view(std::span<const std::byte> payload, const Limits& limits);

[[nodiscard]] std::vector<std::byte> encode_explain_request(const ParticipantId& participant,
                                                            const WorkerBootId& boot, const Limits& limits);
struct ExplainRequestPayload {
  ParticipantId participant;
  WorkerBootId boot;
};
[[nodiscard]] ExplainRequestPayload decode_explain_request(std::span<const std::byte> payload,
                                                           const Limits& limits);
[[nodiscard]] std::vector<std::byte> encode_explanation(const Explanation& explanation, const Limits& limits);
[[nodiscard]] Explanation decode_explanation(std::span<const std::byte> payload, const Limits& limits);

[[nodiscard]] std::vector<std::byte> encode_snapshot_request(const Limits& limits);
[[nodiscard]] std::vector<std::byte> encode_snapshot_response(const AuthoritySnapshot& snapshot,
                                                              const Limits& limits);
[[nodiscard]] AuthoritySnapshot decode_snapshot_response(std::span<const std::byte> payload,
                                                         const Limits& limits);

[[nodiscard]] std::vector<std::byte> encode_retire_request(const RetireParticipantRequest& request,
                                                           const Limits& limits);
[[nodiscard]] RetireParticipantRequest decode_retire_request(std::span<const std::byte> payload,
                                                             const Limits& limits);
[[nodiscard]] std::vector<std::byte> encode_retire_result(const RetireResult& result, const Limits& limits);
[[nodiscard]] RetireResult decode_retire_result(std::span<const std::byte> payload, const Limits& limits);

[[nodiscard]] std::vector<std::byte> encode_error(std::uint16_t code, std::string_view message,
                                                  const Limits& limits);
struct ErrorPayload {
  std::uint16_t code = 0;
  std::string message;
};
[[nodiscard]] ErrorPayload decode_error(std::span<const std::byte> payload, const Limits& limits);

/// Canonical scope-set codec shared by several payloads.
void encode_scope_set(detail::ByteWriter& writer, const ScopeSet& scopes);
[[nodiscard]] ScopeSet decode_scope_set(detail::ByteReader& reader, const Limits& limits);

}  // namespace fabric_epoch
