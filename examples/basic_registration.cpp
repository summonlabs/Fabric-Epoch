// Fabric Epoch 1.0.0 - Summon Software Labs
// Basic participant registration against the durable epoch runtime.
#include <iostream>

#include "example_support.hpp"

int main() {
  using namespace fabric_epoch;

  example::Workspace workspace;
  EpochRuntime runtime(example::options(workspace.store()));

  std::cout << "epoch=" << runtime.current_epoch().to_string()
            << " coordinator_boot=" << runtime.coordinator_boot().to_string() << std::endl;

  const WorkerBootId boot = WorkerBootId::generate();
  RegisterParticipantRequest request =
      example::registration("publisher.alpha", boot, runtime.current_epoch());
  const RegistrationResult result = runtime.register_participant(request);

  example::require(result.outcome == RegistrationOutcome::Registered, "registration was refused");
  std::cout << "registration outcome=" << to_string(result.outcome)
            << " incarnation=" << result.incarnation->to_string()
            << " registration_generation=" << result.registration_generation->to_string() << std::endl;

  // Replaying the same semantic request is idempotent: it never creates a
  // second incarnation and never advances any generation.
  const RegistrationResult replay = runtime.register_participant(request);
  example::require(replay.outcome == RegistrationOutcome::Registered, "replay was not idempotent");
  example::require(replay.replayed, "the replay was not recognised");
  example::require(replay.incarnation == result.incarnation, "the replay changed the incarnation");

  const auto view = runtime.query_participant(request.participant);
  example::require(view.has_value(), "the participant is not present");
  std::cout << view->render() << std::endl;
  return 0;
}
