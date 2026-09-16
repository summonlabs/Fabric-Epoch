// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fabric_epoch/authority.hpp"
#include "fabric_epoch/explanation.hpp"
#include "fabric_epoch/fence.hpp"
#include "fabric_epoch/identity.hpp"
#include "fabric_epoch/scope.hpp"

namespace fabric_epoch {

/// Incarnation policy for a participant.
enum class IncarnationPolicy : std::uint8_t {
  /// At most one current WorkerBootId per ParticipantId (default).
  SingleIncarnation = 0,
  /// A participant may hold several simultaneously current incarnations. Must
  /// be requested explicitly at registration time.
  MultiIncarnation = 1,
};

[[nodiscard]] std::string_view to_string(IncarnationPolicy policy) noexcept;
[[nodiscard]] std::optional<IncarnationPolicy> incarnation_policy_from_wire(std::uint8_t value) noexcept;

/// Public view of one durable participant record.
struct ParticipantView {
  ParticipantId id;
  RegistrationGeneration registration_generation;
  FencingGeneration fence_generation;
  IncarnationSequence incarnation_floor;
  IncarnationPolicy policy = IncarnationPolicy::SingleIncarnation;
  bool retired = false;
  bool revalidation_required = false;
  bool has_active_incarnation = false;
  WorkerBootId active_boot;
  IncarnationSequence active_incarnation;
  std::vector<WorkerBootId> active_boots;
  ScopeSet scopes;
  std::uint64_t accepted_registrations = 0;
  std::uint64_t replay_hits = 0;

  [[nodiscard]] std::size_t active_incarnation_count() const noexcept {
    return (has_active_incarnation ? 1u : 0u) + active_boots.size();
  }

  [[nodiscard]] std::string render() const;
};

struct RegisterParticipantRequest {
  MutationAttemptId attempt;
  CoordinatorEpoch expected_epoch;
  ParticipantId participant;
  WorkerBootId boot;
  ScopeSet scopes;
  IncarnationPolicy policy = IncarnationPolicy::SingleIncarnation;
  /// Optional expected registration generation. A mismatch reports
  /// REVALIDATION_REQUIRED. Excluded from replay identity on purpose: a caller
  /// retrying with a refreshed generation must still be recognised as an exact
  /// retry of the same semantic request.
  std::optional<RegistrationGeneration> expected_registration_generation;
  BoundedText provenance;
};

enum class RegistrationOutcome : std::uint8_t {
  Registered = 0,
  Idempotent = 1,
  StaleEpoch = 2,
  StaleWorkerBoot = 3,
  AlreadyActive = 4,
  ConflictingIncarnation = 5,
  UnauthorizedScope = 6,
  MalformedRequest = 7,
  ResourceLimit = 8,
  RetiredParticipant = 9,
  ConflictingAttempt = 10,
  /// The presented expected registration generation no longer matches the
  /// durable registration generation; the caller must revalidate.
  RevalidationRequired = 11,
};

[[nodiscard]] std::string_view to_string(RegistrationOutcome outcome) noexcept;

struct RetireParticipantRequest {
  MutationAttemptId attempt;
  CoordinatorEpoch expected_epoch;
  ParticipantId participant;
  BoundedText note;
};

enum class RetireOutcome : std::uint8_t {
  Retired = 0,
  AlreadyRetired = 1,
  StaleEpoch = 2,
  UnknownParticipant = 3,
  MalformedRequest = 4,
  ConflictingAttempt = 5,
};

[[nodiscard]] std::string_view to_string(RetireOutcome outcome) noexcept;

struct RetireResult {
  RetireOutcome outcome = RetireOutcome::MalformedRequest;
  CoordinatorEpoch epoch;
  std::size_t incarnations_fenced = 0;
  Explanation explanation;
};

struct RegistrationResult {
  RegistrationOutcome outcome = RegistrationOutcome::MalformedRequest;
  CoordinatorEpoch epoch;
  CoordinatorBootId coordinator_boot;
  std::optional<RegistrationGeneration> registration_generation;
  std::optional<IncarnationSequence> incarnation;
  bool replayed = false;
  Explanation explanation;
};

}  // namespace fabric_epoch
