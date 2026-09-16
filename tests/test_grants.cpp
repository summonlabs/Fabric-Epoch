// Fabric Epoch 1.0.0 - Summon Software Labs
#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

namespace {

struct GrantFixture {
  RuntimeFixture storage;
  fe::EpochRuntime runtime{runtime_options(storage)};
  fe::WorkerBootId boot = fe::WorkerBootId::generate();
  fe::ParticipantId id = participant("publisher.grantor");
  fe::RegistrationResult registration =
      runtime.register_participant(registration_request(
          "publisher.grantor", boot, runtime.current_epoch(),
          scopes({scope(fe::ScopeKind::Site, "site-a"), scope(fe::ScopeKind::Entity, "node-1"),
                  scope(fe::ScopeKind::Entity, "node-2")})));

  [[nodiscard]] fe::GrantAuthorityRequest request(fe::ScopeSet set) const {
    fe::GrantAuthorityRequest grant;
    grant.attempt = fe::MutationAttemptId::generate();
    grant.expected_epoch = runtime.current_epoch();
    grant.participant = id;
    grant.boot = boot;
    grant.incarnation = *registration.incarnation;
    grant.scopes = std::move(set);
    return grant;
  }
};

}  // namespace

FE_TEST(grants, a_grant_is_bound_to_epoch_participant_and_incarnation) {
  GrantFixture fixture;
  const fe::GrantResult granted =
      fixture.runtime.grant_authority(fixture.request(scopes({scope(fe::ScopeKind::Entity, "node-1")})));
  FE_REQUIRE_EQ(granted.outcome, fe::GrantOutcome::Granted);
  FE_REQUIRE_EQ(granted.epoch, fixture.runtime.current_epoch());
  FE_REQUIRE(granted.grant.has_value());

  const auto grants = fixture.runtime.list_grants();
  FE_REQUIRE_EQ(grants.size(), std::size_t{1});
  FE_REQUIRE_EQ(grants.front().participant, fixture.id);
  FE_REQUIRE_EQ(grants.front().boot, fixture.boot);
  FE_REQUIRE_EQ(grants.front().epoch, fixture.runtime.current_epoch());
  FE_REQUIRE(!grants.front().revoked);
  FE_REQUIRE(!grants.front().delegated);
}

FE_TEST(grants, scope_can_never_exceed_the_registration_binding) {
  GrantFixture fixture;
  const fe::GrantResult denied =
      fixture.runtime.grant_authority(fixture.request(scopes({scope(fe::ScopeKind::Site, "site-z")})));
  FE_REQUIRE_EQ(denied.outcome, fe::GrantOutcome::UnauthorizedScope);
  FE_REQUIRE(fixture.runtime.list_grants().empty());
}

FE_TEST(grants, a_stale_incarnation_can_never_receive_a_grant) {
  GrantFixture fixture;
  fe::GrantAuthorityRequest request = fixture.request(scopes({scope(fe::ScopeKind::Entity, "node-1")}));
  request.incarnation = fe::IncarnationSequence(request.incarnation.value() + 3);
  FE_REQUIRE_EQ(fixture.runtime.grant_authority(request).outcome, fe::GrantOutcome::StaleWorkerBoot);

  fe::GrantAuthorityRequest stale_epoch = fixture.request(scopes({scope(fe::ScopeKind::Entity, "node-1")}));
  stale_epoch.expected_epoch = fe::CoordinatorEpoch(fixture.runtime.current_epoch().value() + 1);
  FE_REQUIRE_EQ(fixture.runtime.grant_authority(stale_epoch).outcome, fe::GrantOutcome::StaleEpoch);
}

FE_TEST(grants, delegation_cannot_exceed_the_parent_scope) {
  GrantFixture fixture;
  const fe::GrantResult parent =
      fixture.runtime.grant_authority(fixture.request(scopes({scope(fe::ScopeKind::Entity, "node-1")})));
  FE_REQUIRE_EQ(parent.outcome, fe::GrantOutcome::Granted);

  fe::GrantAuthorityRequest delegate = fixture.request(scopes({scope(fe::ScopeKind::Entity, "node-1")}));
  delegate.parent = parent.grant;
  const fe::GrantResult child = fixture.runtime.grant_authority(delegate);
  FE_REQUIRE_EQ(child.outcome, fe::GrantOutcome::Granted);
  bool saw_delegated_child = false;
  for (const auto& view : fixture.runtime.list_grants()) {
    if (view.id == *child.grant) {
      saw_delegated_child = view.delegated;
    }
  }
  FE_REQUIRE(saw_delegated_child);

  fe::GrantAuthorityRequest overreach = fixture.request(scopes({scope(fe::ScopeKind::Entity, "node-2")}));
  overreach.parent = parent.grant;
  const fe::GrantResult denied = fixture.runtime.grant_authority(overreach);
  FE_REQUIRE_EQ(denied.outcome, fe::GrantOutcome::DelegationRejected);
  FE_REQUIRE_EQ(field_text(denied.explanation.render(), "reason"),
                std::optional<std::string>("delegated"));

  fe::GrantAuthorityRequest orphan = fixture.request(scopes({scope(fe::ScopeKind::Entity, "node-1")}));
  orphan.parent = fe::AuthorityGrantId::generate();
  FE_REQUIRE_EQ(fixture.runtime.grant_authority(orphan).outcome, fe::GrantOutcome::DelegationRejected);
}

