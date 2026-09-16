// Fabric Epoch 1.0.0 - Summon Software Labs
#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

namespace {

fe::FenceWorkerRequest fence_request(const fe::ParticipantId& id, const fe::WorkerBootId& boot,
                                     fe::CoordinatorEpoch epoch,
                                     fe::FenceReason reason = fe::FenceReason::ExplicitOperatorFence) {
  fe::FenceWorkerRequest request;
  request.attempt = fe::MutationAttemptId::generate();
  request.expected_epoch = epoch;
  request.participant = id;
  request.boot = boot;
  request.reason = reason;
  return request;
}

}  // namespace

FE_TEST(fencing, fencing_is_idempotent_and_creates_no_semantic_churn) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::ParticipantId id = participant("publisher.fence");
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  FE_REQUIRE_EQ(runtime.register_participant(registration_request("publisher.fence", boot, runtime.current_epoch()))
                    .outcome,
                fe::RegistrationOutcome::Registered);

  const fe::FenceResult first = runtime.fence_worker(fence_request(id, boot, runtime.current_epoch()));
  FE_REQUIRE_EQ(first.outcome, fe::FenceOutcome::Fenced);
  const fe::EpochGeneration generation = runtime.generation();
  const fe::EpochState state = runtime.state();

  const fe::FenceResult second = runtime.fence_worker(fence_request(id, boot, runtime.current_epoch()));
  FE_REQUIRE_EQ(second.outcome, fe::FenceOutcome::AlreadyFenced);
  FE_REQUIRE_EQ(second.fence_generation, first.fence_generation);
  FE_REQUIRE_EQ(runtime.generation(), generation);
  FE_REQUIRE_EQ(runtime.state().fence_floor, state.fence_floor);
  FE_REQUIRE_EQ(runtime.list_fences().size(), std::size_t{1});
  FE_REQUIRE(runtime.check_indexes());
}

FE_TEST(fencing, stale_epoch_and_unknown_participant_are_distinguished) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::CoordinatorEpoch epoch = runtime.current_epoch();

  const fe::FenceResult stale = runtime.fence_worker(fence_request(
      participant("publisher.fence"), fe::WorkerBootId::generate(), fe::CoordinatorEpoch(epoch.value() + 3)));
  FE_REQUIRE_EQ(stale.outcome, fe::FenceOutcome::StaleEpoch);

  const fe::FenceResult unknown = runtime.fence_worker(
      fence_request(participant("publisher.absent"), fe::WorkerBootId::generate(), epoch));
  FE_REQUIRE_EQ(unknown.outcome, fe::FenceOutcome::UnknownParticipant);
}

FE_TEST(fencing, a_preventive_fence_is_recorded_but_unknown_participants_are_not) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::ParticipantId id = participant("publisher.preventive");
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  FE_REQUIRE_EQ(runtime.register_participant(registration_request("publisher.preventive", boot, runtime.current_epoch()))
                    .outcome,
                fe::RegistrationOutcome::Registered);
  const fe::WorkerBootId never_seen = fe::WorkerBootId::generate();

  const fe::FenceResult result = runtime.fence_worker(fence_request(id, never_seen, runtime.current_epoch()));
  FE_REQUIRE_EQ(result.outcome, fe::FenceOutcome::Fenced);
  FE_REQUIRE(!result.incarnation.has_value());

  const fe::FenceResult again = runtime.fence_worker(fence_request(id, never_seen, runtime.current_epoch()));
  FE_REQUIRE_EQ(again.outcome, fe::FenceOutcome::AlreadyFenced);

  const fe::RegistrationResult rejected =
      runtime.register_participant(registration_request("publisher.preventive", never_seen, runtime.current_epoch()));
  FE_REQUIRE_EQ(rejected.outcome, fe::RegistrationOutcome::StaleWorkerBoot);
}

FE_TEST(fencing, malformed_and_conflicting_requests_are_rejected) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::ParticipantId id = participant("publisher.fence");
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  FE_REQUIRE_EQ(runtime.register_participant(registration_request("publisher.fence", boot, runtime.current_epoch()))
                    .outcome,
                fe::RegistrationOutcome::Registered);

  fe::FenceWorkerRequest malformed = fence_request(id, boot, runtime.current_epoch());
  malformed.attempt = fe::MutationAttemptId{};
  FE_REQUIRE_EQ(runtime.fence_worker(malformed).outcome, fe::FenceOutcome::MalformedRequest);

  fe::FenceWorkerRequest nil_boot = fence_request(id, boot, runtime.current_epoch());
  nil_boot.boot = fe::WorkerBootId{};
  FE_REQUIRE_EQ(runtime.fence_worker(nil_boot).outcome, fe::FenceOutcome::MalformedRequest);

  fe::FenceWorkerRequest request = fence_request(id, boot, runtime.current_epoch());
  FE_REQUIRE_EQ(runtime.fence_worker(request).outcome, fe::FenceOutcome::Fenced);
  fe::FenceWorkerRequest conflicting = request;
  conflicting.reason = fe::FenceReason::Shutdown;
  FE_REQUIRE_EQ(runtime.fence_worker(conflicting).outcome, fe::FenceOutcome::ConflictingAttempt);
}

