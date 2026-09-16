// Fabric Epoch 1.0.0 - Summon Software Labs
// Adversarial persistence suite: every attack must be rejected, and rejection
// must never produce a partially trusted image.
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "codec.hpp"
#include "fabric_epoch/detail/sha256.hpp"
#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

namespace {

constexpr std::uint32_t kStoreMagic = 0x46424550u;
constexpr std::uint32_t kStoreVersion = 1;

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

fe::DurableState base_state() {
  fe::DurableState state;
  state.current_epoch = fe::CoordinatorEpoch(5);
  state.coordinator_boot = fe::CoordinatorBootId::generate();
  state.generation = fe::EpochGeneration(4);
  state.registration_floor = fe::RegistrationGeneration(1);
  state.fence_floor = fe::FencingGeneration(1);

  fe::DurableParticipant record;
  record.id = participant("publisher.corrupt");
  record.registration_generation = fe::RegistrationGeneration(1);
  record.fence_generation = fe::FencingGeneration(0);
  record.incarnation_floor = fe::IncarnationSequence(0);
  record.next_incarnation = fe::IncarnationSequence(2);
  record.policy = fe::IncarnationPolicy::SingleIncarnation;
  record.has_active_boot = true;
  record.active_boot = fe::WorkerBootId::generate();
  record.active_incarnation = fe::IncarnationSequence(1);
  record.scopes = default_scopes();
  record.accepted_registrations = 1;
  state.participants.push_back(record);
  return state;
}

fe::EpochStore make_store(const std::filesystem::path& path) {
  fe::StoreOptions options;
  options.path = path;
  options.create_if_missing = false;
  return fe::EpochStore(options);
}

void expect_rejected(const std::filesystem::path& path, const std::vector<std::byte>& image,
                     const std::string& label) {
  fe::EpochStore store = make_store(path);
  bool rejected = false;
  try {
    (void)store.decode(std::span<const std::byte>(image));
  } catch (const fe::EpochError&) {
    rejected = true;
  }
  FE_REQUIRE_MSG(rejected, label);
}

void expect_rejected_state(const std::filesystem::path& path, const fe::DurableState& state,
                           const std::string& label) {
  expect_rejected(path, build_image(fe::detail::encode_durable_payload(state), kStoreMagic, kStoreVersion),
                  label);
}

}  // namespace

FE_TEST(corruption, empty_and_short_images_are_rejected) {
  RuntimeFixture fixture;
  const std::vector<std::byte> valid =
      build_image(fe::detail::encode_durable_payload(base_state()), kStoreMagic, kStoreVersion);
  expect_rejected(fixture.store_path, {}, "empty image");
  for (std::size_t length = 1; length < 12; ++length) {
    expect_rejected(fixture.store_path, std::vector<std::byte>(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(length)),
                    "truncated header at " + std::to_string(length));
  }
}

FE_TEST(corruption, bad_magic_and_unsupported_version_are_rejected) {
  RuntimeFixture fixture;
  const std::vector<std::byte> payload = fe::detail::encode_durable_payload(base_state());
  expect_rejected(fixture.store_path, build_image(payload, 0x00000000u, kStoreVersion), "zero magic");
  expect_rejected(fixture.store_path, build_image(payload, 0x46424551u, kStoreVersion), "wrong magic");
  expect_rejected(fixture.store_path, build_image(payload, kStoreMagic, 0u), "version zero");
  expect_rejected(fixture.store_path, build_image(payload, kStoreMagic, 2u), "future version");
  expect_rejected(fixture.store_path, build_image(payload, kStoreMagic, 0xFFFFFFFFu), "absurd version");
}

