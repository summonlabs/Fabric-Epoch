// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

#include "fabric_epoch/authority.hpp"
#include "fabric_epoch/durable.hpp"
#include "fabric_epoch/epoch.hpp"
#include "fabric_epoch/fence.hpp"
#include "fabric_epoch/identity.hpp"
#include "fabric_epoch/limits.hpp"
#include "fabric_epoch/participant.hpp"
#include "fabric_epoch/snapshot.hpp"
#include "fabric_epoch/store.hpp"

namespace fabric_epoch {

/// What happens when a new incarnation registers while a previous incarnation
/// of a single-incarnation participant is still live.
enum class IncarnationReplacementPolicy : std::uint8_t {
  /// The incumbent still holds live authority; the new registration is
  /// rejected with CONFLICTING_INCARNATION until the incumbent is fenced.
  RequireExplicitFence = 0,
  /// The incumbent is fenced atomically with the acceptance of the new
  /// incarnation (fence reason REINCARNATION).
  ReplaceIncumbent = 1,
};

[[nodiscard]] std::string_view to_string(IncarnationReplacementPolicy policy) noexcept;

/// Why a session ended. Drives the fence reason recorded when session loss
/// implies incarnation loss.
enum class SessionClosureReason : std::uint8_t {
  PeerDisconnect = 0,
  ProtocolViolation = 1,
  ServerShutdown = 2,
  ExplicitClose = 3,
  RegistrationReplaced = 4,
};

[[nodiscard]] std::string_view to_string(SessionClosureReason reason) noexcept;

struct EpochRuntimeOptions {
  std::filesystem::path store_path;
  Limits limits;
  BoundedText provenance;

  IncarnationReplacementPolicy replacement_policy = IncarnationReplacementPolicy::RequireExplicitFence;
  bool allow_multi_incarnation = false;

  /// Scope kinds that a participant may bind to itself at registration time.
  /// Anything else is rejected with UNAUTHORIZED_SCOPE. Wildcard global
  /// authority is not representable.
  std::array<bool, 6> registrable_scope_kinds = {true, true, true, true, true, true};

  /// When true (default) the runtime implements the documented model
  /// "one session per worker incarnation": losing that session fences the
  /// incarnation. When false, session loss leaves the incarnation live and the
  /// runtime operates a durable-incarnation model instead.
  bool fence_on_session_loss = true;

  /// When true the runtime establishes a fresh coordinator incarnation on
  /// construction and advances the durable epoch before serving any request.
  /// The recorded transition cause is derived: a store that already has an
  /// established epoch records COORDINATOR_RESTART, a pristine store records
  /// COORDINATOR_START.
  bool advance_epoch_on_open = true;

  /// Testing and recovery hook. When absent the runtime generates a fresh
  /// CoordinatorBootId from the operating-system CSPRNG.
  std::optional<CoordinatorBootId> coordinator_boot;
};

/// Session binding held by a transport. Not authority by itself: it records
/// which incarnation a live session currently represents.
struct SessionBinding {
  SessionId id;
  ParticipantId participant;
  WorkerBootId boot;
  IncarnationSequence incarnation;
  CoordinatorEpoch epoch;
  RegistrationGeneration registration_generation;
};

/// The Fabric Epoch authority engine.
///
/// Thread safety: all public methods are safe to call concurrently from many
/// threads. Read paths take a shared lock; mutating paths take an exclusive
/// lock. No callback, event or user code is ever invoked while a lock is held.
/// Snapshots are value types copied out under the shared lock.
///
/// Error model: authority decisions are returned as structured outcome values
/// (never exceptions). Unrecoverable infrastructure failures -- unusable
/// persistence, failed durability barriers, corruption, exhausted limits --
/// throw EpochError.
class EpochRuntime {
 public:
  explicit EpochRuntime(EpochRuntimeOptions options);
  ~EpochRuntime();

