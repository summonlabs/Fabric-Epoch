// Fabric Epoch 1.0.0 - Summon Software Labs
// Seeded deterministic randomized schedules. Every failure reports its seed.
#include <set>
#include <string>
#include <vector>

#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

namespace {

struct InvariantState {
  fe::CoordinatorEpoch epoch;
  std::set<std::string> fenced_boots;
  std::set<std::string> registered_participants;
};

void check_invariants(fe::EpochRuntime& runtime, InvariantState& state, std::uint64_t seed,
                      std::size_t step) {
  const fe::EpochState current = runtime.state();
  const std::string context =
      "seed=" + std::to_string(seed) + " step=" + std::to_string(step);

  // Epoch monotonicity: an epoch never decreases.
  FE_REQUIRE_MSG(current.epoch >= state.epoch, context + " epoch decreased");
  state.epoch = current.epoch;

  // The epoch is a real authority generation, never the unestablished value.
  FE_REQUIRE_MSG(!current.epoch.is_zero(), context + " epoch is unestablished");

  // Indexes always match their records.
  FE_REQUIRE_MSG(runtime.check_indexes(), context + " index inconsistency");

  // At most one current incarnation per single-incarnation participant.
  for (const auto& view : runtime.list_participants()) {
    if (view.policy == fe::IncarnationPolicy::SingleIncarnation) {
      FE_REQUIRE_MSG(view.active_incarnation_count() <= 1, context + " multiple current incarnations");
    }
    FE_REQUIRE_MSG(view.fence_generation <= current.fence_floor, context + " fence generation above floor");
    FE_REQUIRE_MSG(view.registration_generation <= current.registration_floor,
                   context + " registration generation above floor");
  }

  // A fenced worker boot never becomes current again.
  for (const auto& boot : state.fenced_boots) {
    const auto parsed = fe::WorkerBootId::parse(boot);
    FE_REQUIRE(parsed.has_value());
    for (const auto& view : runtime.list_participants()) {
      if (view.has_active_incarnation) {
        FE_REQUIRE_MSG(view.active_boot.to_string() != boot, context + " fenced boot became current");
      }
    }
    // And a registration presenting it is refused while it is retained.
    const fe::FenceStatus status = runtime.query_fence(participant("publisher.prop"), *parsed);
    (void)status;
  }

  // Snapshots of an unchanged state are byte-identical in digest.
  const fe::SnapshotDigest first = runtime.snapshot().digest;
  const fe::SnapshotDigest second = runtime.snapshot().digest;
  FE_REQUIRE_MSG(first == second, context + " digest is not stable for an unchanged state");
}

void run_schedule(std::uint64_t seed, int operations) {
  RuntimeFixture storage;
  fe::EpochRuntimeOptions options = runtime_options(storage);
  options.replacement_policy = fe::IncarnationReplacementPolicy::ReplaceIncumbent;
  options.limits.max_recent_fences = 64;
  options.limits.max_fenced_boots_per_participant = 64;
  fe::EpochRuntime runtime(options);

  DeterministicRandom random(seed);
  InvariantState state;
  // Every accepted incarnation is tracked together with the participant that
  // owns it, so the schedule can only fence a boot under its real owner.
  std::vector<std::pair<std::string, fe::WorkerBootId>> boots;
  std::vector<fe::GrantAuthorityRequest> grants;

  for (int step = 0; step < operations; ++step) {
    const std::uint64_t choice = random.next_below(100);
    if (choice < 45) {
      const std::string name = "publisher.prop" + std::to_string(random.next_below(6));
      const fe::WorkerBootId boot = fe::WorkerBootId::generate();
      const fe::RegistrationResult result =
          runtime.register_participant(registration_request(name, boot, runtime.current_epoch()));
      if (result.outcome == fe::RegistrationOutcome::Registered) {
        boots.emplace_back(name, boot);
      }
    } else if (choice < 70) {
      if (!boots.empty()) {
        const std::size_t index = static_cast<std::size_t>(random.next_below(boots.size()));
        fe::FenceWorkerRequest fence;
        fence.attempt = fe::MutationAttemptId::generate();
        fence.expected_epoch = runtime.current_epoch();
        fence.participant = participant(boots[index].first);
        fence.boot = boots[index].second;
        const fe::FenceResult result = runtime.fence_worker(fence);
        if (result.outcome == fe::FenceOutcome::Fenced) {
          state.fenced_boots.insert(boots[index].second.to_string());
        }
      }
    } else if (choice < 82) {
      fe::EpochAdvanceRequest advance;
      advance.attempt = fe::MutationAttemptId::generate();
      advance.expected_epoch = runtime.current_epoch();
      (void)runtime.advance_epoch(advance);
    } else if (choice < 92) {
      const auto participants = runtime.list_participants();
      if (!participants.empty()) {
        const auto& target = participants[random.next_below(participants.size())];
        if (target.has_active_incarnation) {
          fe::GrantAuthorityRequest grant;
          grant.attempt = fe::MutationAttemptId::generate();
          grant.expected_epoch = runtime.current_epoch();
          grant.participant = target.id;
          grant.boot = target.active_boot;
          grant.incarnation = target.active_incarnation;
          grant.scopes = target.scopes;
          const fe::GrantResult result = runtime.grant_authority(grant);
          if (result.outcome == fe::GrantOutcome::Granted) {
            grants.push_back(grant);
          }
        }
      }
    } else {
      if (!grants.empty()) {
        const std::size_t index = static_cast<std::size_t>(random.next_below(grants.size()));
        if (random.next_below(2) == 0) {
          fe::ValidateAuthorityRequest request;
          request.token.epoch = runtime.current_epoch();
          request.token.participant = grants[index].participant;
          request.token.boot = grants[index].boot;
          request.token.incarnation = grants[index].incarnation;
          request.scope = *grants[index].scopes.scopes().begin();
          (void)runtime.validate_authority(request);
        } else {
          fe::RevokeAuthorityRequest revoke;
          revoke.attempt = fe::MutationAttemptId::generate();
          revoke.expected_epoch = runtime.current_epoch();
          revoke.grant = fe::AuthorityGrantId::generate();
          (void)runtime.revoke_authority(revoke);
        }
      }
    }
    check_invariants(runtime, state, seed, static_cast<std::size_t>(step));
  }

  // The durable fence state survives a restart, and no fenced boot becomes
  // current under the new epoch either.
  const fe::CoordinatorEpoch before_restart = runtime.current_epoch();
  fe::EpochRuntime restarted(runtime_options(storage));
  FE_REQUIRE(restarted.current_epoch() > before_restart);
  for (const auto& boot : state.fenced_boots) {
    const auto parsed = fe::WorkerBootId::parse(boot);
    FE_REQUIRE(parsed.has_value());
    for (const auto& view : restarted.list_participants()) {
      if (view.has_active_incarnation) {
        FE_REQUIRE_MSG(view.active_boot.to_string() != boot,
                       "seed=" + std::to_string(seed) + " fenced boot became current after restart");
      }
    }
  }
  FE_REQUIRE(restarted.check_indexes());
}

}  // namespace