FE_TEST(corruption, truncation_at_every_byte_position_is_rejected) {
  RuntimeFixture fixture;
  const std::vector<std::byte> valid =
      build_image(fe::detail::encode_durable_payload(base_state()), kStoreMagic, kStoreVersion);
  FE_REQUIRE(valid.size() > 64);
  for (std::size_t length = 0; length < valid.size(); ++length) {
    expect_rejected(fixture.store_path,
                    std::vector<std::byte>(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(length)),
                    "truncation at " + std::to_string(length));
  }
}

FE_TEST(corruption, a_single_flipped_bit_anywhere_is_rejected) {
  RuntimeFixture fixture;
  const std::vector<std::byte> valid =
      build_image(fe::detail::encode_durable_payload(base_state()), kStoreMagic, kStoreVersion);
  for (std::size_t index = 0; index < valid.size(); ++index) {
    std::vector<std::byte> flipped = valid;
    flipped[index] ^= std::byte{0x01};
    expect_rejected(fixture.store_path, flipped, "bit flip at " + std::to_string(index));
  }
}

FE_TEST(corruption, trailing_bytes_are_rejected) {
  RuntimeFixture fixture;
  std::vector<std::byte> valid =
      build_image(fe::detail::encode_durable_payload(base_state()), kStoreMagic, kStoreVersion);
  valid.push_back(std::byte{0x00});
  expect_rejected(fixture.store_path, valid, "one trailing byte");
  valid.push_back(std::byte{0xFF});
  expect_rejected(fixture.store_path, valid, "two trailing bytes");
}

FE_TEST(corruption, absurd_declared_counts_are_rejected_before_allocation) {
  RuntimeFixture fixture;
  const std::vector<std::byte> payload = fe::detail::encode_durable_payload(base_state());
  std::vector<std::byte> hostile = payload;
  // participant_count is the first 32-bit field after the fixed header block.
  const std::size_t offset = 8 + 16 + 8 + 8 + 8;
  hostile[offset] = std::byte{0xFF};
  hostile[offset + 1] = std::byte{0xFF};
  hostile[offset + 2] = std::byte{0xFF};
  hostile[offset + 3] = std::byte{0xFF};
  expect_rejected(fixture.store_path, build_image(hostile, kStoreMagic, kStoreVersion),
                  "absurd participant count");
}

