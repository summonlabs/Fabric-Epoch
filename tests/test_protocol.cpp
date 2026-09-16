// Fabric Epoch 1.0.0 - Summon Software Labs
#include <cstring>
#include <string>
#include <vector>

#include "fabric_epoch/detail/encoding.hpp"
#include "fabric_epoch/detail/sha256.hpp"
#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

namespace {

std::vector<std::byte> make_frame(fe::MessageType type, const std::vector<std::byte>& payload,
                                  const fe::Limits& limits = fe::default_limits()) {
  return fe::encode_frame(type, 7, fe::CoordinatorEpoch(11), fe::MutationAttemptId::generate(), payload,
                          limits);
}

struct Split {
  std::vector<std::byte> header;
  std::vector<std::byte> payload;
};

Split split(const std::vector<std::byte>& frame) {
  Split out;
  out.header.assign(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(fe::frame_header_size));
  out.payload.assign(frame.begin() + static_cast<std::ptrdiff_t>(fe::frame_header_size), frame.end());
  return out;
}

}  // namespace

FE_TEST(protocol, wire_identifiers_are_stable_and_explicit) {
  FE_REQUIRE_EQ(static_cast<std::uint16_t>(fe::MessageType::Hello), std::uint16_t{1});
  FE_REQUIRE_EQ(static_cast<std::uint16_t>(fe::MessageType::RegisterParticipant), std::uint16_t{3});
  FE_REQUIRE_EQ(static_cast<std::uint16_t>(fe::MessageType::RegisterAck), std::uint16_t{4});
  FE_REQUIRE_EQ(static_cast<std::uint16_t>(fe::MessageType::FenceWorker), std::uint16_t{5});
  FE_REQUIRE_EQ(static_cast<std::uint16_t>(fe::MessageType::ValidateAuthority), std::uint16_t{7});
  FE_REQUIRE_EQ(static_cast<std::uint16_t>(fe::MessageType::ValidateResult), std::uint16_t{8});
  FE_REQUIRE_EQ(static_cast<std::uint16_t>(fe::MessageType::AdvanceEpoch), std::uint16_t{9});
  FE_REQUIRE_EQ(static_cast<std::uint16_t>(fe::MessageType::EpochStateResponse), std::uint16_t{11});
  FE_REQUIRE_EQ(static_cast<std::uint16_t>(fe::MessageType::RevokeAuthority), std::uint16_t{12});
  FE_REQUIRE_EQ(static_cast<std::uint16_t>(fe::MessageType::SnapshotResponse), std::uint16_t{17});
  FE_REQUIRE_EQ(static_cast<std::uint16_t>(fe::MessageType::Error), std::uint16_t{21});
  FE_REQUIRE_EQ(static_cast<std::uint16_t>(fe::MessageType::RetireParticipant), std::uint16_t{23});
  FE_REQUIRE_EQ(static_cast<std::uint16_t>(fe::MessageType::RetireAck), std::uint16_t{24});
  FE_REQUIRE(!fe::message_type_from_wire(0).has_value());
  FE_REQUIRE(!fe::message_type_from_wire(25).has_value());
  FE_REQUIRE(!fe::message_type_from_wire(0xFFFF).has_value());
  for (std::uint16_t value = 1; value <= 24; ++value) {
    FE_REQUIRE_MSG(fe::message_type_from_wire(value).has_value(), std::to_string(value));
  }
}

FE_TEST(protocol, frame_round_trips_and_carries_stable_header_fields) {
  const std::vector<std::byte> payload = {std::byte{1}, std::byte{2}, std::byte{3}};
  const fe::MutationAttemptId request_id = fe::MutationAttemptId::generate();
  const std::vector<std::byte> frame =
      fe::encode_frame(fe::MessageType::SnapshotRequest, 42, fe::CoordinatorEpoch(9), request_id, payload,
                       fe::default_limits());
  FE_REQUIRE_EQ(frame.size(), fe::frame_header_size + payload.size());
  const Split parts = split(frame);
  const fe::Frame decoded = fe::decode_frame(parts.header, parts.payload, fe::default_limits());
  FE_REQUIRE_EQ(decoded.header.magic, fe::wire_protocol_magic);
  FE_REQUIRE_EQ(decoded.header.protocol_version, fe::wire_protocol_version);
  FE_REQUIRE_EQ(decoded.header.type, fe::MessageType::SnapshotRequest);
  FE_REQUIRE_EQ(decoded.header.sequence, std::uint64_t{42});
  FE_REQUIRE_EQ(decoded.header.epoch, std::uint64_t{9});
  FE_REQUIRE_EQ(decoded.header.payload_length, std::uint32_t{3});
  FE_REQUIRE_EQ(decoded.header.flags, std::uint16_t{0});
  FE_REQUIRE(std::memcmp(decoded.header.request_id.data(), request_id.bytes().data(), 16) == 0);
  FE_REQUIRE(decoded.payload == payload);
}

