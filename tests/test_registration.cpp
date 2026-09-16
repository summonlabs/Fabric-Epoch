// Fabric Epoch 1.0.0 - Summon Software Labs
#include <string>
#include <vector>

#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

FE_TEST(registration, accepted_registration_binds_every_field) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const fe::RegisterParticipantRequest request =
      registration_request("publisher.one", boot, runtime.current_epoch());
  const fe::RegistrationResult result = runtime.register_participant(request);

  FE_REQUIRE_EQ(result.outcome, fe::RegistrationOutcome::Registered);
  FE_REQUIRE(!result.replayed);
  FE_REQUIRE_EQ(result.epoch, runtime.current_epoch());
  FE_REQUIRE_EQ(result.coordinator_boot, runtime.coordinator_boot());
  FE_REQUIRE(result.registration_generation.has_value());
  FE_REQUIRE(result.incarnation.has_value());
  FE_REQUIRE_EQ(*result.incarnation, fe::IncarnationSequence(1));

  const auto view = runtime.query_participant(request.participant);
  FE_REQUIRE(view.has_value());
  FE_REQUIRE(view->has_active_incarnation);
  FE_REQUIRE_EQ(view->active_boot, boot);
  FE_REQUIRE_EQ(view->active_incarnation, *result.incarnation);
  FE_REQUIRE_EQ(view->registration_generation, *result.registration_generation);
  FE_REQUIRE_EQ(view->accepted_registrations, std::uint64_t{1});
  FE_REQUIRE(!view->revalidation_required);
  FE_REQUIRE(!view->retired);
  FE_REQUIRE(runtime.check_indexes());
}

FE_TEST(registration, malformed_requests_are_rejected_structurally) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::CoordinatorEpoch epoch = runtime.current_epoch();

  fe::RegisterParticipantRequest nil_attempt =
      registration_request("publisher.bad", fe::WorkerBootId::generate(), epoch);
  nil_attempt.attempt = fe::MutationAttemptId{};
  FE_REQUIRE_EQ(runtime.register_participant(nil_attempt).outcome,
                fe::RegistrationOutcome::MalformedRequest);

  fe::RegisterParticipantRequest nil_boot =
      registration_request("publisher.bad", fe::WorkerBootId::generate(), epoch);
  nil_boot.boot = fe::WorkerBootId{};
  FE_REQUIRE_EQ(runtime.register_participant(nil_boot).outcome, fe::RegistrationOutcome::MalformedRequest);

  fe::RegisterParticipantRequest empty_scopes =
      registration_request("publisher.bad", fe::WorkerBootId::generate(), epoch);
  empty_scopes.scopes = fe::ScopeSet{};
  FE_REQUIRE_EQ(runtime.register_participant(empty_scopes).outcome,
                fe::RegistrationOutcome::MalformedRequest);

  FE_REQUIRE_EQ(runtime.list_participants().size(), std::size_t{0});
  FE_REQUIRE(runtime.check_indexes());
}

FE_TEST(registration, stale_epoch_is_rejected_before_anything_else_is_recorded) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::EpochGeneration generation = runtime.generation();
  const fe::CoordinatorEpoch epoch = runtime.current_epoch();

  fe::RegisterParticipantRequest stale = registration_request(
      "publisher.stale", fe::WorkerBootId::generate(), fe::CoordinatorEpoch(epoch.value() + 7));
  const fe::RegistrationResult result = runtime.register_participant(stale);
  FE_REQUIRE_EQ(result.outcome, fe::RegistrationOutcome::StaleEpoch);
  FE_REQUIRE_EQ(runtime.generation(), generation);
  FE_REQUIRE(!runtime.query_participant(stale.participant).has_value());
}