FE_TEST(corruption, structurally_valid_but_semantically_impossible_images_are_rejected) {
  RuntimeFixture fixture;

  {
    fe::DurableState state = base_state();
    state.current_epoch = fe::CoordinatorEpoch(0);
    expect_rejected_state(fixture.store_path, state, "unestablished epoch carrying participants");
  }
  {
    fe::DurableState state = base_state();
    state.participants.push_back(state.participants.front());
    expect_rejected_state(fixture.store_path, state, "duplicate participant record");
  }
  {
    fe::DurableState state = base_state();
    fe::DurableParticipant second = state.participants.front();
    second.id = participant("publisher.other");
    second.registration_generation = fe::RegistrationGeneration(2);
    state.registration_floor = fe::RegistrationGeneration(2);
    state.participants.push_back(second);
    expect_rejected_state(fixture.store_path, state, "one worker boot bound twice");
  }
  {
    fe::DurableState state = base_state();
    state.participants.front().fence_generation = fe::FencingGeneration(9);
    expect_rejected_state(fixture.store_path, state, "fence generation above the durable floor");
  }
  {
    fe::DurableState state = base_state();
    state.participants.front().incarnation_floor = fe::IncarnationSequence(2);
    expect_rejected_state(fixture.store_path, state, "fencing floor at the incarnation allocator");
  }
  {
    fe::DurableState state = base_state();
    state.participants.front().fenced_boots.push_back(state.participants.front().active_boot);
    expect_rejected_state(fixture.store_path, state, "active worker boot also listed as fenced");
  }
  {
    fe::DurableState state = base_state();
    state.participants.front().next_incarnation = fe::IncarnationSequence(1);
    expect_rejected_state(fixture.store_path, state, "incarnation allocator at the active incarnation");
  }
  {
    fe::DurableState state = base_state();
    state.participants.front().registration_generation = fe::RegistrationGeneration(7);
    expect_rejected_state(fixture.store_path, state, "registration generation above the floor");
  }
  {
    fe::DurableState state = base_state();
    state.participants.front().has_active_boot = true;
    state.participants.front().active_boot = fe::WorkerBootId{};
    expect_rejected_state(fixture.store_path, state, "nil active worker boot");
  }
  {
    fe::DurableState state = base_state();
    state.participants.front().retired = true;
    expect_rejected_state(fixture.store_path, state, "retired participant with an active incarnation");
  }
  {
    fe::DurableState state = base_state();
    state.participants.front().policy = static_cast<fe::IncarnationPolicy>(9);
    expect_rejected_state(fixture.store_path, state, "unknown incarnation policy");
  }
  {
    fe::DurableState state = base_state();
    fe::AuthorityScope hostile = *fe::AuthorityScope::parse(static_cast<fe::ScopeKind>(9), "subject");
    state.participants.front().scopes.insert(hostile, 32);
    expect_rejected_state(fixture.store_path, state, "unknown scope kind");
  }
  {
    fe::DurableState state = base_state();
    fe::EpochTransitionRecord transition;
    transition.id = fe::EpochTransitionId::generate();
    transition.from_epoch = fe::CoordinatorEpoch(5);
    transition.to_epoch = fe::CoordinatorEpoch(6);
    transition.generation = fe::EpochGeneration(1);
    transition.coordinator_boot = state.coordinator_boot;
    state.transitions.push_back(transition);
    expect_rejected_state(fixture.store_path, state, "current epoch below a recorded transition target");
  }
  {
    fe::DurableState state = base_state();
    fe::EpochTransitionRecord transition;
    transition.id = fe::EpochTransitionId::generate();
    transition.from_epoch = fe::CoordinatorEpoch(5);
    transition.to_epoch = fe::CoordinatorEpoch(4);
    transition.generation = fe::EpochGeneration(1);
    transition.coordinator_boot = state.coordinator_boot;
    state.transitions.push_back(transition);
    expect_rejected_state(fixture.store_path, state, "non-advancing transition");
  }
  {
    fe::DurableState state = base_state();
    state.transitions.resize(9);
    for (std::size_t index = 0; index < state.transitions.size(); ++index) {
      state.transitions[index].id = fe::EpochTransitionId::generate();
      state.transitions[index].from_epoch = fe::CoordinatorEpoch(index);
      state.transitions[index].to_epoch = fe::CoordinatorEpoch(index + 1);
      state.transitions[index].generation = fe::EpochGeneration(index + 1);
      state.transitions[index].coordinator_boot = state.coordinator_boot;
    }
    expect_rejected_state(fixture.store_path, state, "generation below the transition count");
  }
  {
    fe::DurableState state = base_state();
    fe::FenceRecord fence;
    fence.boot = fe::WorkerBootId::generate();
    fence.participant = state.participants.front().id;
    fence.reason = static_cast<fe::FenceReason>(200);
    fence.generation = fe::FencingGeneration(5);
    fence.epoch = state.current_epoch;
    state.recent_fences.push_back(fence);
    expect_rejected_state(fixture.store_path, state, "unknown fence reason");
  }
  {
    fe::DurableState state = base_state();
    fe::FenceRecord fence;
    fence.boot = fe::WorkerBootId::generate();
    fence.participant = state.participants.front().id;
    fence.reason = fe::FenceReason::ProcessLoss;
    fence.generation = fe::FencingGeneration(6);
    fence.epoch = state.current_epoch;
    state.recent_fences.push_back(fence);
    expect_rejected_state(fixture.store_path, state, "fence generation above the durable floor");
  }
  {
    fe::DurableState state = base_state();
    fe::AuthorityGrant grant;
    grant.id = fe::AuthorityGrantId::generate();
    grant.epoch = state.current_epoch;
    grant.participant = state.participants.front().id;
    grant.boot = state.participants.front().active_boot;
    grant.incarnation = fe::IncarnationSequence(1);
    grant.scopes = default_scopes();
    grant.generation = fe::GrantGeneration(1);
    state.grants.push_back(grant);
    fe::AuthorityGrant delegate = grant;
    delegate.id = fe::AuthorityGrantId::generate();
    delegate.parent = fe::AuthorityGrantId::generate();
    state.grants.push_back(delegate);
    expect_rejected_state(fixture.store_path, state, "delegated grant without a parent");
  }
  {
    fe::DurableState state = base_state();
    fe::AuthorityGrant grant;
    grant.id = fe::AuthorityGrantId::generate();
    grant.epoch = state.current_epoch;
    grant.participant = state.participants.front().id;
    grant.boot = state.participants.front().active_boot;
    grant.incarnation = fe::IncarnationSequence(1);
    grant.scopes = default_scopes();
    grant.generation = fe::GrantGeneration(1);
    state.grants.push_back(grant);
    state.grants.push_back(grant);
    expect_rejected_state(fixture.store_path, state, "duplicate grant identifier");
  }
  {
    fe::DurableState state = base_state();
    state.control_attempts.push_back(fe::IdempotencyRecord{});
    state.control_attempts.back().attempt = fe::MutationAttemptId::generate();
    state.control_attempts.back().operation = fe::OperationKind::RegisterParticipant;
    expect_rejected_state(fixture.store_path, state, "non control-plane operation in the control table");
  }
  {
    fe::DurableState state = base_state();
    state.control_attempts.push_back(fe::IdempotencyRecord{});
    expect_rejected_state(fixture.store_path, state, "nil attempt identifier");
  }
}