FE_TEST(protocol, integrity_covers_every_semantic_header_field_and_the_payload) {
  const std::vector<std::byte> payload = {std::byte{9}, std::byte{8}, std::byte{7}, std::byte{6}};
  const std::vector<std::byte> frame =
      fe::encode_frame(fe::MessageType::RegisterParticipant, 3, fe::CoordinatorEpoch(5),
                       fe::MutationAttemptId::generate(), payload, fe::default_limits());
  for (std::size_t index = 0; index < frame.size(); ++index) {
    std::vector<std::byte> corrupted = frame;
    corrupted[index] ^= std::byte{0x40};
    const Split parts = split(corrupted);
    bool rejected = false;
    try {
      (void)fe::decode_frame(parts.header, parts.payload, fe::default_limits());
    } catch (const fe::EpochError&) {
      rejected = true;
    }
    FE_REQUIRE_MSG(rejected, "byte " + std::to_string(index) + " was not covered by the integrity check");
  }
}

FE_TEST(protocol, malformed_frames_are_rejected_deterministically) {
  const fe::Limits limits = fe::default_limits();
  const std::vector<std::byte> payload = {std::byte{1}};
  const std::vector<std::byte> frame = make_frame(fe::MessageType::ValidateAuthority, payload, limits);
  const Split parts = split(frame);

  FE_REQUIRE_THROWS(fe::decode_frame(parts.header, {}, limits));
  FE_REQUIRE_THROWS(fe::decode_frame(parts.header,
                                     std::vector<std::byte>(parts.payload.begin(), parts.payload.end() - 1),
                                     limits));

  std::vector<std::byte> extended = parts.payload;
  extended.push_back(std::byte{0});
  FE_REQUIRE_THROWS(fe::decode_frame(parts.header, extended, limits));

  std::vector<std::byte> short_header = parts.header;
  short_header.pop_back();
  FE_REQUIRE_THROWS(fe::decode_frame_header(short_header, limits));

  std::vector<std::byte> long_header = parts.header;
  long_header.push_back(std::byte{0});
  FE_REQUIRE_THROWS(fe::decode_frame_header(long_header, limits));

  std::vector<std::byte> bad_magic = parts.header;
  bad_magic[0] = std::byte{0x00};
  FE_REQUIRE_THROWS(fe::decode_frame_header(bad_magic, limits));

  std::vector<std::byte> bad_version = parts.header;
  bad_version[4] = std::byte{0x09};
  FE_REQUIRE_THROWS(fe::decode_frame_header(bad_version, limits));

  std::vector<std::byte> unknown_type = parts.header;
  unknown_type[6] = std::byte{0x7F};
  unknown_type[7] = std::byte{0x00};
  FE_REQUIRE_THROWS(fe::decode_frame_header(unknown_type, limits));

  std::vector<std::byte> bad_flags = parts.header;
  bad_flags[8] = std::byte{0x01};
  FE_REQUIRE_THROWS(fe::decode_frame_header(bad_flags, limits));
}

FE_TEST(protocol, trailing_bytes_are_rejected_by_every_payload_decoder) {
  const fe::Limits limits = fe::default_limits();
  const fe::MutationAttemptId attempt = fe::MutationAttemptId::generate();
  const fe::ParticipantId id = participant("publisher.wire");
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();

  fe::RegisterParticipantRequest registration;
  registration.attempt = attempt;
  registration.expected_epoch = fe::CoordinatorEpoch(1);
  registration.participant = id;
  registration.boot = boot;
  registration.scopes = default_scopes();
  std::vector<std::byte> encoded =
      fe::encode_register_request(registration, fe::SessionId::generate(), limits);
  encoded.push_back(std::byte{0});
  FE_REQUIRE_THROWS(fe::decode_register_request(encoded, limits));

  std::vector<std::byte> hello = fe::encode_hello("1.0.0", "cli");
  hello.push_back(std::byte{0});
  FE_REQUIRE_THROWS(fe::decode_hello(hello, limits));

  std::vector<std::byte> epoch_state = fe::encode_epoch_state(fe::EpochState{}, limits);
  epoch_state.push_back(std::byte{0});
  FE_REQUIRE_THROWS(fe::decode_epoch_state(epoch_state, limits));
}

