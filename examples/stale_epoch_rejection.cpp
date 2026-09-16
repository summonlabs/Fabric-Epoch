// Fabric Epoch 1.0.0 - Summon Software Labs
// A coordinator restart invalidates every previous epoch's authority.
#include <iostream>

#include "example_support.hpp"

int main() {
  using namespace fabric_epoch;

  example::Workspace workspace;
  const WorkerBootId boot = WorkerBootId::generate();
  RegistrationResult registered;
  CoordinatorEpoch first_epoch(0);
  {
    EpochRuntime runtime(example::options(workspace.store()));
    first_epoch = runtime.current_epoch();
    registered = runtime.register_participant(
        example::registration("publisher.epsilon", boot, first_epoch));
    example::require(registered.outcome == RegistrationOutcome::Registered, "registration was refused");
    std::cout << "epoch=" << first_epoch.to_string() << " token accepted" << std::endl;
  }

  // A fresh coordinator process over the same durable store.
  EpochRuntime restarted(example::options(workspace.store()));
  const CoordinatorEpoch second_epoch = restarted.current_epoch();
  example::require(second_epoch > first_epoch, "the epoch did not advance across the restart");
  std::cout << "restarted epoch=" << second_epoch.to_string() << std::endl;

  ValidateAuthorityRequest stale;
  stale.token.epoch = registered.epoch;
  stale.token.participant = example::participant("publisher.epsilon");
  stale.token.boot = boot;
  stale.token.incarnation = *registered.incarnation;
  stale.scope = example::scope(ScopeKind::Fabric, "fabric-main");
  const AuthorityValidation denied = restarted.validate_authority(stale);
  example::require(denied.code == AuthorityValidationCode::StaleEpoch, "old epoch traffic was accepted");
  std::cout << "stale: " << denied.render() << std::endl;
  std::cout << "explanation: " << denied.explanation.render() << std::endl;

  // The durable participant identity survived; only live authority did not.
  const auto view = restarted.query_participant(example::participant("publisher.epsilon"));
  example::require(view.has_value() && !view->retired, "the durable identity did not survive");
  example::require(!view->has_active_incarnation, "live authority survived a restart");
  example::require(view->revalidation_required, "revalidation was not requested");
  std::cout << view->render() << std::endl;

  // A fresh incarnation re-establishes authority without deleting identity.
  const WorkerBootId fresh = WorkerBootId::generate();
  example::require(restarted.register_participant(
                       example::registration("publisher.epsilon", fresh, second_epoch))
                       .outcome == RegistrationOutcome::Registered,
                   "revalidation was refused");
  return 0;
}