FE_TEST(corruption, a_valid_image_is_accepted_and_round_trips) {
  RuntimeFixture fixture;
  const fe::DurableState state = base_state();
  const std::vector<std::byte> image =
      build_image(fe::detail::encode_durable_payload(state), kStoreMagic, kStoreVersion);
  fe::EpochStore store = make_store(fixture.store_path);
  const fe::DurableState decoded = store.decode(std::span<const std::byte>(image));
  FE_REQUIRE_EQ(decoded.current_epoch, state.current_epoch);
  FE_REQUIRE_EQ(decoded.participants.size(), state.participants.size());
  FE_REQUIRE_EQ(decoded.participants.front().active_boot, state.participants.front().active_boot);
}

FE_TEST(corruption, the_library_encoder_refuses_to_produce_an_impossible_image) {
  RuntimeFixture fixture;
  const fe::DurableState valid = base_state();
  fe::EpochStore store = make_store(fixture.store_path);
  FE_REQUIRE_NO_THROW(store.encode(valid));

  fe::DurableState impossible = valid;
  impossible.participants.front().fence_generation = fe::FencingGeneration(99);
  FE_REQUIRE_THROWS(store.encode(impossible));
}

FE_TEST(corruption, a_corrupt_primary_image_fails_the_load) {
  RuntimeFixture fixture;
  fe::EpochRuntimeOptions options = runtime_options(fixture);
  {
    fe::EpochRuntime runtime(options);
  }
  const std::vector<std::byte> valid = [&]() {
    std::ifstream in(fixture.store_path, std::ios::binary);
    FE_REQUIRE(in.good());
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
      bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }
    return bytes;
  }();
  FE_REQUIRE(valid.size() > 40);
  std::vector<std::byte> corrupt = valid;
  corrupt[corrupt.size() / 2] ^= std::byte{0xFF};

  std::ofstream out(fixture.store_path, std::ios::binary | std::ios::trunc);
  FE_REQUIRE(out.good());
  out.write(reinterpret_cast<const char*>(corrupt.data()), static_cast<std::streamsize>(corrupt.size()));
  out.close();

  expect_runtime_construction_fails(options);
}