FE_TEST(grants, revocation_cascades_to_derived_grants_and_is_idempotent) {
  GrantFixture fixture;
  const fe::GrantResult parent =
      fixture.runtime.grant_authority(fixture.request(scopes({scope(fe::ScopeKind::Entity, "node-1")})));
  fe::GrantAuthorityRequest delegate = fixture.request(scopes({scope(fe::ScopeKind::Entity, "node-1")}));
  delegate.parent = parent.grant;
  const fe::GrantResult child = fixture.runtime.grant_authority(delegate);
  FE_REQUIRE_EQ(child.outcome, fe::GrantOutcome::Granted);

  fe::RevokeAuthorityRequest revoke;
  revoke.attempt = fe::MutationAttemptId::generate();
  revoke.expected_epoch = fixture.runtime.current_epoch();
  revoke.grant = *parent.grant;
  revoke.reason = fe::FenceReason::AdministrativeRevoke;
  const fe::RevokeResult revoked = fixture.runtime.revoke_authority(revoke);
  FE_REQUIRE_EQ(revoked.outcome, fe::RevokeOutcome::Revoked);
  FE_REQUIRE_EQ(revoked.derived_grants_revoked, std::size_t{1});

  for (const auto& grant : fixture.runtime.list_grants()) {
    FE_REQUIRE(grant.revoked);
  }

  const fe::RevokeResult again = fixture.runtime.revoke_authority(revoke);
  FE_REQUIRE_EQ(again.outcome, fe::RevokeOutcome::Revoked);
  FE_REQUIRE_EQ(again.derived_grants_revoked, std::size_t{1});

  fe::RevokeAuthorityRequest unknown = revoke;
  unknown.attempt = fe::MutationAttemptId::generate();
  unknown.grant = fe::AuthorityGrantId::generate();
  FE_REQUIRE_EQ(fixture.runtime.revoke_authority(unknown).outcome, fe::RevokeOutcome::UnknownGrant);

  // Revocation is durably recorded and survives a restart.
  fe::EpochRuntime restarted(runtime_options(fixture.storage));
  for (const auto& grant : restarted.list_grants()) {
    FE_REQUIRE(grant.revoked);
  }
}

FE_TEST(grants, a_revoked_parent_blocks_further_delegation) {
  GrantFixture fixture;
  const fe::GrantResult parent =
      fixture.runtime.grant_authority(fixture.request(scopes({scope(fe::ScopeKind::Entity, "node-1")})));
  fe::RevokeAuthorityRequest revoke;
  revoke.attempt = fe::MutationAttemptId::generate();
  revoke.expected_epoch = fixture.runtime.current_epoch();
  revoke.grant = *parent.grant;
  FE_REQUIRE_EQ(fixture.runtime.revoke_authority(revoke).outcome, fe::RevokeOutcome::Revoked);

  fe::GrantAuthorityRequest delegate = fixture.request(scopes({scope(fe::ScopeKind::Entity, "node-1")}));
  delegate.parent = parent.grant;
  FE_REQUIRE_EQ(fixture.runtime.grant_authority(delegate).outcome, fe::GrantOutcome::DelegationRejected);
}

FE_TEST(grants, grant_and_revoke_replay_exactly) {
  GrantFixture fixture;
  const fe::GrantAuthorityRequest request =
      fixture.request(scopes({scope(fe::ScopeKind::Entity, "node-1")}));
  const fe::GrantResult first = fixture.runtime.grant_authority(request);
  FE_REQUIRE_EQ(first.outcome, fe::GrantOutcome::Granted);
  const fe::EpochGeneration generation = fixture.runtime.generation();
  const fe::GrantResult replay = fixture.runtime.grant_authority(request);
  FE_REQUIRE(replay.replayed);
  FE_REQUIRE_EQ(replay.grant, first.grant);
  FE_REQUIRE_EQ(fixture.runtime.generation(), generation);

  fe::RevokeAuthorityRequest revoke;
  revoke.attempt = fe::MutationAttemptId::generate();
  revoke.expected_epoch = fixture.runtime.current_epoch();
  revoke.grant = *first.grant;
  FE_REQUIRE_EQ(fixture.runtime.revoke_authority(revoke).outcome, fe::RevokeOutcome::Revoked);
  const fe::EpochGeneration after_revoke = fixture.runtime.generation();
  FE_REQUIRE(after_revoke > generation);
  const fe::RevokeResult revoke_replay = fixture.runtime.revoke_authority(revoke);
  FE_REQUIRE_EQ(revoke_replay.outcome, fe::RevokeOutcome::Revoked);
  FE_REQUIRE_EQ(fixture.runtime.generation(), after_revoke);
}

FE_TEST(grants, an_epoch_advance_makes_every_grant_stale) {
  GrantFixture fixture;
  const fe::GrantResult granted =
      fixture.runtime.grant_authority(fixture.request(scopes({scope(fe::ScopeKind::Entity, "node-1")})));
  FE_REQUIRE_EQ(granted.outcome, fe::GrantOutcome::Granted);

  fe::EpochAdvanceRequest advance;
  advance.attempt = fe::MutationAttemptId::generate();
  advance.expected_epoch = fixture.runtime.current_epoch();
  FE_REQUIRE_EQ(fixture.runtime.advance_epoch(advance).outcome, fe::EpochAdvanceOutcome::Advanced);

  fe::ValidateAuthorityRequest request;
  request.token = token_for(fixture.registration, fixture.id, fixture.boot);
  request.token.epoch = granted.epoch;
  request.scope = scope(fe::ScopeKind::Entity, "node-1");
  request.grant = granted.grant;
  const fe::AuthorityValidation validation = fixture.runtime.validate_authority(request);
  FE_REQUIRE_EQ(validation.code, fe::AuthorityValidationCode::StaleEpoch);
}
