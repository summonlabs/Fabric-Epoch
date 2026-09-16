// Fabric Epoch 1.0.0 - Summon Software Labs
// Guarded benchmarks. Every benchmark reports the number of *completed*
// operations and the measured throughput of the machine that runs it; no
// machine-independent performance claim is made or implied.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "fabric_epoch/fabric_epoch.hpp"

namespace fe = fabric_epoch;

namespace {

using Clock = std::chrono::steady_clock;

struct Workspace {
  std::filesystem::path path;

  Workspace() {
    std::array<std::byte, 16> entropy{};
    fe::detail::fill_random_16(entropy.data());
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string suffix;
    for (const std::byte value : entropy) {
      const auto raw = static_cast<unsigned>(value);
      suffix.push_back(kDigits[(raw >> 4) & 0xFu]);
      suffix.push_back(kDigits[raw & 0xFu]);
    }
    std::error_code error;
    path = std::filesystem::temp_directory_path(error) / ("fabric_epoch_bench_" + suffix);
    std::filesystem::create_directories(path, error);
    if (error) {
      std::fprintf(stderr, "cannot create the benchmark workspace\n");
      std::exit(1);
    }
  }
  ~Workspace() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  Workspace(const Workspace&) = delete;
  Workspace& operator=(const Workspace&) = delete;

  [[nodiscard]] std::filesystem::path store() const { return path / "epoch.store"; }
};

void report(const char* name, std::size_t operations, double seconds) {
  const double rate = seconds > 0.0 ? static_cast<double>(operations) / seconds : 0.0;
  std::printf("%-32s completed=%-8zu seconds=%-9.4f ops_per_second=%.0f\n", name, operations, seconds,
              rate);
}

[[nodiscard]] double seconds_between(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double>(end - start).count();
}

[[nodiscard]] fe::AuthorityScope scope(fe::ScopeKind kind, const std::string& subject) {
  const auto parsed = fe::AuthorityScope::parse(kind, subject);
  if (!parsed.has_value()) {
    std::fprintf(stderr, "malformed benchmark scope\n");
    std::exit(1);
  }
  return *parsed;
}

[[nodiscard]] fe::ScopeSet default_scopes() {
  fe::ScopeSet set;
  set.insert(scope(fe::ScopeKind::Fabric, "fabric-main"), 8);
  return set;
}

[[nodiscard]] fe::EpochRuntimeOptions runtime_options(const Workspace& workspace,
                                                      std::size_t capacity) {
  fe::EpochRuntimeOptions options;
  options.store_path = workspace.store();
  options.provenance = fe::BoundedText::from_validated("benchmark");
  options.limits.max_participants = std::max<std::size_t>(capacity + 4096, 8192);
  options.limits.max_active_workers = options.limits.max_participants;
  options.limits.max_authority_grants = options.limits.max_participants;
  options.limits.max_recent_fences = 65536;
  options.limits.max_fenced_boots_per_participant = 8;
  return options;
}

[[nodiscard]] fe::RegisterParticipantRequest registration(const std::string& name,
                                                          const fe::WorkerBootId& boot,
                                                          fe::CoordinatorEpoch epoch) {
  fe::RegisterParticipantRequest request;
  request.attempt = fe::MutationAttemptId::generate();
  request.expected_epoch = epoch;
  request.participant = *fe::ParticipantId::parse(name);
  request.boot = boot;
  request.scopes = default_scopes();
  return request;
}

[[nodiscard]] fe::AuthorityToken token_of(const fe::RegistrationResult& result,
                                          const fe::ParticipantId& participant,
                                          const fe::WorkerBootId& boot) {
  fe::AuthorityToken token;
  token.epoch = result.epoch;
  token.participant = participant;
  token.boot = boot;
  token.incarnation = *result.incarnation;
  token.registration_generation = *result.registration_generation;
  return token;
}

struct Population {
  std::vector<fe::ParticipantId> participants;
  std::vector<fe::WorkerBootId> boots;
  std::vector<fe::RegistrationResult> results;
};

/// Registers a population with a single durable commit so that the measured
/// per-operation benchmarks start from a representative state without paying
/// quadratic commit costs during setup.
[[nodiscard]] Population seed(fe::EpochRuntime& runtime, std::size_t count,
                              const std::string& prefix) {
  Population population;
  std::vector<fe::RegisterParticipantRequest> requests;
  requests.reserve(count);
  population.participants.reserve(count);
  population.boots.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const std::string name = prefix + std::to_string(index);
    const fe::WorkerBootId boot = fe::WorkerBootId::generate();
    population.participants.push_back(*fe::ParticipantId::parse(name));
    population.boots.push_back(boot);
    requests.push_back(registration(name, boot, runtime.current_epoch()));
  }
  population.results = runtime.register_participants(requests);
  for (const auto& result : population.results) {
    if (result.outcome != fe::RegistrationOutcome::Registered) {
      std::fprintf(stderr, "benchmark setup failed to register a participant\n");
      std::exit(1);
    }
  }
  return population;
}

