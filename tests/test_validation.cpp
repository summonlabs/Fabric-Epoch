// Fabric Epoch 1.0.0 - Summon Software Labs
#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

namespace {

struct Fixture {
  RuntimeFixture storage;
  fe::EpochRuntime runtime{runtime_options(storage)};
  fe::WorkerBootId boot = fe::WorkerBootId::generate();
  fe::ParticipantId id = participant("publisher.validated");
  fe::RegistrationResult registration;

  Fixture() { registration = runtime.register_participant(registration_request("publisher.validated", boot, runtime.current_epoch())); }
};

}  // namespace

FE_TEST(validation, a_current_token_authorises_its_scope) {
  Fixture fixture;
  fe::ValidateAuthorityRequest request;
  request.token = token_for(fixture.registration, fixture.id, fixture.boot);
  request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
  const fe::AuthorityValidation validation = fixture.runtime.validate_authority(request);
  FE_REQUIRE_EQ(validation.code, fe::AuthorityValidationCode::Current);
  FE_REQUIRE_EQ(validation.currentness, fe::AuthorityCurrentness::Current);
  FE_REQUIRE(validation.authorized());
  FE_REQUIRE_EQ(validation.epoch, fixture.runtime.current_epoch());
  FE_REQUIRE_EQ(validation.registration_generation, fixture.registration.registration_generation);
}

FE_TEST(validation, malformed_tokens_are_reported_as_malformed) {
  Fixture fixture;
  fe::ValidateAuthorityRequest request;
  request.token = token_for(fixture.registration, fixture.id, fixture.boot);
  request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");

  fe::ValidateAuthorityRequest nil_boot = request;
  nil_boot.token.boot = fe::WorkerBootId{};
  FE_REQUIRE_EQ(fixture.runtime.validate_authority(nil_boot).code, fe::AuthorityValidationCode::Malformed);

  fe::ValidateAuthorityRequest zero_incarnation = request;
  zero_incarnation.token.incarnation = fe::IncarnationSequence(0);
  FE_REQUIRE_EQ(fixture.runtime.validate_authority(zero_incarnation).code,
                fe::AuthorityValidationCode::Malformed);
}

FE_TEST(validation, a_stale_epoch_token_is_rejected_with_the_replacing_epoch) {
  Fixture fixture;
  fe::ValidateAuthorityRequest request;
  request.token = token_for(fixture.registration, fixture.id, fixture.boot);
  request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");

  const fe::CoordinatorEpoch old_epoch = fixture.runtime.current_epoch();
  fe::EpochAdvanceRequest advance;
  advance.attempt = fe::MutationAttemptId::generate();
  advance.expected_epoch = old_epoch;
  FE_REQUIRE_EQ(fixture.runtime.advance_epoch(advance).outcome, fe::EpochAdvanceOutcome::Advanced);

  const fe::AuthorityValidation validation = fixture.runtime.validate_authority(request);
  FE_REQUIRE_EQ(validation.code, fe::AuthorityValidationCode::StaleEpoch);
  FE_REQUIRE_EQ(validation.currentness, fe::AuthorityCurrentness::StaleEpoch);
  FE_REQUIRE_EQ(validation.replaced_by_epoch, std::optional<fe::CoordinatorEpoch>(fixture.runtime.current_epoch()));
}

FE_TEST(validation, unknown_participant_and_missing_scope_are_distinguished) {
  Fixture fixture;
  fe::ValidateAuthorityRequest unknown;
  unknown.token.epoch = fixture.runtime.current_epoch();
  unknown.token.participant = participant("publisher.absent");
  unknown.token.boot = fe::WorkerBootId::generate();
  unknown.token.incarnation = fe::IncarnationSequence(1);
  unknown.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
  const fe::AuthorityValidation missing = fixture.runtime.validate_authority(unknown);
  FE_REQUIRE_EQ(missing.code, fe::AuthorityValidationCode::UnknownParticipant);
  FE_REQUIRE_EQ(missing.currentness, fe::AuthorityCurrentness::Unknown);

  fe::ValidateAuthorityRequest out_of_scope;
  out_of_scope.token = token_for(fixture.registration, fixture.id, fixture.boot);
  out_of_scope.scope = scope(fe::ScopeKind::Site, "site-z");
  const fe::AuthorityValidation denied = fixture.runtime.validate_authority(out_of_scope);
  FE_REQUIRE_EQ(denied.code, fe::AuthorityValidationCode::UnauthorizedScope);
  // The incarnation itself is current; only the scope is missing. The two
  // facts are reported separately instead of collapsing into a bool.
  FE_REQUIRE_EQ(denied.currentness, fe::AuthorityCurrentness::Current);
  FE_REQUIRE_EQ(field_text(denied.explanation.render(), "scope"), std::optional<std::string>("SITE:site-z"));
}

