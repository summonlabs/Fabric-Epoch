// Fabric Epoch 1.0.0 - Summon Software Labs
#include <set>
#include <string>

#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

FE_TEST(reincarnation, restart_is_distinguishable_from_reconnect) {
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  const fe::ParticipantId id = participant("publisher.P");

  // Epoch 9 is current for this participant: Boot A holds authority.
  const fe::WorkerBootId boot_a = fe::WorkerBootId::generate();
  const fe::RegistrationResult a =
      runtime.register_participant(registration_request("publisher.P", boot_a, runtime.current_epoch()));
  FE_REQUIRE_EQ(a.outcome, fe::RegistrationOutcome::Registered);
  FE_REQUIRE_EQ(*a.incarnation, fe::IncarnationSequence(1));

  fe::GrantAuthorityRequest grant;
  grant.attempt = fe::MutationAttemptId::generate();
  grant.expected_epoch = runtime.current_epoch();
  grant.participant = id;
  grant.boot = boot_a;
  grant.incarnation = *a.incarnation;
  grant.scopes = default_scopes();
  const fe::GrantResult granted = runtime.grant_authority(grant);
  FE_REQUIRE_EQ(granted.outcome, fe::GrantOutcome::Granted);
  const fe::GrantGeneration grant_generation = *granted.generation;

  // Process A dies. The control path observes the loss and fences Boot A.
  fe::FenceWorkerRequest death;
  death.attempt = fe::MutationAttemptId::generate();
  death.expected_epoch = runtime.current_epoch();
  death.participant = id;
  death.boot = boot_a;
  death.reason = fe::FenceReason::ProcessLoss;
  FE_REQUIRE_EQ(runtime.fence_worker(death).outcome, fe::FenceOutcome::Fenced);

  // P restarts as Boot B and must register fresh.
  const fe::WorkerBootId boot_b = fe::WorkerBootId::generate();
  const fe::RegistrationResult b =
      runtime.register_participant(registration_request("publisher.P", boot_b, runtime.current_epoch()));
  FE_REQUIRE_EQ(b.outcome, fe::RegistrationOutcome::Registered);
  FE_REQUIRE(*b.incarnation > *a.incarnation);
  FE_REQUIRE(*b.registration_generation > *a.registration_generation);
  FE_REQUIRE(*granted.generation == grant_generation);

  // Boot A stays stale permanently.
  fe::ValidateAuthorityRequest token_a;
  token_a.token = token_for(a, id, boot_a);
  token_a.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
  FE_REQUIRE_EQ(runtime.validate_authority(token_a).code, fe::AuthorityValidationCode::StaleWorkerBoot);

  // Grants bound to A remain unusable.
  fe::ValidateAuthorityRequest grant_a = token_a;
  grant_a.grant = granted.grant;
  FE_REQUIRE_EQ(runtime.validate_authority(grant_a).code, fe::AuthorityValidationCode::StaleWorkerBoot);

  // Boot B holds fresh authority.
  fe::ValidateAuthorityRequest token_b;
  token_b.token = token_for(b, id, boot_b);
  token_b.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
  FE_REQUIRE_EQ(runtime.validate_authority(token_b).code, fe::AuthorityValidationCode::Current);

  // And A can never come back, even after repeated restarts of P.
  for (int cycle = 0; cycle < 4; ++cycle) {
    const fe::WorkerBootId next = fe::WorkerBootId::generate();
    fe::FenceWorkerRequest fence;
    fence.attempt = fe::MutationAttemptId::generate();
    fence.expected_epoch = runtime.current_epoch();
    fence.participant = id;
    fence.boot = fe::WorkerBootId::generate();
    const auto view = runtime.query_participant(id);
    FE_REQUIRE(view.has_value());
    fence.boot = view->active_boot;
    FE_REQUIRE_EQ(runtime.fence_worker(fence).outcome, fe::FenceOutcome::Fenced);
    FE_REQUIRE_EQ(runtime.register_participant(registration_request("publisher.P", next, runtime.current_epoch()))
                      .outcome,
                  fe::RegistrationOutcome::Registered);
  }
  FE_REQUIRE_EQ(runtime.validate_authority(token_a).code, fe::AuthorityValidationCode::StaleWorkerBoot);
  FE_REQUIRE(runtime.check_indexes());
}

