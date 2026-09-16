// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/runtime.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

#include "codec.hpp"
#include "fabric_epoch/detail/encoding.hpp"
#include "fabric_epoch/error.hpp"

namespace fabric_epoch {

std::string_view to_string(IncarnationReplacementPolicy policy) noexcept {
  switch (policy) {
    case IncarnationReplacementPolicy::RequireExplicitFence:
      return "REQUIRE_EXPLICIT_FENCE";
    case IncarnationReplacementPolicy::ReplaceIncumbent:
      return "REPLACE_INCUMBENT";
  }
  return "UNKNOWN";
}

std::string_view to_string(SessionClosureReason reason) noexcept {
  switch (reason) {
    case SessionClosureReason::PeerDisconnect:
      return "PEER_DISCONNECT";
    case SessionClosureReason::ProtocolViolation:
      return "PROTOCOL_VIOLATION";
    case SessionClosureReason::ServerShutdown:
      return "SERVER_SHUTDOWN";
    case SessionClosureReason::ExplicitClose:
      return "EXPLICIT_CLOSE";
    case SessionClosureReason::RegistrationReplaced:
      return "REGISTRATION_REPLACED";
  }
  return "UNKNOWN";
}

namespace {

using detail::ByteWriter;
using detail::encode_scope_set_canonical;

[[nodiscard]] SnapshotDigest semantic_digest(const RegisterParticipantRequest& request) {
  ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(OperationKind::RegisterParticipant));
  writer.string(request.participant.value());
  writer.fixed16(request.boot.bytes());
  writer.u8(static_cast<std::uint8_t>(request.policy));
  encode_scope_set_canonical(writer, request.scopes);
  return detail::compute_semantic_digest(writer.span());
}

[[nodiscard]] SnapshotDigest semantic_digest(const FenceWorkerRequest& request) {
  ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(OperationKind::FenceWorker));
  writer.string(request.participant.value());
  writer.fixed16(request.boot.bytes());
  writer.u8(static_cast<std::uint8_t>(request.reason));
  return detail::compute_semantic_digest(writer.span());
}

[[nodiscard]] SnapshotDigest semantic_digest(const GrantAuthorityRequest& request) {
  ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(OperationKind::GrantAuthority));
  writer.string(request.participant.value());
  writer.fixed16(request.boot.bytes());
  writer.u64(request.incarnation.value());
  encode_scope_set_canonical(writer, request.scopes);
  const std::array<std::byte, 16> parent =
      request.parent.has_value() ? request.parent->bytes() : std::array<std::byte, 16>{};
  writer.fixed16(parent);
  return detail::compute_semantic_digest(writer.span());
}

[[nodiscard]] SnapshotDigest semantic_digest(const RevokeAuthorityRequest& request) {
  ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(OperationKind::RevokeAuthority));
  writer.fixed16(request.grant.bytes());
  writer.u8(static_cast<std::uint8_t>(request.reason));
  return detail::compute_semantic_digest(writer.span());
}

[[nodiscard]] SnapshotDigest semantic_digest(const RetireParticipantRequest& request) {
  ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(OperationKind::RetireParticipant));
  writer.string(request.participant.value());
  return detail::compute_semantic_digest(writer.span());
}

[[nodiscard]] SnapshotDigest semantic_digest(const EpochAdvanceRequest& request) {
  ByteWriter writer;
  writer.u8(static_cast<std::uint8_t>(OperationKind::AdvanceEpoch));
  writer.u8(static_cast<std::uint8_t>(request.cause));
  return detail::compute_semantic_digest(writer.span());
}

/// Runs registered undo actions in reverse order unless committed, so a failed
/// durability barrier leaves in-memory state exactly as it was.
class UndoLog {
 public:
  UndoLog() = default;
  UndoLog(const UndoLog&) = delete;
  UndoLog& operator=(const UndoLog&) = delete;
  ~UndoLog() { rollback(); }

  void push(std::function<void()> action) { actions_.push_back(std::move(action)); }

  void rollback() {
    if (committed_) {
      return;
    }
    committed_ = true;
    for (auto it = actions_.rbegin(); it != actions_.rend(); ++it) {
      (*it)();
    }
    actions_.clear();
  }

  void commit() {
    actions_.clear();
    committed_ = true;
  }

 private:
  std::vector<std::function<void()>> actions_;
  bool committed_ = false;
};

[[nodiscard]] FenceReason fence_reason_for(SessionClosureReason reason) noexcept {
  switch (reason) {
    case SessionClosureReason::ProtocolViolation:
      return FenceReason::SessionProtocolViolation;
    case SessionClosureReason::ServerShutdown:
      return FenceReason::Shutdown;
    case SessionClosureReason::RegistrationReplaced:
      return FenceReason::AuthorityReplaced;
    case SessionClosureReason::ExplicitClose:
    case SessionClosureReason::PeerDisconnect:
      return FenceReason::ProcessLoss;
  }
  return FenceReason::ProcessLoss;
}

}  // namespace

struct EpochRuntime::Impl {
  EpochRuntimeOptions options;
  EpochStore store;
  mutable std::shared_mutex mutex;
  DurableState state;

  std::unordered_map<std::string, std::size_t> participant_index;
  std::unordered_map<std::string, std::size_t> boot_index;
  std::unordered_map<std::string, std::vector<std::size_t>> fence_index;
  std::unordered_map<std::string, std::size_t> grant_index;
  std::unordered_map<std::string, std::vector<std::size_t>> grant_children;
  std::unordered_map<std::string, SessionBinding> session_index;
  std::unordered_map<std::string, std::string> boot_session_index;
  std::size_t active_workers = 0;

  explicit Impl(EpochRuntimeOptions runtime_options)
      : options(std::move(runtime_options)),
        store(StoreOptions{options.store_path, options.limits, true, true}) {
    if (!is_viable(options.limits)) {
      throw invalid_argument("runtime limits are not viable");
    }
    if (options.store_path.empty()) {
      throw invalid_argument("runtime store path must not be empty");
    }

    state = store.load();
    const bool established = !state.current_epoch.is_zero();
    const CoordinatorBootId boot = options.coordinator_boot.has_value() ? *options.coordinator_boot
                                                                       : CoordinatorBootId::generate();
    options.coordinator_boot = boot;
    rebuild_indexes();

    if (options.advance_epoch_on_open) {
      const EpochTransitionCause cause =
          established ? EpochTransitionCause::CoordinatorRestart : EpochTransitionCause::CoordinatorStart;
      establish_epoch(boot, cause, established);
    } else if (!established) {
      throw invalid_argument("durable store has no established epoch and advance_epoch_on_open is false");
    }
  }

  // ---- indexes -----------------------------------------------------------

  void rebuild_indexes() {
    participant_index.clear();
    boot_index.clear();
    fence_index.clear();
    grant_index.clear();
    grant_children.clear();
    active_workers = 0;
    participant_index.reserve(state.participants.size());
    for (std::size_t i = 0; i < state.participants.size(); ++i) {
      const auto& participant = state.participants[i];
      participant_index[participant.id.value()] = i;
      if (participant.has_active_boot) {
        boot_index[participant.active_boot.to_string()] = i;
        ++active_workers;
      }
      for (const auto& incarnation : participant.extra_incarnations) {
        boot_index[incarnation.boot.to_string()] = i;
        ++active_workers;
      }
    }
    for (std::size_t i = 0; i < state.recent_fences.size(); ++i) {
      fence_index[state.recent_fences[i].boot.to_string()].push_back(i);
    }
    for (std::size_t i = 0; i < state.grants.size(); ++i) {
      grant_index[state.grants[i].id.to_string()] = i;
      if (state.grants[i].parent.has_value()) {
        grant_children[state.grants[i].parent->to_string()].push_back(i);
      }
    }
  }

  [[nodiscard]] bool indexes_match() const {
    std::unordered_map<std::string, std::size_t> expected_participants;
    std::unordered_map<std::string, std::size_t> expected_boots;
    std::unordered_map<std::string, std::vector<std::size_t>> expected_fences;
    std::unordered_map<std::string, std::size_t> expected_grants;
    std::unordered_map<std::string, std::vector<std::size_t>> expected_children;
    std::size_t expected_active = 0;
    for (std::size_t i = 0; i < state.participants.size(); ++i) {
      const auto& participant = state.participants[i];
      expected_participants[participant.id.value()] = i;
      if (participant.has_active_boot) {
        expected_boots[participant.active_boot.to_string()] = i;
        ++expected_active;
      }
      for (const auto& incarnation : participant.extra_incarnations) {
        expected_boots[incarnation.boot.to_string()] = i;
        ++expected_active;
      }
    }
    for (std::size_t i = 0; i < state.recent_fences.size(); ++i) {
      expected_fences[state.recent_fences[i].boot.to_string()].push_back(i);
    }
    for (std::size_t i = 0; i < state.grants.size(); ++i) {
      expected_grants[state.grants[i].id.to_string()] = i;
      if (state.grants[i].parent.has_value()) {
        expected_children[state.grants[i].parent->to_string()].push_back(i);
      }
    }
    return participant_index == expected_participants && boot_index == expected_boots &&
           fence_index == expected_fences && grant_index == expected_grants &&
           grant_children == expected_children && active_workers == expected_active;
  }

  /// Restores the canonical durable ordering: participants sorted by stable
  /// identifier. Applied before every persistence barrier so the durable image
  /// is canonical and independent of arrival order.
  void canonicalize() {
    // Fencing and revocation never reorder participants, so the ordering check
    // keeps the per-mutation cost linear instead of O(n log n) on a population
    // that is already canonical.
    if (std::is_sorted(state.participants.begin(), state.participants.end(),
                       [](const DurableParticipant& lhs, const DurableParticipant& rhs) {
                         return lhs.id < rhs.id;
                       })) {
      return;
    }
    std::sort(state.participants.begin(), state.participants.end(),
              [](const DurableParticipant& lhs, const DurableParticipant& rhs) { return lhs.id < rhs.id; });
  }

  [[nodiscard]] DurableParticipant* raw_find_participant(const ParticipantId& id) {
    for (auto& participant : state.participants) {
      if (participant.id == id) {
        return &participant;
      }
    }
    return nullptr;
  }

  [[nodiscard]] DurableParticipant* find_participant(const ParticipantId& id) {
    const auto it = participant_index.find(id.value());
    if (it == participant_index.end() || it->second >= state.participants.size()) {
      return nullptr;
    }
    return &state.participants[it->second];
  }

  [[nodiscard]] const DurableParticipant* find_participant(const ParticipantId& id) const {
    const auto it = participant_index.find(id.value());
    if (it == participant_index.end() || it->second >= state.participants.size()) {
      return nullptr;
    }
    return &state.participants[it->second];
  }

  struct FenceLookup {
    bool fenced = false;
    const FenceRecord* record = nullptr;
  };