FE_TEST(protocol, malformed_enum_values_are_rejected_not_coerced) {
  const fe::Limits limits = fe::default_limits();
  const fe::MutationAttemptId attempt = fe::MutationAttemptId::generate();
  const fe::ParticipantId id = participant("publisher.wire");
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();

  // Hand-crafted register payload carrying a scope kind that does not exist.
  fe::detail::ByteWriter writer;
  writer.fixed16(attempt.bytes());
  writer.u64(1);
  writer.string(id.value());
  writer.fixed16(boot.bytes());
  writer.u32(1);
  writer.u8(9);
  writer.string("fabric-main");
  writer.u8(0);
  writer.u8(0);
  writer.u64(0);
  writer.string("test");
  writer.fixed16(fe::SessionId{}.bytes());
  const std::vector<std::byte> encoded = writer.take();
  FE_REQUIRE_THROWS(fe::decode_register_request(encoded, limits));

  // The same payload with a valid scope kind decodes.
  fe::detail::ByteWriter valid_writer;
  valid_writer.fixed16(attempt.bytes());
  valid_writer.u64(1);
  valid_writer.string(id.value());
  valid_writer.fixed16(boot.bytes());
  valid_writer.u32(1);
  valid_writer.u8(0);
  valid_writer.string("fabric-main");
  valid_writer.u8(0);
  valid_writer.u8(0);
  valid_writer.u64(0);
  valid_writer.string("test");
  valid_writer.fixed16(fe::SessionId{}.bytes());
  const fe::RegisterWireRequest decoded =
      fe::decode_register_request(valid_writer.take(), limits);
  FE_REQUIRE_EQ(decoded.request.participant, id);
  FE_REQUIRE_EQ(decoded.request.boot, boot);
}

FE_TEST(protocol, payload_codecs_round_trip_all_messages) {
  const fe::Limits limits = fe::default_limits();
  const fe::ParticipantId id = participant("publisher.codec");
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const fe::SessionId session = fe::SessionId::generate();

  const std::vector<std::byte> hello_ack = fe::encode_hello_ack(fe::CoordinatorEpoch(4),
                                                                fe::CoordinatorBootId::generate(), session,
                                                                "1.0.0");
  const fe::HelloAckPayload decoded_ack = fe::decode_hello_ack(hello_ack, limits);
  FE_REQUIRE_EQ(decoded_ack.epoch, fe::CoordinatorEpoch(4));
  FE_REQUIRE_EQ(decoded_ack.session, session);
  FE_REQUIRE_EQ(decoded_ack.product_version, std::string("1.0.0"));

  fe::FenceWorkerRequest fence;
  fence.attempt = fe::MutationAttemptId::generate();
  fence.expected_epoch = fe::CoordinatorEpoch(2);
  fence.participant = id;
  fence.boot = boot;
  fence.reason = fe::FenceReason::Reincarnation;
  fence.note = fe::BoundedText::from_validated("operator note");
  const fe::FenceWorkerRequest decoded_fence =
      fe::decode_fence_request(fe::encode_fence_request(fence, limits), limits);
  FE_REQUIRE_EQ(decoded_fence.attempt, fence.attempt);
  FE_REQUIRE_EQ(decoded_fence.boot, fence.boot);
  FE_REQUIRE_EQ(decoded_fence.reason, fe::FenceReason::Reincarnation);
  FE_REQUIRE_EQ(decoded_fence.note.value(), fence.note.value());

  fe::ValidateAuthorityRequest validate;
  validate.token.epoch = fe::CoordinatorEpoch(2);
  validate.token.participant = id;
  validate.token.boot = boot;
  validate.token.incarnation = fe::IncarnationSequence(3);
  validate.token.registration_generation = fe::RegistrationGeneration(5);
  validate.scope = scope(fe::ScopeKind::Site, "site-a");
  validate.expected_generation = fe::RegistrationGeneration(5);
  validate.grant = fe::AuthorityGrantId::generate();
  const fe::ValidateAuthorityRequest decoded_validate =
      fe::decode_validate_request(fe::encode_validate_request(validate, limits), limits);
  FE_REQUIRE_EQ(decoded_validate.token.boot, boot);
  FE_REQUIRE_EQ(decoded_validate.token.incarnation, fe::IncarnationSequence(3));
  FE_REQUIRE_EQ(decoded_validate.scope.subject(), std::string("site-a"));
  FE_REQUIRE_EQ(decoded_validate.grant, validate.grant);
}

FE_TEST(protocol, explanation_payloads_round_trip_in_canonical_order) {
  const fe::Limits limits = fe::default_limits();
  fe::Explanation explanation(fe::ExplanationCode::WorkerBootFenced);
  explanation.with("zeta", "1").with("alpha", "2").with("mid", "3");
  const fe::Explanation decoded =
      fe::decode_explanation(fe::encode_explanation(explanation, limits), limits);
  FE_REQUIRE_EQ(decoded.code(), explanation.code());
  FE_REQUIRE_EQ(decoded.render(), explanation.render());
}
