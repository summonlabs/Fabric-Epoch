// Fabric Epoch 1.0.0 - Summon Software Labs
// A fenced WorkerBootId is rejected permanently, across a restart.
#include <iostream>

#include "example_support.hpp"

int main() {
  using namespace fabric_epoch;

  example::Workspace workspace;
  const ParticipantId publisher = example::participant("publisher.zeta");
  const WorkerBootId boot = WorkerBootId::generate();
  CoordinatorEpoch fenced_epoch(0);
  {
    EpochRuntime runtime(example::options(workspace.store()));
    fenced_epoch = runtime.current_epoch();
    example::require(runtime.register_participant(example::registration("publisher.zeta", boot, fenced_epoch))
                         .outcome == RegistrationOutcome::Registered,
                     "registration was refused");
    FenceWorkerRequest fence;
    fence.attempt = MutationAttemptId::generate();
    fence.expected_epoch = fenced_epoch;
    fence.participant = publisher;
    fence.boot = boot;
    fence.reason = FenceReason::AdministrativeRevoke;
    example::require(runtime.fence_worker(fence).outcome == FenceOutcome::Fenced, "the fence failed");
  }

  EpochRuntime restarted(example::options(workspace.store()));
  std::cout << "restarted epoch=" << restarted.current_epoch().to_string() << std::endl;

  const RegistrationResult refused = restarted.register_participant(
      example::registration("publisher.zeta", boot, restarted.current_epoch()));
  example::require(refused.outcome == RegistrationOutcome::StaleWorkerBoot,
                   "a fenced boot was accepted after a restart");
  std::cout << "registration: " << refused.explanation.render() << std::endl;

  const FenceStatus status = restarted.query_fence(publisher, boot);
  example::require(status.fenced, "the fence did not survive the restart");
  example::require(status.reason == FenceReason::AdministrativeRevoke, "the fence reason was lost");
  std::cout << status.render() << std::endl;

  const Explanation explanation = restarted.explain_authority(publisher, boot);
  example::require(explanation.code() == ExplanationCode::WorkerBootFenced, "the explanation is wrong");
  std::cout << "explanation: " << explanation.render() << std::endl;
  return 0;
}