  [[nodiscard]] FenceLookup lookup_fence(const ParticipantId& participant,
                                         const WorkerBootId& boot) const {
    FenceLookup lookup;
    const auto it = fence_index.find(boot.to_string());
    if (it != fence_index.end()) {
      for (const std::size_t index : it->second) {
        if (index >= state.recent_fences.size()) {
          continue;
        }
        const FenceRecord& record = state.recent_fences[index];
        if (record.participant == participant) {
          lookup.fenced = true;
          lookup.record = &record;
          break;
        }
      }
    }
    if (!lookup.fenced) {
      if (const DurableParticipant* owner = find_participant(participant); owner != nullptr) {
        for (const auto& retained : owner->fenced_boots) {
          if (retained == boot) {
            lookup.fenced = true;
            break;
          }
        }
      }
    }
    return lookup;
  }

  [[nodiscard]] static std::optional<IncarnationSequence> find_incarnation(
      const DurableParticipant& participant, const WorkerBootId& boot) {
    if (participant.has_active_boot && participant.active_boot == boot) {
      return participant.active_incarnation;
    }
    for (const auto& incarnation : participant.extra_incarnations) {
      if (incarnation.boot == boot) {
        return incarnation.sequence;
      }
    }
    return std::nullopt;
  }

  struct IncarnationEvaluation {
    AuthorityValidationCode code = AuthorityValidationCode::Malformed;
    AuthorityCurrentness currentness = AuthorityCurrentness::Unknown;
    std::optional<IncarnationSequence> incarnation;
    std::optional<WorkerBootId> superseded_by;
    std::optional<CoordinatorEpoch> replaced_by_epoch;
    std::optional<FenceReason> fence_reason;
  };

  [[nodiscard]] IncarnationEvaluation evaluate_incarnation(const DurableParticipant& participant,
                                                           const WorkerBootId& boot,
                                                           IncarnationSequence claimed) const {
    IncarnationEvaluation result;
    const auto current = find_incarnation(participant, boot);
    if (current.has_value()) {
      if (claimed.value() != 0 && claimed != *current) {
        result.code = AuthorityValidationCode::StaleWorkerBoot;
        result.currentness = AuthorityCurrentness::Fenced;
        result.incarnation = *current;
        return result;
      }
      result.code = AuthorityValidationCode::Current;
      result.currentness = AuthorityCurrentness::Current;
      result.incarnation = *current;
      return result;
    }

    const FenceLookup fence = lookup_fence(participant.id, boot);
    if (fence.fenced) {
      result.code = AuthorityValidationCode::StaleWorkerBoot;
      result.currentness = AuthorityCurrentness::Fenced;
      if (fence.record != nullptr) {
        result.fence_reason = fence.record->reason;
        if (!fence.record->incarnation.is_zero()) {
          result.incarnation = fence.record->incarnation;
        }
        if (fence.record->epoch.value() != 0) {
          result.replaced_by_epoch = fence.record->epoch;
        }
      }
      if (participant.has_active_boot) {
        result.superseded_by = participant.active_boot;
      } else if (!participant.extra_incarnations.empty()) {
        result.superseded_by = participant.extra_incarnations.front().boot;
      }
      return result;
    }

    if (claimed.value() != 0 && claimed <= participant.incarnation_floor) {
      result.code = AuthorityValidationCode::StaleWorkerBoot;
      result.currentness = AuthorityCurrentness::Fenced;
      return result;
    }

    const bool has_current = participant.has_active_boot || !participant.extra_incarnations.empty();
    if (!has_current) {
      if (participant.revalidation_required) {
        result.code = AuthorityValidationCode::RevalidationRequired;
        result.currentness = AuthorityCurrentness::RevalidationRequired;
      } else {
        result.code = AuthorityValidationCode::StaleWorkerBoot;
        result.currentness = AuthorityCurrentness::Fenced;
      }
      return result;
    }

    result.code = AuthorityValidationCode::StaleWorkerBoot;
    result.currentness = AuthorityCurrentness::Fenced;
    result.superseded_by = participant.has_active_boot ? participant.active_boot
                                                       : participant.extra_incarnations.front().boot;
    return result;
  }

  // ---- idempotency -------------------------------------------------------

  [[nodiscard]] static const IdempotencyRecord* find_attempt(
      const std::vector<IdempotencyRecord>& table, const MutationAttemptId& attempt) {
    for (const auto& record : table) {
      if (record.attempt == attempt) {
        return &record;
      }
    }
    return nullptr;
  }

  static void remember_attempt(std::vector<IdempotencyRecord>& table, const Limits& limits,
                               IdempotencyRecord record) {
    if (table.size() >= limits.max_idempotency_records_per_participant) {
      table.erase(table.begin());
    }
    table.push_back(std::move(record));
  }

  // ---- durability --------------------------------------------------------

  void bump_generation(UndoLog& undo) {
    const EpochGeneration previous = state.generation;
    undo.push([this, previous]() { state.generation = previous; });
    const auto next = state.generation.next();
    if (!next.has_value()) {
      throw overflow("durable state generation counter is exhausted");
    }
    state.generation = *next;
  }

  void persist(UndoLog& undo) {
    canonicalize();
    try {
      store.save(state);
    } catch (...) {
      undo.rollback();
      rebuild_indexes();
      throw;
    }
    rebuild_indexes();
    undo.commit();
  }

  // ---- epoch establishment ----------------------------------------------

  void establish_epoch(const CoordinatorBootId& boot, EpochTransitionCause cause,
                       bool invalidate_live_authority) {
    UndoLog undo;
    mutate_epoch(undo, boot, cause, invalidate_live_authority);
    persist(undo);
  }

  /// Applies a durable epoch advancement. The caller owns the durability
  /// barrier so that the new epoch and the record of the request that caused it
  /// become durable together.
  void mutate_epoch(UndoLog& undo, const CoordinatorBootId& boot, EpochTransitionCause cause,
                    bool invalidate_live_authority) {
    const CoordinatorEpoch previous_epoch = state.current_epoch;
    const CoordinatorBootId previous_boot = state.coordinator_boot;
    const EpochGeneration previous_generation = state.generation;
    std::vector<DurableParticipant> previous_participants = state.participants;
    const std::size_t previous_transition_count = state.transitions.size();

    const auto next = state.current_epoch.next();
    if (!next.has_value()) {
      throw overflow("CoordinatorEpoch cannot advance: the maximum representable epoch is in use");
    }

    undo.push([this, previous_epoch, previous_boot, previous_generation,
               previous_participants = std::move(previous_participants),
               previous_transition_count]() mutable {
      state.current_epoch = previous_epoch;
      state.coordinator_boot = previous_boot;
      state.generation = previous_generation;
      state.participants = std::move(previous_participants);
      state.transitions.resize(previous_transition_count);
    });

    state.current_epoch = *next;
    state.coordinator_boot = boot;
    const auto generation = state.generation.next();
    if (!generation.has_value()) {
      throw overflow("durable state generation counter is exhausted");
    }
    state.generation = *generation;

    if (invalidate_live_authority) {
      for (auto& participant : state.participants) {
        if (participant.has_active_boot || !participant.extra_incarnations.empty()) {
          participant.revalidation_required = true;
        }
        participant.has_active_boot = false;
        participant.active_boot = WorkerBootId{};
        participant.active_incarnation = IncarnationSequence(0);
        participant.extra_incarnations.clear();
      }
    }

    EpochTransitionRecord transition;
    transition.id = EpochTransitionId::generate();
    transition.from_epoch = previous_epoch;
    transition.to_epoch = state.current_epoch;
    transition.cause = cause;
    transition.coordinator_boot = boot;
    transition.generation = state.generation;
    transition.provenance = options.provenance;
    if (state.transitions.size() >= options.limits.max_transition_history) {
      state.transitions.erase(state.transitions.begin());
    }
    state.transitions.push_back(std::move(transition));
  }

  // ---- fencing -----------------------------------------------------------

