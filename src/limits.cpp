// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/limits.hpp"

namespace fabric_epoch {

const Limits& default_limits() noexcept {
  static const Limits limits{};
  return limits;
}

bool is_viable(const Limits& limits) noexcept {
  if (limits.max_frame_payload == 0 || limits.max_participants == 0 || limits.max_active_workers == 0) {
    return false;
  }
  if (limits.max_recent_fences == 0 || limits.max_authority_grants == 0 || limits.max_scopes_per_grant == 0) {
    return false;
  }
  if (limits.max_transition_history == 0 || limits.max_request_size == 0 || limits.max_string_size == 0) {
    return false;
  }
  if (limits.max_sessions == 0) {
    return false;
  }
  if (limits.max_idempotency_records_per_participant == 0 || limits.max_fenced_boots_per_participant == 0) {
    return false;
  }
  if (limits.listen_backlog == 0) {
    return false;
  }
  if (limits.max_request_size > limits.max_frame_payload) {
    return false;
  }
  if (limits.max_active_workers > limits.max_participants) {
    // Not fatal, but the active-worker bound can never be reached before the
    // participant bound; keeping it bounded by participants is the intended
    // invariant.
    return false;
  }
  return true;
}

}  // namespace fabric_epoch
