// Fabric Epoch 1.0.0 - Summon Software Labs
// The cheap deterministic validation primitive and its exact reason codes.
#include <iostream>

#include "example_support.hpp"

int main() {
  using namespace fabric_epoch;

  example::Workspace workspace;
  EpochRuntime runtime(example::options(workspace.store()));

  const WorkerBootId boot = WorkerBootId::generate();
  RegisterParticipantRequest request =
      example::registration("publisher.beta", boot, runtime.current_epoch());
  request.scopes = example::scopes({example::scope(ScopeKind::Site, "site-a")});
  const RegistrationResult registered = runtime.register_participant(request);
  example::require(registered.outcome == RegistrationOutcome::Registered, "registration was refused");

  ValidateAuthorityRequest validation;
  validation.token.epoch = registered.epoch;
  validation.token.participant = request.participant;
  validation.token.boot = boot;
  validation.token.incarnation = *registered.incarnation;
  validation.token.registration_generation = *registered.registration_generation;
  validation.scope = example::scope(ScopeKind::Site, "site-a");

  const AuthorityValidation current = runtime.validate_authority(validation);
  example::require(current.code == AuthorityValidationCode::Current, "a current token was rejected");
  std::cout << "current: " << current.render() << std::endl;

  ValidateAuthorityRequest out_of_scope = validation;
  out_of_scope.scope = example::scope(ScopeKind::Site, "site-b");
  const AuthorityValidation denied = runtime.validate_authority(out_of_scope);
  example::require(denied.code == AuthorityValidationCode::UnauthorizedScope, "scope was not enforced");
  example::require(denied.currentness == AuthorityCurrentness::Current,
                   "the incarnation itself is still current");
  std::cout << "out_of_scope: " << denied.render() << std::endl;
  std::cout << "explanation: " << denied.explanation.render() << std::endl;

  ValidateAuthorityRequest unknown = validation;
  unknown.token.participant = example::participant("publisher.absent");
  example::require(runtime.validate_authority(unknown).code ==
                       AuthorityValidationCode::UnknownParticipant,
                   "unknown participant was not reported");
  return 0;
}
