// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/participant.hpp"

namespace fabric_epoch {

std::string_view to_string(IncarnationPolicy policy) noexcept {
  switch (policy) {
    case IncarnationPolicy::SingleIncarnation:
      return "SINGLE_INCARNATION";
    case IncarnationPolicy::MultiIncarnation:
      return "MULTI_INCARNATION";
  }
  return "UNKNOWN";
}

std::optional<IncarnationPolicy> incarnation_policy_from_wire(std::uint8_t value) noexcept {
  switch (value) {
    case 0:
      return IncarnationPolicy::SingleIncarnation;
    case 1:
      return IncarnationPolicy::MultiIncarnation;
    default:
      return std::nullopt;
  }
}

std::string_view to_string(RegistrationOutcome outcome) noexcept {
  switch (outcome) {
    case RegistrationOutcome::Registered:
      return "REGISTERED";
    case RegistrationOutcome::Idempotent:
      return "IDEMPOTENT";
    case RegistrationOutcome::StaleEpoch:
      return "STALE_EPOCH";
    case RegistrationOutcome::StaleWorkerBoot:
      return "STALE_WORKER_BOOT";
    case RegistrationOutcome::AlreadyActive:
      return "ALREADY_ACTIVE";
    case RegistrationOutcome::ConflictingIncarnation:
      return "CONFLICTING_INCARNATION";
    case RegistrationOutcome::UnauthorizedScope:
      return "UNAUTHORIZED_SCOPE";
    case RegistrationOutcome::MalformedRequest:
      return "MALFORMED_REQUEST";
    case RegistrationOutcome::ResourceLimit:
      return "RESOURCE_LIMIT";
    case RegistrationOutcome::RetiredParticipant:
      return "RETIRED_PARTICIPANT";
    case RegistrationOutcome::ConflictingAttempt:
      return "CONFLICTING_ATTEMPT";
    case RegistrationOutcome::RevalidationRequired:
      return "REVALIDATION_REQUIRED";
  }
  return "UNKNOWN";
}

std::string_view to_string(RetireOutcome outcome) noexcept {
  switch (outcome) {
    case RetireOutcome::Retired:
      return "RETIRED";
    case RetireOutcome::AlreadyRetired:
      return "ALREADY_RETIRED";
    case RetireOutcome::StaleEpoch:
      return "STALE_EPOCH";
    case RetireOutcome::UnknownParticipant:
      return "UNKNOWN_PARTICIPANT";
    case RetireOutcome::MalformedRequest:
      return "MALFORMED_REQUEST";
    case RetireOutcome::ConflictingAttempt:
      return "CONFLICTING_ATTEMPT";
  }
  return "UNKNOWN";
}

std::string ParticipantView::render() const {
  std::string out;
  out.append("participant=");
  out.append(id.to_string());
  out.append(" registration_generation=");
  out.append(registration_generation.to_string());
  out.append(" fence_generation=");
  out.append(fence_generation.to_string());
  out.append(" incarnation_floor=");
  out.append(incarnation_floor.to_string());
  out.append(" policy=");
  out.append(fabric_epoch::to_string(policy));
  out.append(" retired=");
  out.append(retired ? "true" : "false");
  out.append(" revalidation_required=");
  out.append(revalidation_required ? "true" : "false");
  out.append(" active_incarnations=");
  out.append(std::to_string(active_incarnation_count()));
  if (has_active_incarnation) {
    out.append(" active_boot=");
    out.append(active_boot.to_string());
    out.append(" active_incarnation=");
    out.append(active_incarnation.to_string());
  }
  out.append(" scopes=");
  out.append(scopes.to_string());
  out.append(" accepted_registrations=");
  out.append(std::to_string(accepted_registrations));
  out.append(" replay_hits=");
  out.append(std::to_string(replay_hits));
  return out;
}

}  // namespace fabric_epoch