  /// Records a fence for one worker boot. Idempotent: an already fenced boot
  /// produces AlreadyFenced without touching any generation.
  [[nodiscard]] FenceResult record_fence(const ParticipantId& participant_id,
                                         const WorkerBootId& boot_reference, FenceReason reason,
                                         const BoundedText& note, bool allow_defensive, UndoLog& undo,
                                         bool& mutated) {
    // Callers may pass a reference to the very incarnation this fence removes
    // (for example the active boot of the participant being reincarnated), so
    // the identifier is captured by value before any mutation happens.
    const WorkerBootId boot = boot_reference;
    FenceResult result;
    result.epoch = state.current_epoch;

    DurableParticipant* participant = find_participant(participant_id);
    if (participant == nullptr) {
      result.outcome = FenceOutcome::UnknownParticipant;
      result.explanation = Explanation(ExplanationCode::ParticipantUnknown)
                               .with("participant", participant_id.to_string());
      return result;
    }

    const FenceLookup existing = lookup_fence(participant_id, boot);
    if (existing.fenced) {
      result.outcome = FenceOutcome::AlreadyFenced;
      result.fence_generation = participant->fence_generation;
      if (existing.record != nullptr) {
        result.incarnation = existing.record->incarnation;
      }
      result.explanation = Explanation(ExplanationCode::FenceIdempotent)
                               .with("boot", boot.to_string())
                               .with("fence_generation", participant->fence_generation.to_string());
      return result;
    }

    const auto sequence = find_incarnation(*participant, boot);
    if (!sequence.has_value() && !allow_defensive) {
      result.outcome = FenceOutcome::AlreadyFenced;
      result.fence_generation = participant->fence_generation;
      result.explanation = Explanation(ExplanationCode::FenceIdempotent)
                               .with("boot", boot.to_string())
                               .with("reason", "the boot is not a current incarnation");
      return result;
    }

    if (!sequence.has_value() && state.recent_fences.size() >= options.limits.max_recent_fences) {
      // A defensive fence of a boot that was never an incarnation is a purely
      // descriptive record; refuse it when the retained history is full rather
      // than evicting a semantically required record.
      result.outcome = FenceOutcome::ResourceLimit;
      result.explanation = Explanation(ExplanationCode::ResourceLimitReached)
                               .with("limit", "max_recent_fences");
      return result;
    }

    if (!sequence.has_value()) {
      const auto generation = state.fence_floor.next();
      if (!generation.has_value()) {
        throw overflow("fencing generation counter is exhausted");
      }
      const FencingGeneration previous_participant_generation = participant->fence_generation;
      const FencingGeneration previous_floor = state.fence_floor;
      const std::size_t previous_fence_count = state.recent_fences.size();
      undo.push([this, previous_participant_generation, previous_floor, previous_fence_count,
                 participant_id]() {
        state.fence_floor = previous_floor;
        state.recent_fences.resize(previous_fence_count);
        if (DurableParticipant* target = raw_find_participant(participant_id); target != nullptr) {
          target->fence_generation = previous_participant_generation;
        }
      });
      state.fence_floor = *generation;
      participant->fence_generation = *generation;
      FenceRecord record;
      record.boot = boot;
      record.participant = participant_id;
      record.reason = reason;
      record.generation = *generation;
      record.epoch = state.current_epoch;
      record.incarnation = IncarnationSequence(0);
      record.note = note;
      state.recent_fences.push_back(record);
      bump_generation(undo);
      mutated = true;
      result.outcome = FenceOutcome::Fenced;
      result.fence_generation = *generation;
      result.explanation = Explanation(ExplanationCode::FenceRecorded)
                               .with("boot", boot.to_string())
                               .with("reason", std::string(to_string(reason)))
                               .with("kind", "preventive");
      return result;
    }

    const IncarnationSequence incarnation = *sequence;
    const FencingGeneration previous_participant_generation = participant->fence_generation;
    const FencingGeneration previous_floor = state.fence_floor;
    const IncarnationSequence previous_incarnation_floor = participant->incarnation_floor;
    const bool previous_had_active = participant->has_active_boot;
    const WorkerBootId previous_active_boot = participant->active_boot;
    const IncarnationSequence previous_active_incarnation = participant->active_incarnation;
    const std::vector<ActiveIncarnation> previous_extra = participant->extra_incarnations;
    const std::vector<WorkerBootId> previous_fenced_boots = participant->fenced_boots;
    const std::size_t previous_fence_count = state.recent_fences.size();
    const std::size_t previous_active_workers = active_workers;

    undo.push([this, participant_id, previous_participant_generation, previous_floor,
               previous_incarnation_floor, previous_had_active, previous_active_boot,
               previous_active_incarnation, previous_extra, previous_fenced_boots,
               previous_fence_count, previous_active_workers]() {
      state.fence_floor = previous_floor;
      state.recent_fences.resize(previous_fence_count);
      active_workers = previous_active_workers;
      if (DurableParticipant* target = raw_find_participant(participant_id); target != nullptr) {
        target->fence_generation = previous_participant_generation;
        target->incarnation_floor = previous_incarnation_floor;
        target->has_active_boot = previous_had_active;
        target->active_boot = previous_active_boot;
        target->active_incarnation = previous_active_incarnation;
        target->extra_incarnations = previous_extra;
        target->fenced_boots = previous_fenced_boots;
      }
    });

    // Remove the incarnation from the live set.
    if (participant->has_active_boot && participant->active_boot == boot) {
      if (!participant->extra_incarnations.empty()) {
        participant->active_boot = participant->extra_incarnations.front().boot;
        participant->active_incarnation = participant->extra_incarnations.front().sequence;
        participant->extra_incarnations.erase(participant->extra_incarnations.begin());
      } else {
        participant->has_active_boot = false;
        participant->active_boot = WorkerBootId{};
        participant->active_incarnation = IncarnationSequence(0);
      }
    } else {
      participant->extra_incarnations.erase(
          std::remove_if(participant->extra_incarnations.begin(), participant->extra_incarnations.end(),
                         [&boot](const ActiveIncarnation& incarnation) { return incarnation.boot == boot; }),
          participant->extra_incarnations.end());
    }
    active_workers -= 1;

    if (incarnation > participant->incarnation_floor) {
      participant->incarnation_floor = incarnation;
    }

    const auto generation = state.fence_floor.next();
    if (!generation.has_value()) {
      throw overflow("fencing generation counter is exhausted");
    }
    state.fence_floor = *generation;
    participant->fence_generation = *generation;

    if (participant->fenced_boots.size() >= options.limits.max_fenced_boots_per_participant) {
      // Bounded retention. Pruning never resurrects authority: the incarnation
      // allocator is monotonic, so the pruned boot's incarnation sequence is
      // never reissued, and the fencing floor below proves the pruned
      // incarnation can never be current again.
      participant->fenced_boots.erase(participant->fenced_boots.begin());
    }
    participant->fenced_boots.push_back(boot);

    FenceRecord record;
    record.boot = boot;
    record.participant = participant_id;
    record.reason = reason;
    record.generation = *generation;
    record.epoch = state.current_epoch;
    record.incarnation = incarnation;
    record.note = note;
    if (state.recent_fences.size() >= options.limits.max_recent_fences) {
      state.recent_fences.erase(state.recent_fences.begin());
    }
    state.recent_fences.push_back(std::move(record));

    bump_generation(undo);
    mutated = true;
    result.outcome = FenceOutcome::Fenced;
    result.fence_generation = *generation;
    result.incarnation = incarnation;
    result.explanation = Explanation(ExplanationCode::FenceRecorded)
                             .with("boot", boot.to_string())
                             .with("participant", participant_id.to_string())
                             .with("reason", std::string(to_string(reason)))
                             .with("incarnation", incarnation.to_string())
                             .with("epoch", state.current_epoch.to_string());
    return result;
  }

  // ---- registration ------------------------------------------------------

  [[nodiscard]] RegistrationResult apply_registration(const RegisterParticipantRequest& request,
                                                      UndoLog& undo, bool& mutated,
                                                      bool commit_generation = true) {
    RegistrationResult result;
    result.epoch = state.current_epoch;
    result.coordinator_boot = state.coordinator_boot;

    if (request.attempt.is_nil()) {
      result.outcome = RegistrationOutcome::MalformedRequest;
      result.explanation = Explanation(ExplanationCode::MalformedRequest)
                               .with("reason", "attempt identifier is nil");
      return result;
    }
    if (request.participant.empty()) {
      result.outcome = RegistrationOutcome::MalformedRequest;
      result.explanation = Explanation(ExplanationCode::MalformedRequest)
                               .with("reason", "participant identifier is empty");
      return result;
    }
    if (request.boot.is_nil()) {
      result.outcome = RegistrationOutcome::MalformedRequest;
      result.explanation = Explanation(ExplanationCode::MalformedRequest)
                               .with("reason", "worker boot identifier is nil");
      return result;
    }
    if (!BoundedText::is_valid(request.provenance.value(), options.limits.max_string_size)) {
      result.outcome = RegistrationOutcome::MalformedRequest;
      result.explanation = Explanation(ExplanationCode::MalformedRequest)
                               .with("reason", "provenance text exceeds the configured bound");
      return result;
    }
    if (request.scopes.empty()) {
      result.outcome = RegistrationOutcome::MalformedRequest;
      result.explanation = Explanation(ExplanationCode::MalformedRequest)
                               .with("reason", "registration binds no authority scope");
      return result;
    }
    if (request.scopes.size() > options.limits.max_scopes_per_grant) {
      result.outcome = RegistrationOutcome::ResourceLimit;
      result.explanation = Explanation(ExplanationCode::ResourceLimitReached)
                               .with("limit", "max_scopes_per_grant")
                               .with("requested", std::to_string(request.scopes.size()));
      return result;
    }
    if (request.policy == IncarnationPolicy::MultiIncarnation && !options.allow_multi_incarnation) {
      result.outcome = RegistrationOutcome::UnauthorizedScope;
      result.explanation = Explanation(ExplanationCode::ScopeMissing)
                               .with("reason", "multi-incarnation participation is not enabled")
                               .with("policy", std::string(to_string(request.policy)));
      return result;
    }
    for (const auto& scope : request.scopes.scopes()) {
      if (!options.registrable_scope_kinds[static_cast<std::size_t>(scope.kind())]) {
        result.outcome = RegistrationOutcome::UnauthorizedScope;
        result.explanation = Explanation(ExplanationCode::ScopeMissing)
                                 .with("scope", scope.to_string())
                                 .with("reason", "scope kind is not registrable by participants");
        return result;
      }
    }

    // Epoch currentness is established before replay classification so that a
    // stale request can never learn anything from the idempotency table.
    if (request.expected_epoch != state.current_epoch) {
      result.outcome = RegistrationOutcome::StaleEpoch;
      result.explanation = Explanation(ExplanationCode::EpochStale)
                               .with("request_epoch", request.expected_epoch.to_string())
                               .with("current_epoch", state.current_epoch.to_string());
      return result;
    }

    DurableParticipant* participant = find_participant(request.participant);
    if (participant != nullptr && participant->retired) {
      result.outcome = RegistrationOutcome::RetiredParticipant;
      result.explanation = Explanation(ExplanationCode::ParticipantRetired)
                               .with("participant", request.participant.to_string());
      return result;
    }

    if (participant != nullptr) {
      const FenceLookup fence = lookup_fence(request.participant, request.boot);
      if (fence.fenced) {
        result.outcome = RegistrationOutcome::StaleWorkerBoot;
        result.explanation = Explanation(ExplanationCode::WorkerBootFenced)
                                 .with("boot", request.boot.to_string())
                                 .with("fence_reason", fence.record != nullptr
                                                           ? std::string(to_string(fence.record->reason))
                                                           : std::string("RETAINED"));
        return result;
      }
    }

    const SnapshotDigest digest = semantic_digest(request);

    if (participant != nullptr) {
      if (const IdempotencyRecord* previous = find_attempt(participant->idempotency, request.attempt);
          previous != nullptr) {
        if (previous->semantic != digest) {
          result.outcome = RegistrationOutcome::ConflictingAttempt;
          result.explanation = Explanation(ExplanationCode::AttemptConflict)
                                   .with("attempt", request.attempt.to_string())
                                   .with("reason", "attempt identifier reused with a different payload");
          return result;
        }
        result.outcome = static_cast<RegistrationOutcome>(previous->outcome);
        result.registration_generation = RegistrationGeneration(previous->auxiliary);
        if (previous->auxiliary_high != 0) {
          result.incarnation = IncarnationSequence(previous->auxiliary_high);
        }
        result.replayed = true;
        result.explanation = Explanation(ExplanationCode::RegistrationIdempotentReplay)
                                 .with("attempt", request.attempt.to_string())
                                 .with("outcome", std::string(to_string(result.outcome)))
                                 .with("registration_generation", previous->auxiliary
                                                                      ? std::to_string(previous->auxiliary)
                                                                      : std::string("NONE"));
        return result;
      }
      if (request.expected_registration_generation.has_value() &&
          *request.expected_registration_generation != participant->registration_generation) {
        result.outcome = RegistrationOutcome::RevalidationRequired;
        result.explanation = Explanation(ExplanationCode::GenerationMismatch)
                                 .with("expected", request.expected_registration_generation->to_string())
                                 .with("current", participant->registration_generation.to_string());
        return result;
      }
      if (participant->policy != request.policy) {
        result.outcome = RegistrationOutcome::ConflictingIncarnation;
        result.explanation = Explanation(ExplanationCode::IncarnationConflict)
                                 .with("reason", "participant is registered with a different incarnation policy")
                                 .with("current_policy", std::string(to_string(participant->policy)))
                                 .with("requested_policy", std::string(to_string(request.policy)));
        return result;
      }
      if (find_incarnation(*participant, request.boot).has_value()) {
        result.outcome = RegistrationOutcome::AlreadyActive;
        result.registration_generation = participant->registration_generation;
        result.incarnation = find_incarnation(*participant, request.boot);
        result.explanation = Explanation(ExplanationCode::IncarnationConflict)
                                 .with("reason", "this worker boot already holds a current incarnation")
                                 .with("boot", request.boot.to_string());
        return result;
      }
      const bool has_incumbent =
          participant->has_active_boot || !participant->extra_incarnations.empty();
      if (has_incumbent && participant->policy == IncarnationPolicy::SingleIncarnation) {
        if (options.replacement_policy == IncarnationReplacementPolicy::RequireExplicitFence) {
          result.outcome = RegistrationOutcome::ConflictingIncarnation;
          result.explanation = Explanation(ExplanationCode::IncarnationConflict)
                                   .with("reason", "a live incarnation already holds authority")
                                   .with("incumbent", participant->has_active_boot
                                                          ? participant->active_boot.to_string()
                                                          : participant->extra_incarnations.front().boot.to_string())
                                   .with("policy", std::string(to_string(options.replacement_policy)));
          return result;
        }
        const WorkerBootId incumbent = participant->has_active_boot
                                           ? participant->active_boot
                                           : participant->extra_incarnations.front().boot;
        FenceResult fence = record_fence(request.participant, incumbent, FenceReason::Reincarnation,
                                         BoundedText{}, false, undo, mutated);
        if (fence.outcome != FenceOutcome::Fenced && fence.outcome != FenceOutcome::AlreadyFenced) {
          result.outcome = RegistrationOutcome::ConflictingIncarnation;
          result.explanation = Explanation(ExplanationCode::IncarnationConflict)
                                   .with("reason", "incumbent could not be fenced atomically")
                                   .with("fence_outcome", std::string(to_string(fence.outcome)));
          return result;
        }
        participant = find_participant(request.participant);
        if (participant == nullptr) {
          throw invariant("participant disappeared while replacing an incumbent incarnation");
        }
      }
    } else if (state.participants.size() >= options.limits.max_participants) {
      result.outcome = RegistrationOutcome::ResourceLimit;
      result.explanation = Explanation(ExplanationCode::ResourceLimitReached)
                               .with("limit", "max_participants")
                               .with("current", std::to_string(state.participants.size()));
      return result;
    }

    if (active_workers >= options.limits.max_active_workers) {
      result.outcome = RegistrationOutcome::ResourceLimit;
      result.explanation = Explanation(ExplanationCode::ResourceLimitReached)
                               .with("limit", "max_active_workers")
                               .with("current", std::to_string(active_workers));
      return result;
    }

    const bool creating = participant == nullptr;
    if (creating) {
      DurableParticipant fresh;
      fresh.id = request.participant;
      fresh.registration_generation = RegistrationGeneration(0);
      fresh.fence_generation = FencingGeneration(0);
      fresh.incarnation_floor = IncarnationSequence(0);
      fresh.next_incarnation = IncarnationSequence(1);
      fresh.policy = request.policy;
      state.participants.push_back(std::move(fresh));
      const std::size_t index = state.participants.size() - 1;
      participant_index[request.participant.value()] = index;
      undo.push([this]() { state.participants.pop_back(); });
      participant = &state.participants[index];
    } else {
      // The undo restores by durable identity rather than by vector position:
      // the persistence barrier canonicalises participant ordering before it
      // attempts the durable write, so a positional restore could otherwise
      // rewrite the wrong record.
      DurableParticipant previous = *participant;
      undo.push([this, previous = std::move(previous), id = request.participant]() {
        if (DurableParticipant* target = raw_find_participant(id); target != nullptr) {
          *target = previous;
        }
      });
      participant = &state.participants[participant_index[request.participant.value()]];
    }

    const auto registration_generation = state.registration_floor.next();
    if (!registration_generation.has_value()) {
      throw overflow("registration generation counter is exhausted");
    }
    const RegistrationGeneration previous_floor = state.registration_floor;
    undo.push([this, previous_floor]() { state.registration_floor = previous_floor; });
    state.registration_floor = *registration_generation;

    const IncarnationSequence incarnation = participant->next_incarnation;
    const auto next_incarnation = incarnation.next();
    if (!next_incarnation.has_value()) {
      throw overflow("incarnation sequence counter is exhausted for this participant");
    }

    participant->registration_generation = *registration_generation;
    participant->next_incarnation = *next_incarnation;
    participant->policy = request.policy;
    participant->scopes = request.scopes;
    participant->revalidation_required = false;
    participant->retired = false;
    ++participant->accepted_registrations;

    if (!participant->has_active_boot) {
      participant->has_active_boot = true;
      participant->active_boot = request.boot;
      participant->active_incarnation = incarnation;
    } else {
      participant->extra_incarnations.push_back(
          ActiveIncarnation{request.boot, incarnation, *registration_generation});
    }
    active_workers += 1;
    boot_index[request.boot.to_string()] = participant_index[request.participant.value()];

    IdempotencyRecord record;
    record.attempt = request.attempt;
    record.operation = OperationKind::RegisterParticipant;
    record.semantic = digest;
    record.outcome = static_cast<std::uint16_t>(RegistrationOutcome::Registered);
    record.auxiliary = registration_generation->value();
    record.auxiliary_high = incarnation.value();
    remember_attempt(participant->idempotency, options.limits, std::move(record));

    if (commit_generation) {
      bump_generation(undo);
    }
    mutated = true;

    result.outcome = RegistrationOutcome::Registered;
    result.registration_generation = *registration_generation;
    result.incarnation = incarnation;
    result.explanation = Explanation(ExplanationCode::RegistrationAccepted)
                             .with("participant", request.participant.to_string())
                             .with("boot", request.boot.to_string())
                             .with("epoch", state.current_epoch.to_string())
                             .with("incarnation", incarnation.to_string())
                             .with("registration_generation", registration_generation->to_string())
                             .with("scopes", request.scopes.to_string());
    return result;
  }

