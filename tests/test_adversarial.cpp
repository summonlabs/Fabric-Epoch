// Fabric Epoch 1.0.0 - Summon Software Labs
// Deliberate attacks against the authority engine and the wire codec.
#include <fstream>
#include <string>
#include <vector>

#include "fabric_epoch/detail/encoding.hpp"
#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

namespace {

std::vector<std::byte> frame_with(fe::MessageType type, const std::vector<std::byte>& payload,
                                  const fe::Limits& limits) {
  return fe::encode_frame(type, 1, fe::CoordinatorEpoch(1), fe::MutationAttemptId::generate(), payload,
                          limits);
}

}  // namespace

FE_TEST(adversarial, forged_epochs_never_grant_authority) {
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const fe::RegistrationResult registered =
      runtime.register_participant(registration_request("publisher.forge", boot, runtime.current_epoch()));

  const std::vector<fe::CoordinatorEpoch> forged = {
      fe::CoordinatorEpoch(0),
      fe::CoordinatorEpoch(fe::CoordinatorEpoch::max_value),
      fe::CoordinatorEpoch(runtime.current_epoch().value() + 1000),
      fe::CoordinatorEpoch(runtime.current_epoch().value() - 1)};
  for (const auto epoch : forged) {
    fe::ValidateAuthorityRequest request;
    request.token = token_for(registered, participant("publisher.forge"), boot);
    request.token.epoch = epoch;
    request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
    const fe::AuthorityValidation validation = runtime.validate_authority(request);
    FE_REQUIRE_MSG(validation.code == fe::AuthorityValidationCode::StaleEpoch,
                   "forged epoch " + epoch.to_string() + " produced " +
                       std::string(fe::to_string(validation.code)));
  }
  FE_REQUIRE(runtime.check_indexes());
}

FE_TEST(adversarial, forged_worker_boots_and_incarnations_never_grant_authority) {
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const fe::RegistrationResult registered =
      runtime.register_participant(registration_request("publisher.forgeboot", boot, runtime.current_epoch()));

  for (int attempt = 0; attempt < 32; ++attempt) {
    fe::ValidateAuthorityRequest request;
    request.token = token_for(registered, participant("publisher.forgeboot"), fe::WorkerBootId::generate());
    request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
    const fe::AuthorityValidation validation = runtime.validate_authority(request);
    FE_REQUIRE_EQ(validation.code, fe::AuthorityValidationCode::StaleWorkerBoot);
  }

  for (const std::uint64_t sequence : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{2},
                                       std::uint64_t{1000000}, fe::CoordinatorEpoch::max_value}) {
    fe::ValidateAuthorityRequest request;
    request.token = token_for(registered, participant("publisher.forgeboot"), boot);
    request.token.incarnation = fe::IncarnationSequence(sequence);
    request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
    const fe::AuthorityValidation validation = runtime.validate_authority(request);
    if (sequence == registered.incarnation->value()) {
      FE_REQUIRE_EQ(validation.code, fe::AuthorityValidationCode::Current);
    } else {
      FE_REQUIRE_MSG(validation.code == fe::AuthorityValidationCode::StaleWorkerBoot ||
                         validation.code == fe::AuthorityValidationCode::Malformed,
                     "forged incarnation " + std::to_string(sequence) + " produced " +
                         std::string(fe::to_string(validation.code)));
    }
  }
}

FE_TEST(adversarial, unauthorized_scope_is_never_granted_implicitly) {
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const fe::RegistrationResult registered =
      runtime.register_participant(registration_request(
          "publisher.scoped", boot, runtime.current_epoch(),
          scopes({scope(fe::ScopeKind::Site, "site-a")})));

  const std::vector<fe::AuthorityScope> denied = {
      scope(fe::ScopeKind::Site, "site-b"),
      scope(fe::ScopeKind::Fabric, "site-a"),
      scope(fe::ScopeKind::Entity, "site-a"),
      scope(fe::ScopeKind::OperationFamily, "site-a"),
      scope(fe::ScopeKind::Domain, "site-a"),
      scope(fe::ScopeKind::EntityClass, "site-a")};
  for (const auto& requested : denied) {
    fe::ValidateAuthorityRequest request;
    request.token = token_for(registered, participant("publisher.scoped"), boot);
    request.scope = requested;
    FE_REQUIRE_MSG(
        runtime.validate_authority(request).code == fe::AuthorityValidationCode::UnauthorizedScope,
        "scope " + requested.to_string() + " was granted implicitly");
  }
  fe::ValidateAuthorityRequest allowed;
  allowed.token = token_for(registered, participant("publisher.scoped"), boot);
  allowed.scope = scope(fe::ScopeKind::Site, "site-a");
  FE_REQUIRE_EQ(runtime.validate_authority(allowed).code, fe::AuthorityValidationCode::Current);
}

