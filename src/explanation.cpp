// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/explanation.hpp"

#include <algorithm>

namespace fabric_epoch {

std::string_view to_string(ExplanationCode code) noexcept {
  switch (code) {
    case ExplanationCode::AuthorityCurrent:
      return "AUTHORITY_CURRENT";
    case ExplanationCode::RegistrationAccepted:
      return "REGISTRATION_ACCEPTED";
    case ExplanationCode::RegistrationIdempotentReplay:
      return "REGISTRATION_IDEMPOTENT_REPLAY";
    case ExplanationCode::EpochStale:
      return "EPOCH_STALE";
    case ExplanationCode::WorkerBootFenced:
      return "WORKER_BOOT_FENCED";
    case ExplanationCode::WorkerBootSuperseded:
      return "WORKER_BOOT_SUPERSEDED";
    case ExplanationCode::ParticipantUnknown:
      return "PARTICIPANT_UNKNOWN";
    case ExplanationCode::ParticipantRetired:
      return "PARTICIPANT_RETIRED";
    case ExplanationCode::ScopeMissing:
      return "SCOPE_MISSING";
    case ExplanationCode::RevalidationRequired:
      return "REVALIDATION_REQUIRED";
    case ExplanationCode::MalformedRequest:
      return "MALFORMED_REQUEST";
    case ExplanationCode::ResourceLimitReached:
      return "RESOURCE_LIMIT_REACHED";
    case ExplanationCode::IncarnationConflict:
      return "INCARNATION_CONFLICT";
    case ExplanationCode::AttemptConflict:
      return "ATTEMPT_CONFLICT";
    case ExplanationCode::GrantRevoked:
      return "GRANT_REVOKED";
    case ExplanationCode::GrantSuperseded:
      return "GRANT_SUPERSEDED";
    case ExplanationCode::EpochAdvanced:
      return "EPOCH_ADVANCED";
    case ExplanationCode::FenceRecorded:
      return "FENCE_RECORDED";
    case ExplanationCode::FenceIdempotent:
      return "FENCE_IDEMPOTENT";
    case ExplanationCode::SessionClosed:
      return "SESSION_CLOSED";
    case ExplanationCode::DelegationRejected:
      return "DELEGATION_REJECTED";
    case ExplanationCode::GenerationMismatch:
      return "GENERATION_MISMATCH";
  }
  return "UNKNOWN";
}

Explanation::Explanation(ExplanationCode code) : code_(code) {}

Explanation& Explanation::with(std::string key, std::string value) {
  details_.emplace_back(std::move(key), std::move(value));
  std::sort(details_.begin(), details_.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
  return *this;
}

Explanation& Explanation::with_code(ExplanationCode code) {
  code_ = code;
  return *this;
}

std::string Explanation::summary() const { return std::string(to_string(code_)); }

std::string Explanation::render() const {
  std::string out;
  out.append(to_string(code_));
  if (!details_.empty()) {
    out.push_back(' ');
    out.push_back('(');
    for (std::size_t i = 0; i < details_.size(); ++i) {
      if (i != 0) {
        out.append(", ");
      }
      out.append(details_[i].first);
      out.push_back('=');
      out.append(details_[i].second);
    }
    out.push_back(')');
  }
  return out;
}

}  // namespace fabric_epoch