  // ---- authority validation ---------------------------------------------

  [[nodiscard]] AuthorityValidation validate(const ValidateAuthorityRequest& request) const {
    AuthorityValidation result;
    result.epoch = state.current_epoch;
    result.coordinator_boot = state.coordinator_boot;

    if (request.token.participant.empty() || request.token.boot.is_nil() ||
        request.token.incarnation.is_zero()) {
      result.code = AuthorityValidationCode::Malformed;
      result.currentness = AuthorityCurrentness::Unknown;
      result.explanation = Explanation(ExplanationCode::MalformedRequest)
                               .with("reason", "authority token is structurally invalid");
      return result;
    }

    if (request.grant.has_value()) {
      const auto it = grant_index.find(request.grant->to_string());
      if (it == grant_index.end() || it->second >= state.grants.size()) {
        result.code = AuthorityValidationCode::UnknownGrant;
        result.currentness = AuthorityCurrentness::Unknown;
        result.explanation = Explanation(ExplanationCode::GrantSuperseded)
                                 .with("grant", request.grant->to_string())
                                 .with("reason", "grant does not exist");
        return result;
      }
      const AuthorityGrant& grant = state.grants[it->second];
      if (grant.epoch != state.current_epoch) {
        result.code = AuthorityValidationCode::StaleEpoch;
        result.currentness = AuthorityCurrentness::StaleEpoch;
        result.replaced_by_epoch = state.current_epoch;
        result.explanation = Explanation(ExplanationCode::EpochStale)
                                 .with("grant_epoch", grant.epoch.to_string())
                                 .with("current_epoch", state.current_epoch.to_string());
        return result;
      }
      if (grant.revoked) {
        result.code = AuthorityValidationCode::Revoked;
        result.currentness = AuthorityCurrentness::Revoked;
        result.explanation = Explanation(ExplanationCode::GrantRevoked)
                                 .with("grant", grant.id.to_string())
                                 .with("revoked_at_epoch", grant.revoked_at_epoch.to_string());
        return result;
      }
      if (grant.boot != request.token.boot) {
        result.code = AuthorityValidationCode::StaleWorkerBoot;
        result.currentness = AuthorityCurrentness::Fenced;
        result.superseded_by = grant.boot;
        result.explanation = Explanation(ExplanationCode::WorkerBootSuperseded)
                                 .with("token_boot", request.token.boot.to_string())
                                 .with("grant_boot", grant.boot.to_string());
        return result;
      }
      const DurableParticipant* owner = find_participant(grant.participant);
      if (owner == nullptr) {
        result.code = AuthorityValidationCode::UnknownParticipant;
        result.currentness = AuthorityCurrentness::Unknown;
        result.explanation = Explanation(ExplanationCode::ParticipantUnknown)
                                 .with("participant", grant.participant.to_string());
        return result;
      }
      const IncarnationEvaluation evaluation =
          evaluate_incarnation(*owner, request.token.boot, request.token.incarnation);
      if (evaluation.code != AuthorityValidationCode::Current) {
        result.code = evaluation.code;
        result.currentness = evaluation.currentness;
        result.superseded_by = evaluation.superseded_by;
        result.replaced_by_epoch = evaluation.replaced_by_epoch;
        result.explanation = Explanation(ExplanationCode::WorkerBootFenced)
                                 .with("boot", request.token.boot.to_string())
                                 .with("currentness", std::string(to_string(evaluation.currentness)));
        return result;
      }
      result.incarnation = evaluation.incarnation;
      result.registration_generation = owner->registration_generation;
      if (!grant.scopes.contains(request.scope)) {
        result.code = AuthorityValidationCode::UnauthorizedScope;
        result.currentness = AuthorityCurrentness::Current;
        result.explanation = Explanation(ExplanationCode::ScopeMissing)
                                 .with("scope", request.scope.to_string())
                                 .with("grant", grant.id.to_string());
        return result;
      }
      if (grant.generation != GrantGeneration(owner->registration_generation.value()) &&
          request.expected_generation.has_value() &&
          *request.expected_generation != owner->registration_generation) {
        result.code = AuthorityValidationCode::RevalidationRequired;
        result.currentness = AuthorityCurrentness::RevalidationRequired;
        result.explanation = Explanation(ExplanationCode::GenerationMismatch)
                                 .with("expected", request.expected_generation->to_string())
                                 .with("current", owner->registration_generation.to_string());
        return result;
      }
      result.code = AuthorityValidationCode::Current;
      result.currentness = AuthorityCurrentness::Current;
      result.explanation = Explanation(ExplanationCode::AuthorityCurrent)
                               .with("grant", grant.id.to_string())
                               .with("epoch", state.current_epoch.to_string())
                               .with("scope", request.scope.to_string());
      return result;
    }

    if (request.token.epoch != state.current_epoch) {
      result.code = AuthorityValidationCode::StaleEpoch;
      result.currentness = AuthorityCurrentness::StaleEpoch;
      result.replaced_by_epoch = state.current_epoch;
      result.explanation = Explanation(ExplanationCode::EpochStale)
                               .with("token_epoch", request.token.epoch.to_string())
                               .with("current_epoch", state.current_epoch.to_string());
      return result;
    }

    const DurableParticipant* participant = find_participant(request.token.participant);
    if (participant == nullptr) {
      result.code = AuthorityValidationCode::UnknownParticipant;
      result.currentness = AuthorityCurrentness::Unknown;
      result.explanation = Explanation(ExplanationCode::ParticipantUnknown)
                               .with("participant", request.token.participant.to_string());
      return result;
    }
    if (participant->retired) {
      result.code = AuthorityValidationCode::Retired;
      result.currentness = AuthorityCurrentness::Retired;
      result.explanation = Explanation(ExplanationCode::ParticipantRetired)
                               .with("participant", participant->id.to_string());
      return result;
    }

    const IncarnationEvaluation evaluation =
        evaluate_incarnation(*participant, request.token.boot, request.token.incarnation);
    result.registration_generation = participant->registration_generation;
    if (evaluation.code != AuthorityValidationCode::Current) {
      result.code = evaluation.code;
      result.currentness = evaluation.currentness;
      result.incarnation = evaluation.incarnation;
      result.superseded_by = evaluation.superseded_by;
      result.replaced_by_epoch = evaluation.replaced_by_epoch;
      Explanation explanation(evaluation.currentness == AuthorityCurrentness::Fenced
                                  ? ExplanationCode::WorkerBootFenced
                                  : (evaluation.currentness == AuthorityCurrentness::RevalidationRequired
                                         ? ExplanationCode::RevalidationRequired
                                         : ExplanationCode::WorkerBootSuperseded));
      explanation.with("participant", participant->id.to_string());
      explanation.with("boot", request.token.boot.to_string());
      explanation.with("currentness", std::string(to_string(evaluation.currentness)));
      explanation.with("current_epoch", state.current_epoch.to_string());
      if (evaluation.fence_reason.has_value()) {
        explanation.with("fence_reason", std::string(to_string(*evaluation.fence_reason)));
      }
      if (evaluation.superseded_by.has_value()) {
        explanation.with("superseded_by", evaluation.superseded_by->to_string());
      }
      if (participant->has_active_boot) {
        explanation.with("active_boot", participant->active_boot.to_string());
      }
      result.explanation = std::move(explanation);
      return result;
    }

    result.incarnation = evaluation.incarnation;
    if (!participant->scopes.contains(request.scope)) {
      result.code = AuthorityValidationCode::UnauthorizedScope;
      result.currentness = AuthorityCurrentness::Current;
      result.explanation = Explanation(ExplanationCode::ScopeMissing)
                               .with("scope", request.scope.to_string())
                               .with("granted", participant->scopes.to_string());
      return result;
    }
    if (request.expected_generation.has_value() &&
        *request.expected_generation != participant->registration_generation) {
      result.code = AuthorityValidationCode::RevalidationRequired;
      result.currentness = AuthorityCurrentness::RevalidationRequired;
      result.explanation = Explanation(ExplanationCode::GenerationMismatch)
                               .with("expected", request.expected_generation->to_string())
                               .with("current", participant->registration_generation.to_string());
      return result;
    }

    result.code = AuthorityValidationCode::Current;
    result.currentness = AuthorityCurrentness::Current;
    result.explanation = Explanation(ExplanationCode::AuthorityCurrent)
                             .with("participant", participant->id.to_string())
                             .with("boot", request.token.boot.to_string())
                             .with("epoch", state.current_epoch.to_string())
                             .with("scope", request.scope.to_string());
    return result;
  }

