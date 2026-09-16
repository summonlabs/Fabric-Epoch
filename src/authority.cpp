// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/authority.hpp"

namespace fabric_epoch {

std::string_view to_string(AuthorityCurrentness state) noexcept {
  switch (state) {
    case AuthorityCurrentness::Current:
      return "CURRENT";
    case AuthorityCurrentness::RevalidationRequired:
      return "REVALIDATION_REQUIRED";
    case AuthorityCurrentness::Fenced:
      return "FENCED";
    case AuthorityCurrentness::StaleEpoch:
      return "STALE_EPOCH";
    case AuthorityCurrentness::Retired:
      return "RETIRED";
    case AuthorityCurrentness::Unknown:
      return "UNKNOWN";
    case AuthorityCurrentness::Revoked:
      return "REVOKED";
  }
  return "UNKNOWN";
}

std::string_view to_string(AuthorityValidationCode code) noexcept {
  switch (code) {
    case AuthorityValidationCode::Current:
      return "CURRENT";
    case AuthorityValidationCode::StaleEpoch:
      return "STALE_EPOCH";
    case AuthorityValidationCode::StaleWorkerBoot:
      return "STALE_WORKER_BOOT";
    case AuthorityValidationCode::UnknownParticipant:
      return "UNKNOWN_PARTICIPANT";
    case AuthorityValidationCode::UnauthorizedScope:
      return "UNAUTHORIZED_SCOPE";
    case AuthorityValidationCode::RevalidationRequired:
      return "REVALIDATION_REQUIRED";
    case AuthorityValidationCode::Malformed:
      return "MALFORMED";
    case AuthorityValidationCode::Retired:
      return "RETIRED";
    case AuthorityValidationCode::UnknownGrant:
      return "UNKNOWN_GRANT";
    case AuthorityValidationCode::Revoked:
      return "REVOKED";
  }
  return "UNKNOWN";
}

std::string AuthorityValidation::render() const {
  std::string out;
  out.append("code=");
  out.append(fabric_epoch::to_string(code));
  out.append(" currentness=");
  out.append(fabric_epoch::to_string(currentness));
  out.append(" epoch=");
  out.append(epoch.to_string());
  out.append(" coordinator_boot=");
  out.append(coordinator_boot.to_string());
  out.append(" registration_generation=");
  out.append(registration_generation.has_value() ? registration_generation->to_string() : std::string("NONE"));
  out.append(" incarnation=");
  out.append(incarnation.has_value() ? incarnation->to_string() : std::string("NONE"));
  out.append(" replaced_by_epoch=");
  out.append(replaced_by_epoch.has_value() ? replaced_by_epoch->to_string() : std::string("NONE"));
  out.append(" superseded_by=");
  out.append(superseded_by.has_value() ? superseded_by->to_string() : std::string("NONE"));
  return out;
}

std::string_view to_string(GrantOutcome outcome) noexcept {
  switch (outcome) {
    case GrantOutcome::Granted:
      return "GRANTED";
    case GrantOutcome::StaleEpoch:
      return "STALE_EPOCH";
    case GrantOutcome::StaleWorkerBoot:
      return "STALE_WORKER_BOOT";
    case GrantOutcome::UnknownParticipant:
      return "UNKNOWN_PARTICIPANT";
    case GrantOutcome::UnauthorizedScope:
      return "UNAUTHORIZED_SCOPE";
    case GrantOutcome::DelegationRejected:
      return "DELEGATION_REJECTED";
    case GrantOutcome::MalformedRequest:
      return "MALFORMED_REQUEST";
    case GrantOutcome::ResourceLimit:
      return "RESOURCE_LIMIT";
    case GrantOutcome::ConflictingAttempt:
      return "CONFLICTING_ATTEMPT";
  }
  return "UNKNOWN";
}

std::string_view to_string(RevokeOutcome outcome) noexcept {
  switch (outcome) {
    case RevokeOutcome::Revoked:
      return "REVOKED";
    case RevokeOutcome::AlreadyRevoked:
      return "ALREADY_REVOKED";
    case RevokeOutcome::StaleEpoch:
      return "STALE_EPOCH";
    case RevokeOutcome::UnknownGrant:
      return "UNKNOWN_GRANT";
    case RevokeOutcome::MalformedRequest:
      return "MALFORMED_REQUEST";
    case RevokeOutcome::ConflictingAttempt:
      return "CONFLICTING_ATTEMPT";
  }
  return "UNKNOWN";
}

}  // namespace fabric_epoch
