// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fabric_epoch/explanation.hpp"
#include "fabric_epoch/fence.hpp"
#include "fabric_epoch/identity.hpp"
#include "fabric_epoch/scope.hpp"

namespace fabric_epoch {

/// Explicit currentness states. Invalid authority is never collapsed into a
/// single false: downstream runtimes need the exact reason.
enum class AuthorityCurrentness : std::uint8_t {
  Current = 0,
  RevalidationRequired = 1,
  Fenced = 2,
  StaleEpoch = 3,
  Retired = 4,
  Unknown = 5,
  Revoked = 6,
};

[[nodiscard]] std::string_view to_string(AuthorityCurrentness state) noexcept;

/// Result codes of the cheap deterministic validation primitive.
enum class AuthorityValidationCode : std::uint8_t {
  Current = 0,
  StaleEpoch = 1,
  StaleWorkerBoot = 2,
  UnknownParticipant = 3,
  UnauthorizedScope = 4,
  RevalidationRequired = 5,
  Malformed = 6,
  Retired = 7,
  /// Presentation named a specific authority grant that does not exist.
  UnknownGrant = 8,
  /// The authority grant exists and has been explicitly revoked.
  Revoked = 9,
};

[[nodiscard]] std::string_view to_string(AuthorityValidationCode code) noexcept;

/// The full authority token a participant presents downstream. A token is not
/// a bearer credential: it is a claim that is re-checked against current state.
struct AuthorityToken {
  CoordinatorEpoch epoch;
  ParticipantId participant;
  WorkerBootId boot;
  IncarnationSequence incarnation;
  RegistrationGeneration registration_generation;
};

struct ValidateAuthorityRequest {
  AuthorityToken token;
  AuthorityScope scope;
  /// Optional expected registration generation. A mismatch reports
  /// REVALIDATION_REQUIRED. Excluded from replay identity (see runtime docs).
  std::optional<RegistrationGeneration> expected_generation;
  /// Optional specific grant to validate instead of the registration binding.
  std::optional<AuthorityGrantId> grant;
};

struct AuthorityValidation {
  AuthorityValidationCode code = AuthorityValidationCode::Malformed;
  AuthorityCurrentness currentness = AuthorityCurrentness::Unknown;
  CoordinatorEpoch epoch;
  CoordinatorBootId coordinator_boot;
  std::optional<RegistrationGeneration> registration_generation;
  std::optional<IncarnationSequence> incarnation;
  std::optional<CoordinatorEpoch> replaced_by_epoch;
  std::optional<WorkerBootId> superseded_by;
  Explanation explanation;

  [[nodiscard]] bool authorized() const noexcept { return code == AuthorityValidationCode::Current; }
  [[nodiscard]] std::string render() const;
};

/// Explicit authority grant. A grant is invalid if its epoch is not current,
/// if its boot is fenced, or if it has been revoked.
struct AuthorityGrant {
  AuthorityGrantId id;
  CoordinatorEpoch epoch;
  ParticipantId participant;
  WorkerBootId boot;
  IncarnationSequence incarnation;
  ScopeSet scopes;
  GrantGeneration generation;
  std::optional<AuthorityGrantId> parent;
  std::optional<WorkerBootId> delegator_boot;
  bool revoked = false;
  CoordinatorEpoch revoked_at_epoch;
  BoundedText provenance;
};

struct GrantAuthorityRequest {
  MutationAttemptId attempt;
  CoordinatorEpoch expected_epoch;
  ParticipantId participant;
  WorkerBootId boot;
  IncarnationSequence incarnation;
  ScopeSet scopes;
  std::optional<AuthorityGrantId> parent;
  BoundedText provenance;
};

enum class GrantOutcome : std::uint8_t {
  Granted = 0,
  StaleEpoch = 1,
  StaleWorkerBoot = 2,
  UnknownParticipant = 3,
  UnauthorizedScope = 4,
  DelegationRejected = 5,
  MalformedRequest = 6,
  ResourceLimit = 7,
  ConflictingAttempt = 8,
};

[[nodiscard]] std::string_view to_string(GrantOutcome outcome) noexcept;

struct GrantResult {
  GrantOutcome outcome = GrantOutcome::MalformedRequest;
  CoordinatorEpoch epoch;
  std::optional<AuthorityGrantId> grant;
  std::optional<GrantGeneration> generation;
  bool replayed = false;
  Explanation explanation;
};

struct RevokeAuthorityRequest {
  MutationAttemptId attempt;
  CoordinatorEpoch expected_epoch;
  AuthorityGrantId grant;
  FenceReason reason = FenceReason::AdministrativeRevoke;
  BoundedText note;
};

enum class RevokeOutcome : std::uint8_t {
  Revoked = 0,
  AlreadyRevoked = 1,
  StaleEpoch = 2,
  UnknownGrant = 3,
  MalformedRequest = 4,
  ConflictingAttempt = 5,
};

[[nodiscard]] std::string_view to_string(RevokeOutcome outcome) noexcept;

struct RevokeResult {
  RevokeOutcome outcome = RevokeOutcome::MalformedRequest;
  CoordinatorEpoch epoch;
  std::optional<GrantGeneration> generation;
  std::size_t derived_grants_revoked = 0;
  Explanation explanation;
};

}  // namespace fabric_epoch