  [[nodiscard]] ParticipantView to_view(const DurableParticipant& participant) const {
    ParticipantView view;
    view.id = participant.id;
    view.registration_generation = participant.registration_generation;
    view.fence_generation = participant.fence_generation;
    view.incarnation_floor = participant.incarnation_floor;
    view.policy = participant.policy;
    view.retired = participant.retired;
    view.revalidation_required = participant.revalidation_required;
    view.has_active_incarnation = participant.has_active_boot;
    view.active_boot = participant.active_boot;
    view.active_incarnation = participant.active_incarnation;
    view.scopes = participant.scopes;
    view.accepted_registrations = participant.accepted_registrations;
    view.replay_hits = participant.replay_hits;
    for (const auto& incarnation : participant.extra_incarnations) {
      view.active_boots.push_back(incarnation.boot);
    }
    return view;
  }

  [[nodiscard]] AuthoritySnapshot build_snapshot() const {
    AuthoritySnapshot snapshot;
    snapshot.id = EpochSnapshotId::generate();
    snapshot.epoch = state.current_epoch;
    snapshot.coordinator_boot = state.coordinator_boot;
    snapshot.generation = state.generation;
    snapshot.registration_floor = state.registration_floor;
    snapshot.fence_floor = state.fence_floor;
    snapshot.participants.reserve(state.participants.size());
    for (const auto& participant : state.participants) {
      snapshot.participants.push_back(to_view(participant));
    }
    std::sort(snapshot.participants.begin(), snapshot.participants.end(),
              [](const ParticipantView& lhs, const ParticipantView& rhs) { return lhs.id < rhs.id; });
    snapshot.fences = state.recent_fences;
    std::sort(snapshot.fences.begin(), snapshot.fences.end(),
              [](const FenceRecord& lhs, const FenceRecord& rhs) {
                if (lhs.participant != rhs.participant) {
                  return lhs.participant < rhs.participant;
                }
                if (lhs.boot != rhs.boot) {
                  return lhs.boot < rhs.boot;
                }
                return lhs.generation < rhs.generation;
              });
    snapshot.grants.reserve(state.grants.size());
    for (const auto& grant : state.grants) {
      GrantView view;
      view.id = grant.id;
      view.epoch = grant.epoch;
      view.participant = grant.participant;
      view.boot = grant.boot;
      view.generation = grant.generation;
      view.revoked = grant.revoked;
      view.delegated = grant.parent.has_value();
      view.scopes = grant.scopes;
      snapshot.grants.push_back(std::move(view));
    }
    std::sort(snapshot.grants.begin(), snapshot.grants.end(),
              [](const GrantView& lhs, const GrantView& rhs) { return lhs.id < rhs.id; });
    snapshot.digest = detail::compute_snapshot_digest(snapshot);
    return snapshot;
  }

  [[nodiscard]] EpochState build_state() const {
    EpochState out;
    out.epoch = state.current_epoch;
    out.coordinator_boot = state.coordinator_boot;
    out.generation = state.generation;
    out.registration_floor = state.registration_floor;
    out.fence_floor = state.fence_floor;
    out.participant_count = state.participants.size();
    out.active_worker_count = active_workers;
    out.retained_fence_count = state.recent_fences.size();
    out.transition_history_count = state.transitions.size();
    std::size_t live = 0;
    for (const auto& grant : state.grants) {
      if (!grant.revoked) {
        ++live;
      }
    }
    out.live_grant_count = live;
    return out;
  }

  // ---- grants ------------------------------------------------------------

  [[nodiscard]] GrantResult apply_grant(const GrantAuthorityRequest& request, UndoLog& undo,
                                        bool& mutated) {
    GrantResult result;
    result.epoch = state.current_epoch;

    if (request.attempt.is_nil() || request.participant.empty() || request.boot.is_nil() ||
        request.incarnation.is_zero()) {
      result.outcome = GrantOutcome::MalformedRequest;
      result.explanation = Explanation(ExplanationCode::MalformedRequest)
                               .with("reason", "grant request is structurally invalid");
      return result;
    }
    if (request.scopes.empty()) {
      result.outcome = GrantOutcome::MalformedRequest;
      result.explanation = Explanation(ExplanationCode::MalformedRequest)
                               .with("reason", "grant binds no authority scope");
      return result;
    }
    if (request.scopes.size() > options.limits.max_scopes_per_grant) {
      result.outcome = GrantOutcome::ResourceLimit;
      result.explanation = Explanation(ExplanationCode::ResourceLimitReached)
                               .with("limit", "max_scopes_per_grant");
      return result;
    }
    if (request.expected_epoch != state.current_epoch) {
      result.outcome = GrantOutcome::StaleEpoch;
      result.explanation = Explanation(ExplanationCode::EpochStale)
                               .with("request_epoch", request.expected_epoch.to_string())
                               .with("current_epoch", state.current_epoch.to_string());
      return result;
    }

    DurableParticipant* participant = find_participant(request.participant);
    if (participant == nullptr) {
      result.outcome = GrantOutcome::UnknownParticipant;
      result.explanation = Explanation(ExplanationCode::ParticipantUnknown)
                               .with("participant", request.participant.to_string());
      return result;
    }
    const IncarnationEvaluation evaluation =
        evaluate_incarnation(*participant, request.boot, request.incarnation);
    if (evaluation.code != AuthorityValidationCode::Current) {
      result.outcome = GrantOutcome::StaleWorkerBoot;
      result.explanation = Explanation(ExplanationCode::WorkerBootFenced)
                               .with("boot", request.boot.to_string())
                               .with("currentness", std::string(to_string(evaluation.currentness)));
      return result;
    }
    if (!participant->scopes.contains_all(request.scopes)) {
      result.outcome = GrantOutcome::UnauthorizedScope;
      result.explanation = Explanation(ExplanationCode::ScopeMissing)
                               .with("requested", request.scopes.to_string())
                               .with("granted", participant->scopes.to_string());
      return result;
    }

    const SnapshotDigest digest = semantic_digest(request);
    if (const IdempotencyRecord* previous = find_attempt(participant->idempotency, request.attempt);
        previous != nullptr) {
      if (previous->semantic != digest) {
        result.outcome = GrantOutcome::ConflictingAttempt;
        result.explanation = Explanation(ExplanationCode::AttemptConflict)
                                 .with("attempt", request.attempt.to_string());
        return result;
      }
      result.outcome = static_cast<GrantOutcome>(previous->outcome);
      result.grant = AuthorityGrantId::from_bytes(previous->auxiliary_id);
      result.generation = GrantGeneration(previous->auxiliary);
      result.replayed = true;
      result.explanation = Explanation(ExplanationCode::RegistrationIdempotentReplay)
                               .with("attempt", request.attempt.to_string())
                               .with("grant", result.grant->to_string());
      return result;
    }

    const AuthorityGrant* parent = nullptr;
    if (request.parent.has_value()) {
      const auto it = grant_index.find(request.parent->to_string());
      if (it == grant_index.end() || it->second >= state.grants.size()) {
        result.outcome = GrantOutcome::DelegationRejected;
        result.explanation = Explanation(ExplanationCode::DelegationRejected)
                                 .with("reason", "parent grant does not exist")
                                 .with("parent", request.parent->to_string());
        return result;
      }
      parent = &state.grants[it->second];
      if (parent->revoked || parent->epoch != state.current_epoch) {
        result.outcome = GrantOutcome::DelegationRejected;
        result.explanation = Explanation(ExplanationCode::DelegationRejected)
                                 .with("reason", parent->revoked ? "parent grant is revoked"
                                                                 : "parent grant is bound to a stale epoch")
                                 .with("parent", parent->id.to_string());
        return result;
      }
      const DurableParticipant* delegator = find_participant(parent->participant);
      if (delegator == nullptr) {
        result.outcome = GrantOutcome::DelegationRejected;
        result.explanation = Explanation(ExplanationCode::DelegationRejected)
                                 .with("reason", "delegating participant no longer exists");
        return result;
      }
      const IncarnationEvaluation delegator_evaluation =
          evaluate_incarnation(*delegator, parent->boot, parent->incarnation);
      if (delegator_evaluation.code != AuthorityValidationCode::Current) {
        result.outcome = GrantOutcome::DelegationRejected;
        result.explanation = Explanation(ExplanationCode::DelegationRejected)
                                 .with("reason", "delegating incarnation is not current")
                                 .with("delegator", parent->participant.to_string());
        return result;
      }
      // Delegated scope may never exceed parent scope.
      if (!parent->scopes.contains_all(request.scopes)) {
        result.outcome = GrantOutcome::DelegationRejected;
        result.explanation = Explanation(ExplanationCode::DelegationRejected)
                                 .with("reason", "delegated scope exceeds parent scope")
                                 .with("parent_scopes", parent->scopes.to_string())
                                 .with("requested", request.scopes.to_string());
        return result;
      }
    }

    if (state.grants.size() >= options.limits.max_authority_grants) {
      result.outcome = GrantOutcome::ResourceLimit;
      result.explanation = Explanation(ExplanationCode::ResourceLimitReached)
                               .with("limit", "max_authority_grants");
      return result;
    }

    AuthorityGrant grant;
    grant.id = AuthorityGrantId::generate();
    grant.epoch = state.current_epoch;
    grant.participant = request.participant;
    grant.boot = request.boot;
    grant.incarnation = request.incarnation;
    grant.scopes = request.scopes;
    grant.parent = request.parent;
    if (parent != nullptr) {
      grant.delegator_boot = parent->boot;
    }
    grant.provenance = request.provenance;

    const std::size_t index = state.grants.size();
    state.grants.push_back(std::move(grant));
    undo.push([this]() { state.grants.pop_back(); });
    grant_index[state.grants[index].id.to_string()] = index;
    if (state.grants[index].parent.has_value()) {
      grant_children[state.grants[index].parent->to_string()].push_back(index);
    }

    bump_generation(undo);
    state.grants[index].generation = GrantGeneration(state.generation.value());

    IdempotencyRecord record;
    record.attempt = request.attempt;
    record.operation = OperationKind::GrantAuthority;
    record.semantic = digest;
    record.outcome = static_cast<std::uint16_t>(GrantOutcome::Granted);
    record.auxiliary = state.grants[index].generation.value();
    record.auxiliary_id = state.grants[index].id.bytes();
    remember_attempt(participant->idempotency, options.limits, std::move(record));
    mutated = true;

    result.outcome = GrantOutcome::Granted;
    result.grant = state.grants[index].id;
    result.generation = state.grants[index].generation;
    result.explanation = Explanation(ExplanationCode::RegistrationAccepted)
                             .with("grant", result.grant->to_string())
                             .with("participant", request.participant.to_string())
                             .with("epoch", state.current_epoch.to_string())
                             .with("scopes", request.scopes.to_string());
    return result;
  }

