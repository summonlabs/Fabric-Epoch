// Fabric Epoch 1.0.0 - Summon Software Labs
// Repeated coordinator restarts prove monotonic epoch progression and that no
// pre-restart incarnation becomes live again.
#include <iostream>
#include <vector>

#include "example_support.hpp"

int main() {
  using namespace fabric_epoch;

  example::Workspace workspace;
  std::vector<std::uint64_t> epochs;
  std::vector<WorkerBootId> boots;

  for (int generation = 0; generation < 4; ++generation) {
    EpochRuntime runtime(example::options(workspace.store()));
    epochs.push_back(runtime.current_epoch().value());
    const WorkerBootId boot = WorkerBootId::generate();
    boots.push_back(boot);
    const RegistrationResult registered = runtime.register_participant(
        example::registration("publisher.eta", boot, runtime.current_epoch()));
    example::require(registered.outcome == RegistrationOutcome::Registered, "registration was refused");
    std::cout << "generation=" << generation << " epoch=" << runtime.current_epoch().to_string()
              << " boot=" << boot.to_string() << " incarnation=" << registered.incarnation->to_string()
              << std::endl;
  }

  for (std::size_t index = 1; index < epochs.size(); ++index) {
    example::require(epochs[index] == epochs[index - 1] + 1, "the epoch did not advance by exactly one");
  }

  EpochRuntime final_runtime(example::options(workspace.store()));
  const auto before = final_runtime.query_participant(example::participant("publisher.eta"));
  example::require(before.has_value(), "the durable identity did not survive");
  example::require(!before->has_active_incarnation, "live authority survived a restart");
  example::require(before->revalidation_required, "revalidation was not requested");

  // A pre-restart boot is not live. Re-presenting it cannot restore its old
  // incarnation: any accepted registration allocates a strictly greater
  // incarnation sequence, which is a brand new authority object.
  const IncarnationSequence floor_before = before->incarnation_floor;
  const RegistrationResult attempted = final_runtime.register_participant(
      example::registration("publisher.eta", boots.front(), final_runtime.current_epoch()));
  example::require(attempted.outcome == RegistrationOutcome::Registered ||
                       attempted.outcome == RegistrationOutcome::StaleWorkerBoot,
                   "an impossible outcome was reported");
  if (attempted.outcome == RegistrationOutcome::Registered) {
    example::require(*attempted.incarnation > floor_before,
                     "a pre-restart boot regained its old incarnation");
  }

  // The remaining historical boots cannot displace the live incumbent: the
  // default policy requires an explicit fence first.
  for (std::size_t index = 1; index + 1 < boots.size(); ++index) {
    const RegistrationResult blocked = final_runtime.register_participant(
        example::registration("publisher.eta", boots[index], final_runtime.current_epoch()));
    example::require(blocked.outcome == RegistrationOutcome::ConflictingIncarnation ||
                         blocked.outcome == RegistrationOutcome::StaleWorkerBoot ||
                         blocked.outcome == RegistrationOutcome::AlreadyActive,
                     "a live incumbent was silently displaced");
  }

  const auto view = final_runtime.query_participant(example::participant("publisher.eta"));
  example::require(view.has_value() && !view->retired, "the durable identity did not survive");
  std::cout << "final epoch=" << final_runtime.current_epoch().to_string() << std::endl;
  std::cout << view->render() << std::endl;
  return 0;
}
