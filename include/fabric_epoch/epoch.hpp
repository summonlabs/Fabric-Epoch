// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "fabric_epoch/explanation.hpp"
#include "fabric_epoch/identity.hpp"

namespace fabric_epoch {

/// Why an epoch transition happened. Structured; never a bare string.
enum class EpochTransitionCause : std::uint8_t {
  CoordinatorStart = 0,
  CoordinatorRestart = 1,
  ExplicitOperatorAdvance = 2,
  DurableRecovery = 3,
};

[[nodiscard]] std::string_view to_string(EpochTransitionCause cause) noexcept;
[[nodiscard]] std::optional<EpochTransitionCause> epoch_transition_cause_from_wire(std::uint8_t value) noexcept;

/// One bounded diagnostic record describing an epoch transition. History is
/// diagnostic only: current authority always comes from current state.
struct EpochTransitionRecord {
  EpochTransitionId id;
  CoordinatorEpoch from_epoch;
  CoordinatorEpoch to_epoch;
  EpochTransitionCause cause = EpochTransitionCause::CoordinatorStart;
  CoordinatorBootId coordinator_boot;
  EpochGeneration generation;
  BoundedText provenance;
};

struct EpochAdvanceRequest {
  MutationAttemptId attempt;
  CoordinatorEpoch expected_epoch;
  EpochTransitionCause cause = EpochTransitionCause::ExplicitOperatorAdvance;
  BoundedText provenance;
};

enum class EpochAdvanceOutcome : std::uint8_t {
  Advanced = 0,
  StaleExpectedEpoch = 1,
  EpochExhausted = 2,
  MalformedRequest = 3,
  ConflictingAttempt = 4,
};

[[nodiscard]] std::string_view to_string(EpochAdvanceOutcome outcome) noexcept;

struct EpochAdvanceResult {
  EpochAdvanceOutcome outcome = EpochAdvanceOutcome::MalformedRequest;
  CoordinatorEpoch epoch;
  CoordinatorBootId coordinator_boot;
  std::optional<EpochTransitionId> transition;
  bool replayed = false;
  Explanation explanation;
};

/// Public view of current epoch state. Stable rendering for scripting.
struct EpochState {
  CoordinatorEpoch epoch;
  CoordinatorBootId coordinator_boot;
  EpochGeneration generation;
  RegistrationGeneration registration_floor;
  FencingGeneration fence_floor;
  std::size_t participant_count = 0;
  std::size_t active_worker_count = 0;
  std::size_t retained_fence_count = 0;
  std::size_t live_grant_count = 0;
  std::size_t transition_history_count = 0;

  [[nodiscard]] std::string render() const;
};

}  // namespace fabric_epoch
