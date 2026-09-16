// Fabric Epoch 1.0.0 - Summon Software Labs
// Fencing is permanent for the targeted incarnation and idempotent.
#include <iostream>

#include "example_support.hpp"

int main() {
  using namespace fabric_epoch;

  example::Workspace workspace;
  EpochRuntime runtime(example::options(workspace.store()));
  const ParticipantId publisher = example::participant("publisher.delta");

  const WorkerBootId boot = WorkerBootId::generate();
  const RegistrationResult registered =
      runtime.register_participant(example::registration("publisher.delta", boot, runtime.current_epoch()));
  example::require(registered.outcome == RegistrationOutcome::Registered, "registration was refused");

  FenceWorkerRequest request;
  request.attempt = MutationAttemptId::generate();
  request.expected_epoch = runtime.current_epoch();
  request.participant = publisher;
  request.boot = boot;
  request.reason = FenceReason::ExplicitOperatorFence;
  request.note = BoundedText::from_validated("operator fence during maintenance");

  const FenceResult first = runtime.fence_worker(request);
  example::require(first.outcome == FenceOutcome::Fenced, "the fence was not applied");
  const EpochGeneration generation = runtime.generation();
  std::cout << "fence generation=" << first.fence_generation->to_string() << std::endl;

  // Repeating the fence is idempotent: no new record, no new generation.
  FenceWorkerRequest repeat = request;
  repeat.attempt = MutationAttemptId::generate();
  const FenceResult again = runtime.fence_worker(repeat);
  example::require(again.outcome == FenceOutcome::AlreadyFenced, "the fence was not idempotent");
  example::require(runtime.generation() == generation, "an idempotent fence advanced a generation");
  std::cout << "repeat outcome=" << to_string(again.outcome) << std::endl;

  const FenceStatus status = runtime.query_fence(publisher, boot);
  example::require(status.fenced, "the fence is not visible");
  std::cout << status.render() << std::endl;

  // The participant identity survives: a fresh incarnation may re-establish
  // authority, but the fenced boot never returns.
  const WorkerBootId fresh = WorkerBootId::generate();
  example::require(runtime.register_participant(
                       example::registration("publisher.delta", fresh, runtime.current_epoch()))
                       .outcome == RegistrationOutcome::Registered,
                   "a fresh incarnation was refused");
  example::require(runtime.register_participant(
                       example::registration("publisher.delta", boot, runtime.current_epoch()))
                       .outcome == RegistrationOutcome::StaleWorkerBoot,
                   "the fenced boot registered again");
  return 0;
}
