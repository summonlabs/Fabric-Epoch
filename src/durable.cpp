// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/durable.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "fabric_epoch/error.hpp"

namespace fabric_epoch {
namespace {

/// Structural ceilings for a decoded durable image. These are deliberately
/// independent of the configurable Limits: a store written by a process with
/// larger retention settings must remain loadable, and the ceiling still stops
/// an absurd declaration before it reaches the allocator.
constexpr std::size_t kAbsoluteMaxRecords = 4u * 1024u * 1024u;
constexpr std::size_t kAbsoluteMaxScopesPerSet = 64;

}  // namespace

std::string_view to_string(OperationKind kind) noexcept {
  switch (kind) {
    case OperationKind::RegisterParticipant:
      return "REGISTER_PARTICIPANT";
    case OperationKind::GrantAuthority:
      return "GRANT_AUTHORITY";
    case OperationKind::FenceWorker:
      return "FENCE_WORKER";
    case OperationKind::RevokeAuthority:
      return "REVOKE_AUTHORITY";
    case OperationKind::AdvanceEpoch:
      return "ADVANCE_EPOCH";
    case OperationKind::RetireParticipant:
      return "RETIRE_PARTICIPANT";
  }
  return "UNKNOWN";
}

void validate_durable_state(const DurableState& state) {
  if (state.participants.size() > kAbsoluteMaxRecords) {
    throw resource_limit("durable participant count exceeds the structural ceiling");
  }
  if (state.recent_fences.size() > kAbsoluteMaxRecords) {
    throw resource_limit("durable fence count exceeds the structural ceiling");
  }
  if (state.grants.size() > kAbsoluteMaxRecords) {
    throw resource_limit("durable grant count exceeds the structural ceiling");
  }
  if (state.transitions.size() > kAbsoluteMaxRecords) {
    throw resource_limit("durable transition count exceeds the structural ceiling");
  }

  // A pristine, never-established store is representable but must be empty.
  if (state.current_epoch.is_zero()) {
    if (!state.participants.empty() || !state.grants.empty() || !state.recent_fences.empty() ||
        !state.transitions.empty() || !state.coordinator_boot.is_nil() || !state.generation.is_zero() ||
        !state.registration_floor.is_zero() || !state.fence_floor.is_zero()) {
      throw corrupt("durable image declares an unestablished epoch while carrying authority state");
    }
    return;
  }

  // A committed epoch can never be lower than the highest epoch a transition
  // reached. This rejects a rolled-back epoch header inside a self-consistent
  // looking image.
  for (const auto& transition : state.transitions) {
    if (transition.to_epoch > state.current_epoch) {
      throw corrupt("durable image current epoch is lower than a recorded transition target epoch");
    }
    if (transition.to_epoch <= transition.from_epoch) {
      throw corrupt("durable image records a non-advancing epoch transition");
    }
  }
  if (state.generation.value() < static_cast<std::uint64_t>(state.transitions.size())) {
    throw corrupt("durable generation is lower than the number of committed transitions");
  }

  std::unordered_set<std::string> participant_ids;
  std::unordered_map<std::string, std::string> boot_owner;
  std::uint64_t max_registration_generation = 0;
  std::uint64_t max_fence_generation = 0;

  for (const auto& participant : state.participants) {
    if (participant.id.empty()) {
      throw corrupt("durable participant record has an empty identifier");
    }
    if (!participant_ids.insert(participant.id.value()).second) {
      throw corrupt("durable image contains a duplicate participant record");
    }
    if (participant.registration_generation > state.registration_floor) {
      throw corrupt("participant registration generation exceeds the durable registration floor");
    }
    if (participant.fence_generation > state.fence_floor) {
      throw corrupt("participant fence generation exceeds the durable fence floor");
    }
    if (participant.has_active_boot) {
      if (participant.active_boot.is_nil()) {
        throw corrupt("durable participant declares a nil active worker boot");
      }
      if (participant.active_incarnation >= participant.next_incarnation) {
        throw corrupt("durable active incarnation was never issued by the incarnation allocator");
      }
      if (participant.retired) {
        throw corrupt("durable participant is retired while still declaring an active incarnation");
      }
      const auto inserted = boot_owner.emplace(participant.active_boot.to_string(), participant.id.value());
      if (!inserted.second) {
        throw corrupt("durable image binds one worker boot to more than one participant");
      }
    }
    if (participant.retired && participant.has_active_boot) {
      throw corrupt("durable participant is retired while still declaring an active incarnation");
    }
    if (participant.next_incarnation <= participant.incarnation_floor) {
      throw corrupt("durable participant incarnation allocator is not above the fencing floor");
    }
    for (const auto& fenced : participant.fenced_boots) {
      if (participant.has_active_boot && fenced == participant.active_boot) {
        throw corrupt("durable participant lists its active worker boot as fenced");
      }
      for (const auto& incarnation : participant.extra_incarnations) {
        if (fenced == incarnation.boot) {
          throw corrupt("durable participant lists a current worker boot as fenced");
        }
      }
    }
    if (participant.has_active_boot && participant.next_incarnation <= participant.active_incarnation) {
      throw corrupt("durable participant incarnation allocator does not exceed the active incarnation");
    }
    if (participant.extra_incarnations.size() + (participant.has_active_boot ? 1u : 0u) >
        kAbsoluteMaxRecords) {
      throw resource_limit("durable participant active incarnation count exceeds the structural ceiling");
    }
    for (const auto& incarnation : participant.extra_incarnations) {
      if (incarnation.boot.is_nil()) {
        throw corrupt("durable participant declares a nil worker boot");
      }
      if (incarnation.boot == participant.active_boot) {
        throw corrupt("durable participant lists one worker boot twice as a current incarnation");
      }
      for (const auto& other : participant.extra_incarnations) {
        if (&other != &incarnation && other.boot == incarnation.boot) {
          throw corrupt("durable participant lists one worker boot twice as a current incarnation");
        }
      }
      if (participant.next_incarnation <= incarnation.sequence) {
        throw corrupt("durable participant incarnation allocator does not exceed an issued incarnation");
      }
      const auto inserted = boot_owner.emplace(incarnation.boot.to_string(), participant.id.value());
      if (!inserted.second) {
        throw corrupt("durable image binds one worker boot to more than one participant");
      }
    }
    if (participant.idempotency.size() > kAbsoluteMaxRecords) {
      throw resource_limit("durable idempotency record count exceeds the structural ceiling");
    }
    if (participant.fenced_boots.size() > kAbsoluteMaxRecords) {
      throw resource_limit("durable fenced boot count exceeds the structural ceiling");
    }
    std::unordered_set<std::string> fenced;
    for (const auto& boot : participant.fenced_boots) {
      if (boot.is_nil()) {
        throw corrupt("durable participant retains a nil fenced worker boot");
      }
      if (!fenced.insert(boot.to_string()).second) {
        throw corrupt("durable participant retains a duplicate fenced worker boot");
      }
    }
    std::unordered_set<std::string> attempts;
    for (const auto& record : participant.idempotency) {
      if (record.attempt.is_nil()) {
        throw corrupt("durable idempotency record has a nil attempt identifier");
      }
      if (!attempts.insert(record.attempt.to_string()).second) {
        throw corrupt("durable participant retains a duplicate attempt identifier");
      }
    }
    max_registration_generation = std::max(max_registration_generation, participant.registration_generation.value());
    max_fence_generation = std::max(max_fence_generation, participant.fence_generation.value());
  }

  if (max_registration_generation > state.registration_floor.value()) {
    throw corrupt("registration floor is lower than a participant registration generation");
  }
  if (max_fence_generation > state.fence_floor.value()) {
    throw corrupt("fence floor is lower than a participant fence generation");
  }

  std::unordered_set<std::string> fence_boots;
  for (const auto& fence : state.recent_fences) {
    if (fence.boot.is_nil()) {
      throw corrupt("durable fence record has a nil worker boot");
    }
    if (fence.generation > state.fence_floor) {
      throw corrupt("durable fence record generation exceeds the durable fence floor");
    }
    if (fence.epoch > state.current_epoch) {
      throw corrupt("durable fence record epoch is newer than the current epoch");
    }
    if (!fence_boots.insert(fence.participant.value() + "|" + fence.boot.to_string()).second) {
      throw corrupt("durable image retains a duplicate fence record for one participant and boot");
    }
  }

  std::unordered_set<std::string> grant_ids;
  for (const auto& grant : state.grants) {
    if (grant.id.is_nil()) {
      throw corrupt("durable grant has a nil identifier");
    }
    if (!grant_ids.insert(grant.id.to_string()).second) {
      throw corrupt("durable image contains a duplicate grant identifier");
    }
    if (grant.epoch.value() > state.current_epoch.value()) {
      throw corrupt("durable grant is bound to an epoch newer than the current epoch");
    }
    if (grant.scopes.empty()) {
      throw corrupt("durable grant carries an empty scope set");
    }
    if (grant.scopes.size() > kAbsoluteMaxScopesPerSet) {
      throw resource_limit("durable grant scope count exceeds the structural ceiling");
    }
    if (grant.revoked && grant.revoked_at_epoch.value() > state.current_epoch.value()) {
      throw corrupt("durable grant revocation epoch is newer than the current epoch");
    }
    if (grant.boot.is_nil() && !grant.revoked) {
      throw corrupt("durable live grant is bound to a nil worker boot");
    }
  }
  for (const auto& grant : state.grants) {
    if (grant.parent.has_value() && grant_ids.find(grant.parent->to_string()) == grant_ids.end()) {
      throw corrupt("durable delegated grant references a parent grant that is not present");
    }
    if (grant.parent.has_value() && *grant.parent == grant.id) {
      throw corrupt("durable grant is its own delegation parent");
    }
  }

  if (state.control_attempts.size() > kAbsoluteMaxRecords) {
    throw resource_limit("durable control attempt count exceeds the structural ceiling");
  }
  std::unordered_set<std::string> control_attempts;
  for (const auto& record : state.control_attempts) {
    if (record.attempt.is_nil()) {
      throw corrupt("durable control attempt has a nil attempt identifier");
    }
    if (record.operation != OperationKind::AdvanceEpoch &&
        record.operation != OperationKind::RevokeAuthority) {
      throw corrupt("durable control attempt records an operation that is not control-plane");
    }
    if (!control_attempts.insert(record.attempt.to_string()).second) {
      throw corrupt("durable image retains a duplicate control attempt identifier");
    }
  }

  std::unordered_set<std::string> transition_ids;
  for (const auto& transition : state.transitions) {
    if (transition.id.is_nil()) {
      throw corrupt("durable transition record has a nil identifier");
    }
    if (!transition_ids.insert(transition.id.to_string()).second) {
      throw corrupt("durable image contains a duplicate transition identifier");
    }
  }
}

}  // namespace fabric_epoch
