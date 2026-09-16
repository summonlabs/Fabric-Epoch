// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "fabric_epoch/authority.hpp"
#include "fabric_epoch/digest.hpp"
#include "fabric_epoch/epoch.hpp"
#include "fabric_epoch/fence.hpp"
#include "fabric_epoch/identity.hpp"
#include "fabric_epoch/limits.hpp"
#include "fabric_epoch/participant.hpp"
#include "fabric_epoch/scope.hpp"

namespace fabric_epoch {

/// Operation kinds recorded in the idempotency table. Stable wire-free codes.
enum class OperationKind : std::uint8_t {
  RegisterParticipant = 1,
  GrantAuthority = 2,
  FenceWorker = 3,
  RevokeAuthority = 4,
  AdvanceEpoch = 5,
  RetireParticipant = 6,
};

[[nodiscard]] std::string_view to_string(OperationKind kind) noexcept;

/// One retained idempotency record. The semantic digest deliberately excludes
/// expected-epoch and expected-generation fields so that a caller refreshing
/// those fields still produces an exact replay of the same semantic request.
struct IdempotencyRecord {
  MutationAttemptId attempt;
  OperationKind operation = OperationKind::RegisterParticipant;
  SnapshotDigest semantic;
  std::uint16_t outcome = 0;
  std::uint64_t auxiliary = 0;
  std::uint64_t auxiliary_high = 0;
  std::array<std::byte, 16> auxiliary_id{};
};

/// One active incarnation (used by multi-incarnation participants).
struct ActiveIncarnation {
  WorkerBootId boot;
  IncarnationSequence sequence;
  RegistrationGeneration registered_at_generation;
};

/// Durable participant record. Durable participant identity is deliberately
/// separate from live authority: after a coordinator restart the record
/// survives while every active incarnation is invalidated.
struct DurableParticipant {
  ParticipantId id;
  RegistrationGeneration registration_generation;
  FencingGeneration fence_generation;
  IncarnationSequence incarnation_floor;
  IncarnationPolicy policy = IncarnationPolicy::SingleIncarnation;
  bool retired = false;
  bool revalidation_required = false;
  bool has_active_boot = false;
  WorkerBootId active_boot;
  IncarnationSequence active_incarnation;
  /// Monotonic allocator of incarnation sequences for this participant. Never
  /// decreases and never reuses a value, so an incarnation sequence is never
  /// reissued even after fencing or pruning.
  IncarnationSequence next_incarnation;
  std::vector<ActiveIncarnation> extra_incarnations;
  ScopeSet scopes;
  std::uint64_t accepted_registrations = 0;
  std::uint64_t replay_hits = 0;
  std::vector<WorkerBootId> fenced_boots;
  std::vector<IdempotencyRecord> idempotency;
};

/// Complete durable authority state.
struct DurableState {
  CoordinatorEpoch current_epoch;
  CoordinatorBootId coordinator_boot;
  EpochGeneration generation;
  RegistrationGeneration registration_floor;
  FencingGeneration fence_floor;
  std::vector<DurableParticipant> participants;
  std::vector<FenceRecord> recent_fences;
  std::vector<AuthorityGrant> grants;
  std::vector<EpochTransitionRecord> transitions;
  /// Bounded idempotency table for control-plane operations that are not bound
  /// to a participant (currently epoch advancement).
  std::vector<IdempotencyRecord> control_attempts;
};

/// Absolute structural ceilings. A durable image is validated against these
/// rather than against the loading process's configurable retention limits:
/// those limits bound what this process will *add*, they do not invalidate what
/// is already durable. Throws EpochError(CorruptState) when the image is
/// self-contradictory and EpochError(ResourceLimit) when it exceeds a ceiling.
void validate_durable_state(const DurableState& state);

}  // namespace fabric_epoch