void bench_registration(Workspace& workspace) {
  fe::EpochRuntime runtime(runtime_options(workspace, 8192));
  constexpr std::size_t kCount = 1000;
  std::vector<fe::RegisterParticipantRequest> requests;
  requests.reserve(kCount);
  for (std::size_t index = 0; index < kCount; ++index) {
    requests.push_back(registration("bench.register" + std::to_string(index),
                                    fe::WorkerBootId::generate(), runtime.current_epoch()));
  }
  std::size_t completed = 0;
  const auto start = Clock::now();
  for (auto& request : requests) {
    if (runtime.register_participant(request).outcome == fe::RegistrationOutcome::Registered) {
      ++completed;
    }
  }
  report("register participant", completed, seconds_between(start, Clock::now()));
}

void bench_bulk_registration(Workspace& workspace) {
  fe::EpochRuntime runtime(runtime_options(workspace, 20000));
  constexpr std::size_t kCount = 10000;
  std::vector<fe::RegisterParticipantRequest> requests;
  requests.reserve(kCount);
  for (std::size_t index = 0; index < kCount; ++index) {
    requests.push_back(registration("bench.bulk" + std::to_string(index), fe::WorkerBootId::generate(),
                                    runtime.current_epoch()));
  }
  const auto start = Clock::now();
  const std::vector<fe::RegistrationResult> results = runtime.register_participants(requests);
  const auto end = Clock::now();
  std::size_t completed = 0;
  for (const auto& result : results) {
    if (result.outcome == fe::RegistrationOutcome::Registered) {
      ++completed;
    }
  }
  report("bulk register participant", completed, seconds_between(start, end));
}

void bench_validation(Workspace& workspace) {
  fe::EpochRuntime runtime(runtime_options(workspace, 8192));
  Population population = seed(runtime, 1000, "bench.validate");
  constexpr std::size_t kIterations = 200000;
  std::size_t completed = 0;
  const auto start = Clock::now();
  for (std::size_t index = 0; index < kIterations; ++index) {
    const std::size_t slot = index % population.participants.size();
    fe::ValidateAuthorityRequest request;
    request.token = token_of(population.results[slot], population.participants[slot],
                             population.boots[slot]);
    request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
    if (runtime.validate_authority(request).code == fe::AuthorityValidationCode::Current) {
      ++completed;
    }
  }
  report("validate authority", completed, seconds_between(start, Clock::now()));
}

void bench_fence_and_reincarnate(Workspace& workspace) {
  fe::EpochRuntime runtime(runtime_options(workspace, 8192));
  Population population = seed(runtime, 1000, "bench.fence");

  std::size_t fenced = 0;
  const auto fence_start = Clock::now();
  for (std::size_t index = 0; index < population.participants.size(); ++index) {
    fe::FenceWorkerRequest request;
    request.attempt = fe::MutationAttemptId::generate();
    request.expected_epoch = runtime.current_epoch();
    request.participant = population.participants[index];
    request.boot = population.boots[index];
    request.reason = fe::FenceReason::ProcessLoss;
    if (runtime.fence_worker(request).outcome == fe::FenceOutcome::Fenced) {
      ++fenced;
    }
  }
  report("fence worker", fenced, seconds_between(fence_start, Clock::now()));

  std::size_t reincarnated = 0;
  const auto reincarnate_start = Clock::now();
  for (std::size_t index = 0; index < population.participants.size(); ++index) {
    if (runtime
            .register_participant(registration(population.participants[index].value(),
                                               fe::WorkerBootId::generate(), runtime.current_epoch()))
            .outcome == fe::RegistrationOutcome::Registered) {
      ++reincarnated;
    }
  }
  report("reincarnate worker", reincarnated, seconds_between(reincarnate_start, Clock::now()));
}

