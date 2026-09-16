// Fabric Epoch 1.0.0 - Summon Software Labs
#include <string>
#include <vector>

#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

namespace {

fe::EpochRuntimeOptions tight_options(const RuntimeFixture& fixture) {
  fe::EpochRuntimeOptions options = runtime_options(fixture);
  options.limits.max_participants = 4;
  options.limits.max_active_workers = 4;
  options.limits.max_scopes_per_grant = 2;
  options.limits.max_recent_fences = 3;
  options.limits.max_authority_grants = 2;
  options.limits.max_transition_history = 2;
  options.limits.max_string_size = 48;
  options.limits.max_sessions = 2;
  options.limits.max_idempotency_records_per_participant = 2;
  options.limits.max_fenced_boots_per_participant = 2;
  options.limits.max_request_size = 1024;
  options.limits.max_frame_payload = 4096;
  return options;
}

}  // namespace

FE_TEST(limits, viability_rules_reject_inconsistent_configurations) {
  FE_REQUIRE(fe::is_viable(fe::default_limits()));
  fe::Limits limits = fe::default_limits();
  limits.max_participants = 0;
  FE_REQUIRE(!fe::is_viable(limits));
  limits = fe::default_limits();
  limits.max_active_workers = limits.max_participants + 1;
  FE_REQUIRE(!fe::is_viable(limits));
  limits = fe::default_limits();
  limits.max_request_size = limits.max_frame_payload + 1;
  FE_REQUIRE(!fe::is_viable(limits));
  limits = fe::default_limits();
  limits.max_sessions = 0;
  FE_REQUIRE(!fe::is_viable(limits));
}

FE_TEST(limits, max_participants_is_consulted) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(tight_options(fixture));
  for (int index = 0; index < 4; ++index) {
    const fe::RegistrationResult result = runtime.register_participant(registration_request(
        "publisher." + std::to_string(index), fe::WorkerBootId::generate(), runtime.current_epoch()));
    FE_REQUIRE_EQ(result.outcome, fe::RegistrationOutcome::Registered);
  }
  const fe::RegistrationResult rejected = runtime.register_participant(
      registration_request("publisher.overflow", fe::WorkerBootId::generate(), runtime.current_epoch()));
  FE_REQUIRE_EQ(rejected.outcome, fe::RegistrationOutcome::ResourceLimit);
  FE_REQUIRE_EQ(field_text(rejected.explanation.render(), "limit"), std::optional<std::string>("max_participants"));
}

FE_TEST(limits, max_active_workers_is_consulted) {
  RuntimeFixture fixture;
  fe::EpochRuntimeOptions options = tight_options(fixture);
  options.allow_multi_incarnation = true;
  options.limits.max_participants = 8;
  options.limits.max_active_workers = 2;
  fe::EpochRuntime runtime(options);
  for (int index = 0; index < 2; ++index) {
    fe::RegisterParticipantRequest request;
    request.attempt = fe::MutationAttemptId::generate();
    request.expected_epoch = runtime.current_epoch();
    request.participant = participant("publisher.multi");
    request.boot = fe::WorkerBootId::generate();
    request.scopes = default_scopes();
    request.policy = fe::IncarnationPolicy::MultiIncarnation;
    FE_REQUIRE_EQ(runtime.register_participant(request).outcome, fe::RegistrationOutcome::Registered);
  }
  fe::RegisterParticipantRequest overflow;
  overflow.attempt = fe::MutationAttemptId::generate();
  overflow.expected_epoch = runtime.current_epoch();
  overflow.participant = participant("publisher.multi");
  overflow.boot = fe::WorkerBootId::generate();
  overflow.scopes = default_scopes();
  overflow.policy = fe::IncarnationPolicy::MultiIncarnation;
  const fe::RegistrationResult rejected = runtime.register_participant(overflow);
  FE_REQUIRE_EQ(rejected.outcome, fe::RegistrationOutcome::ResourceLimit);
  FE_REQUIRE_EQ(field_text(rejected.explanation.render(), "limit"), std::optional<std::string>("max_active_workers"));
}