FE_TEST(adversarial, stale_registration_fence_and_revoke_requests_are_rejected) {
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  FE_REQUIRE_EQ(runtime.register_participant(registration_request("publisher.staleop", boot, runtime.current_epoch()))
                    .outcome,
                fe::RegistrationOutcome::Registered);
  const fe::CoordinatorEpoch stale_epoch(runtime.current_epoch().value() + 5);

  FE_REQUIRE_EQ(runtime.register_participant(registration_request(
                    "publisher.staleop", fe::WorkerBootId::generate(), stale_epoch))
                    .outcome,
                fe::RegistrationOutcome::StaleEpoch);

  fe::FenceWorkerRequest fence;
  fence.attempt = fe::MutationAttemptId::generate();
  fence.expected_epoch = stale_epoch;
  fence.participant = participant("publisher.staleop");
  fence.boot = boot;
  FE_REQUIRE_EQ(runtime.fence_worker(fence).outcome, fe::FenceOutcome::StaleEpoch);

  fe::RevokeAuthorityRequest revoke;
  revoke.attempt = fe::MutationAttemptId::generate();
  revoke.expected_epoch = stale_epoch;
  revoke.grant = fe::AuthorityGrantId::generate();
  FE_REQUIRE_EQ(runtime.revoke_authority(revoke).outcome, fe::RevokeOutcome::StaleEpoch);

  fe::EpochAdvanceRequest advance;
  advance.attempt = fe::MutationAttemptId::generate();
  advance.expected_epoch = stale_epoch;
  FE_REQUIRE_EQ(runtime.advance_epoch(advance).outcome, fe::EpochAdvanceOutcome::StaleExpectedEpoch);
}

FE_TEST(adversarial, a_reconnect_storm_cannot_resurrect_an_incarnation) {
  RuntimeFixture storage;
  fe::EpochRuntimeOptions options = runtime_options(storage);
  options.limits.max_recent_fences = 128;
  options.limits.max_fenced_boots_per_participant = 128;
  fe::EpochRuntime runtime(options);
  const fe::ParticipantId id = participant("publisher.storm");
  std::vector<fe::WorkerBootId> retired_boots;

  for (int wave = 0; wave < 40; ++wave) {
    const fe::WorkerBootId boot = fe::WorkerBootId::generate();
    const fe::RegistrationResult registered =
        runtime.register_participant(registration_request("publisher.storm", boot, runtime.current_epoch()));
    FE_REQUIRE_EQ(registered.outcome, fe::RegistrationOutcome::Registered);
    retired_boots.push_back(boot);
    fe::FenceWorkerRequest fence;
    fence.attempt = fe::MutationAttemptId::generate();
    fence.expected_epoch = runtime.current_epoch();
    fence.participant = id;
    fence.boot = boot;
    fence.reason = fe::FenceReason::ProcessLoss;
    FE_REQUIRE_EQ(runtime.fence_worker(fence).outcome, fe::FenceOutcome::Fenced);
  }

  // Every fenced boot is still refused, and none of them holds authority.
  for (const auto& boot : retired_boots) {
    const fe::RegistrationResult result =
        runtime.register_participant(registration_request("publisher.storm", boot, runtime.current_epoch()));
    FE_REQUIRE_MSG(result.outcome == fe::RegistrationOutcome::StaleWorkerBoot,
                   "a fenced boot was accepted again: " + boot.to_string());
  }
  FE_REQUIRE(runtime.check_indexes());
}

FE_TEST(adversarial, resource_exhaustion_is_refused_with_a_structured_limit) {
  RuntimeFixture storage;
  fe::EpochRuntimeOptions options = runtime_options(storage);
  options.limits.max_participants = 16;
  options.limits.max_active_workers = 16;
  fe::EpochRuntime runtime(options);
  for (int index = 0; index < 16; ++index) {
    FE_REQUIRE_EQ(runtime
                      .register_participant(registration_request("publisher.exhaust" + std::to_string(index),
                                                                 fe::WorkerBootId::generate(),
                                                                 runtime.current_epoch()))
                      .outcome,
                  fe::RegistrationOutcome::Registered);
  }
  for (int index = 0; index < 8; ++index) {
    const fe::RegistrationResult refused = runtime.register_participant(
        registration_request("publisher.overflow" + std::to_string(index), fe::WorkerBootId::generate(),
                             runtime.current_epoch()));
    FE_REQUIRE_EQ(refused.outcome, fe::RegistrationOutcome::ResourceLimit);
  }
  FE_REQUIRE_EQ(runtime.list_participants().size(), std::size_t{16});
}

