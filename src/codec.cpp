// Fabric Epoch 1.0.0 - Summon Software Labs
#include "codec.hpp"

#include <algorithm>

#include "fabric_epoch/detail/sha256.hpp"
#include "fabric_epoch/error.hpp"

namespace fabric_epoch {

void encode_scope_set(detail::ByteWriter& writer, const ScopeSet& scopes) {
  detail::encode_scope_set_canonical(writer, scopes);
}

ScopeSet decode_scope_set(detail::ByteReader& reader, const Limits& limits) {
  return detail::decode_scope_set_canonical(reader, limits);
}

namespace detail {
namespace {

constexpr std::size_t kMaxParticipantIdLength = ParticipantId::max_length;
constexpr std::size_t kMaxScopeSubjectLength = AuthorityScope::max_subject_length;

/// Structural ceilings used when decoding a durable image. The loading
/// process's configurable retention limits bound what it will *add*; they must
/// not make an image written under a larger configuration unloadable.
constexpr std::size_t kAbsoluteMaxRecords = 4u * 1024u * 1024u;
constexpr std::size_t kAbsoluteMaxScopesPerSet = 64;
constexpr std::size_t kAbsoluteMaxDurableText = 4096;

[[nodiscard]] Limits structural_limits(const Limits& limits) {
  Limits structural = limits;
  structural.max_participants = kAbsoluteMaxRecords;
  structural.max_active_workers = kAbsoluteMaxRecords;
  structural.max_recent_fences = kAbsoluteMaxRecords;
  structural.max_authority_grants = kAbsoluteMaxRecords;
  structural.max_transition_history = kAbsoluteMaxRecords;
  structural.max_idempotency_records_per_participant = kAbsoluteMaxRecords;
  structural.max_fenced_boots_per_participant = kAbsoluteMaxRecords;
  structural.max_scopes_per_grant = kAbsoluteMaxScopesPerSet;
  structural.max_string_size = kAbsoluteMaxDurableText;
  return structural;
}

[[nodiscard]] ParticipantId decode_participant_id(ByteReader& reader, const Limits& limits) {
  const std::string text = reader.string(limits, kMaxParticipantIdLength);
  const auto parsed = ParticipantId::parse(text);
  if (!parsed.has_value()) {
    throw malformed("malformed participant identifier");
  }
  return *parsed;
}

[[nodiscard]] BoundedText decode_bounded_text(ByteReader& reader, const Limits& limits) {
  const std::string text = reader.string(limits, limits.max_string_size);
  if (!BoundedText::is_valid(text, limits.max_string_size)) {
    throw malformed("malformed bounded text");
  }
  return BoundedText::from_validated(text);
}

void encode_bounded_text(ByteWriter& writer, const BoundedText& text, const Limits& limits) {
  if (!BoundedText::is_valid(text.value(), limits.max_string_size)) {
    throw malformed("bounded text exceeds limits.max_string_size");
  }
  writer.string(text.value());
}

}  // namespace

void encode_scope_set_canonical(ByteWriter& writer, const ScopeSet& scopes) {
  writer.u32(static_cast<std::uint32_t>(scopes.size()));
  for (const auto& scope : scopes.scopes()) {
    writer.u8(static_cast<std::uint8_t>(scope.kind()));
    writer.string(scope.subject());
  }
}

ScopeSet decode_scope_set_canonical(ByteReader& reader, const Limits& limits) {
  const std::uint32_t count = reader.u32();
  if (count > limits.max_scopes_per_grant) {
    throw resource_limit("scope count exceeds limits.max_scopes_per_grant");
  }
  ScopeSet set;
  std::optional<AuthorityScope> previous;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto kind = scope_kind_from_wire(reader.u8());
    if (!kind.has_value()) {
      throw malformed("unknown scope kind on the wire");
    }
    const std::string subject = reader.string(limits, kMaxScopeSubjectLength);
    const auto scope = AuthorityScope::parse(*kind, subject);
    if (!scope.has_value()) {
      throw malformed("malformed authority scope subject");
    }
    if (previous.has_value() && !(*previous < *scope)) {
      // Rejects both duplicates and non-canonical ordering: the encoding of a
      // scope set is required to be a strictly increasing sequence.
      throw malformed("scope set is not in canonical strictly increasing order");
    }
    previous = scope;
    if (!set.insert(*scope, limits.max_scopes_per_grant)) {
      throw resource_limit("scope set exceeds limits.max_scopes_per_grant");
    }
  }
  return set;
}

void encode_participant_view(ByteWriter& writer, const ParticipantView& view, const Limits& limits) {
  writer.string(view.id.value());
  writer.u64(view.registration_generation.value());
  writer.u64(view.fence_generation.value());
  writer.u64(view.incarnation_floor.value());
  writer.u8(static_cast<std::uint8_t>(view.policy));
  std::uint8_t flags = 0;
  if (view.retired) {
    flags |= 0x01u;
  }
  if (view.revalidation_required) {
    flags |= 0x02u;
  }
  if (view.has_active_incarnation) {
    flags |= 0x04u;
  }
  writer.u8(flags);
  writer.fixed16(view.active_boot.bytes());
  writer.u64(view.active_incarnation.value());
  writer.u32(static_cast<std::uint32_t>(view.active_boots.size()));
  for (const auto& boot : view.active_boots) {
    writer.fixed16(boot.bytes());
  }
  encode_scope_set_canonical(writer, view.scopes);
  writer.u64(view.accepted_registrations);
  writer.u64(view.replay_hits);
  (void)limits;
}

ParticipantView decode_participant_view(ByteReader& reader, const Limits& limits) {
  ParticipantView view;
  view.id = decode_participant_id(reader, limits);
  view.registration_generation = RegistrationGeneration(reader.u64());
  view.fence_generation = FencingGeneration(reader.u64());
  view.incarnation_floor = IncarnationSequence(reader.u64());
  const auto policy = incarnation_policy_from_wire(reader.u8());
  if (!policy.has_value()) {
    throw malformed("unknown incarnation policy on the wire");
  }
  view.policy = *policy;
  const std::uint8_t flags = reader.u8();
  if ((flags & 0xF8u) != 0) {
    throw malformed("unknown participant view flag bits set");
  }
  view.retired = (flags & 0x01u) != 0;
  view.revalidation_required = (flags & 0x02u) != 0;
  view.has_active_incarnation = (flags & 0x04u) != 0;
  view.active_boot = WorkerBootId::from_bytes(reader.fixed16());
  view.active_incarnation = IncarnationSequence(reader.u64());
  const std::uint32_t boot_count = reader.u32();
  if (boot_count > limits.max_active_workers) {
    throw resource_limit("active boot count exceeds limits.max_active_workers");
  }
  for (std::uint32_t i = 0; i < boot_count; ++i) {
    view.active_boots.push_back(WorkerBootId::from_bytes(reader.fixed16()));
  }
  view.scopes = decode_scope_set_canonical(reader, limits);
  view.accepted_registrations = reader.u64();
  view.replay_hits = reader.u64();
  return view;
}

std::vector<std::byte> encode_durable_payload(const DurableState& state) {
  ByteWriter writer;
  writer.u64(state.current_epoch.value());
  writer.fixed16(state.coordinator_boot.bytes());
  writer.u64(state.generation.value());
  writer.u64(state.registration_floor.value());
  writer.u64(state.fence_floor.value());

  writer.u32(static_cast<std::uint32_t>(state.participants.size()));
  for (const auto& participant : state.participants) {
    writer.string(participant.id.value());
    writer.u64(participant.registration_generation.value());
    writer.u64(participant.fence_generation.value());
    writer.u64(participant.incarnation_floor.value());
    writer.u8(static_cast<std::uint8_t>(participant.policy));
    std::uint8_t flags = 0;
    if (participant.retired) {
      flags |= 0x01u;
    }
    if (participant.revalidation_required) {
      flags |= 0x02u;
    }
    if (participant.has_active_boot) {
      flags |= 0x04u;
    }
    writer.u8(flags);
    writer.fixed16(participant.active_boot.bytes());
    writer.u64(participant.active_incarnation.value());
    writer.u64(participant.next_incarnation.value());
    writer.u32(static_cast<std::uint32_t>(participant.extra_incarnations.size()));
    for (const auto& incarnation : participant.extra_incarnations) {
      writer.fixed16(incarnation.boot.bytes());
      writer.u64(incarnation.sequence.value());
      writer.u64(incarnation.registered_at_generation.value());
    }
    encode_scope_set_canonical(writer, participant.scopes);
    writer.u64(participant.accepted_registrations);
    writer.u64(participant.replay_hits);
    writer.u32(static_cast<std::uint32_t>(participant.fenced_boots.size()));
    for (const auto& boot : participant.fenced_boots) {
      writer.fixed16(boot.bytes());
    }
    writer.u32(static_cast<std::uint32_t>(participant.idempotency.size()));
    for (const auto& record : participant.idempotency) {
      writer.fixed16(record.attempt.bytes());
      writer.u8(static_cast<std::uint8_t>(record.operation));
      writer.fixed32(record.semantic.bytes());
      writer.u16(record.outcome);
      writer.u64(record.auxiliary);
      writer.u64(record.auxiliary_high);
      writer.fixed16(record.auxiliary_id);
    }
  }

  writer.u32(static_cast<std::uint32_t>(state.recent_fences.size()));
  for (const auto& fence : state.recent_fences) {
    writer.fixed16(fence.boot.bytes());
    writer.string(fence.participant.value());
    writer.u8(static_cast<std::uint8_t>(fence.reason));
    writer.u64(fence.generation.value());
    writer.u64(fence.epoch.value());
    writer.u64(fence.incarnation.value());
    writer.string(fence.note.value());
  }

  writer.u32(static_cast<std::uint32_t>(state.grants.size()));
  for (const auto& grant : state.grants) {
    writer.fixed16(grant.id.bytes());
    writer.u64(grant.epoch.value());
    writer.string(grant.participant.value());
    writer.fixed16(grant.boot.bytes());
    writer.u64(grant.incarnation.value());
    encode_scope_set_canonical(writer, grant.scopes);
    writer.u64(grant.generation.value());
    std::uint8_t flags = 0;
    if (grant.parent.has_value()) {
      flags |= 0x01u;
    }
    if (grant.delegator_boot.has_value()) {
      flags |= 0x02u;
    }
    if (grant.revoked) {
      flags |= 0x04u;
    }
    writer.u8(flags);
    writer.fixed16(grant.parent.has_value() ? grant.parent->bytes() : std::array<std::byte, 16>{});
    writer.fixed16(grant.delegator_boot.has_value() ? grant.delegator_boot->bytes()
                                                    : std::array<std::byte, 16>{});
    writer.u64(grant.revoked_at_epoch.value());
    writer.string(grant.provenance.value());
  }

  writer.u32(static_cast<std::uint32_t>(state.transitions.size()));
  for (const auto& transition : state.transitions) {
    writer.fixed16(transition.id.bytes());
    writer.u64(transition.from_epoch.value());
    writer.u64(transition.to_epoch.value());
    writer.u8(static_cast<std::uint8_t>(transition.cause));
    writer.fixed16(transition.coordinator_boot.bytes());
    writer.u64(transition.generation.value());
    writer.string(transition.provenance.value());
  }

  writer.u32(static_cast<std::uint32_t>(state.control_attempts.size()));
  for (const auto& record : state.control_attempts) {
    writer.fixed16(record.attempt.bytes());
    writer.u8(static_cast<std::uint8_t>(record.operation));
    writer.fixed32(record.semantic.bytes());
    writer.u16(record.outcome);
    writer.u64(record.auxiliary);
    writer.u64(record.auxiliary_high);
    writer.fixed16(record.auxiliary_id);
  }

  return writer.take();
}

DurableState decode_durable_payload(std::span<const std::byte> payload, const Limits& raw_limits) {
  const Limits limits = structural_limits(raw_limits);
  ByteReader reader(payload);
  DurableState state;
  state.current_epoch = CoordinatorEpoch(reader.u64());
  state.coordinator_boot = CoordinatorBootId::from_bytes(reader.fixed16());
  state.generation = EpochGeneration(reader.u64());
  state.registration_floor = RegistrationGeneration(reader.u64());
  state.fence_floor = FencingGeneration(reader.u64());

  const std::uint32_t participant_count = reader.u32();
  if (participant_count > limits.max_participants) {
    throw resource_limit("participant count exceeds limits.max_participants");
  }
  if (static_cast<std::uint64_t>(participant_count) * 32u > reader.remaining()) {
    throw malformed("participant count exceeds remaining durable input");
  }
  state.participants.reserve(participant_count);
  for (std::uint32_t i = 0; i < participant_count; ++i) {
    DurableParticipant participant;
    participant.id = decode_participant_id(reader, limits);
    participant.registration_generation = RegistrationGeneration(reader.u64());
    participant.fence_generation = FencingGeneration(reader.u64());
    participant.incarnation_floor = IncarnationSequence(reader.u64());
    const auto policy = incarnation_policy_from_wire(reader.u8());
    if (!policy.has_value()) {
      throw malformed("unknown incarnation policy in durable image");
    }
    participant.policy = *policy;
    const std::uint8_t flags = reader.u8();
    if ((flags & 0xF8u) != 0) {
      throw malformed("unknown participant flag bits in durable image");
    }
    participant.retired = (flags & 0x01u) != 0;
    participant.revalidation_required = (flags & 0x02u) != 0;
    participant.has_active_boot = (flags & 0x04u) != 0;
    participant.active_boot = WorkerBootId::from_bytes(reader.fixed16());
    participant.active_incarnation = IncarnationSequence(reader.u64());
    participant.next_incarnation = IncarnationSequence(reader.u64());

    const std::uint32_t extra_count = reader.u32();
    if (extra_count > limits.max_active_workers) {
      throw resource_limit("extra incarnation count exceeds limits.max_active_workers");
    }
    if (static_cast<std::uint64_t>(extra_count) * 32u > reader.remaining()) {
      throw malformed("extra incarnation count exceeds remaining durable input");
    }
    for (std::uint32_t j = 0; j < extra_count; ++j) {
      ActiveIncarnation incarnation;
      incarnation.boot = WorkerBootId::from_bytes(reader.fixed16());
      incarnation.sequence = IncarnationSequence(reader.u64());
      incarnation.registered_at_generation = RegistrationGeneration(reader.u64());
      participant.extra_incarnations.push_back(incarnation);
    }

    participant.scopes = decode_scope_set_canonical(reader, limits);
    participant.accepted_registrations = reader.u64();
    participant.replay_hits = reader.u64();

    const std::uint32_t fenced_count = reader.u32();
    if (fenced_count > limits.max_fenced_boots_per_participant) {
      throw resource_limit("fenced boot count exceeds limits.max_fenced_boots_per_participant");
    }
    if (static_cast<std::uint64_t>(fenced_count) * 16u > reader.remaining()) {
      throw malformed("fenced boot count exceeds remaining durable input");
    }
    for (std::uint32_t j = 0; j < fenced_count; ++j) {
      participant.fenced_boots.push_back(WorkerBootId::from_bytes(reader.fixed16()));
    }

    const std::uint32_t idempotency_count = reader.u32();
    if (idempotency_count > limits.max_idempotency_records_per_participant) {
      throw resource_limit("idempotency count exceeds limits.max_idempotency_records_per_participant");
    }
    if (static_cast<std::uint64_t>(idempotency_count) * 83u > reader.remaining()) {
      throw malformed("idempotency count exceeds remaining durable input");
    }
    for (std::uint32_t j = 0; j < idempotency_count; ++j) {
      IdempotencyRecord record;
      record.attempt = MutationAttemptId::from_bytes(reader.fixed16());
      const std::uint8_t operation = reader.u8();
      if (operation < 1 || operation > 6) {
        throw malformed("unknown operation kind in durable image");
      }
      record.operation = static_cast<OperationKind>(operation);
      record.semantic = SnapshotDigest::from_bytes(reader.fixed32());
      record.outcome = reader.u16();
      record.auxiliary = reader.u64();
      record.auxiliary_high = reader.u64();
      record.auxiliary_id = reader.fixed16();
      participant.idempotency.push_back(record);
    }

    state.participants.push_back(std::move(participant));
  }

  const std::uint32_t fence_count = reader.u32();
  if (fence_count > limits.max_recent_fences) {
    throw resource_limit("fence count exceeds limits.max_recent_fences");
  }
  if (static_cast<std::uint64_t>(fence_count) * 49u > reader.remaining()) {
    throw malformed("fence count exceeds remaining durable input");
  }
  for (std::uint32_t i = 0; i < fence_count; ++i) {
    FenceRecord fence;
    fence.boot = WorkerBootId::from_bytes(reader.fixed16());
    fence.participant = decode_participant_id(reader, limits);
    const std::uint8_t reason = reader.u8();
    const auto parsed_reason = fence_reason_from_wire(reason);
    if (!parsed_reason.has_value()) {
      throw malformed("unknown fence reason in durable image");
    }
    fence.reason = *parsed_reason;
    fence.generation = FencingGeneration(reader.u64());
    fence.epoch = CoordinatorEpoch(reader.u64());
    fence.incarnation = IncarnationSequence(reader.u64());
    fence.note = decode_bounded_text(reader, limits);
    state.recent_fences.push_back(std::move(fence));
  }

  const std::uint32_t grant_count = reader.u32();
  if (grant_count > limits.max_authority_grants) {
    throw resource_limit("grant count exceeds limits.max_authority_grants");
  }
  if (static_cast<std::uint64_t>(grant_count) * 89u > reader.remaining()) {
    throw malformed("grant count exceeds remaining durable input");
  }
  for (std::uint32_t i = 0; i < grant_count; ++i) {
    AuthorityGrant grant;
    grant.id = AuthorityGrantId::from_bytes(reader.fixed16());
    grant.epoch = CoordinatorEpoch(reader.u64());
    grant.participant = decode_participant_id(reader, limits);
    grant.boot = WorkerBootId::from_bytes(reader.fixed16());
    grant.incarnation = IncarnationSequence(reader.u64());
    grant.scopes = decode_scope_set_canonical(reader, limits);
    grant.generation = GrantGeneration(reader.u64());
    const std::uint8_t flags = reader.u8();
    if ((flags & 0xF8u) != 0) {
      throw malformed("unknown grant flag bits in durable image");
    }
    const auto parent_bytes = reader.fixed16();
    const auto delegator_bytes = reader.fixed16();
    if ((flags & 0x01u) != 0) {
      grant.parent = AuthorityGrantId::from_bytes(parent_bytes);
    }
    if ((flags & 0x02u) != 0) {
      grant.delegator_boot = WorkerBootId::from_bytes(delegator_bytes);
    }
    grant.revoked = (flags & 0x04u) != 0;
    grant.revoked_at_epoch = CoordinatorEpoch(reader.u64());
    grant.provenance = decode_bounded_text(reader, limits);
    state.grants.push_back(std::move(grant));
  }

  const std::uint32_t transition_count = reader.u32();
  if (transition_count > limits.max_transition_history) {
    throw resource_limit("transition count exceeds limits.max_transition_history");
  }
  if (static_cast<std::uint64_t>(transition_count) * 65u > reader.remaining()) {
    throw malformed("transition count exceeds remaining durable input");
  }
  for (std::uint32_t i = 0; i < transition_count; ++i) {
    EpochTransitionRecord transition;
    transition.id = EpochTransitionId::from_bytes(reader.fixed16());
    transition.from_epoch = CoordinatorEpoch(reader.u64());
    transition.to_epoch = CoordinatorEpoch(reader.u64());
    const std::uint8_t cause = reader.u8();
    const auto parsed_cause = epoch_transition_cause_from_wire(cause);
    if (!parsed_cause.has_value()) {
      throw malformed("unknown transition cause in durable image");
    }
    transition.cause = *parsed_cause;
    transition.coordinator_boot = CoordinatorBootId::from_bytes(reader.fixed16());
    transition.generation = EpochGeneration(reader.u64());
    transition.provenance = decode_bounded_text(reader, limits);
    state.transitions.push_back(std::move(transition));
  }

  const std::uint32_t control_count = reader.u32();
  if (control_count > limits.max_idempotency_records_per_participant) {
    throw resource_limit("control attempt count exceeds limits.max_idempotency_records_per_participant");
  }
  if (static_cast<std::uint64_t>(control_count) * 83u > reader.remaining()) {
    throw malformed("control attempt count exceeds remaining durable input");
  }
  for (std::uint32_t i = 0; i < control_count; ++i) {
    IdempotencyRecord record;
    record.attempt = MutationAttemptId::from_bytes(reader.fixed16());
    const std::uint8_t operation = reader.u8();
    if (operation < 1 || operation > 6) {
      throw malformed("unknown operation kind in durable control attempts");
    }
    record.operation = static_cast<OperationKind>(operation);
    record.semantic = SnapshotDigest::from_bytes(reader.fixed32());
    record.outcome = reader.u16();
    record.auxiliary = reader.u64();
    record.auxiliary_high = reader.u64();
    record.auxiliary_id = reader.fixed16();
    state.control_attempts.push_back(record);
  }

  reader.require_end();
  return state;
}

SnapshotDigest compute_semantic_digest(std::span<const std::byte> canonical) {
  return SnapshotDigest::from_bytes(Sha256::hash(canonical));
}

SnapshotDigest compute_snapshot_digest(const AuthoritySnapshot& snapshot) {
  ByteWriter writer;
  writer.string("FABRIC-EPOCH-SNAPSHOT-V1");
  writer.u64(snapshot.epoch.value());
  writer.fixed16(snapshot.coordinator_boot.bytes());
  writer.u64(snapshot.generation.value());
  writer.u64(snapshot.registration_floor.value());
  writer.u64(snapshot.fence_floor.value());

  std::vector<const ParticipantView*> participants;
  participants.reserve(snapshot.participants.size());
  for (const auto& participant : snapshot.participants) {
    participants.push_back(&participant);
  }
  std::sort(participants.begin(), participants.end(),
            [](const ParticipantView* lhs, const ParticipantView* rhs) { return lhs->id < rhs->id; });
  writer.u32(static_cast<std::uint32_t>(participants.size()));
  for (const auto* participant : participants) {
    writer.string(participant->id.value());
    writer.u64(participant->registration_generation.value());
    writer.u64(participant->fence_generation.value());
    writer.u64(participant->incarnation_floor.value());
    writer.u8(static_cast<std::uint8_t>(participant->policy));
    writer.u8(participant->retired ? 1u : 0u);
    writer.u8(participant->revalidation_required ? 1u : 0u);
    writer.fixed16(participant->active_boot.bytes());
    writer.u64(participant->active_incarnation.value());
    std::vector<WorkerBootId> boots = participant->active_boots;
    std::sort(boots.begin(), boots.end());
    writer.u32(static_cast<std::uint32_t>(boots.size()));
    for (const auto& boot : boots) {
      writer.fixed16(boot.bytes());
    }
    encode_scope_set_canonical(writer, participant->scopes);
  }

  std::vector<const FenceRecord*> fences;
  fences.reserve(snapshot.fences.size());
  for (const auto& fence : snapshot.fences) {
    fences.push_back(&fence);
  }
  std::sort(fences.begin(), fences.end(), [](const FenceRecord* lhs, const FenceRecord* rhs) {
    if (lhs->participant != rhs->participant) {
      return lhs->participant < rhs->participant;
    }
    if (lhs->boot != rhs->boot) {
      return lhs->boot < rhs->boot;
    }
    return lhs->generation < rhs->generation;
  });
  writer.u32(static_cast<std::uint32_t>(fences.size()));
  for (const auto* fence : fences) {
    writer.fixed16(fence->boot.bytes());
    writer.string(fence->participant.value());
    writer.u8(static_cast<std::uint8_t>(fence->reason));
    writer.u64(fence->generation.value());
    writer.u64(fence->epoch.value());
    writer.u64(fence->incarnation.value());
  }

  std::vector<const GrantView*> grants;
  grants.reserve(snapshot.grants.size());
  for (const auto& grant : snapshot.grants) {
    grants.push_back(&grant);
  }
  std::sort(grants.begin(), grants.end(),
            [](const GrantView* lhs, const GrantView* rhs) { return lhs->id < rhs->id; });
  writer.u32(static_cast<std::uint32_t>(grants.size()));
  for (const auto* grant : grants) {
    writer.fixed16(grant->id.bytes());
    writer.u64(grant->epoch.value());
    writer.string(grant->participant.value());
    writer.fixed16(grant->boot.bytes());
    writer.u64(grant->generation.value());
    writer.u8(grant->revoked ? 1u : 0u);
    writer.u8(grant->delegated ? 1u : 0u);
    encode_scope_set_canonical(writer, grant->scopes);
  }

  return compute_semantic_digest(writer.span());
}

}  // namespace detail
}  // namespace fabric_epoch