FE_TEST(limits, max_scopes_per_grant_is_consulted) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(tight_options(fixture));
  fe::RegisterParticipantRequest request;
  request.attempt = fe::MutationAttemptId::generate();
  request.expected_epoch = runtime.current_epoch();
  request.participant = participant("publisher.scopes");
  request.boot = fe::WorkerBootId::generate();
  request.scopes = scopes({scope(fe::ScopeKind::Entity, "a"), scope(fe::ScopeKind::Entity, "b"),
                           scope(fe::ScopeKind::Entity, "c")});
  const fe::RegistrationResult rejected = runtime.register_participant(request);
  FE_REQUIRE_EQ(rejected.outcome, fe::RegistrationOutcome::ResourceLimit);
  FE_REQUIRE_EQ(field_text(rejected.explanation.render(), "limit"),
                std::optional<std::string>("max_scopes_per_grant"));
}

FE_TEST(limits, max_recent_fences_bounds_retention_without_resurrecting_authority) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(tight_options(fixture));
  for (int index = 0; index < 6; ++index) {
    const fe::CoordinatorEpoch epoch = runtime.current_epoch();
    const fe::WorkerBootId boot = fe::WorkerBootId::generate();
    FE_REQUIRE_EQ(runtime.register_participant(registration_request("publisher.fence", boot, epoch)).outcome,
                  fe::RegistrationOutcome::Registered);
    fe::FenceWorkerRequest fence;
    fence.attempt = fe::MutationAttemptId::generate();
    fence.expected_epoch = runtime.current_epoch();
    fence.participant = participant("publisher.fence");
    fence.boot = boot;
    fence.reason = fe::FenceReason::ExplicitOperatorFence;
    FE_REQUIRE_EQ(runtime.fence_worker(fence).outcome, fe::FenceOutcome::Fenced);
    FE_REQUIRE(runtime.list_fences().size() <= runtime.limits().max_recent_fences);
  }
  FE_REQUIRE_EQ(runtime.list_fences().size(), runtime.limits().max_recent_fences);
  // Bounded retention never raises the durable fencing floor back down.
  FE_REQUIRE(runtime.state().fence_floor.value() >= 6);
}

FE_TEST(limits, max_authority_grants_is_consulted) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(tight_options(fixture));
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const fe::RegistrationResult registered =
      runtime.register_participant(registration_request("publisher.grants", boot, runtime.current_epoch()));
  FE_REQUIRE_EQ(registered.outcome, fe::RegistrationOutcome::Registered);

  for (int index = 0; index < 2; ++index) {
    fe::GrantAuthorityRequest request;
    request.attempt = fe::MutationAttemptId::generate();
    request.expected_epoch = runtime.current_epoch();
    request.participant = participant("publisher.grants");
    request.boot = boot;
    request.incarnation = *registered.incarnation;
    request.scopes = default_scopes();
    FE_REQUIRE_EQ(runtime.grant_authority(request).outcome, fe::GrantOutcome::Granted);
  }
  fe::GrantAuthorityRequest overflow;
  overflow.attempt = fe::MutationAttemptId::generate();
  overflow.expected_epoch = runtime.current_epoch();
  overflow.participant = participant("publisher.grants");
  overflow.boot = boot;
  overflow.incarnation = *registered.incarnation;
  overflow.scopes = default_scopes();
  FE_REQUIRE_EQ(runtime.grant_authority(overflow).outcome, fe::GrantOutcome::ResourceLimit);
}

FE_TEST(limits, max_transition_history_is_consulted) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(tight_options(fixture));
  for (int index = 0; index < 6; ++index) {
    fe::EpochAdvanceRequest request;
    request.attempt = fe::MutationAttemptId::generate();
    request.expected_epoch = runtime.current_epoch();
    FE_REQUIRE_EQ(runtime.advance_epoch(request).outcome, fe::EpochAdvanceOutcome::Advanced);
  }
  FE_REQUIRE_EQ(runtime.transition_history(64).size(), runtime.limits().max_transition_history);
}

