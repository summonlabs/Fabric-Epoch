// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <cstddef>

namespace fabric_epoch {

/// Every field of this structure is consulted by exactly one named code path.
/// see tests/test_limits.cpp which exercises each field against its path.
struct Limits {
  /// Maximum bytes of one protocol frame payload. Enforced by the frame codec
  /// before any allocation proportional to the declared length occurs.
  std::size_t max_frame_payload = 64u * 1024u;

  /// Maximum number of durable participant records.
  std::size_t max_participants = 200000;

  /// Maximum number of simultaneously active worker incarnations.
  std::size_t max_active_workers = 200000;

  /// Maximum number of retained descriptive fence records. Retention pruning
  /// never resurrects authority because fencing floors are separate counters.
  std::size_t max_recent_fences = 8192;

  /// Maximum number of live authority grants.
  std::size_t max_authority_grants = 200000;

  /// Maximum number of scopes carried by one registration or one grant.
  std::size_t max_scopes_per_grant = 32;

  /// Maximum number of retained epoch transition history records.
  std::size_t max_transition_history = 1024;

  /// Maximum encoded length of an application request payload. Enforced by the
  /// frame decoder for every request message type, separately from the
  /// transport-level max_frame_payload bound that applies to all frames.
  std::size_t max_request_size = 16u * 1024u;

  /// Maximum length of any free-form string (provenance, explanation detail,
  /// human fence note). Longer input is rejected, never truncated silently.
  std::size_t max_string_size = 256;

  /// Maximum number of concurrent sessions accepted by a coordinator server.
  std::size_t max_sessions = 1024;

  /// Maximum number of idempotency records retained per participant.
  std::size_t max_idempotency_records_per_participant = 64;

  /// Maximum number of fenced WorkerBootId literals retained per participant
  /// for explanation purposes (semantic staleness uses the fencing floor).
  std::size_t max_fenced_boots_per_participant = 8;

  /// Maximum number of accepted TCP connections awaiting service.
  std::size_t listen_backlog = 128;
};

/// Process-wide default limits.
[[nodiscard]] const Limits& default_limits() noexcept;

/// Rejects configurations whose individual bounds are zero or inconsistent.
[[nodiscard]] bool is_viable(const Limits& limits) noexcept;

}  // namespace fabric_epoch
