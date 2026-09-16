// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "fabric_epoch/authority.hpp"
#include "fabric_epoch/digest.hpp"
#include "fabric_epoch/epoch.hpp"
#include "fabric_epoch/fence.hpp"
#include "fabric_epoch/identity.hpp"
#include "fabric_epoch/participant.hpp"

namespace fabric_epoch {

struct GrantView {
  AuthorityGrantId id;
  CoordinatorEpoch epoch;
  ParticipantId participant;
  WorkerBootId boot;
  GrantGeneration generation;
  bool revoked = false;
  bool delegated = false;
  ScopeSet scopes;
};

/// Immutable snapshot of authority state at one epoch generation.
struct AuthoritySnapshot {
  EpochSnapshotId id;
  CoordinatorEpoch epoch;
  CoordinatorBootId coordinator_boot;
  EpochGeneration generation;
  RegistrationGeneration registration_floor;
  FencingGeneration fence_floor;
  std::vector<ParticipantView> participants;
  std::vector<FenceRecord> fences;
  std::vector<GrantView> grants;
  SnapshotDigest digest;

  /// True when this snapshot describes the runtime's current epoch and
  /// generation. An old snapshot stays inspectable but cannot authorize
  /// mutation: every mutating entry point re-checks live state.
  [[nodiscard]] bool is_current_for(CoordinatorEpoch current_epoch,
                                    EpochGeneration current_generation) const noexcept {
    return epoch == current_epoch && generation == current_generation;
  }

  [[nodiscard]] std::string render() const;
};

}  // namespace fabric_epoch