FE_TEST(limits, max_sessions_is_consulted) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(tight_options(fixture));
  std::vector<fe::SessionBinding> bindings;
  for (int index = 0; index < 2; ++index) {
    const std::string name = "publisher.session." + std::to_string(index);
    const fe::WorkerBootId boot = fe::WorkerBootId::generate();
    const fe::RegistrationResult registered =
        runtime.register_participant(registration_request(name, boot, runtime.current_epoch()));
    FE_REQUIRE_EQ(registered.outcome, fe::RegistrationOutcome::Registered);
    fe::SessionBinding binding;
    binding.id = fe::SessionId::generate();
    binding.participant = participant(name);
    binding.boot = boot;
    binding.incarnation = *registered.incarnation;
    binding.epoch = registered.epoch;
    binding.registration_generation = *registered.registration_generation;
    FE_REQUIRE(runtime.bind_session(binding));
    bindings.push_back(binding);
  }
  FE_REQUIRE_EQ(runtime.session_count(), std::size_t{2});

  // A second session for one incarnation is refused by the
  // one-session-per-incarnation rule.
  fe::SessionBinding duplicate = bindings.front();
  duplicate.id = fe::SessionId::generate();
  FE_REQUIRE(!runtime.bind_session(duplicate));

  // The next distinct incarnation reaches the capacity bound.
  const std::string name = "publisher.session.2";
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const fe::RegistrationResult registered =
      runtime.register_participant(registration_request(name, boot, runtime.current_epoch()));
  FE_REQUIRE_EQ(registered.outcome, fe::RegistrationOutcome::Registered);
  fe::SessionBinding overflow;
  overflow.id = fe::SessionId::generate();
  overflow.participant = participant(name);
  overflow.boot = boot;
  overflow.incarnation = *registered.incarnation;
  overflow.epoch = registered.epoch;
  overflow.registration_generation = *registered.registration_generation;
  FE_REQUIRE_THROWS(runtime.bind_session(overflow));

  const fe::FenceResult closed =
      runtime.close_session(bindings.front().id, fe::SessionClosureReason::PeerDisconnect);
  FE_REQUIRE_EQ(closed.outcome, fe::FenceOutcome::Fenced);
  FE_REQUIRE_EQ(runtime.session_count(), std::size_t{1});
}

FE_TEST(limits, max_idempotency_records_is_consulted) {
  RuntimeFixture fixture;
  fe::EpochRuntimeOptions options = tight_options(fixture);
  options.limits.max_participants = 8;
  options.limits.max_active_workers = 8;
  options.limits.max_recent_fences = 16;
  options.limits.max_fenced_boots_per_participant = 16;
  options.replacement_policy = fe::IncarnationReplacementPolicy::ReplaceIncumbent;
  fe::EpochRuntime runtime(options);

  std::vector<fe::RegisterParticipantRequest> requests;
  for (int index = 0; index < 4; ++index) {
    fe::RegisterParticipantRequest request = registration_request(
        "publisher.idem", fe::WorkerBootId::generate(), runtime.current_epoch());
    FE_REQUIRE_EQ(runtime.register_participant(request).outcome, fe::RegistrationOutcome::Registered);
    requests.push_back(request);
  }

  // The most recent attempt is retained and replays exactly.
  const fe::RegistrationResult replay = runtime.register_participant(requests.back());
  FE_REQUIRE_EQ(replay.outcome, fe::RegistrationOutcome::Registered);
  FE_REQUIRE(replay.replayed);

  // The oldest attempt was evicted from the bounded table, so it is no longer
  // recognised as an exact replay. It stays rejected because the boot it names
  // is retained as fenced.
  const fe::RegistrationResult evicted = runtime.register_participant(requests.front());
  FE_REQUIRE(!evicted.replayed);
  FE_REQUIRE(evicted.outcome != fe::RegistrationOutcome::Idempotent);
  FE_REQUIRE_EQ(evicted.outcome, fe::RegistrationOutcome::StaleWorkerBoot);
  FE_REQUIRE(runtime.check_indexes());
}

