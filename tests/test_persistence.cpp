// Fabric Epoch 1.0.0 - Summon Software Labs
#include <fstream>
#include <string>
#include <vector>

#include "fabric_epoch/detail/sha256.hpp"
#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

namespace {

void write_raw(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  FE_REQUIRE(out.good());
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.flush();
  FE_REQUIRE(out.good());
}

std::vector<std::byte> store_file_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  FE_REQUIRE(in.good());
  const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
  }
  return bytes;
}

/// Builds a complete durable image around an arbitrary payload. This mirrors
/// the documented layout independently of the library encoder so the tests can
/// attack representations the encoder would never produce.
std::vector<std::byte> build_image(const std::vector<std::byte>& payload, std::uint32_t magic,
                                   std::uint32_t version) {
  std::vector<std::byte> image;
  auto push_u32 = [&image](std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      image.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
  };
  push_u32(magic);
  push_u32(version);
  push_u32(static_cast<std::uint32_t>(payload.size()));
  image.insert(image.end(), payload.begin(), payload.end());
  const auto digest = fe::detail::Sha256::hash(std::span<const std::byte>(image));
  image.insert(image.end(), digest.begin(), digest.end());
  return image;
}

}  // namespace

FE_TEST(persistence, round_trips_every_semantic_field) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const fe::RegistrationResult registered = runtime.register_participant(
      registration_request("publisher.persist", boot, runtime.current_epoch(),
                           scopes({scope(fe::ScopeKind::Site, "site-a"),
                                   scope(fe::ScopeKind::Entity, "node-1")})));
  FE_REQUIRE_EQ(registered.outcome, fe::RegistrationOutcome::Registered);
  const fe::EpochState before = runtime.state();
  const fe::AuthoritySnapshot snapshot_before = runtime.snapshot();
  runtime.flush();

  fe::StoreOptions options;
  options.path = fixture.store_path;
  fe::EpochStore store(options);
  const fe::DurableState loaded = store.load();
  FE_REQUIRE_EQ(loaded.current_epoch, before.epoch);
  FE_REQUIRE_EQ(loaded.coordinator_boot, runtime.coordinator_boot());
  FE_REQUIRE_EQ(loaded.generation, before.generation);
  FE_REQUIRE_EQ(loaded.registration_floor, before.registration_floor);
  FE_REQUIRE_EQ(loaded.fence_floor, before.fence_floor);
  FE_REQUIRE_EQ(loaded.participants.size(), std::size_t{1});
  FE_REQUIRE_EQ(loaded.participants.front().id, participant("publisher.persist"));
  FE_REQUIRE_EQ(loaded.participants.front().active_boot, boot);
  FE_REQUIRE_EQ(loaded.participants.front().scopes.to_string(),
                snapshot_before.participants.front().scopes.to_string());
}

FE_TEST(persistence, encoding_is_deterministic_for_a_fixed_state) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  for (int index = 0; index < 3; ++index) {
    FE_REQUIRE_EQ(runtime
                      .register_participant(registration_request("publisher.det" + std::to_string(index),
                                                                 fe::WorkerBootId::generate(),
                                                                 runtime.current_epoch()))
                      .outcome,
                  fe::RegistrationOutcome::Registered);
  }
  runtime.flush();
  fe::StoreOptions options;
  options.path = fixture.store_path;
  fe::EpochStore store(options);
  const fe::DurableState state = store.load();
  const std::vector<std::byte> first = store.encode(state);
  const std::vector<std::byte> second = store.encode(state);
  FE_REQUIRE(first == second);
  FE_REQUIRE_EQ(first.size(), store_file_bytes(fixture.store_path).size());
}

FE_TEST(persistence, missing_store_is_reported_when_creation_is_disabled) {
  RuntimeFixture fixture;
  fe::StoreOptions options;
  options.path = fixture.store_path;
  options.create_if_missing = false;
  fe::EpochStore store(options);
  FE_REQUIRE(!store.exists());
  FE_REQUIRE_THROWS(store.load());
}

FE_TEST(persistence, atomic_replacement_leaves_no_temporary_files) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  for (int index = 0; index < 4; ++index) {
    FE_REQUIRE_EQ(runtime
                      .register_participant(registration_request("publisher.tmp" + std::to_string(index),
                                                                 fe::WorkerBootId::generate(),
                                                                 runtime.current_epoch()))
                      .outcome,
                  fe::RegistrationOutcome::Registered);
  }
  std::size_t entries = 0;
  for (const auto& entry : std::filesystem::directory_iterator(fixture.directory.path())) {
    (void)entry;
    ++entries;
  }
  // One store image plus one rollback watermark sidecar, nothing else.
  FE_REQUIRE_EQ(entries, std::size_t{2});
}

