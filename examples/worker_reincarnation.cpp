// Fabric Epoch 1.0.0 - Summon Software Labs
// A participant restart is distinguishable from a reconnect.
#include <iostream>

#include "example_support.hpp"

int main() {
  using namespace fabric_epoch;

  example::Workspace workspace;
  EpochRuntime runtime(example::options(workspace.store()));
  const ParticipantId publisher = example::participant("publisher.gamma");

  const WorkerBootId boot_a = WorkerBootId::generate();
  const RegistrationResult a =
      runtime.register_participant(example::registration("publisher.gamma", boot_a, runtime.current_epoch()));
  example::require(a.outcome == RegistrationOutcome::Registered, "Boot A was refused");
  std::cout << "boot_a incarnation=" << a.incarnation->to_string() << std::endl;

  // The process dies; the control path observes the loss and fences Boot A.
  FenceWorkerRequest death;
  death.attempt = MutationAttemptId::generate();
  death.expected_epoch = runtime.current_epoch();
  death.participant = publisher;
  death.boot = boot_a;
  death.reason = FenceReason::ProcessLoss;
  const FenceResult fenced = runtime.fence_worker(death);
  example::require(fenced.outcome == FenceOutcome::Fenced, "Boot A was not fenced");
  const FenceStatus fenced_status = runtime.query_fence(publisher, boot_a);
  example::require(fenced_status.reason == FenceReason::ProcessLoss, "the fence reason was lost");
  std::cout << "boot_a fenced reason=" << to_string(*fenced_status.reason) << std::endl;

  // The publisher restarts as Boot B and must register fresh.
  const WorkerBootId boot_b = WorkerBootId::generate();
  const RegistrationResult b =
      runtime.register_participant(example::registration("publisher.gamma", boot_b, runtime.current_epoch()));
  example::require(b.outcome == RegistrationOutcome::Registered, "Boot B was refused");
  example::require(*b.incarnation > *a.incarnation, "the incarnation counter went backwards");
  std::cout << "boot_b incarnation=" << b.incarnation->to_string() << std::endl;

  // Boot A can never become current again.
  ValidateAuthorityRequest old_token;
  old_token.token.epoch = a.epoch;
  old_token.token.participant = publisher;
  old_token.token.boot = boot_a;
  old_token.token.incarnation = *a.incarnation;
  old_token.scope = example::scope(ScopeKind::Fabric, "fabric-main");
  const AuthorityValidation stale = runtime.validate_authority(old_token);
  example::require(stale.code == AuthorityValidationCode::StaleWorkerBoot, "Boot A came back");
  std::cout << "boot_a now: " << stale.render() << std::endl;

  ValidateAuthorityRequest fresh_token = old_token;
  fresh_token.token.boot = boot_b;
  fresh_token.token.incarnation = *b.incarnation;
  example::require(runtime.validate_authority(fresh_token).code == AuthorityValidationCode::Current,
                   "Boot B does not hold authority");
  return 0;
}