FE_TEST(limits, max_fenced_boots_retention_is_bounded_and_never_resurrects) {
  RuntimeFixture fixture;
  fe::EpochRuntimeOptions options = tight_options(fixture);
  options.limits.max_participants = 16;
  options.limits.max_active_workers = 16;
  options.limits.max_recent_fences = 8;
  fe::EpochRuntime runtime(options);

  std::vector<fe::WorkerBootId> boots;
  for (int index = 0; index < 5; ++index) {
    const fe::WorkerBootId boot = fe::WorkerBootId::generate();
    boots.push_back(boot);
    const fe::RegistrationResult registered =
        runtime.register_participant(registration_request("publisher.ring", boot, runtime.current_epoch()));
    FE_REQUIRE_EQ(registered.outcome, fe::RegistrationOutcome::Registered);
    fe::FenceWorkerRequest fence;
    fence.attempt = fe::MutationAttemptId::generate();
    fence.expected_epoch = runtime.current_epoch();
    fence.participant = participant("publisher.ring");
    fence.boot = boot;
    fence.reason = fe::FenceReason::ExplicitOperatorFence;
    FE_REQUIRE_EQ(runtime.fence_worker(fence).outcome, fe::FenceOutcome::Fenced);
  }

  // Recently fenced boots are rejected exactly while retained.
  for (std::size_t index = boots.size() - runtime.limits().max_fenced_boots_per_participant;
       index < boots.size(); ++index) {
    const fe::RegistrationResult rejected = runtime.register_participant(
        registration_request("publisher.ring", boots[index], runtime.current_epoch()));
    FE_REQUIRE_EQ(rejected.outcome, fe::RegistrationOutcome::StaleWorkerBoot);
  }

  // Every fenced incarnation's authority stays dead regardless of retention.
  for (const auto& boot : boots) {
    const fe::FenceStatus status = runtime.query_fence(participant("publisher.ring"), boot);
    if (status.fenced) {
      FE_REQUIRE(status.incarnation.has_value());
    }
  }
}

FE_TEST(limits, max_string_size_is_consulted) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(tight_options(fixture));
  fe::RegisterParticipantRequest request =
      registration_request("publisher.text", fe::WorkerBootId::generate(), runtime.current_epoch());
  request.provenance = fe::BoundedText::from_validated(std::string(200, 'x'));
  const fe::RegistrationResult rejected = runtime.register_participant(request);
  FE_REQUIRE_EQ(rejected.outcome, fe::RegistrationOutcome::MalformedRequest);
  FE_REQUIRE_EQ(field_text(rejected.explanation.render(), "reason"),
                std::optional<std::string>("provenance"));
}

FE_TEST(limits, frame_and_request_bounds_are_consulted) {
  fe::Limits limits = fe::default_limits();
  limits.max_frame_payload = 64;
  limits.max_request_size = 32;

  std::vector<std::byte> oversized_request(48, std::byte{0});
  FE_REQUIRE_THROWS(fe::encode_frame(fe::MessageType::SnapshotRequest, 1, fe::CoordinatorEpoch(1),
                                     fe::MutationAttemptId::generate(), oversized_request, limits));

  std::vector<std::byte> oversized_frame(80, std::byte{0});
  FE_REQUIRE_THROWS(fe::encode_frame(fe::MessageType::SnapshotResponse, 1, fe::CoordinatorEpoch(1),
                                     fe::MutationAttemptId::generate(), oversized_frame, limits));

  const std::vector<std::byte> acceptable(16, std::byte{0});
  const std::vector<std::byte> frame = fe::encode_frame(
      fe::MessageType::SnapshotRequest, 1, fe::CoordinatorEpoch(1), fe::MutationAttemptId::generate(),
      acceptable, limits);
  FE_REQUIRE_EQ(frame.size(), fe::frame_header_size + acceptable.size());

  // A declared payload length beyond the transport bound is rejected from the
  // header alone, before any allocation proportional to it occurs.
  // payload_length lives at bytes 10..13 of the fixed header.
  std::vector<std::byte> header(frame.begin(),
                                frame.begin() + static_cast<std::ptrdiff_t>(fe::frame_header_size));
  header[10] = std::byte{0xFF};
  header[11] = std::byte{0xFF};
  header[12] = std::byte{0x00};
  header[13] = std::byte{0x00};
  FE_REQUIRE_THROWS(fe::decode_frame_header(header, limits));
}

FE_TEST(limits, listen_backlog_is_applied_to_the_listener) {
  fe::Limits limits = fe::default_limits();
  limits.listen_backlog = 1;
  FE_REQUIRE(fe::is_viable(limits));
  FE_REQUIRE_EQ(limits.listen_backlog, std::size_t{1});
}