FE_TEST(validation, generation_mismatch_asks_for_revalidation_not_rejection) {
  Fixture fixture;
  fe::ValidateAuthorityRequest request;
  request.token = token_for(fixture.registration, fixture.id, fixture.boot);
  request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
  request.expected_generation = fe::RegistrationGeneration(99);
  const fe::AuthorityValidation validation = fixture.runtime.validate_authority(request);
  FE_REQUIRE_EQ(validation.code, fe::AuthorityValidationCode::RevalidationRequired);
  FE_REQUIRE_EQ(validation.currentness, fe::AuthorityCurrentness::RevalidationRequired);
}

FE_TEST(validation, a_superseded_boot_is_fenced_and_names_its_successor) {
  RuntimeFixture storage;
  fe::EpochRuntimeOptions options = runtime_options(storage);
  options.replacement_policy = fe::IncarnationReplacementPolicy::ReplaceIncumbent;
  fe::EpochRuntime runtime(options);
  const fe::WorkerBootId first = fe::WorkerBootId::generate();
  const fe::RegistrationResult initial =
      runtime.register_participant(registration_request("publisher.succession", first, runtime.current_epoch()));
  fe::ValidateAuthorityRequest request;
  request.token = token_for(initial, participant("publisher.succession"), first);
  request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
  FE_REQUIRE_EQ(runtime.validate_authority(request).code, fe::AuthorityValidationCode::Current);

  const fe::WorkerBootId second = fe::WorkerBootId::generate();
  FE_REQUIRE_EQ(runtime
                    .register_participant(
                        registration_request("publisher.succession", second, runtime.current_epoch()))
                    .outcome,
                fe::RegistrationOutcome::Registered);

  const fe::AuthorityValidation superseded = runtime.validate_authority(request);
  FE_REQUIRE_EQ(superseded.code, fe::AuthorityValidationCode::StaleWorkerBoot);
  FE_REQUIRE_EQ(superseded.currentness, fe::AuthorityCurrentness::Fenced);
  FE_REQUIRE_EQ(superseded.superseded_by, std::optional<fe::WorkerBootId>(second));
}

FE_TEST(validation, a_wrong_incarnation_sequence_for_a_live_boot_is_rejected) {
  Fixture fixture;
  fe::ValidateAuthorityRequest request;
  request.token = token_for(fixture.registration, fixture.id, fixture.boot);
  request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
  request.token.incarnation = fe::IncarnationSequence(request.token.incarnation.value() + 5);
  const fe::AuthorityValidation validation = fixture.runtime.validate_authority(request);
  FE_REQUIRE_EQ(validation.code, fe::AuthorityValidationCode::StaleWorkerBoot);
  FE_REQUIRE_EQ(validation.currentness, fe::AuthorityCurrentness::Fenced);
}

FE_TEST(validation, grant_validation_distinguishes_unknown_grant_from_revoked_grant) {
  Fixture fixture;
  fe::GrantAuthorityRequest grant_request;
  grant_request.attempt = fe::MutationAttemptId::generate();
  grant_request.expected_epoch = fixture.runtime.current_epoch();
  grant_request.participant = fixture.id;
  grant_request.boot = fixture.boot;
  grant_request.incarnation = *fixture.registration.incarnation;
  grant_request.scopes = default_scopes();
  const fe::GrantResult granted = fixture.runtime.grant_authority(grant_request);
  FE_REQUIRE_EQ(granted.outcome, fe::GrantOutcome::Granted);

  fe::ValidateAuthorityRequest request;
  request.token = token_for(fixture.registration, fixture.id, fixture.boot);
  request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
  request.grant = granted.grant;
  FE_REQUIRE_EQ(fixture.runtime.validate_authority(request).code, fe::AuthorityValidationCode::Current);

  fe::ValidateAuthorityRequest missing = request;
  missing.grant = fe::AuthorityGrantId::generate();
  FE_REQUIRE_EQ(fixture.runtime.validate_authority(missing).code,
                fe::AuthorityValidationCode::UnknownGrant);

  fe::RevokeAuthorityRequest revoke;
  revoke.attempt = fe::MutationAttemptId::generate();
  revoke.expected_epoch = fixture.runtime.current_epoch();
  revoke.grant = *granted.grant;
  revoke.reason = fe::FenceReason::AdministrativeRevoke;
  FE_REQUIRE_EQ(fixture.runtime.revoke_authority(revoke).outcome, fe::RevokeOutcome::Revoked);

  const fe::AuthorityValidation revoked = fixture.runtime.validate_authority(request);
  FE_REQUIRE_EQ(revoked.code, fe::AuthorityValidationCode::Revoked);
  FE_REQUIRE_EQ(revoked.currentness, fe::AuthorityCurrentness::Revoked);

  // Revoking a grant must not retire the participant identity.
  const auto view = fixture.runtime.query_participant(fixture.id);
  FE_REQUIRE(view.has_value());
  FE_REQUIRE(!view->retired);
}