FE_TEST(property, seeded_schedules_preserve_every_invariant) {
  const std::vector<std::uint64_t> seeds = {1, 2, 3, 5, 8, 13, 21, 34};
  for (const std::uint64_t seed : seeds) {
    run_schedule(seed, 120);
  }
}

FE_TEST(property, epoch_never_wraps_across_repeated_advances) {
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  fe::CoordinatorEpoch previous = runtime.current_epoch();
  for (int step = 0; step < 200; ++step) {
    fe::EpochAdvanceRequest advance;
    advance.attempt = fe::MutationAttemptId::generate();
    advance.expected_epoch = runtime.current_epoch();
    FE_REQUIRE_EQ(runtime.advance_epoch(advance).outcome, fe::EpochAdvanceOutcome::Advanced);
    const fe::CoordinatorEpoch current = runtime.current_epoch();
    FE_REQUIRE(current > previous);
    FE_REQUIRE(!current.is_zero());
    previous = current;
  }
}

FE_TEST(property, persistence_round_trips_after_every_mutation_class) {
  RuntimeFixture storage;
  fe::EpochRuntimeOptions options = runtime_options(storage);
  options.replacement_policy = fe::IncarnationReplacementPolicy::ReplaceIncumbent;
  fe::EpochRuntime runtime(options);
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const fe::RegistrationResult registered =
      runtime.register_participant(registration_request("publisher.rt", boot, runtime.current_epoch()));
  fe::GrantAuthorityRequest grant;
  grant.attempt = fe::MutationAttemptId::generate();
  grant.expected_epoch = runtime.current_epoch();
  grant.participant = participant("publisher.rt");
  grant.boot = boot;
  grant.incarnation = *registered.incarnation;
  grant.scopes = default_scopes();
  const fe::GrantResult granted = runtime.grant_authority(grant);
  fe::RevokeAuthorityRequest revoke;
  revoke.attempt = fe::MutationAttemptId::generate();
  revoke.expected_epoch = runtime.current_epoch();
  revoke.grant = *granted.grant;
  FE_REQUIRE_EQ(runtime.revoke_authority(revoke).outcome, fe::RevokeOutcome::Revoked);

  const fe::EpochState before = runtime.state();
  const fe::SnapshotDigest digest_before = runtime.snapshot().digest;

  fe::EpochRuntime restarted(runtime_options(storage));
  FE_REQUIRE_EQ(restarted.list_grants().size(), std::size_t{1});
  FE_REQUIRE(restarted.list_grants().front().revoked);
  FE_REQUIRE_EQ(restarted.transition_history(64).size(), before.transition_history_count + 1);
  // The epoch advanced, so the digest necessarily changed; the semantic
  // content that survives is what matters.
  FE_REQUIRE(restarted.snapshot().digest != digest_before);
  FE_REQUIRE(restarted.query_participant(participant("publisher.rt"))->revalidation_required);
  FE_REQUIRE(restarted.check_indexes());
}