FE_TEST(fencing, the_fence_floor_is_monotonic_and_never_reused) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::ParticipantId id = participant("publisher.floor");
  std::vector<fe::FencingGeneration> generations;
  for (int index = 0; index < 5; ++index) {
    const fe::WorkerBootId boot = fe::WorkerBootId::generate();
    FE_REQUIRE_EQ(runtime.register_participant(registration_request("publisher.floor", boot, runtime.current_epoch()))
                      .outcome,
                  fe::RegistrationOutcome::Registered);
    const fe::FenceResult result = runtime.fence_worker(fence_request(id, boot, runtime.current_epoch()));
    FE_REQUIRE_EQ(result.outcome, fe::FenceOutcome::Fenced);
    generations.push_back(*result.fence_generation);
  }
  for (std::size_t index = 1; index < generations.size(); ++index) {
    FE_REQUIRE(generations[index] > generations[index - 1]);
  }
  FE_REQUIRE_EQ(runtime.state().fence_floor, generations.back());
}

FE_TEST(fencing, session_loss_fences_exactly_the_incarnation_that_owned_it) {
  RuntimeFixture fixture;
  fe::EpochRuntimeOptions options = runtime_options(fixture);
  options.allow_multi_incarnation = true;
  fe::EpochRuntime runtime(options);
  const fe::ParticipantId id = participant("publisher.sessions");
  std::vector<fe::SessionBinding> bindings;
  for (int index = 0; index < 3; ++index) {
    fe::RegisterParticipantRequest registration;
    registration.attempt = fe::MutationAttemptId::generate();
    registration.expected_epoch = runtime.current_epoch();
    registration.participant = id;
    registration.boot = fe::WorkerBootId::generate();
    registration.scopes = default_scopes();
    registration.policy = fe::IncarnationPolicy::MultiIncarnation;
    const fe::RegistrationResult registered = runtime.register_participant(registration);
    FE_REQUIRE_EQ(registered.outcome, fe::RegistrationOutcome::Registered);
    fe::SessionBinding binding;
    binding.id = fe::SessionId::generate();
    binding.participant = id;
    binding.boot = registration.boot;
    binding.incarnation = *registered.incarnation;
    binding.epoch = registered.epoch;
    binding.registration_generation = *registered.registration_generation;
    FE_REQUIRE(runtime.bind_session(binding));
    bindings.push_back(binding);
  }

  const fe::FenceResult closed =
      runtime.close_session(bindings[1].id, fe::SessionClosureReason::PeerDisconnect);
  FE_REQUIRE_EQ(closed.outcome, fe::FenceOutcome::Fenced);
  const auto view = runtime.query_participant(id);
  FE_REQUIRE(view.has_value());
  // Exactly one incarnation disappeared; the redundant sessions survive.
  FE_REQUIRE_EQ(view->active_incarnation_count(), std::size_t{2});

  const fe::FenceResult unknown = runtime.close_session(bindings[1].id, fe::SessionClosureReason::PeerDisconnect);
  FE_REQUIRE_EQ(unknown.outcome, fe::FenceOutcome::NoFenceRequired);

  const fe::FenceResult shutdown =
      runtime.close_session(bindings[0].id, fe::SessionClosureReason::ServerShutdown);
  FE_REQUIRE_EQ(shutdown.outcome, fe::FenceOutcome::Fenced);
  const auto reason = runtime.list_fences().back().reason;
  FE_REQUIRE_EQ(reason, fe::FenceReason::Shutdown);
}

FE_TEST(fencing, durable_incarnation_model_keeps_the_incarnation_on_session_loss) {
  RuntimeFixture fixture;
  fe::EpochRuntimeOptions options = runtime_options(fixture);
  options.fence_on_session_loss = false;
  fe::EpochRuntime runtime(options);
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const fe::RegistrationResult registered =
      runtime.register_participant(registration_request("publisher.durablemodel", boot, runtime.current_epoch()));
  fe::SessionBinding binding;
  binding.id = fe::SessionId::generate();
  binding.participant = participant("publisher.durablemodel");
  binding.boot = boot;
  binding.incarnation = *registered.incarnation;
  binding.epoch = registered.epoch;
  binding.registration_generation = *registered.registration_generation;
  FE_REQUIRE(runtime.bind_session(binding));
  const fe::FenceResult closed =
      runtime.close_session(binding.id, fe::SessionClosureReason::PeerDisconnect);
  FE_REQUIRE_EQ(closed.outcome, fe::FenceOutcome::NoFenceRequired);

  fe::ValidateAuthorityRequest request;
  request.token = token_for(registered, binding.participant, boot);
  request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
  FE_REQUIRE_EQ(runtime.validate_authority(request).code, fe::AuthorityValidationCode::Current);
}

FE_TEST(fencing, binding_a_session_to_a_stale_incarnation_is_refused) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const fe::RegistrationResult registered =
      runtime.register_participant(registration_request("publisher.binding", boot, runtime.current_epoch()));

  fe::SessionBinding binding;
  binding.id = fe::SessionId::generate();
  binding.participant = participant("publisher.binding");
  binding.boot = boot;
  binding.incarnation = *registered.incarnation;
  binding.epoch = registered.epoch;
  binding.registration_generation = *registered.registration_generation;

  fe::SessionBinding wrong_epoch = binding;
  wrong_epoch.id = fe::SessionId::generate();
  wrong_epoch.epoch = fe::CoordinatorEpoch(registered.epoch.value() + 1);
  FE_REQUIRE(!runtime.bind_session(wrong_epoch));

  fe::SessionBinding wrong_incarnation = binding;
  wrong_incarnation.id = fe::SessionId::generate();
  wrong_incarnation.incarnation = fe::IncarnationSequence(registered.incarnation->value() + 4);
  FE_REQUIRE(!runtime.bind_session(wrong_incarnation));

  FE_REQUIRE(runtime.bind_session(binding));
  FE_REQUIRE_EQ(runtime.active_sessions().size(), std::size_t{1});
}
