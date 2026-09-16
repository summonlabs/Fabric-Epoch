// Fabric Epoch 1.0.0 - Summon Software Labs
#include <string>
#include <vector>

#include <algorithm>

#include "codec.hpp"
#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

namespace {

struct Population {
  std::vector<std::string> names;
  std::vector<fe::WorkerBootId> boots;
};

Population fixed_population() {
  Population population;
  for (int index = 0; index < 8; ++index) {
    population.names.push_back("publisher.pop" + std::to_string(index));
    std::array<std::byte, 16> bytes{};
    bytes[0] = static_cast<std::byte>(index + 1);
    bytes[15] = static_cast<std::byte>(0xA0 + index);
    population.boots.push_back(fe::WorkerBootId::from_bytes(bytes));
  }
  return population;
}

fe::CoordinatorBootId fixed_coordinator_boot() {
  std::array<std::byte, 16> bytes{};
  bytes[0] = std::byte{0x7F};
  bytes[15] = std::byte{0x11};
  return fe::CoordinatorBootId::from_bytes(bytes);
}

/// Builds an identical logical authority state while presenting the
/// participants in a caller-chosen order.
fe::AuthoritySnapshot build_in_order(const RuntimeFixture& storage, const std::vector<std::size_t>& order,
                                     const std::vector<std::string>* fence_notes = nullptr) {
  fe::EpochRuntimeOptions options = runtime_options(storage);
  options.coordinator_boot = fixed_coordinator_boot();
  fe::EpochRuntime runtime(options);
  const Population population = fixed_population();
  for (const std::size_t index : order) {
    const fe::RegistrationResult result = runtime.register_participant(registration_request(
        population.names[index], population.boots[index], runtime.current_epoch()));
    FE_REQUIRE_EQ(result.outcome, fe::RegistrationOutcome::Registered);
    if (fence_notes != nullptr) {
      fe::FenceWorkerRequest fence;
      fence.attempt = fe::MutationAttemptId::generate();
      fence.expected_epoch = runtime.current_epoch();
      fence.participant = participant(population.names[index]);
      fence.boot = population.boots[index];
      fence.reason = fe::FenceReason::AdministrativeRevoke;
      fence.note = fe::BoundedText::from_validated((*fence_notes)[index]);
      FE_REQUIRE_EQ(runtime.fence_worker(fence).outcome, fe::FenceOutcome::Fenced);
    }
  }
  return runtime.snapshot();
}

}  // namespace

FE_TEST(snapshot_digest, digest_is_independent_of_record_ordering) {
  RuntimeFixture storage;
  const std::vector<std::size_t> ascending = {0, 1, 2, 3, 4, 5, 6, 7};
  std::vector<std::string> notes;
  for (int index = 0; index < 8; ++index) {
    notes.push_back("note " + std::to_string(index));
  }
  const fe::AuthoritySnapshot canonical = build_in_order(storage, ascending, &notes);
  FE_REQUIRE_EQ(canonical.participants.size(), std::size_t{8});
  FE_REQUIRE_EQ(canonical.fences.size(), std::size_t{8});

  // The runtime already reports records in canonical order.
  for (std::size_t index = 1; index < canonical.participants.size(); ++index) {
    FE_REQUIRE(canonical.participants[index - 1].id < canonical.participants[index].id);
  }
  FE_REQUIRE_EQ(fe::detail::compute_snapshot_digest(canonical), canonical.digest);

  // Permuting the record vectors must not change the semantic digest: the
  // digest is a property of the authority state, not of iteration order.
  fe::AuthoritySnapshot permuted = canonical;
  std::reverse(permuted.participants.begin(), permuted.participants.end());
  std::reverse(permuted.fences.begin(), permuted.fences.end());
  std::reverse(permuted.grants.begin(), permuted.grants.end());
  FE_REQUIRE_EQ(fe::detail::compute_snapshot_digest(permuted), canonical.digest);

  fe::AuthoritySnapshot rotated = canonical;
  std::rotate(rotated.participants.begin(), rotated.participants.begin() + 3, rotated.participants.end());
  std::rotate(rotated.fences.begin(), rotated.fences.begin() + 5, rotated.fences.end());
  FE_REQUIRE_EQ(fe::detail::compute_snapshot_digest(rotated), canonical.digest);
}

FE_TEST(snapshot_digest, independent_runtimes_in_the_same_state_agree) {
  // Two coordinators that committed the same sequence of authority mutations
  // hold the same semantic state, so their digests must agree exactly.
  const std::vector<std::size_t> order = {0, 1, 2, 3, 4, 5, 6, 7};
  RuntimeFixture first_storage;
  RuntimeFixture second_storage;
  const fe::AuthoritySnapshot first = build_in_order(first_storage, order);
  const fe::AuthoritySnapshot second = build_in_order(second_storage, order);
  FE_REQUIRE_EQ(first.epoch, second.epoch);
  FE_REQUIRE_EQ(first.generation, second.generation);
  FE_REQUIRE_EQ(first.digest, second.digest);
}

