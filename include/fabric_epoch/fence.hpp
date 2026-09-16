// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fabric_epoch/explanation.hpp"
#include "fabric_epoch/identity.hpp"

namespace fabric_epoch {

/// Structured fencing reasons. Human text may accompany a code.
enum class FenceReason : std::uint8_t {
  ProcessLoss = 0,
  Reincarnation = 1,
  CoordinatorEpochAdvance = 2,
  AdministrativeRevoke = 3,
  SessionProtocolViolation = 4,
  ExplicitOperatorFence = 5,
  AuthorityReplaced = 6,
  Shutdown = 7,
};

[[nodiscard]] std::string_view to_string(FenceReason reason) noexcept;
[[nodiscard]] std::optional<FenceReason> fence_reason_from_wire(std::uint8_t value) noexcept;

/// A retained, descriptive fence record. Retention is bounded; semantic
/// staleness is additionally guaranteed by monotonic fencing floors which are
/// never pruned.
struct FenceRecord {
  WorkerBootId boot;
  ParticipantId participant;
  FenceReason reason = FenceReason::ExplicitOperatorFence;
  FencingGeneration generation;
  CoordinatorEpoch epoch;
  IncarnationSequence incarnation = IncarnationSequence(0);
  BoundedText note;
};

struct FenceWorkerRequest {
  MutationAttemptId attempt;
  CoordinatorEpoch expected_epoch;
  ParticipantId participant;
  WorkerBootId boot;
  FenceReason reason = FenceReason::ExplicitOperatorFence;
  BoundedText note;
};

enum class FenceOutcome : std::uint8_t {
  Fenced = 0,
  AlreadyFenced = 1,
  StaleEpoch = 2,
  UnknownParticipant = 3,
  MalformedRequest = 4,
  ConflictingAttempt = 5,
  ResourceLimit = 6,
  /// No fence record was created because none is required: the session was
  /// already closed, or the runtime is configured not to fence on session loss.
  NoFenceRequired = 7,
};

[[nodiscard]] std::string_view to_string(FenceOutcome outcome) noexcept;

struct FenceResult {
  FenceOutcome outcome = FenceOutcome::MalformedRequest;
  CoordinatorEpoch epoch;
  std::optional<FencingGeneration> fence_generation;
  std::optional<IncarnationSequence> incarnation;
  Explanation explanation;
};

/// Structured answer to "why is this boot not current?".
struct FenceStatus {
  WorkerBootId boot;
  ParticipantId participant;
  bool fenced = false;
  bool retained_record = false;
  std::optional<FenceReason> reason;
  std::optional<CoordinatorEpoch> fenced_at_epoch;
  std::optional<CoordinatorEpoch> replaced_by_epoch;
  std::optional<WorkerBootId> superseded_by;
  std::optional<IncarnationSequence> incarnation;
  std::string render() const;
};

}  // namespace fabric_epoch