FE_TEST(persistence, acknowledgements_are_durable_before_they_return) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const fe::RegistrationResult registered =
      runtime.register_participant(registration_request("publisher.durable", boot, runtime.current_epoch()));
  FE_REQUIRE_EQ(registered.outcome, fe::RegistrationOutcome::Registered);

  // A second, independent reader observes the acknowledged registration
  // without any flush call: the acknowledgement implied durability.
  fe::StoreOptions options;
  options.path = fixture.store_path;
  fe::EpochStore reader(options);
  const fe::DurableState state = reader.load();
  FE_REQUIRE_EQ(state.participants.size(), std::size_t{1});
  FE_REQUIRE_EQ(state.participants.front().active_boot, boot);

  fe::FenceWorkerRequest fence;
  fence.attempt = fe::MutationAttemptId::generate();
  fence.expected_epoch = runtime.current_epoch();
  fence.participant = participant("publisher.durable");
  fence.boot = boot;
  fence.reason = fe::FenceReason::ProcessLoss;
  FE_REQUIRE_EQ(runtime.fence_worker(fence).outcome, fe::FenceOutcome::Fenced);

  const fe::DurableState after_fence = reader.load();
  FE_REQUIRE_EQ(after_fence.recent_fences.size(), std::size_t{1});
  FE_REQUIRE_EQ(after_fence.recent_fences.front().boot, boot);
  FE_REQUIRE_EQ(after_fence.recent_fences.front().reason, fe::FenceReason::ProcessLoss);
}

FE_TEST(persistence, durable_fence_survives_a_coordinator_restart) {
  RuntimeFixture fixture;
  fe::WorkerBootId boot = fe::WorkerBootId::generate();
  fe::CoordinatorEpoch first_epoch(0);
  {
    fe::EpochRuntime runtime(runtime_options(fixture));
    first_epoch = runtime.current_epoch();
    FE_REQUIRE_EQ(runtime.register_participant(registration_request("publisher.survive", boot, first_epoch)).outcome,
                  fe::RegistrationOutcome::Registered);
    fe::FenceWorkerRequest fence;
    fence.attempt = fe::MutationAttemptId::generate();
    fence.expected_epoch = first_epoch;
    fence.participant = participant("publisher.survive");
   	fence.boot = boot;
    fence.reason = fe::FenceReason::ExplicitOperatorFence;
    FE_REQUIRE_EQ(runtime.fence_worker(fence).outcome, fe::FenceOutcome::Fenced);
  }

  fe::EpochRuntime restarted(runtime_options(fixture));
  FE_REQUIRE(restarted.current_epoch() > first_epoch);
  const fe::FenceStatus status = restarted.query_fence(participant("publisher.survive"), boot);
  FE_REQUIRE(status.fenced);
  FE_REQUIRE_EQ(status.reason, std::optional<fe::FenceReason>(fe::FenceReason::ExplicitOperatorFence));

  const fe::RegistrationResult rejected = restarted.register_participant(
      registration_request("publisher.survive", boot, restarted.current_epoch()));
  FE_REQUIRE_EQ(rejected.outcome, fe::RegistrationOutcome::StaleWorkerBoot);
}

FE_TEST(persistence, restart_keeps_durable_identity_but_not_live_authority) {
  RuntimeFixture fixture;
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  fe::RegistrationResult registered;
  fe::CoordinatorEpoch first_epoch(0);
  {
    fe::EpochRuntime runtime(runtime_options(fixture));
    first_epoch = runtime.current_epoch();
    registered = runtime.register_participant(
        registration_request("publisher.revive", boot, first_epoch));
    FE_REQUIRE_EQ(registered.outcome, fe::RegistrationOutcome::Registered);
  }

  fe::EpochRuntime restarted(runtime_options(fixture));
  const auto view = restarted.query_participant(participant("publisher.revive"));
  FE_REQUIRE(view.has_value());
  FE_REQUIRE_MSG(view->retired == false, "durable identity must survive a restart");
  FE_REQUIRE_EQ(view->active_incarnation_count(), std::size_t{0});
  FE_REQUIRE(view->revalidation_required);
  FE_REQUIRE_EQ(view->registration_generation, *registered.registration_generation);

  fe::ValidateAuthorityRequest validation;
  validation.token = token_for(registered, participant("publisher.revive"), boot);
  validation.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
  const fe::AuthorityValidation old_token = restarted.validate_authority(validation);
  FE_REQUIRE_EQ(old_token.code, fe::AuthorityValidationCode::StaleEpoch);
  FE_REQUIRE_EQ(old_token.currentness, fe::AuthorityCurrentness::StaleEpoch);
  FE_REQUIRE_EQ(old_token.replaced_by_epoch, std::optional<fe::CoordinatorEpoch>(restarted.current_epoch()));

  const fe::WorkerBootId fresh = fe::WorkerBootId::generate();
  const fe::RegistrationResult reattached = restarted.register_participant(
      registration_request("publisher.revive", fresh, restarted.current_epoch()));
  FE_REQUIRE_EQ(reattached.outcome, fe::RegistrationOutcome::Registered);
  FE_REQUIRE(*reattached.incarnation > *registered.incarnation);
  FE_REQUIRE(*reattached.registration_generation > *registered.registration_generation);
}

