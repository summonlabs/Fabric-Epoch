// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/epoch.hpp"

namespace fabric_epoch {

std::string_view to_string(EpochTransitionCause cause) noexcept {
  switch (cause) {
    case EpochTransitionCause::CoordinatorStart:
      return "COORDINATOR_START";
    case EpochTransitionCause::CoordinatorRestart:
      return "COORDINATOR_RESTART";
    case EpochTransitionCause::ExplicitOperatorAdvance:
      return "EXPLICIT_OPERATOR_ADVANCE";
    case EpochTransitionCause::DurableRecovery:
      return "DURABLE_RECOVERY";
  }
  return "UNKNOWN";
}

std::optional<EpochTransitionCause> epoch_transition_cause_from_wire(std::uint8_t value) noexcept {
  switch (value) {
    case 0:
      return EpochTransitionCause::CoordinatorStart;
    case 1:
      return EpochTransitionCause::CoordinatorRestart;
    case 2:
      return EpochTransitionCause::ExplicitOperatorAdvance;
    case 3:
      return EpochTransitionCause::DurableRecovery;
    default:
      return std::nullopt;
  }
}

std::string_view to_string(EpochAdvanceOutcome outcome) noexcept {
  switch (outcome) {
    case EpochAdvanceOutcome::Advanced:
      return "ADVANCED";
    case EpochAdvanceOutcome::StaleExpectedEpoch:
      return "STALE_EXPECTED_EPOCH";
    case EpochAdvanceOutcome::EpochExhausted:
      return "EPOCH_EXHAUSTED";
    case EpochAdvanceOutcome::MalformedRequest:
      return "MALFORMED_REQUEST";
    case EpochAdvanceOutcome::ConflictingAttempt:
      return "CONFLICTING_ATTEMPT";
  }
  return "UNKNOWN";
}

std::string EpochState::render() const {
  std::string out;
  out.append("epoch=");
  out.append(epoch.to_string());
  out.append(" coordinator_boot=");
  out.append(coordinator_boot.to_string());
  out.append(" generation=");
  out.append(generation.to_string());
  out.append(" registration_floor=");
  out.append(registration_floor.to_string());
  out.append(" fence_floor=");
  out.append(fence_floor.to_string());
  out.append(" participants=");
  out.append(std::to_string(participant_count));
  out.append(" active_workers=");
  out.append(std::to_string(active_worker_count));
  out.append(" retained_fences=");
  out.append(std::to_string(retained_fence_count));
  out.append(" live_grants=");
  out.append(std::to_string(live_grant_count));
  out.append(" transitions=");
  out.append(std::to_string(transition_history_count));
  return out;
}

}  // namespace fabric_epoch