FE_TEST(registration, scope_policy_is_enforced) {
  RuntimeFixture fixture;
  fe::EpochRuntimeOptions options = runtime_options(fixture);
  options.registrable_scope_kinds[static_cast<std::size_t>(fe::ScopeKind::OperationFamily)] = false;
  fe::EpochRuntime runtime(options);

  fe::RegisterParticipantRequest denied =
      registration_request("publisher.scope", fe::WorkerBootId::generate(), runtime.current_epoch(),
                           scopes({scope(fe::ScopeKind::OperationFamily, "publish")}));
  const fe::RegistrationResult result = runtime.register_participant(denied);
  FE_REQUIRE_EQ(result.outcome, fe::RegistrationOutcome::UnauthorizedScope);
  FE_REQUIRE_EQ(field_text(result.explanation.render(), "scope"),
                std::optional<std::string>("OPERATION_FAMILY:publish"));

  fe::RegisterParticipantRequest allowed =
      registration_request("publisher.scope", fe::WorkerBootId::generate(), runtime.current_epoch(),
                           scopes({scope(fe::ScopeKind::Fabric, "fabric-main")}));
  FE_REQUIRE_EQ(runtime.register_participant(allowed).outcome, fe::RegistrationOutcome::Registered);
}

FE_TEST(registration, multi_incarnation_requires_an_explicit_policy) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  fe::RegisterParticipantRequest request =
      registration_request("publisher.multi", fe::WorkerBootId::generate(), runtime.current_epoch());
  request.policy = fe::IncarnationPolicy::MultiIncarnation;
  const fe::RegistrationResult denied = runtime.register_participant(request);
  FE_REQUIRE_EQ(denied.outcome, fe::RegistrationOutcome::UnauthorizedScope);

  RuntimeFixture second;
  fe::EpochRuntimeOptions options = runtime_options(second);
  options.allow_multi_incarnation = true;
  fe::EpochRuntime permissive(options);
  std::vector<fe::WorkerBootId> boots;
  for (int index = 0; index < 3; ++index) {
    fe::RegisterParticipantRequest allowed;
    allowed.attempt = fe::MutationAttemptId::generate();
    allowed.expected_epoch = permissive.current_epoch();
    allowed.participant = participant("publisher.multi");
    allowed.boot = fe::WorkerBootId::generate();
    allowed.scopes = default_scopes();
    allowed.policy = fe::IncarnationPolicy::MultiIncarnation;
    FE_REQUIRE_EQ(permissive.register_participant(allowed).outcome, fe::RegistrationOutcome::Registered);
    boots.push_back(allowed.boot);
  }
  const auto view = permissive.query_participant(participant("publisher.multi"));
  FE_REQUIRE(view.has_value());
  FE_REQUIRE_EQ(view->active_incarnation_count(), std::size_t{3});
  FE_REQUIRE_EQ(view->active_boot, boots.front());
  FE_REQUIRE_EQ(view->active_boots.size(), std::size_t{2});
}

FE_TEST(registration, exact_replay_is_idempotent_and_does_not_advance_generation) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::RegisterParticipantRequest request =
      registration_request("publisher.replay", fe::WorkerBootId::generate(), runtime.current_epoch());
  const fe::RegistrationResult first = runtime.register_participant(request);
  FE_REQUIRE_EQ(first.outcome, fe::RegistrationOutcome::Registered);
  const fe::EpochGeneration generation = runtime.generation();

  const fe::RegistrationResult replay = runtime.register_participant(request);
  FE_REQUIRE_EQ(replay.outcome, fe::RegistrationOutcome::Registered);
  FE_REQUIRE(replay.replayed);
  FE_REQUIRE_EQ(replay.incarnation, first.incarnation);
  FE_REQUIRE_EQ(replay.registration_generation, first.registration_generation);
  FE_REQUIRE_EQ(runtime.generation(), generation);
  FE_REQUIRE_EQ(runtime.list_participants().size(), std::size_t{1});
  FE_REQUIRE(runtime.check_indexes());
}

FE_TEST(registration, refreshed_expectation_fields_still_replay_exactly) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  fe::RegisterParticipantRequest request =
      registration_request("publisher.refresh", fe::WorkerBootId::generate(), runtime.current_epoch());
  request.expected_registration_generation = fe::RegistrationGeneration(3);
  const fe::RegistrationResult first = runtime.register_participant(request);
  FE_REQUIRE_EQ(first.outcome, fe::RegistrationOutcome::Registered);

  // The caller retries with refreshed expectation fields after a duplicate
  // suppression window; the semantic request is unchanged and must still be
  // recognised as an exact retry rather than a new incarnation.
  fe::RegisterParticipantRequest retry = request;
  retry.expected_registration_generation = *first.registration_generation;
  retry.expected_epoch = runtime.current_epoch();
  const fe::RegistrationResult replay = runtime.register_participant(retry);
  FE_REQUIRE_EQ(replay.outcome, fe::RegistrationOutcome::Registered);
  FE_REQUIRE(replay.replayed);
  FE_REQUIRE_EQ(replay.incarnation, first.incarnation);
}