  [[nodiscard]] RevokeResult apply_revoke(const RevokeAuthorityRequest& request, UndoLog& undo,
                                          bool& mutated) {
    RevokeResult result;
    result.epoch = state.current_epoch;

    if (request.attempt.is_nil() || request.grant.is_nil()) {
      result.outcome = RevokeOutcome::MalformedRequest;
      result.explanation = Explanation(ExplanationCode::MalformedRequest)
                               .with("reason", "revocation request is structurally invalid");
      return result;
    }
    if (request.expected_epoch != state.current_epoch) {
      result.outcome = RevokeOutcome::StaleEpoch;
      result.explanation = Explanation(ExplanationCode::EpochStale)
                               .with("request_epoch", request.expected_epoch.to_string())
                               .with("current_epoch", state.current_epoch.to_string());
      return result;
    }

    const SnapshotDigest digest = semantic_digest(request);
    if (const IdempotencyRecord* previous = find_attempt(state.control_attempts, request.attempt);
        previous != nullptr) {
      if (previous->semantic != digest) {
        result.outcome = RevokeOutcome::ConflictingAttempt;
        result.explanation = Explanation(ExplanationCode::AttemptConflict)
                                 .with("attempt", request.attempt.to_string());
        return result;
      }
      result.outcome = static_cast<RevokeOutcome>(previous->outcome);
      result.generation = GrantGeneration(previous->auxiliary);
      result.derived_grants_revoked = static_cast<std::size_t>(previous->auxiliary_high);
      result.explanation = Explanation(ExplanationCode::RegistrationIdempotentReplay)
                               .with("attempt", request.attempt.to_string())
                               .with("grant", request.grant.to_string());
      return result;
    }

    const auto it = grant_index.find(request.grant.to_string());
    if (it == grant_index.end() || it->second >= state.grants.size()) {
      result.outcome = RevokeOutcome::UnknownGrant;
      result.explanation = Explanation(ExplanationCode::GrantSuperseded)
                               .with("reason", "grant does not exist")
                               .with("grant", request.grant.to_string());
      return result;
    }

    AuthorityGrant& grant = state.grants[it->second];
    if (grant.revoked) {
      result.outcome = RevokeOutcome::AlreadyRevoked;
      result.generation = grant.generation;
      result.explanation = Explanation(ExplanationCode::GrantRevoked)
                               .with("grant", grant.id.to_string())
                               .with("revoked_at_epoch", grant.revoked_at_epoch.to_string());
      return result;
    }

    const std::size_t previous_revoked_count = revoked_grant_count();
    const GrantGeneration previous_grant_generation = grant.generation;
    const CoordinatorEpoch previous_revoked_at = grant.revoked_at_epoch;
    const std::string grant_key = grant.id.to_string();
    const std::size_t previous_control_attempts = state.control_attempts.size();
    undo.push([this, grant_key, previous_grant_generation, previous_revoked_at, previous_revoked_count,
               previous_control_attempts]() {
      state.control_attempts.resize(previous_control_attempts);
      restore_revocations(grant_key, previous_revoked_count);
      const auto index = grant_index.find(grant_key);
      if (index != grant_index.end() && index->second < state.grants.size()) {
        state.grants[index->second].generation = previous_grant_generation;
        state.grants[index->second].revoked_at_epoch = previous_revoked_at;
      }
    });

    std::vector<AuthorityGrantId> pending;
    pending.push_back(grant.id);
    std::size_t revoked = 0;
    while (!pending.empty()) {
      const AuthorityGrantId current = pending.back();
      pending.pop_back();
      const auto current_it = grant_index.find(current.to_string());
      if (current_it == grant_index.end() || current_it->second >= state.grants.size()) {
        continue;
      }
      AuthorityGrant& target = state.grants[current_it->second];
      if (target.revoked && current != grant.id) {
        continue;
      }
      if (!target.revoked) {
        target.revoked = true;
        target.revoked_at_epoch = state.current_epoch;
        ++revoked;
      }
      const auto children = grant_children.find(current.to_string());
      if (children != grant_children.end()) {
        for (const std::size_t child : children->second) {
          pending.push_back(state.grants[child].id);
        }
      }
    }

    bump_generation(undo);
    state.grants[it->second].generation = GrantGeneration(state.generation.value());

    IdempotencyRecord record;
    record.attempt = request.attempt;
    record.operation = OperationKind::RevokeAuthority;
    record.semantic = digest;
    record.outcome = static_cast<std::uint16_t>(RevokeOutcome::Revoked);
    record.auxiliary = state.grants[it->second].generation.value();
    record.auxiliary_high = revoked > 0 ? revoked - 1 : 0;
    remember_attempt(state.control_attempts, options.limits, std::move(record));
    mutated = true;

    result.outcome = RevokeOutcome::Revoked;
    result.generation = state.grants[it->second].generation;
    result.derived_grants_revoked = revoked > 0 ? revoked - 1 : 0;
    result.explanation = Explanation(ExplanationCode::GrantRevoked)
                             .with("grant", request.grant.to_string())
                             .with("reason", std::string(to_string(request.reason)))
                             .with("derived_grants_revoked", std::to_string(result.derived_grants_revoked));
    return result;
  }

  [[nodiscard]] std::size_t revoked_grant_count() const {
    std::size_t count = 0;
    for (const auto& grant : state.grants) {
      if (grant.revoked) {
        ++count;
      }
    }
    return count;
  }

  /// Restores the revoked flags of the grants that are currently revoked.
  /// Because revocation is monotonic (a grant is never un-revoked inside a
  /// single operation) the undo is expressed as "revive every grant that this
  /// operation could have touched", which is exactly the sub-graph rooted at
  /// the target grant.
  void restore_revocations(const std::string& root, std::size_t /*previous_revoked_count*/) {
    std::vector<std::string> pending;
    pending.push_back(root);
    while (!pending.empty()) {
      const std::string current = pending.back();
      pending.pop_back();
      const auto it = grant_index.find(current);
      if (it == grant_index.end() || it->second >= state.grants.size()) {
        continue;
      }
      state.grants[it->second].revoked = false;
      state.grants[it->second].revoked_at_epoch = CoordinatorEpoch(0);
      const auto children = grant_children.find(current);
      if (children != grant_children.end()) {
        for (const std::size_t child : children->second) {
          pending.push_back(state.grants[child].id.to_string());
        }
      }
    }
  }

  // ---- retirement --------------------------------------------------------

