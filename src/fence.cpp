// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/fence.hpp"

namespace fabric_epoch {

std::string_view to_string(FenceReason reason) noexcept {
  switch (reason) {
    case FenceReason::ProcessLoss:
      return "PROCESS_LOSS";
    case FenceReason::Reincarnation:
      return "REINCARNATION";
    case FenceReason::CoordinatorEpochAdvance:
      return "COORDINATOR_EPOCH_ADVANCE";
    case FenceReason::AdministrativeRevoke:
      return "ADMINISTRATIVE_REVOKE";
    case FenceReason::SessionProtocolViolation:
      return "SESSION_PROTOCOL_VIOLATION";
    case FenceReason::ExplicitOperatorFence:
      return "EXPLICIT_OPERATOR_FENCE";
    case FenceReason::AuthorityReplaced:
      return "AUTHORITY_REPLACED";
    case FenceReason::Shutdown:
      return "SHUTDOWN";
  }
  return "UNKNOWN";
}

std::optional<FenceReason> fence_reason_from_wire(std::uint8_t value) noexcept {
  switch (value) {
    case 0:
      return FenceReason::ProcessLoss;
    case 1:
      return FenceReason::Reincarnation;
    case 2:
      return FenceReason::CoordinatorEpochAdvance;
    case 3:
      return FenceReason::AdministrativeRevoke;
    case 4:
      return FenceReason::SessionProtocolViolation;
    case 5:
      return FenceReason::ExplicitOperatorFence;
    case 6:
      return FenceReason::AuthorityReplaced;
    case 7:
      return FenceReason::Shutdown;
    default:
      return std::nullopt;
  }
}

std::string_view to_string(FenceOutcome outcome) noexcept {
  switch (outcome) {
    case FenceOutcome::Fenced:
      return "FENCED";
    case FenceOutcome::AlreadyFenced:
      return "ALREADY_FENCED";
    case FenceOutcome::StaleEpoch:
      return "STALE_EPOCH";
    case FenceOutcome::UnknownParticipant:
      return "UNKNOWN_PARTICIPANT";
    case FenceOutcome::MalformedRequest:
      return "MALFORMED_REQUEST";
    case FenceOutcome::ConflictingAttempt:
      return "CONFLICTING_ATTEMPT";
    case FenceOutcome::ResourceLimit:
      return "RESOURCE_LIMIT";
    case FenceOutcome::NoFenceRequired:
      return "NO_FENCE_REQUIRED";
  }
  return "UNKNOWN";
}

std::string FenceStatus::render() const {
  std::string out;
  out.append("boot=");
  out.append(boot.to_string());
  out.append(" participant=");
  out.append(participant.to_string());
  out.append(" fenced=");
  out.append(fenced ? "true" : "false");
  out.append(" retained_record=");
  out.append(retained_record ? "true" : "false");
  out.append(" reason=");
  out.append(reason.has_value() ? std::string(to_string(*reason)) : std::string("NONE"));
  out.append(" fenced_at_epoch=");
  out.append(fenced_at_epoch.has_value() ? fenced_at_epoch->to_string() : std::string("NONE"));
  out.append(" replaced_by_epoch=");
  out.append(replaced_by_epoch.has_value() ? replaced_by_epoch->to_string() : std::string("NONE"));
  out.append(" superseded_by=");
  out.append(superseded_by.has_value() ? superseded_by->to_string() : std::string("NONE"));
  out.append(" incarnation=");
  out.append(incarnation.has_value() ? incarnation->to_string() : std::string("NONE"));
  return out;
}

}  // namespace fabric_epoch
