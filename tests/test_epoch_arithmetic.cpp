// Fabric Epoch 1.0.0 - Summon Software Labs
#include <string>

#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

namespace {

/// Builds a durable image with an explicit epoch so boundary arithmetic can be
/// exercised without iterating the counter.
void write_store_with_epoch(const std::filesystem::path& path, fe::CoordinatorEpoch epoch,
                            fe::EpochGeneration generation) {
  fe::StoreOptions options;
  options.path = path;
  fe::EpochStore store(options);
  fe::DurableState state;
  state.current_epoch = epoch;
  state.coordinator_boot = fe::CoordinatorBootId::generate();
  state.generation = generation;
  state.registration_floor = fe::RegistrationGeneration(0);
  state.fence_floor = fe::FencingGeneration(0);
  store.save(state);
}

}  // namespace

FE_TEST(epoch_arithmetic, monotonic_progression_across_restarts) {
  RuntimeFixture fixture;
  std::vector<std::uint64_t> observed;
  for (int restart = 0; restart < 6; ++restart) {
    fe::EpochRuntime runtime(runtime_options(fixture));
    observed.push_back(runtime.current_epoch().value());
  }
  FE_REQUIRE_EQ(observed.size(), std::size_t{6});
  for (std::size_t i = 1; i < observed.size(); ++i) {
    FE_REQUIRE_MSG(observed[i] == observed[i - 1] + 1,
                   std::to_string(observed[i - 1]) + " -> " + std::to_string(observed[i]));
  }
  FE_REQUIRE_EQ(observed.front(), std::uint64_t{1});
}

FE_TEST(epoch_arithmetic, opening_an_unestablished_store_without_advancing_is_rejected) {
  RuntimeFixture fixture;
  fe::EpochRuntimeOptions options = runtime_options(fixture);
  options.advance_epoch_on_open = false;
  expect_runtime_construction_fails(options);
}

FE_TEST(epoch_arithmetic, explicit_advance_requires_the_exact_expected_epoch) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::CoordinatorEpoch epoch = runtime.current_epoch();

  fe::EpochAdvanceRequest stale;
  stale.attempt = fe::MutationAttemptId::generate();
  stale.expected_epoch = fe::CoordinatorEpoch(epoch.value() - 1);
  const fe::EpochAdvanceResult rejected = runtime.advance_epoch(stale);
  FE_REQUIRE_EQ(rejected.outcome, fe::EpochAdvanceOutcome::StaleExpectedEpoch);
  FE_REQUIRE_EQ(runtime.current_epoch(), epoch);

  fe::EpochAdvanceRequest request;
  request.attempt = fe::MutationAttemptId::generate();
  request.expected_epoch = epoch;
  request.cause = fe::EpochTransitionCause::ExplicitOperatorAdvance;
  const fe::EpochAdvanceResult advanced = runtime.advance_epoch(request);
  FE_REQUIRE_EQ(advanced.outcome, fe::EpochAdvanceOutcome::Advanced);
  FE_REQUIRE_EQ(advanced.epoch.value(), epoch.value() + 1);
  FE_REQUIRE_EQ(runtime.current_epoch().value(), epoch.value() + 1);
}

FE_TEST(epoch_arithmetic, explicit_advance_is_idempotent_and_conflict_checked) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));

  fe::EpochAdvanceRequest request;
  request.attempt = fe::MutationAttemptId::generate();
  request.expected_epoch = runtime.current_epoch();
  const fe::EpochAdvanceResult first = runtime.advance_epoch(request);
  FE_REQUIRE_EQ(first.outcome, fe::EpochAdvanceOutcome::Advanced);

  const fe::EpochAdvanceResult replay = runtime.advance_epoch(request);
  FE_REQUIRE_EQ(replay.outcome, fe::EpochAdvanceOutcome::Advanced);
  FE_REQUIRE(replay.replayed);
  FE_REQUIRE_EQ(replay.epoch, first.epoch);
  FE_REQUIRE_EQ(runtime.current_epoch(), first.epoch);

  fe::EpochAdvanceRequest conflicting = request;
  conflicting.cause = fe::EpochTransitionCause::DurableRecovery;
  const fe::EpochAdvanceResult conflict = runtime.advance_epoch(conflicting);
  FE_REQUIRE_EQ(conflict.outcome, fe::EpochAdvanceOutcome::ConflictingAttempt);
  FE_REQUIRE_EQ(runtime.current_epoch(), first.epoch);
}

FE_TEST(epoch_arithmetic, maximum_epoch_is_terminal_and_never_wraps) {
  RuntimeFixture fixture;
  write_store_with_epoch(fixture.store_path, fe::CoordinatorEpoch(fe::CoordinatorEpoch::max_value),
                         fe::EpochGeneration(7));

  fe::EpochRuntimeOptions options = runtime_options(fixture);
  options.advance_epoch_on_open = false;
  fe::EpochRuntime runtime(options);
  FE_REQUIRE(runtime.current_epoch().is_max());

  fe::EpochAdvanceRequest request;
  request.attempt = fe::MutationAttemptId::generate();
  request.expected_epoch = runtime.current_epoch();
  const fe::EpochAdvanceResult result = runtime.advance_epoch(request);
  FE_REQUIRE_EQ(result.outcome, fe::EpochAdvanceOutcome::EpochExhausted);
  FE_REQUIRE_EQ(runtime.current_epoch().value(), fe::CoordinatorEpoch::max_value);
  FE_REQUIRE(!runtime.current_epoch().is_zero());
}

FE_TEST(epoch_arithmetic, a_fresh_coordinator_process_cannot_open_an_exhausted_epoch) {
  RuntimeFixture fixture;
  write_store_with_epoch(fixture.store_path, fe::CoordinatorEpoch(fe::CoordinatorEpoch::max_value),
                         fe::EpochGeneration(7));
  expect_runtime_construction_fails(runtime_options(fixture));

  fe::StoreOptions options;
  options.path = fixture.store_path;
  fe::EpochStore store(options);
  FE_REQUIRE_EQ(store.load().current_epoch.value(), fe::CoordinatorEpoch::max_value);
}

FE_TEST(epoch_arithmetic, generation_counter_advances_on_every_committed_mutation) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::EpochGeneration after_open = runtime.generation();

  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const fe::RegistrationResult result = runtime.register_participant(
      registration_request("publisher.delta", boot, runtime.current_epoch()));
  FE_REQUIRE_EQ(result.outcome, fe::RegistrationOutcome::Registered);
  const fe::EpochGeneration after_registration = runtime.generation();
  FE_REQUIRE(after_registration > after_open);

  // A rejected request must not advance the durable generation.
  fe::RegisterParticipantRequest rejected =
      registration_request("publisher.delta", fe::WorkerBootId::generate(),
                           fe::CoordinatorEpoch(runtime.current_epoch().value() - 1));
  FE_REQUIRE_EQ(runtime.register_participant(rejected).outcome, fe::RegistrationOutcome::StaleEpoch);
  FE_REQUIRE_EQ(runtime.generation(), after_registration);
}