  [[nodiscard]] RetireResult apply_retire(const RetireParticipantRequest& request, UndoLog& undo,
                                          bool& mutated) {
    RetireResult result;
    result.epoch = state.current_epoch;

    if (request.attempt.is_nil() || request.participant.empty()) {
      result.outcome = RetireOutcome::MalformedRequest;
      result.explanation = Explanation(ExplanationCode::MalformedRequest)
                               .with("reason", "retirement request is structurally invalid");
      return result;
    }
    if (request.expected_epoch != state.current_epoch) {
      result.outcome = RetireOutcome::StaleEpoch;
      result.explanation = Explanation(ExplanationCode::EpochStale)
                               .with("request_epoch", request.expected_epoch.to_string())
                               .with("current_epoch", state.current_epoch.to_string());
      return result;
    }

    DurableParticipant* participant = find_participant(request.participant);
    if (participant == nullptr) {
      result.outcome = RetireOutcome::UnknownParticipant;
      result.explanation = Explanation(ExplanationCode::ParticipantUnknown)
                               .with("participant", request.participant.to_string());
      return result;
    }

    const SnapshotDigest digest = semantic_digest(request);
    if (const IdempotencyRecord* previous = find_attempt(participant->idempotency, request.attempt);
        previous != nullptr) {
      if (previous->semantic != digest) {
        result.outcome = RetireOutcome::ConflictingAttempt;
        result.explanation = Explanation(ExplanationCode::AttemptConflict)
                                 .with("attempt", request.attempt.to_string());
        return result;
      }
      result.outcome = static_cast<RetireOutcome>(previous->outcome);
      result.incarnations_fenced = static_cast<std::size_t>(previous->auxiliary);
      result.explanation = Explanation(ExplanationCode::RegistrationIdempotentReplay)
                               .with("attempt", request.attempt.to_string())
                               .with("outcome", std::string(to_string(result.outcome)));
      return result;
    }

    if (participant->retired) {
      result.outcome = RetireOutcome::AlreadyRetired;
      result.explanation = Explanation(ExplanationCode::ParticipantRetired)
                               .with("participant", participant->id.to_string());
      return result;
    }

    std::vector<WorkerBootId> live;
    if (participant->has_active_boot) {
      live.push_back(participant->active_boot);
    }
    for (const auto& incarnation : participant->extra_incarnations) {
      live.push_back(incarnation.boot);
    }

    for (const auto& boot : live) {
      FenceResult fence = record_fence(request.participant, boot, FenceReason::AdministrativeRevoke,
                                       request.note, false, undo, mutated);
      if (fence.outcome == FenceOutcome::Fenced || fence.outcome == FenceOutcome::AlreadyFenced) {
        ++result.incarnations_fenced;
      }
      participant = find_participant(request.participant);
      if (participant == nullptr) {
        throw invariant("participant disappeared while being retired");
      }
    }

    const IncarnationSequence previous_floor = participant->incarnation_floor;
    const bool previous_retired = participant->retired;
    const bool previous_revalidation = participant->revalidation_required;
    undo.push([this, id = request.participant, previous_floor, previous_retired,
               previous_revalidation]() {
      if (DurableParticipant* target = raw_find_participant(id); target != nullptr) {
        target->incarnation_floor = previous_floor;
        target->retired = previous_retired;
        target->revalidation_required = previous_revalidation;
      }
    });

    // Close the incarnation allocator: every sequence this participant could
    // ever have issued is now at or below the fencing floor. The record itself
    // is retained so retirement is never indistinguishable from "unknown".
    if (!participant->next_incarnation.is_zero()) {
      const IncarnationSequence highest_issued(participant->next_incarnation.value() - 1);
      if (highest_issued > participant->incarnation_floor) {
        participant->incarnation_floor = highest_issued;
      }
    }
    participant->retired = true;
    participant->revalidation_required = false;

    bump_generation(undo);

    IdempotencyRecord record;
    record.attempt = request.attempt;
    record.operation = OperationKind::RetireParticipant;
    record.semantic = digest;
    record.outcome = static_cast<std::uint16_t>(RetireOutcome::Retired);
    record.auxiliary = result.incarnations_fenced;
    remember_attempt(participant->idempotency, options.limits, std::move(record));
    mutated = true;

    result.outcome = RetireOutcome::Retired;
    result.explanation = Explanation(ExplanationCode::ParticipantRetired)
                             .with("participant", request.participant.to_string())
                             .with("incarnations_fenced", std::to_string(result.incarnations_fenced))
                             .with("epoch", state.current_epoch.to_string());
    return result;
  }

  // ---- sessions ----------------------------------------------------------

  void erase_session(const SessionId& session) {
    const auto it = session_index.find(session.to_string());
    if (it == session_index.end()) {
      return;
    }
    boot_session_index.erase(it->second.boot.to_string());
    session_index.erase(it);
  }
};

// ==========================================================================
// Public API
// ==========================================================================

EpochRuntime::EpochRuntime(EpochRuntimeOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

EpochRuntime::~EpochRuntime() = default;

const Limits& EpochRuntime::limits() const noexcept { return impl_->options.limits; }

const std::filesystem::path& EpochRuntime::store_path() const noexcept { return impl_->options.store_path; }

EpochState EpochRuntime::state() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->build_state();
}

CoordinatorEpoch EpochRuntime::current_epoch() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->state.current_epoch;
}

CoordinatorBootId EpochRuntime::coordinator_boot() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->state.coordinator_boot;
}

EpochGeneration EpochRuntime::generation() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->state.generation;
}

std::optional<ParticipantView> EpochRuntime::query_participant(const ParticipantId& id) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  const DurableParticipant* participant = impl_->find_participant(id);
  if (participant == nullptr) {
    return std::nullopt;
  }
  return impl_->to_view(*participant);
}

std::vector<ParticipantView> EpochRuntime::list_participants() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  std::vector<ParticipantView> out;
  out.reserve(impl_->state.participants.size());
  for (const auto& participant : impl_->state.participants) {
    out.push_back(impl_->to_view(participant));
  }
  std::sort(out.begin(), out.end(),
            [](const ParticipantView& lhs, const ParticipantView& rhs) { return lhs.id < rhs.id; });
  return out;
}

std::vector<FenceRecord> EpochRuntime::list_fences() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->state.recent_fences;
}

std::vector<GrantView> EpochRuntime::list_grants() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  std::vector<GrantView> out;
  out.reserve(impl_->state.grants.size());
  for (const auto& grant : impl_->state.grants) {
    GrantView view;
    view.id = grant.id;
    view.epoch = grant.epoch;
    view.participant = grant.participant;
    view.boot = grant.boot;
    view.generation = grant.generation;
    view.revoked = grant.revoked;
    view.delegated = grant.parent.has_value();
    view.scopes = grant.scopes;
    out.push_back(std::move(view));
  }
  std::sort(out.begin(), out.end(), [](const GrantView& lhs, const GrantView& rhs) { return lhs.id < rhs.id; });
  return out;
}

std::vector<EpochTransitionRecord> EpochRuntime::transition_history(std::size_t max_records) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  std::vector<EpochTransitionRecord> out;
  const auto& transitions = impl_->state.transitions;
  const std::size_t count = std::min(max_records, transitions.size());
  out.reserve(count);
  for (std::size_t i = transitions.size() - count; i < transitions.size(); ++i) {
    out.push_back(transitions[i]);
  }
  return out;
}

AuthoritySnapshot EpochRuntime::snapshot() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->build_snapshot();
}

FenceStatus EpochRuntime::query_fence(const ParticipantId& participant, const WorkerBootId& boot) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  FenceStatus status;
  status.boot = boot;
  status.participant = participant;
  const Impl::FenceLookup lookup = impl_->lookup_fence(participant, boot);
  status.fenced = lookup.fenced;
  status.retained_record = lookup.record != nullptr;
  if (lookup.record != nullptr) {
    status.reason = lookup.record->reason;
    status.fenced_at_epoch = lookup.record->epoch;
    status.incarnation = lookup.record->incarnation;
  }
  if (const DurableParticipant* owner = impl_->find_participant(participant); owner != nullptr) {
    if (const auto current = Impl::find_incarnation(*owner, boot); current.has_value()) {
      status.incarnation = *current;
    }
    if (owner->has_active_boot && owner->active_boot != boot) {
      status.superseded_by = owner->active_boot;
    }
    if (status.fenced) {
      status.replaced_by_epoch = impl_->state.current_epoch;
    }
  }
  return status;
}

Explanation EpochRuntime::explain_authority(const ParticipantId& participant,
                                            const WorkerBootId& boot) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  Explanation explanation(ExplanationCode::ParticipantUnknown);
  explanation.with("participant", participant.to_string());
  explanation.with("boot", boot.to_string());
  explanation.with("current_epoch", impl_->state.current_epoch.to_string());
  explanation.with("coordinator_boot", impl_->state.coordinator_boot.to_string());

  const DurableParticipant* owner = impl_->find_participant(participant);
  if (owner == nullptr) {
    return explanation;
  }
  explanation.with("registration_generation", owner->registration_generation.to_string());
  explanation.with("fence_generation", owner->fence_generation.to_string());
  explanation.with("incarnation_floor", owner->incarnation_floor.to_string());

  if (owner->retired) {
    explanation.with_code(ExplanationCode::ParticipantRetired);
    return explanation;
  }

  const auto current = Impl::find_incarnation(*owner, boot);
  if (current.has_value()) {
    explanation.with_code(ExplanationCode::AuthorityCurrent);
    explanation.with("incarnation", current->to_string());
    return explanation;
  }

  const Impl::FenceLookup fence = impl_->lookup_fence(participant, boot);
  if (fence.fenced) {
    explanation.with_code(ExplanationCode::WorkerBootFenced);
    explanation.with("fence_reason", fence.record != nullptr
                                         ? std::string(to_string(fence.record->reason))
                                         : std::string("RETAINED"));
    if (fence.record != nullptr) {
      explanation.with("fenced_at_epoch", fence.record->epoch.to_string());
      explanation.with("fence_generation", fence.record->generation.to_string());
    }
    explanation.with("replaced_by_epoch", impl_->state.current_epoch.to_string());
    return explanation;
  }

  const bool has_current = owner->has_active_boot || !owner->extra_incarnations.empty();
  if (!has_current) {
    if (owner->revalidation_required) {
      explanation.with_code(ExplanationCode::RevalidationRequired);
      explanation.with("reason", "coordinator authority advanced; register a fresh worker incarnation");
    } else {
      explanation.with_code(ExplanationCode::WorkerBootFenced);
      explanation.with("reason", "participant holds no current incarnation");
    }
    return explanation;
  }

  explanation.with_code(ExplanationCode::WorkerBootSuperseded);
  explanation.with("active_boot", owner->has_active_boot ? owner->active_boot.to_string()
                                                         : owner->extra_incarnations.front().boot.to_string());
  explanation.with("reason", "a different worker incarnation holds current authority");
  return explanation;
}

Explanation EpochRuntime::explain_epoch() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  Explanation explanation(ExplanationCode::EpochAdvanced);
  explanation.with("current_epoch", impl_->state.current_epoch.to_string());
  explanation.with("coordinator_boot", impl_->state.coordinator_boot.to_string());
  explanation.with("generation", impl_->state.generation.to_string());
  if (!impl_->state.transitions.empty()) {
    const EpochTransitionRecord& last = impl_->state.transitions.back();
    explanation.with("last_transition_from", last.from_epoch.to_string());
    explanation.with("last_transition_to", last.to_epoch.to_string());
    explanation.with("last_transition_cause", std::string(to_string(last.cause)));
    explanation.with("last_transition_boot", last.coordinator_boot.to_string());
  } else {
    explanation.with("last_transition", "NONE");
  }
  return explanation;
}