FE_TEST(registration, attempt_identifier_reuse_with_a_different_payload_is_a_conflict) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  fe::RegisterParticipantRequest request =
      registration_request("publisher.conflict", fe::WorkerBootId::generate(), runtime.current_epoch());
  FE_REQUIRE_EQ(runtime.register_participant(request).outcome, fe::RegistrationOutcome::Registered);

  fe::RegisterParticipantRequest conflicting = request;
  conflicting.boot = fe::WorkerBootId::generate();
  const fe::RegistrationResult result = runtime.register_participant(conflicting);
  FE_REQUIRE_EQ(result.outcome, fe::RegistrationOutcome::ConflictingAttempt);
  FE_REQUIRE(!result.replayed);
}

FE_TEST(registration, an_active_incarnation_is_not_silently_replaced) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::WorkerBootId first = fe::WorkerBootId::generate();
  FE_REQUIRE_EQ(runtime
                    .register_participant(
                        registration_request("publisher.incumbent", first, runtime.current_epoch()))
                    .outcome,
                fe::RegistrationOutcome::Registered);

  const fe::RegistrationResult replay = runtime.register_participant(
      registration_request("publisher.incumbent", first, runtime.current_epoch()));
  FE_REQUIRE_EQ(replay.outcome, fe::RegistrationOutcome::AlreadyActive);

  const fe::RegistrationResult rejected = runtime.register_participant(
      registration_request("publisher.incumbent", fe::WorkerBootId::generate(), runtime.current_epoch()));
  FE_REQUIRE_EQ(rejected.outcome, fe::RegistrationOutcome::ConflictingIncarnation);
  const auto view = runtime.query_participant(participant("publisher.incumbent"));
  FE_REQUIRE(view.has_value());
  FE_REQUIRE_EQ(view->active_boot, first);
}

FE_TEST(registration, replace_incumbent_policy_fences_atomically) {
  RuntimeFixture fixture;
  fe::EpochRuntimeOptions options = runtime_options(fixture);
  options.replacement_policy = fe::IncarnationReplacementPolicy::ReplaceIncumbent;
  fe::EpochRuntime runtime(options);

  const fe::WorkerBootId first = fe::WorkerBootId::generate();
  const fe::RegistrationResult initial = runtime.register_participant(
      registration_request("publisher.replace", first, runtime.current_epoch()));
  FE_REQUIRE_EQ(initial.outcome, fe::RegistrationOutcome::Registered);

  const fe::WorkerBootId second = fe::WorkerBootId::generate();
  const fe::RegistrationResult replaced = runtime.register_participant(
      registration_request("publisher.replace", second, runtime.current_epoch()));
  FE_REQUIRE_EQ(replaced.outcome, fe::RegistrationOutcome::Registered);
  FE_REQUIRE(*replaced.incarnation > *initial.incarnation);

  const fe::FenceStatus status = runtime.query_fence(participant("publisher.replace"), first);
  FE_REQUIRE(status.fenced);
  FE_REQUIRE_EQ(status.reason, std::optional<fe::FenceReason>(fe::FenceReason::Reincarnation));

  const auto view = runtime.query_participant(participant("publisher.replace"));
  FE_REQUIRE(view.has_value());
  FE_REQUIRE_EQ(view->active_boot, second);
  FE_REQUIRE(runtime.check_indexes());
}