FE_TEST(reincarnation, unrelated_participants_are_unaffected_by_a_fence) {
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  const fe::WorkerBootId boot_a = fe::WorkerBootId::generate();
  const fe::WorkerBootId boot_b = fe::WorkerBootId::generate();
  const fe::RegistrationResult a =
      runtime.register_participant(registration_request("publisher.A", boot_a, runtime.current_epoch()));
  const fe::RegistrationResult b =
      runtime.register_participant(registration_request("publisher.B", boot_b, runtime.current_epoch()));
  FE_REQUIRE_EQ(a.outcome, fe::RegistrationOutcome::Registered);
  FE_REQUIRE_EQ(b.outcome, fe::RegistrationOutcome::Registered);

  fe::FenceWorkerRequest fence;
  fence.attempt = fe::MutationAttemptId::generate();
  fence.expected_epoch = runtime.current_epoch();
  fence.participant = participant("publisher.A");
  fence.boot = boot_a;
  FE_REQUIRE_EQ(runtime.fence_worker(fence).outcome, fe::FenceOutcome::Fenced);

  fe::ValidateAuthorityRequest token_b;
  token_b.token = token_for(b, participant("publisher.B"), boot_b);
  token_b.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
  FE_REQUIRE_EQ(runtime.validate_authority(token_b).code, fe::AuthorityValidationCode::Current);
}

FE_TEST(reincarnation, only_one_incarnation_is_current_under_the_single_policy) {
  RuntimeFixture storage;
  fe::EpochRuntimeOptions options = runtime_options(storage);
  options.replacement_policy = fe::IncarnationReplacementPolicy::ReplaceIncumbent;
  fe::EpochRuntime runtime(options);
  std::set<std::string> observed;
  for (int cycle = 0; cycle < 32; ++cycle) {
    const fe::WorkerBootId boot = fe::WorkerBootId::generate();
    const fe::RegistrationResult registered =
        runtime.register_participant(registration_request("publisher.single", boot, runtime.current_epoch()));
    FE_REQUIRE_EQ(registered.outcome, fe::RegistrationOutcome::Registered);
    const auto view = runtime.query_participant(participant("publisher.single"));
    FE_REQUIRE(view.has_value());
    FE_REQUIRE_EQ(view->active_incarnation_count(), std::size_t{1});
    FE_REQUIRE_EQ(view->active_boot, boot);
    FE_REQUIRE(observed.insert(boot.to_string()).second);
  }
  FE_REQUIRE_EQ(observed.size(), std::size_t{32});
  FE_REQUIRE(runtime.check_indexes());
}

FE_TEST(reincarnation, the_incarnation_allocator_never_reissues_a_sequence) {
  RuntimeFixture storage;
  fe::EpochRuntimeOptions options = runtime_options(storage);
  options.limits.max_recent_fences = 64;
  options.limits.max_fenced_boots_per_participant = 64;
  fe::EpochRuntime runtime(options);
  std::set<std::uint64_t> sequences;
  for (int cycle = 0; cycle < 24; ++cycle) {
    const fe::WorkerBootId boot = fe::WorkerBootId::generate();
    const fe::RegistrationResult registered =
        runtime.register_participant(registration_request("publisher.sequence", boot, runtime.current_epoch()));
    FE_REQUIRE_EQ(registered.outcome, fe::RegistrationOutcome::Registered);
    FE_REQUIRE(sequences.insert(registered.incarnation->value()).second);
    fe::FenceWorkerRequest fence;
    fence.attempt = fe::MutationAttemptId::generate();
    fence.expected_epoch = runtime.current_epoch();
    fence.participant = participant("publisher.sequence");
    fence.boot = boot;
    FE_REQUIRE_EQ(runtime.fence_worker(fence).outcome, fe::FenceOutcome::Fenced);
  }
  const auto view = runtime.query_participant(participant("publisher.sequence"));
  FE_REQUIRE_MSG(view->incarnation_floor.value() >= sequences.size(),
                 "the fencing floor must cover every issued incarnation");
}