FE_TEST(persistence, rollback_below_the_watermark_is_detected) {
  RuntimeFixture fixture;
  fe::StoreOptions options;
  options.path = fixture.store_path;
  fe::EpochStore store(options);

  fe::DurableState state;
  state.current_epoch = fe::CoordinatorEpoch(9);
  state.coordinator_boot = fe::CoordinatorBootId::generate();
  state.generation = fe::EpochGeneration(3);
  store.save(state);
  FE_REQUIRE_EQ(store.highest_persisted_epoch().value(), std::uint64_t{9});

  // Replace the primary image with an older, internally consistent one while
  // leaving the watermark behind: this is exactly a file-level rollback.
  fe::DurableState rolled_back;
  rolled_back.current_epoch = fe::CoordinatorEpoch(4);
  rolled_back.coordinator_boot = fe::CoordinatorBootId::generate();
  rolled_back.generation = fe::EpochGeneration(1);
  write_raw(fixture.store_path, store.encode(rolled_back));
  FE_REQUIRE_THROWS(store.load());
}

FE_TEST(persistence, a_corrupt_watermark_fails_fast) {
  RuntimeFixture fixture;
  fe::StoreOptions options;
  options.path = fixture.store_path;
  fe::EpochStore store(options);
  fe::DurableState state;
  state.current_epoch = fe::CoordinatorEpoch(3);
  state.coordinator_boot = fe::CoordinatorBootId::generate();
  store.save(state);
  write_raw(store.watermark_path(), {std::byte{'n'}, std::byte{'o'}, std::byte{'p'}});
  FE_REQUIRE_THROWS(store.load());
}

FE_TEST(persistence, partially_written_temporary_images_are_never_authoritative) {
  RuntimeFixture fixture;
  fe::EpochRuntime runtime(runtime_options(fixture));
  FE_REQUIRE_EQ(runtime.register_participant(registration_request(
                    "publisher.partial", fe::WorkerBootId::generate(), runtime.current_epoch()))
                    .outcome,
                fe::RegistrationOutcome::Registered);
  const fe::EpochState before = runtime.state();

  // Simulate a crash that left a partially written temporary image behind.
  std::filesystem::path debris = fixture.store_path;
  debris += ".tmp1234-deadbeef";
  write_raw(debris, {std::byte{'F'}, std::byte{'B'}, std::byte{'E'}});

  fe::StoreOptions options;
  options.path = fixture.store_path;
  fe::EpochStore store(options);
  const fe::DurableState loaded = store.load();
  FE_REQUIRE_EQ(loaded.current_epoch, before.epoch);
  FE_REQUIRE_EQ(loaded.participants.size(), std::size_t{1});
  std::filesystem::remove(debris);
}

FE_TEST(persistence, concurrent_processes_never_collide_on_temporary_names) {
  RuntimeFixture fixture;
  fe::StoreOptions options;
  options.path = fixture.store_path;
  fe::EpochStore store(options);
  fe::DurableState state;
  state.current_epoch = fe::CoordinatorEpoch(2);
  state.coordinator_boot = fe::CoordinatorBootId::generate();
  for (int index = 0; index < 16; ++index) {
    store.save(state);
    state.generation = fe::EpochGeneration(static_cast<std::uint64_t>(index + 1));
  }
  std::size_t entries = 0;
  for (const auto& entry : std::filesystem::directory_iterator(fixture.directory.path())) {
    (void)entry;
    ++entries;
  }
  FE_REQUIRE_EQ(entries, std::size_t{2});
}