void bench_epoch_operations(Workspace& workspace) {
  fe::EpochRuntime runtime(runtime_options(workspace, 20000));
  Population population = seed(runtime, 10000, "bench.epoch");

  constexpr std::size_t kAdvances = 100;
  std::size_t advanced = 0;
  const auto advance_start = Clock::now();
  for (std::size_t index = 0; index < kAdvances; ++index) {
    fe::EpochAdvanceRequest request;
    request.attempt = fe::MutationAttemptId::generate();
    request.expected_epoch = runtime.current_epoch();
    if (runtime.advance_epoch(request).outcome == fe::EpochAdvanceOutcome::Advanced) {
      ++advanced;
    }
  }
  report("mass epoch invalidation", advanced, seconds_between(advance_start, Clock::now()));

  // Reverse lookup: worker boot -> owning participant, through the fence index.
  constexpr std::size_t kLookups = 100000;
  std::size_t resolved = 0;
  const auto lookup_start = Clock::now();
  for (std::size_t index = 0; index < kLookups; ++index) {
    const std::size_t slot = index % population.boots.size();
    const fe::FenceStatus status = runtime.query_fence(population.participants[slot],
                                                       population.boots[slot]);
    if (status.participant == population.participants[slot]) {
      ++resolved;
    }
  }
  report("reverse boot lookup", resolved, seconds_between(lookup_start, Clock::now()));
}

void bench_snapshot_and_digest(Workspace& workspace) {
  fe::EpochRuntime runtime(runtime_options(workspace, 20000));
  (void)seed(runtime, 10000, "bench.snapshot");
  constexpr std::size_t kSnapshots = 20;
  std::size_t completed = 0;
  const auto start = Clock::now();
  for (std::size_t index = 0; index < kSnapshots; ++index) {
    const fe::AuthoritySnapshot snapshot = runtime.snapshot();
    if (snapshot.participants.size() == 10000 && !snapshot.digest.to_string().empty()) {
      ++completed;
    }
  }
  report("snapshot and digest", completed, seconds_between(start, Clock::now()));
}

void bench_persistence(Workspace& workspace) {
  fe::EpochRuntime runtime(runtime_options(workspace, 20000));
  (void)seed(runtime, 10000, "bench.persist");

  constexpr std::size_t kSaves = 20;
  std::size_t saved = 0;
  const auto save_start = Clock::now();
  for (std::size_t index = 0; index < kSaves; ++index) {
    runtime.flush();
    ++saved;
  }
  report("persistence save", saved, seconds_between(save_start, Clock::now()));

  fe::StoreOptions store_options;
  store_options.path = workspace.store();
  fe::EpochStore store(store_options);
  constexpr std::size_t kLoads = 20;
  std::size_t loaded = 0;
  const auto load_start = Clock::now();
  for (std::size_t index = 0; index < kLoads; ++index) {
    if (store.load().participants.size() == 10000) {
      ++loaded;
    }
  }
  report("persistence load", loaded, seconds_between(load_start, Clock::now()));
}

void bench_grants(Workspace& workspace) {
  fe::EpochRuntime runtime(runtime_options(workspace, 8192));
  Population population = seed(runtime, 1000, "bench.grant");
  std::size_t granted = 0;
  const auto start = Clock::now();
  for (std::size_t index = 0; index < population.participants.size(); ++index) {
    fe::GrantAuthorityRequest request;
    request.attempt = fe::MutationAttemptId::generate();
    request.expected_epoch = runtime.current_epoch();
    request.participant = population.participants[index];
    request.boot = population.boots[index];
    request.incarnation = *population.results[index].incarnation;
    request.scopes = default_scopes();
    if (runtime.grant_authority(request).outcome == fe::GrantOutcome::Granted) {
      ++granted;
    }
  }
  report("grant authority", granted, seconds_between(start, Clock::now()));
}

}  // namespace

int main(int argc, char** argv) {
  bool all = true;
  std::string selection;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument.rfind("--only=", 0) == 0) {
      selection = argument.substr(7);
      all = false;
    } else {
      std::fprintf(stderr, "usage: fabric_epoch_benchmarks [--only=<name>]\n");
      return 2;
    }
  }

  auto selected = [&](const char* name) { return all || selection == name; };

  std::printf("Fabric Epoch %s benchmarks\n", std::string(fe::version_string).c_str());
  if (selected("registration")) {
    Workspace workspace;
    bench_registration(workspace);
  }
  if (selected("bulk")) {
    Workspace workspace;
    bench_bulk_registration(workspace);
  }
  if (selected("validation")) {
    Workspace workspace;
    bench_validation(workspace);
  }
  if (selected("fencing")) {
    Workspace workspace;
    bench_fence_and_reincarnate(workspace);
  }
  if (selected("epoch")) {
    Workspace workspace;
    bench_epoch_operations(workspace);
  }
  if (selected("snapshot")) {
    Workspace workspace;
    bench_snapshot_and_digest(workspace);
  }
  if (selected("persistence")) {
    Workspace workspace;
    bench_persistence(workspace);
  }
  if (selected("grants")) {
    Workspace workspace;
    bench_grants(workspace);
  }
  return 0;
}