FE_TEST(snapshot_digest, digest_ignores_diagnostic_text_but_not_semantics) {
  RuntimeFixture first_fixture;
  RuntimeFixture second_fixture;
  std::vector<std::string> notes_a;
  std::vector<std::string> notes_b;
  for (int index = 0; index < 8; ++index) {
    notes_a.push_back("operator note A" + std::to_string(index));
    notes_b.push_back("a completely different note " + std::to_string(index * 3));
  }
  const std::vector<std::size_t> order = {0, 1, 2, 3, 4, 5, 6, 7};
  const fe::AuthoritySnapshot a = build_in_order(first_fixture, order, &notes_a);
  const fe::AuthoritySnapshot b = build_in_order(second_fixture, order, &notes_b);
  FE_REQUIRE_EQ(a.digest, b.digest);
  FE_REQUIRE(a.fences.front().note != b.fences.front().note);
}

FE_TEST(snapshot_digest, a_semantic_change_changes_the_digest) {
  RuntimeFixture storage;
  fe::EpochRuntimeOptions options = runtime_options(storage);
  options.coordinator_boot = fixed_coordinator_boot();
  fe::EpochRuntime runtime(options);
  const Population population = fixed_population();
  for (std::size_t index = 0; index < population.names.size(); ++index) {
    FE_REQUIRE_EQ(runtime
                      .register_participant(registration_request(population.names[index],
                                                                 population.boots[index],
                                                                 runtime.current_epoch()))
                      .outcome,
                  fe::RegistrationOutcome::Registered);
  }
  const fe::SnapshotDigest baseline = runtime.snapshot().digest;

  fe::FenceWorkerRequest fence;
  fence.attempt = fe::MutationAttemptId::generate();
  fence.expected_epoch = runtime.current_epoch();
  fence.participant = participant(population.names[3]);
  fence.boot = population.boots[3];
  fence.reason = fe::FenceReason::Shutdown;
  FE_REQUIRE_EQ(runtime.fence_worker(fence).outcome, fe::FenceOutcome::Fenced);
  FE_REQUIRE(runtime.snapshot().digest != baseline);

  const fe::SnapshotDigest after_fence = runtime.snapshot().digest;
  fe::EpochAdvanceRequest advance;
  advance.attempt = fe::MutationAttemptId::generate();
  advance.expected_epoch = runtime.current_epoch();
  FE_REQUIRE_EQ(runtime.advance_epoch(advance).outcome, fe::EpochAdvanceOutcome::Advanced);
  FE_REQUIRE(runtime.snapshot().digest != after_fence);
}

FE_TEST(snapshot_digest, repeated_snapshots_of_an_unchanged_state_agree) {
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  for (int index = 0; index < 4; ++index) {
    FE_REQUIRE_EQ(runtime
                      .register_participant(registration_request("publisher.stable" + std::to_string(index),
                                                                 fe::WorkerBootId::generate(),
                                                                 runtime.current_epoch()))
                      .outcome,
                  fe::RegistrationOutcome::Registered);
  }
  const fe::AuthoritySnapshot first = runtime.snapshot();
  const fe::AuthoritySnapshot second = runtime.snapshot();
  FE_REQUIRE_EQ(first.digest, second.digest);
  FE_REQUIRE(first.id != second.id);
}

FE_TEST(snapshot_digest, currentness_is_explicit_and_old_snapshots_stay_inspectable) {
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  FE_REQUIRE_EQ(runtime.register_participant(registration_request(
                    "publisher.snap", fe::WorkerBootId::generate(), runtime.current_epoch()))
                    .outcome,
                fe::RegistrationOutcome::Registered);
  const fe::AuthoritySnapshot before = runtime.snapshot();
  FE_REQUIRE(before.is_current_for(runtime.current_epoch(), runtime.generation()));

  fe::EpochAdvanceRequest advance;
  advance.attempt = fe::MutationAttemptId::generate();
  advance.expected_epoch = runtime.current_epoch();
  FE_REQUIRE_EQ(runtime.advance_epoch(advance).outcome, fe::EpochAdvanceOutcome::Advanced);

  FE_REQUIRE(!before.is_current_for(runtime.current_epoch(), runtime.generation()));
  // The old snapshot remains inspectable: it still describes exactly what it
  // described when it was taken.
  FE_REQUIRE_EQ(before.participants.size(), std::size_t{1});
  FE_REQUIRE(before.participants.front().active_incarnation_count() == 1);
  FE_REQUIRE_EQ(before.digest, before.digest);

  // It cannot authorise mutation: the live runtime rejects the epoch it names.
  fe::ValidateAuthorityRequest request;
  request.token.epoch = before.epoch;
  request.token.participant = before.participants.front().id;
  request.token.boot = before.participants.front().active_boot;
  request.token.incarnation = before.participants.front().active_incarnation;
  request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
  FE_REQUIRE_EQ(runtime.validate_authority(request).code, fe::AuthorityValidationCode::StaleEpoch);
}

FE_TEST(snapshot_digest, digest_is_a_stable_hex_string) {
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  const fe::SnapshotDigest digest = runtime.snapshot().digest;
  FE_REQUIRE_EQ(digest.to_string().size(), std::size_t{64});
  FE_REQUIRE_EQ(fe::SnapshotDigest::from_bytes(digest.bytes()), digest);
  for (const char c : digest.to_string()) {
    FE_REQUIRE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
  }
}