  EpochRuntime(const EpochRuntime&) = delete;
  EpochRuntime& operator=(const EpochRuntime&) = delete;
  EpochRuntime(EpochRuntime&&) = delete;
  EpochRuntime& operator=(EpochRuntime&&) = delete;

  // ---- observation -------------------------------------------------------

  [[nodiscard]] EpochState state() const;
  [[nodiscard]] CoordinatorEpoch current_epoch() const;
  [[nodiscard]] CoordinatorBootId coordinator_boot() const;
  [[nodiscard]] EpochGeneration generation() const;
  [[nodiscard]] const Limits& limits() const noexcept;

  [[nodiscard]] std::optional<ParticipantView> query_participant(const ParticipantId& id) const;
  [[nodiscard]] std::vector<ParticipantView> list_participants() const;
  [[nodiscard]] std::vector<FenceRecord> list_fences() const;
  [[nodiscard]] std::vector<GrantView> list_grants() const;
  [[nodiscard]] std::vector<EpochTransitionRecord> transition_history(std::size_t max_records) const;
  [[nodiscard]] AuthoritySnapshot snapshot() const;
  [[nodiscard]] FenceStatus query_fence(const ParticipantId& participant, const WorkerBootId& boot) const;
  [[nodiscard]] Explanation explain_authority(const ParticipantId& participant,
                                              const WorkerBootId& boot) const;
  [[nodiscard]] Explanation explain_epoch() const;

  // ---- mutation ----------------------------------------------------------

  [[nodiscard]] EpochAdvanceResult advance_epoch(const EpochAdvanceRequest& request);
  [[nodiscard]] RegistrationResult register_participant(const RegisterParticipantRequest& request);

  /// Applies a sequence of registrations and commits the whole batch durably
  /// once. Every request is evaluated with exactly the same semantics as
  /// register_participant; the returned vector reports one outcome per request.
  /// Nothing is returned until the batch is durable as a whole, so a
  /// REGISTERED outcome in a batch is as durable as a single registration.
  /// Unrecoverable persistence failures throw and leave in-memory state
  /// unchanged.
  [[nodiscard]] std::vector<RegistrationResult> register_participants(
      std::span<const RegisterParticipantRequest> requests);
  [[nodiscard]] GrantResult grant_authority(const GrantAuthorityRequest& request);
  [[nodiscard]] RevokeResult revoke_authority(const RevokeAuthorityRequest& request);
  [[nodiscard]] FenceResult fence_worker(const FenceWorkerRequest& request);

  /// Retires a participant's durable identity: every live incarnation is
  /// fenced, the incarnation allocator is closed, and the durable record is
  /// retained in a retired state so that no historical incarnation can ever be
  /// revived. Retirement is intentionally not deletion.
  [[nodiscard]] RetireResult retire_participant(const RetireParticipantRequest& request);

  // ---- cheap deterministic validation primitive --------------------------

  [[nodiscard]] AuthorityValidation validate_authority(const ValidateAuthorityRequest& request) const;

  // ---- session lifecycle -------------------------------------------------

  /// Binds a session to a worker incarnation. Returns false when the
  /// incarnation is not current (the session must then be refused).
  [[nodiscard]] bool bind_session(const SessionBinding& binding);

  /// Records that a session ended. Implements the documented model
  /// "one session per worker incarnation" when fence_on_session_loss is set.
  [[nodiscard]] FenceResult close_session(const SessionId& session, SessionClosureReason reason);

  [[nodiscard]] std::vector<SessionBinding> active_sessions() const;
  [[nodiscard]] std::size_t session_count() const;

  /// Forces the durable image to disk. Returns only after durability.
  void flush();

  /// Rebuilds every secondary index from the durable records and reports
  /// whether the live indexes matched. Used by property tests to prove that no
  /// operation leaves an index inconsistent with its records.
  [[nodiscard]] bool check_indexes() const;

  [[nodiscard]] const std::filesystem::path& store_path() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fabric_epoch