EpochAdvanceResult EpochRuntime::advance_epoch(const EpochAdvanceRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  EpochAdvanceResult result;
  result.epoch = impl_->state.current_epoch;
  result.coordinator_boot = impl_->state.coordinator_boot;

  if (request.attempt.is_nil()) {
    result.outcome = EpochAdvanceOutcome::MalformedRequest;
    result.explanation = Explanation(ExplanationCode::MalformedRequest)
                             .with("reason", "attempt identifier is nil");
    return result;
  }
  // Replay classification precedes the expected-epoch check for epoch
  // advancement only. An exact retry of a successful advancement necessarily
  // carries the pre-advancement expectation, and the attempt table is keyed by
  // a caller-supplied 128-bit identifier, so nothing about another caller's
  // state is observable here. Participant-bound operations keep the stricter
  // ordering in which epoch currentness is established first.
  const SnapshotDigest digest = semantic_digest(request);
  if (const IdempotencyRecord* previous = Impl::find_attempt(impl_->state.control_attempts, request.attempt);
      previous != nullptr) {
    if (previous->semantic != digest) {
      result.outcome = EpochAdvanceOutcome::ConflictingAttempt;
      result.explanation = Explanation(ExplanationCode::AttemptConflict)
                               .with("attempt", request.attempt.to_string());
      return result;
    }
    result.outcome = static_cast<EpochAdvanceOutcome>(previous->outcome);
    result.epoch = CoordinatorEpoch(previous->auxiliary);
    if (previous->auxiliary_high != 0 && result.epoch == impl_->state.current_epoch &&
        !impl_->state.transitions.empty()) {
      result.transition = impl_->state.transitions.back().id;
    }
    result.replayed = true;
    result.explanation = Explanation(ExplanationCode::RegistrationIdempotentReplay)
                             .with("attempt", request.attempt.to_string())
                             .with("epoch", result.epoch.to_string());
    return result;
  }

  if (request.expected_epoch != impl_->state.current_epoch) {
    result.outcome = EpochAdvanceOutcome::StaleExpectedEpoch;
    result.explanation = Explanation(ExplanationCode::EpochStale)
                             .with("expected_epoch", request.expected_epoch.to_string())
                             .with("current_epoch", impl_->state.current_epoch.to_string());
    return result;
  }

  if (impl_->state.current_epoch.is_max()) {
    result.outcome = EpochAdvanceOutcome::EpochExhausted;
    result.explanation = Explanation(ExplanationCode::ResourceLimitReached)
                             .with("limit", "CoordinatorEpoch")
                             .with("reason", "the maximum representable epoch is already authoritative")
                             .with("epoch", impl_->state.current_epoch.to_string());
    return result;
  }

  UndoLog undo;
  impl_->mutate_epoch(undo, impl_->state.coordinator_boot, request.cause, true);

  IdempotencyRecord record;
  record.attempt = request.attempt;
  record.operation = OperationKind::AdvanceEpoch;
  record.semantic = digest;
  record.outcome = static_cast<std::uint16_t>(EpochAdvanceOutcome::Advanced);
  record.auxiliary = impl_->state.current_epoch.value();
  record.auxiliary_high = 1;
  Impl::remember_attempt(impl_->state.control_attempts, impl_->options.limits, std::move(record));
  impl_->persist(undo);

  result.outcome = EpochAdvanceOutcome::Advanced;
  result.epoch = impl_->state.current_epoch;
  result.coordinator_boot = impl_->state.coordinator_boot;
  if (!impl_->state.transitions.empty()) {
    result.transition = impl_->state.transitions.back().id;
  }
  result.explanation = Explanation(ExplanationCode::EpochAdvanced)
                           .with("epoch", result.epoch.to_string())
                           .with("cause", std::string(to_string(request.cause)))
                           .with("coordinator_boot", result.coordinator_boot.to_string());
  return result;
}

RegistrationResult EpochRuntime::register_participant(const RegisterParticipantRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  UndoLog undo;
  bool mutated = false;
  RegistrationResult result = impl_->apply_registration(request, undo, mutated);
  if (mutated) {
    impl_->persist(undo);
  }
  return result;
}

std::vector<RegistrationResult> EpochRuntime::register_participants(
    std::span<const RegisterParticipantRequest> requests) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  std::vector<RegistrationResult> results;
  results.reserve(requests.size());
  UndoLog undo;
  bool mutated = false;
  for (const auto& request : requests) {
    results.push_back(impl_->apply_registration(request, undo, mutated, false));
  }
  if (mutated) {
    // A batch is one committed state generation, not one per element.
    impl_->bump_generation(undo);
    impl_->persist(undo);
  }
  return results;
}

GrantResult EpochRuntime::grant_authority(const GrantAuthorityRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  UndoLog undo;
  bool mutated = false;
  GrantResult result = impl_->apply_grant(request, undo, mutated);
  if (mutated) {
    impl_->persist(undo);
  }
  return result;
}

RevokeResult EpochRuntime::revoke_authority(const RevokeAuthorityRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  UndoLog undo;
  bool mutated = false;
  RevokeResult result = impl_->apply_revoke(request, undo, mutated);
  if (mutated) {
    impl_->persist(undo);
  }
  return result;
}

RetireResult EpochRuntime::retire_participant(const RetireParticipantRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  UndoLog undo;
  bool mutated = false;
  RetireResult result = impl_->apply_retire(request, undo, mutated);
  if (mutated) {
    impl_->persist(undo);
  }
  return result;
}

FenceResult EpochRuntime::fence_worker(const FenceWorkerRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  FenceResult result;
  result.epoch = impl_->state.current_epoch;

  if (request.attempt.is_nil()) {
    result.outcome = FenceOutcome::MalformedRequest;
    result.explanation = Explanation(ExplanationCode::MalformedRequest)
                             .with("reason", "attempt identifier is nil");
    return result;
  }
  if (request.participant.empty() || request.boot.is_nil()) {
    result.outcome = FenceOutcome::MalformedRequest;
    result.explanation = Explanation(ExplanationCode::MalformedRequest)
                             .with("reason", "fence request is structurally invalid");
    return result;
  }
  if (request.expected_epoch != impl_->state.current_epoch) {
    result.outcome = FenceOutcome::StaleEpoch;
    result.explanation = Explanation(ExplanationCode::EpochStale)
                             .with("request_epoch", request.expected_epoch.to_string())
                             .with("current_epoch", impl_->state.current_epoch.to_string());
    return result;
  }

  DurableParticipant* participant = impl_->find_participant(request.participant);
  if (participant == nullptr) {
    result.outcome = FenceOutcome::UnknownParticipant;
    result.explanation = Explanation(ExplanationCode::ParticipantUnknown)
                             .with("participant", request.participant.to_string());
    return result;
  }

  const SnapshotDigest digest = semantic_digest(request);
  if (const IdempotencyRecord* previous = Impl::find_attempt(participant->idempotency, request.attempt);
      previous != nullptr) {
    if (previous->semantic != digest) {
      result.outcome = FenceOutcome::ConflictingAttempt;
      result.explanation = Explanation(ExplanationCode::AttemptConflict)
                               .with("attempt", request.attempt.to_string());
      return result;
    }
    result.outcome = static_cast<FenceOutcome>(previous->outcome);
    if (previous->auxiliary != 0) {
      result.fence_generation = FencingGeneration(previous->auxiliary);
    }
    result.explanation = Explanation(ExplanationCode::FenceIdempotent)
                             .with("attempt", request.attempt.to_string())
                             .with("replay", "true");
    return result;
  }

  UndoLog undo;
  bool mutated = false;
  result = impl_->record_fence(request.participant, request.boot, request.reason, request.note, true, undo,
                               mutated);
  if (mutated) {
    IdempotencyRecord record;
    record.attempt = request.attempt;
    record.operation = OperationKind::FenceWorker;
    record.semantic = digest;
    record.outcome = static_cast<std::uint16_t>(result.outcome);
    record.auxiliary = result.fence_generation.has_value() ? result.fence_generation->value() : 0;
    record.auxiliary_high = result.incarnation.has_value() ? result.incarnation->value() : 0;
    DurableParticipant* target = impl_->find_participant(request.participant);
    if (target != nullptr) {
      Impl::remember_attempt(target->idempotency, impl_->options.limits, std::move(record));
    }
    impl_->persist(undo);
  }
  return result;
}

AuthorityValidation EpochRuntime::validate_authority(const ValidateAuthorityRequest& request) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->validate(request);
}

bool EpochRuntime::bind_session(const SessionBinding& binding) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  if (binding.epoch != impl_->state.current_epoch) {
    return false;
  }
  const DurableParticipant* participant = impl_->find_participant(binding.participant);
  if (participant == nullptr || participant->retired) {
    return false;
  }
  const auto current = Impl::find_incarnation(*participant, binding.boot);
  if (!current.has_value() || *current != binding.incarnation) {
    return false;
  }
  if (impl_->boot_session_index.find(binding.boot.to_string()) != impl_->boot_session_index.end()) {
    // One session per worker incarnation.
    return false;
  }
  if (impl_->session_index.size() >= impl_->options.limits.max_sessions) {
    throw resource_limit("session count exceeds limits.max_sessions");
  }
  impl_->session_index[binding.id.to_string()] = binding;
  impl_->boot_session_index[binding.boot.to_string()] = binding.id.to_string();
  return true;
}

FenceResult EpochRuntime::close_session(const SessionId& session, SessionClosureReason reason) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  FenceResult result;
  result.epoch = impl_->state.current_epoch;

  const auto it = impl_->session_index.find(session.to_string());
  if (it == impl_->session_index.end()) {
    result.outcome = FenceOutcome::NoFenceRequired;
    result.explanation = Explanation(ExplanationCode::SessionClosed)
                             .with("session", session.to_string())
                             .with("reason", "session is not bound");
    return result;
  }
  const SessionBinding binding = it->second;
  impl_->erase_session(session);

  if (!impl_->options.fence_on_session_loss) {
    result.outcome = FenceOutcome::NoFenceRequired;
    result.explanation = Explanation(ExplanationCode::SessionClosed)
                             .with("session", session.to_string())
                             .with("reason", "runtime is configured as a durable-incarnation model")
                             .with("closure", std::string(to_string(reason)));
    return result;
  }

  const DurableParticipant* participant = impl_->find_participant(binding.participant);
  if (participant == nullptr) {
    result.outcome = FenceOutcome::NoFenceRequired;
    result.explanation = Explanation(ExplanationCode::SessionClosed)
                             .with("session", session.to_string())
                             .with("reason", "participant no longer exists");
    return result;
  }
  const auto current = Impl::find_incarnation(*participant, binding.boot);
  if (!current.has_value()) {
    result.outcome = FenceOutcome::NoFenceRequired;
    result.explanation = Explanation(ExplanationCode::SessionClosed)
                             .with("session", session.to_string())
                             .with("reason", "the incarnation is already superseded or fenced");
    return result;
  }

  UndoLog undo;
  bool mutated = false;
  result = impl_->record_fence(binding.participant, binding.boot, fence_reason_for(reason), BoundedText{},
                               false, undo, mutated);
  if (mutated) {
    impl_->persist(undo);
  }
  return result;
}

std::vector<SessionBinding> EpochRuntime::active_sessions() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  std::vector<SessionBinding> out;
  out.reserve(impl_->session_index.size());
  for (const auto& entry : impl_->session_index) {
    out.push_back(entry.second);
  }
  std::sort(out.begin(), out.end(), [](const SessionBinding& lhs, const SessionBinding& rhs) {
    return lhs.id < rhs.id;
  });
  return out;
}

std::size_t EpochRuntime::session_count() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->session_index.size();
}

void EpochRuntime::flush() {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  impl_->store.save(impl_->state);
}

bool EpochRuntime::check_indexes() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->indexes_match();
}

}  // namespace fabric_epoch
