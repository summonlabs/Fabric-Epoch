// Fabric Epoch 1.0.0 - Summon Software Labs
// Realistic scale exercise. Timings are reported, never asserted: they measure
// completed operations on the machine that runs the suite.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double seconds_between(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double>(end - start).count();
}

void report(const std::string& label, std::size_t operations, double seconds) {
  const double rate = seconds > 0.0 ? static_cast<double>(operations) / seconds : 0.0;
  std::printf("        [scale] %-42s ops=%-8zu seconds=%-9.4f ops_per_second=%.0f\n", label.c_str(),
              operations, seconds, rate);
}

struct Population {
  std::vector<fe::RegisterParticipantRequest> requests;
};

Population build_population(std::size_t count, fe::CoordinatorEpoch epoch) {
  Population population;
  population.requests.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    fe::RegisterParticipantRequest request;
    request.attempt = fe::MutationAttemptId::generate();
    request.expected_epoch = epoch;
    request.participant = participant("publisher.scale" + std::to_string(index));
    request.boot = fe::WorkerBootId::generate();
    request.scopes = default_scopes();
    population.requests.push_back(std::move(request));
  }
  return population;
}

void exercise_scale(std::size_t count) {
  RuntimeFixture storage;
  fe::EpochRuntimeOptions options = runtime_options(storage);
  options.limits.max_participants = 300000;
  options.limits.max_active_workers = 300000;
  fe::EpochRuntime runtime(options);

  Population population = build_population(count, runtime.current_epoch());
  const auto register_start = Clock::now();
  const std::vector<fe::RegistrationResult> results = runtime.register_participants(population.requests);
  const auto register_end = Clock::now();
  report("register participant", count, seconds_between(register_start, register_end));
  FE_REQUIRE_EQ(results.size(), count);
  for (const auto& result : results) {
    FE_REQUIRE_EQ(result.outcome, fe::RegistrationOutcome::Registered);
  }

  // Bulk authority validation over a deterministic stride sample.
  const std::size_t sample = std::min<std::size_t>(count, 2000);
  const std::size_t stride = std::max<std::size_t>(1, count / sample);
  const auto validate_start = Clock::now();
  std::size_t validated = 0;
  for (std::size_t index = 0; index < count && validated < sample; index += stride, ++validated) {
    fe::ValidateAuthorityRequest request;
    request.token.epoch = runtime.current_epoch();
    request.token.participant = population.requests[index].participant;
    request.token.boot = population.requests[index].boot;
    request.token.incarnation = *results[index].incarnation;
    request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
    FE_REQUIRE_EQ(runtime.validate_authority(request).code, fe::AuthorityValidationCode::Current);
  }
  const auto validate_end = Clock::now();
  report("validate authority", validated, seconds_between(validate_start, validate_end));

  const auto snapshot_start = Clock::now();
  const fe::AuthoritySnapshot snapshot = runtime.snapshot();
  const auto snapshot_end = Clock::now();
  report("snapshot", count, seconds_between(snapshot_start, snapshot_end));
  FE_REQUIRE_EQ(snapshot.participants.size(), count);
  FE_REQUIRE(!snapshot.digest.to_string().empty());

  const auto fence_start = Clock::now();
  std::size_t fenced = 0;
  // Every acknowledged fence rewrites the whole versioned durable image, so the
  // sample is kept proportionate to the population being fenced.
  const std::size_t fence_sample =
      count >= 100000 ? 100 : std::min<std::size_t>(count, 500);
  for (std::size_t index = 0; index < fence_sample; ++index) {
    fe::FenceWorkerRequest fence;
    fence.attempt = fe::MutationAttemptId::generate();
    fence.expected_epoch = runtime.current_epoch();
    fence.participant = population.requests[index].participant;
    fence.boot = population.requests[index].boot;
    fence.reason = fe::FenceReason::ProcessLoss;
    FE_REQUIRE_EQ(runtime.fence_worker(fence).outcome, fe::FenceOutcome::Fenced);
    ++fenced;
  }
  const auto fence_end = Clock::now();
  report("fence worker", fenced, seconds_between(fence_start, fence_end));

  const auto reincarnate_start = Clock::now();
  std::vector<fe::RegisterParticipantRequest> reincarnations;
  reincarnations.reserve(fence_sample);
  for (std::size_t index = 0; index < fence_sample; ++index) {
    reincarnations.push_back(registration_request(population.requests[index].participant.value(),
                                                  fe::WorkerBootId::generate(), runtime.current_epoch()));
  }
  const std::vector<fe::RegistrationResult> reincarnated =
      runtime.register_participants(reincarnations);
  const auto reincarnate_end = Clock::now();
  for (const auto& result : reincarnated) {
    FE_REQUIRE_EQ(result.outcome, fe::RegistrationOutcome::Registered);
  }
  report("reincarnate worker", fence_sample, seconds_between(reincarnate_start, reincarnate_end));

  const auto flush_start = Clock::now();
  runtime.flush();
  const auto flush_end = Clock::now();
  report("persistence save", count, seconds_between(flush_start, flush_end));

  const auto load_start = Clock::now();
  fe::StoreOptions store_options;
  store_options.path = storage.store_path;
  fe::EpochStore store(store_options);
  const fe::DurableState loaded = store.load();
  const auto load_end = Clock::now();
  FE_REQUIRE_EQ(loaded.participants.size(), count);
  report("persistence load", count, seconds_between(load_start, load_end));

  FE_REQUIRE(runtime.check_indexes());
  std::printf("        [scale] participants=%zu image_bytes=%llu\n", count,
              static_cast<unsigned long long>(std::filesystem::file_size(storage.store_path)));
}

}  // namespace

FE_TEST(scale, one_thousand_participants) { exercise_scale(1000); }

FE_TEST(scale, ten_thousand_participants) { exercise_scale(10000); }

FE_TEST(scale, one_hundred_thousand_participants) { exercise_scale(100000); }
