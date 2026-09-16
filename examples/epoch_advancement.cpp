// Fabric Epoch 1.0.0 - Summon Software Labs
// Epoch advancement is explicit, transactional and durable before it is
// acknowledged.
#include <iostream>

#include "example_support.hpp"

int main() {
  using namespace fabric_epoch;

  example::Workspace workspace;
  EpochRuntime runtime(example::options(workspace.store()));
  const CoordinatorEpoch before = runtime.current_epoch();
  std::cout << "epoch before=" << before.to_string() << std::endl;

  EpochAdvanceRequest stale;
  stale.attempt = MutationAttemptId::generate();
  stale.expected_epoch = CoordinatorEpoch(before.value() + 4);
  const EpochAdvanceResult refused = runtime.advance_epoch(stale);
  example::require(refused.outcome == EpochAdvanceOutcome::StaleExpectedEpoch,
                   "an unexpected epoch expectation was accepted");
  std::cout << "refused: " << refused.explanation.render() << std::endl;
  example::require(runtime.current_epoch() == before, "a refused advance changed the epoch");

  EpochAdvanceRequest request;
  request.attempt = MutationAttemptId::generate();
  request.expected_epoch = before;
  request.cause = EpochTransitionCause::ExplicitOperatorAdvance;
  const EpochAdvanceResult advanced = runtime.advance_epoch(request);
  example::require(advanced.outcome == EpochAdvanceOutcome::Advanced, "the advance was refused");
  example::require(advanced.epoch.value() == before.value() + 1, "the epoch did not advance by one");
  std::cout << "epoch after=" << advanced.epoch.to_string() << std::endl;

  // Durability precedes acknowledgment: an independent reader sees the new
  // epoch without any flush call.
  StoreOptions store_options;
  store_options.path = workspace.store();
  EpochStore store(store_options);
  example::require(store.load().current_epoch == advanced.epoch,
                   "the acknowledged epoch was not durable");

  const auto history = runtime.transition_history(8);
  example::require(!history.empty(), "no transition history was retained");
  std::cout << "last transition " << history.back().from_epoch.to_string() << " -> "
            << history.back().to_epoch.to_string() << " cause=" << to_string(history.back().cause)
            << std::endl;
  return 0;
}