FE_TEST(adversarial, wire_attacks_are_refused_without_partial_trust) {
  fe::Limits limits = fe::default_limits();
  limits.max_frame_payload = 512;
  limits.max_request_size = 256;
  const std::vector<std::byte> payload(64, std::byte{0xAB});
  const std::vector<std::byte> frame = frame_with(fe::MessageType::RegisterParticipant, payload, limits);
  const std::vector<std::byte> header(frame.begin(),
                                      frame.begin() + static_cast<std::ptrdiff_t>(fe::frame_header_size));
  const std::vector<std::byte> body(frame.begin() + static_cast<std::ptrdiff_t>(fe::frame_header_size),
                                    frame.end());

  // Declared length larger than the transport bound.
  std::vector<std::byte> oversize = header;
  oversize[10] = std::byte{0xFF};
  oversize[11] = std::byte{0xFF};
  oversize[12] = std::byte{0x00};
  oversize[13] = std::byte{0x00};
  FE_REQUIRE_THROWS(fe::decode_frame(oversize, body, limits));

  // Declared length larger than the request bound but inside the frame bound.
  std::vector<std::byte> request_oversize = header;
  request_oversize[10] = std::byte{0x00};
  request_oversize[11] = std::byte{0x01};
  request_oversize[12] = std::byte{0x00};
  request_oversize[13] = std::byte{0x00};
  FE_REQUIRE_THROWS(fe::decode_frame(request_oversize, body, limits));

  // Integrity field rewritten to a valid-looking value for different content.
  std::vector<std::byte> swapped = header;
  for (std::size_t index = fe::frame_integrity_offset; index < swapped.size(); ++index) {
    swapped[index] = std::byte{0x00};
  }
  FE_REQUIRE_THROWS(fe::decode_frame(swapped, body, limits));
}

FE_TEST(adversarial, oversized_and_malformed_payloads_are_rejected_before_allocation) {
  fe::Limits limits = fe::default_limits();
  limits.max_string_size = 16;

  fe::detail::ByteWriter writer;
  writer.u16(1);
  writer.u32(4096);
  writer.string(std::string(4096, 'a'));
  FE_REQUIRE_THROWS(fe::decode_hello(writer.take(), limits));

  fe::detail::ByteWriter truncated;
  truncated.u16(1);
  truncated.u32(64);
  FE_REQUIRE_THROWS(fe::decode_hello(truncated.take(), limits));

  // A length prefix that overflows the remaining input.
  fe::detail::ByteWriter overflow;
  overflow.u16(1);
  overflow.u32(0xFFFFFFFFu);
  FE_REQUIRE_THROWS(fe::decode_hello(overflow.take(), limits));
}

FE_TEST(adversarial, impossible_epoch_and_rollback_images_are_refused_by_the_store) {
  RuntimeFixture storage;
  fe::StoreOptions options;
  options.path = storage.store_path;
  fe::EpochStore store(options);

  fe::DurableState impossible;
  impossible.current_epoch = fe::CoordinatorEpoch(0);
  fe::DurableParticipant record;
  record.id = participant("publisher.impossible");
  record.next_incarnation = fe::IncarnationSequence(1);
  impossible.participants.push_back(record);
  FE_REQUIRE_THROWS(store.encode(impossible));

  fe::DurableState high;
  high.current_epoch = fe::CoordinatorEpoch(1000);
  high.coordinator_boot = fe::CoordinatorBootId::generate();
  store.save(high);
  FE_REQUIRE_EQ(store.highest_persisted_epoch().value(), std::uint64_t{1000});

  fe::DurableState low;
  low.current_epoch = fe::CoordinatorEpoch(999);
  low.coordinator_boot = fe::CoordinatorBootId::generate();
  // Saving a lower epoch does not lower the watermark, and loading it is
  // refused as a rollback.
  const std::vector<std::byte> image = store.encode(low);
  std::ofstream out(storage.store_path, std::ios::binary | std::ios::trunc);
  FE_REQUIRE(out.good());
  out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
  out.close();
  FE_REQUIRE_THROWS(store.load());
}

FE_TEST(adversarial, path_traversal_in_the_store_path_is_not_interpreted) {
  // The store treats its path as an opaque filesystem location: a name that
  // contains traversal segments resolves to a literal sibling directory and is
  // never reinterpreted by Fabric Epoch itself.
  TempDirectory directory;
  const std::filesystem::path nested = directory.file("..") / (directory.path().filename().string() + "_x") /
                                       "epoch.store";
  fe::StoreOptions options;
  options.path = nested;
  fe::EpochStore store(options);
  fe::DurableState state;
  state.current_epoch = fe::CoordinatorEpoch(2);
  state.coordinator_boot = fe::CoordinatorBootId::generate();
  store.save(state);
  FE_REQUIRE(store.exists());
  FE_REQUIRE_EQ(store.load().current_epoch, fe::CoordinatorEpoch(2));
  std::error_code error;
  std::filesystem::remove_all(directory.path().parent_path() / nested.parent_path().filename(), error);
}
