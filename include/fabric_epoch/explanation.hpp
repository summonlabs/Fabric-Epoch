// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fabric_epoch {

/// Structured reason codes behind every authority decision. Free-form text may
/// accompany a code but is never the sole representation.
enum class ExplanationCode : std::uint16_t {
  AuthorityCurrent = 1,
  RegistrationAccepted = 2,
  RegistrationIdempotentReplay = 3,
  EpochStale = 4,
  WorkerBootFenced = 5,
  WorkerBootSuperseded = 6,
  ParticipantUnknown = 7,
  ParticipantRetired = 8,
  ScopeMissing = 9,
  RevalidationRequired = 10,
  MalformedRequest = 11,
  ResourceLimitReached = 12,
  IncarnationConflict = 13,
  AttemptConflict = 14,
  GrantRevoked = 15,
  GrantSuperseded = 16,
  EpochAdvanced = 17,
  FenceRecorded = 18,
  FenceIdempotent = 19,
  SessionClosed = 20,
  DelegationRejected = 21,
  GenerationMismatch = 22,
};

[[nodiscard]] std::string_view to_string(ExplanationCode code) noexcept;

/// Deterministic, structured explanation. Rendering is stable for a given
/// (code, detail set) pair so operators and tests can diff it.
class Explanation {
 public:
  Explanation() = default;
  explicit Explanation(ExplanationCode code);

  Explanation& with(std::string key, std::string value);
  Explanation& with_code(ExplanationCode code);

  [[nodiscard]] ExplanationCode code() const noexcept { return code_; }
  [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& details() const noexcept {
    return details_;
  }
  [[nodiscard]] std::string summary() const;
  [[nodiscard]] std::string render() const;

  friend bool operator==(const Explanation&, const Explanation&) noexcept = default;

 private:
  ExplanationCode code_ = ExplanationCode::MalformedRequest;
  std::vector<std::pair<std::string, std::string>> details_;
};

}  // namespace fabric_epoch