FE_TEST(validation, explanations_are_available_for_every_rejection) {
  Fixture fixture;
  const fe::Explanation current = fixture.runtime.explain_authority(fixture.id, fixture.boot);
  FE_REQUIRE_EQ(current.code(), fe::ExplanationCode::AuthorityCurrent);
  FE_REQUIRE_EQ(field_text(current.render(), "incarnation"),
                std::optional<std::string>(fixture.registration.incarnation->to_string()));

  const fe::Explanation unknown =
      fixture.runtime.explain_authority(participant("publisher.absent"), fe::WorkerBootId::generate());
  FE_REQUIRE_EQ(unknown.code(), fe::ExplanationCode::ParticipantUnknown);

  const fe::Explanation superseded =
      fixture.runtime.explain_authority(fixture.id, fe::WorkerBootId::generate());
  FE_REQUIRE_EQ(superseded.code(), fe::ExplanationCode::WorkerBootSuperseded);
  FE_REQUIRE_EQ(field_text(superseded.render(), "active_boot"),
                std::optional<std::string>(fixture.boot.to_string()));

  const fe::Explanation epoch = fixture.runtime.explain_epoch();
  FE_REQUIRE_EQ(epoch.code(), fe::ExplanationCode::EpochAdvanced);
  FE_REQUIRE_EQ(field_text(epoch.render(), "current_epoch"),
                std::optional<std::string>(fixture.runtime.current_epoch().to_string()));
}

FE_TEST(validation, identity_alone_never_confers_authority) {
  // The same participant identity, the same durable record and the same
  // executable never make an old incarnation current again.
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  const fe::ParticipantId id = participant("publisher.identity");
  const fe::WorkerBootId first = fe::WorkerBootId::generate();
  const fe::RegistrationResult initial =
      runtime.register_participant(registration_request("publisher.identity", first, runtime.current_epoch()));
  FE_REQUIRE_EQ(initial.outcome, fe::RegistrationOutcome::Registered);

  fe::FenceWorkerRequest fence;
  fence.attempt = fe::MutationAttemptId::generate();
  fence.expected_epoch = runtime.current_epoch();
  fence.participant = id;
  fence.boot = first;
  fence.reason = fe::FenceReason::ProcessLoss;
  FE_REQUIRE_EQ(runtime.fence_worker(fence).outcome, fe::FenceOutcome::Fenced);

  fe::ValidateAuthorityRequest request;
  request.token = token_for(initial, id, first);
  request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
  FE_REQUIRE_EQ(runtime.validate_authority(request).code, fe::AuthorityValidationCode::StaleWorkerBoot);

  // A brand new incarnation of the same participant re-establishes authority.
  const fe::WorkerBootId second = fe::WorkerBootId::generate();
  const fe::RegistrationResult fresh =
      runtime.register_participant(registration_request("publisher.identity", second, runtime.current_epoch()));
  FE_REQUIRE_EQ(fresh.outcome, fe::RegistrationOutcome::Registered);

  fe::ValidateAuthorityRequest fresh_request;
  fresh_request.token = token_for(fresh, id, second);
  fresh_request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
  FE_REQUIRE_EQ(runtime.validate_authority(fresh_request).code, fe::AuthorityValidationCode::Current);
  // The old token stays dead forever.
  FE_REQUIRE_EQ(runtime.validate_authority(request).code, fe::AuthorityValidationCode::StaleWorkerBoot);
}