FE_TEST(registration, expected_registration_generation_mismatch_requires_revalidation) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const fe::RegistrationResult first = runtime.register_participant(
      registration_request("publisher.generation", boot, runtime.current_epoch()));
  FE_REQUIRE_EQ(first.outcome, fe::RegistrationOutcome::Registered);

  fe::RegisterParticipantRequest request =
      registration_request("publisher.generation", fe::WorkerBootId::generate(), runtime.current_epoch());
  request.expected_registration_generation = fe::RegistrationGeneration(7);
  const fe::RegistrationResult result = runtime.register_participant(request);
  FE_REQUIRE_EQ(result.outcome, fe::RegistrationOutcome::RevalidationRequired);
  FE_REQUIRE_EQ(field_text(result.explanation.render(), "expected"), std::optional<std::string>("7"));
  FE_REQUIRE_EQ(field_text(result.explanation.render(), "current"),
                std::optional<std::string>(first.registration_generation->to_string()));
}

FE_TEST(registration, retirement_is_durable_and_terminal_for_the_identity) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const fe::RegistrationResult registered = runtime.register_participant(
      registration_request("publisher.retire", boot, runtime.current_epoch()));
  FE_REQUIRE_EQ(registered.outcome, fe::RegistrationOutcome::Registered);

  fe::RetireParticipantRequest retire;
  retire.attempt = fe::MutationAttemptId::generate();
  retire.expected_epoch = runtime.current_epoch();
  retire.participant = participant("publisher.retire");
  const fe::RetireResult retired = runtime.retire_participant(retire);
  FE_REQUIRE_EQ(retired.outcome, fe::RetireOutcome::Retired);
  FE_REQUIRE_EQ(retired.incarnations_fenced, std::size_t{1});

  const fe::RetireResult again = runtime.retire_participant(retire);
  FE_REQUIRE_EQ(again.outcome, fe::RetireOutcome::Retired);
  FE_REQUIRE(runtime.query_participant(participant("publisher.retire"))->retired);

  const fe::RegistrationResult rejected = runtime.register_participant(
      registration_request("publisher.retire", fe::WorkerBootId::generate(), runtime.current_epoch()));
  FE_REQUIRE_EQ(rejected.outcome, fe::RegistrationOutcome::RetiredParticipant);

  fe::ValidateAuthorityRequest validation;
  validation.token = token_for(registered, participant("publisher.retire"), boot);
  validation.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
  FE_REQUIRE_EQ(runtime.validate_authority(validation).code, fe::AuthorityValidationCode::Retired);

  // The retirement survives a coordinator restart.
  fe::EpochRuntime restarted(runtime_options(fixture));
  FE_REQUIRE(restarted.query_participant(participant("publisher.retire"))->retired);
  FE_REQUIRE_EQ(restarted.register_participant(registration_request(
                    "publisher.retire", fe::WorkerBootId::generate(), restarted.current_epoch()))
                    .outcome,
                fe::RegistrationOutcome::RetiredParticipant);
  FE_REQUIRE(restarted.check_indexes());
}

FE_TEST(registration, batch_commit_reports_per_request_outcomes_and_commits_once) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::CoordinatorEpoch epoch = runtime.current_epoch();

  std::vector<fe::RegisterParticipantRequest> requests;
  for (int index = 0; index < 8; ++index) {
    requests.push_back(registration_request("publisher.batch" + std::to_string(index),
                                            fe::WorkerBootId::generate(), epoch));
  }
  // One request carries a stale epoch and must not be committed.
  requests[5].expected_epoch = fe::CoordinatorEpoch(epoch.value() + 1);

  const fe::EpochGeneration before = runtime.generation();
  const std::vector<fe::RegistrationResult> results = runtime.register_participants(requests);
  FE_REQUIRE_EQ(results.size(), requests.size());
  for (std::size_t index = 0; index < results.size(); ++index) {
    if (index == 5) {
      FE_REQUIRE_EQ(results[index].outcome, fe::RegistrationOutcome::StaleEpoch);
    } else {
      FE_REQUIRE_EQ(results[index].outcome, fe::RegistrationOutcome::Registered);
    }
  }
  FE_REQUIRE_EQ(runtime.list_participants().size(), std::size_t{7});
  // The whole batch cost exactly one committed state generation.
  FE_REQUIRE_EQ(runtime.generation().value(), before.value() + 1);
  FE_REQUIRE(runtime.check_indexes());
}
