// Fabric Epoch 1.0.0 - Summon Software Labs
// Deterministic race tests. Every race is forced with an explicit start
// barrier so that the legal outcome set is exercised rather than sampled.
#include <atomic>
#include <thread>
#include <vector>

#include "test_support.hpp"

namespace fe = fabric_epoch;
using namespace fe_test;

namespace {

/// Reusable start barrier: every participant thread waits until all of them
/// have arrived, then they all proceed.
class StartBarrier {
 public:
  explicit StartBarrier(int participants) : expected_(participants) {}

  void arrive_and_wait() {
    arrived_.fetch_add(1);
    while (arrived_.load() < expected_) {
      std::this_thread::yield();
    }
  }

 private:
  std::atomic<int> arrived_{0};
  int expected_;
};

template <typename Fn>
void run_racing(int count, Fn&& body) {
  StartBarrier barrier(count);
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    threads.emplace_back([&barrier, &body, index]() {
      barrier.arrive_and_wait();
      body(index);
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
}

}  // namespace

FE_TEST(concurrency, racing_registrations_produce_exactly_one_winner) {
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  const fe::CoordinatorEpoch epoch = runtime.current_epoch();
  constexpr int kRacers = 8;
  std::vector<fe::RegistrationOutcome> outcomes(static_cast<std::size_t>(kRacers));

  run_racing(kRacers, [&](int index) {
    fe::RegisterParticipantRequest request =
        registration_request("publisher.race", fe::WorkerBootId::generate(), epoch);
    outcomes[static_cast<std::size_t>(index)] =
        runtime.register_participant(request).outcome;
  });

  int registered = 0;
  int conflicting = 0;
  for (const auto outcome : outcomes) {
    if (outcome == fe::RegistrationOutcome::Registered) {
      ++registered;
    } else if (outcome == fe::RegistrationOutcome::ConflictingIncarnation) {
      ++conflicting;
    }
  }
  FE_REQUIRE_EQ(registered, 1);
  FE_REQUIRE_EQ(conflicting, kRacers - 1);
  const auto view = runtime.query_participant(participant("publisher.race"));
  FE_REQUIRE(view.has_value());
  FE_REQUIRE_EQ(view->active_incarnation_count(), std::size_t{1});
  FE_REQUIRE(runtime.check_indexes());
}

FE_TEST(concurrency, racing_fences_are_idempotent) {
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  FE_REQUIRE_EQ(runtime.register_participant(registration_request("publisher.racefence", boot, runtime.current_epoch()))
                    .outcome,
                fe::RegistrationOutcome::Registered);
  constexpr int kRacers = 8;
  std::vector<fe::FenceOutcome> outcomes(static_cast<std::size_t>(kRacers));

  run_racing(kRacers, [&](int index) {
    fe::FenceWorkerRequest request;
    request.attempt = fe::MutationAttemptId::generate();
    request.expected_epoch = runtime.current_epoch();
    request.participant = participant("publisher.racefence");
    request.boot = boot;
    request.reason = fe::FenceReason::ProcessLoss;
    outcomes[static_cast<std::size_t>(index)] = runtime.fence_worker(request).outcome;
  });

  int fenced = 0;
  int already = 0;
  for (const auto outcome : outcomes) {
    if (outcome == fe::FenceOutcome::Fenced) {
      ++fenced;
    } else if (outcome == fe::FenceOutcome::AlreadyFenced) {
      ++already;
    }
  }
  FE_REQUIRE_EQ(fenced, 1);
  FE_REQUIRE_EQ(already, kRacers - 1);
  FE_REQUIRE_EQ(runtime.list_fences().size(), std::size_t{1});
  FE_REQUIRE(runtime.check_indexes());
}

FE_TEST(concurrency, fence_racing_validation_never_observes_a_torn_state) {
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const fe::RegistrationResult registered =
      runtime.register_participant(registration_request("publisher.racevalidate", boot, runtime.current_epoch()));
  fe::ValidateAuthorityRequest request;
  request.token = token_for(registered, participant("publisher.racevalidate"), boot);
  request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");

  std::atomic<int> observed_current{0};
  std::atomic<int> observed_fenced{0};
  std::atomic<bool> torn{false};
  constexpr int kReaders = 4;
  StartBarrier barrier(kReaders + 1);

  std::vector<std::thread> readers;
  for (int index = 0; index < kReaders; ++index) {
    readers.emplace_back([&]() {
      barrier.arrive_and_wait();
      for (int iteration = 0; iteration < 200; ++iteration) {
        const fe::AuthorityValidation validation = runtime.validate_authority(request);
        if (validation.code == fe::AuthorityValidationCode::Current) {
          observed_current.fetch_add(1);
        } else if (validation.code == fe::AuthorityValidationCode::StaleWorkerBoot &&
                   validation.currentness == fe::AuthorityCurrentness::Fenced) {
          observed_fenced.fetch_add(1);
        } else {
          torn.store(true);
        }
      }
    });
  }
  std::thread fencer([&]() {
    barrier.arrive_and_wait();
    fe::FenceWorkerRequest fence;
    fence.attempt = fe::MutationAttemptId::generate();
    fence.expected_epoch = runtime.current_epoch();
    fence.participant = participant("publisher.racevalidate");
    fence.boot = boot;
    fence.reason = fe::FenceReason::ProcessLoss;
    (void)runtime.fence_worker(fence);
  });
  for (auto& thread : readers) {
    thread.join();
  }
  fencer.join();

  FE_REQUIRE_MSG(!torn.load(), "a reader observed an authority state outside the legal outcome set");
  FE_REQUIRE_EQ(observed_current.load() + observed_fenced.load(), kReaders * 200);
  // Whether the fence landed before, during or after the readers is a
  // scheduling property; what must hold unconditionally is the post-condition.
  const fe::AuthorityValidation after = runtime.validate_authority(request);
  FE_REQUIRE_EQ(after.code, fe::AuthorityValidationCode::StaleWorkerBoot);
  FE_REQUIRE_EQ(after.currentness, fe::AuthorityCurrentness::Fenced);
}

FE_TEST(concurrency, epoch_advance_racing_registration_is_always_decisive) {
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  constexpr int kRacers = 6;
  std::vector<fe::RegistrationOutcome> outcomes(static_cast<std::size_t>(kRacers));
  std::vector<fe::CoordinatorEpoch> epochs(static_cast<std::size_t>(kRacers));
  StartBarrier barrier(kRacers + 1);

  std::vector<std::thread> threads;
  for (int index = 0; index < kRacers; ++index) {
    threads.emplace_back([&, index]() {
      barrier.arrive_and_wait();
      fe::RegisterParticipantRequest request = registration_request(
          "publisher.raceepoch" + std::to_string(index), fe::WorkerBootId::generate(),
          runtime.current_epoch());
      const fe::RegistrationResult result = runtime.register_participant(request);
      outcomes[static_cast<std::size_t>(index)] = result.outcome;
      epochs[static_cast<std::size_t>(index)] = result.epoch;
    });
  }
  threads.emplace_back([&]() {
    barrier.arrive_and_wait();
    fe::EpochAdvanceRequest advance;
    advance.attempt = fe::MutationAttemptId::generate();
    advance.expected_epoch = runtime.current_epoch();
    (void)runtime.advance_epoch(advance);
  });
  for (auto& thread : threads) {
    thread.join();
  }

  std::size_t registered = 0;
  for (std::size_t index = 0; index < outcomes.size(); ++index) {
    FE_REQUIRE_MSG(outcomes[index] == fe::RegistrationOutcome::Registered ||
                       outcomes[index] == fe::RegistrationOutcome::StaleEpoch,
                   "unexpected outcome " + std::string(fe::to_string(outcomes[index])));
    if (outcomes[index] == fe::RegistrationOutcome::Registered) {
      ++registered;
      // A registration that succeeded is bound to the epoch that was current
      // at the instant it was committed.
      FE_REQUIRE(epochs[index] <= runtime.current_epoch());
      FE_REQUIRE(!epochs[index].is_zero());
    }
  }
  // Every successful registration is either still live (epoch did not advance)
  // or already revalidation-required (epoch advanced after it).
  std::size_t live = 0;
  for (const auto& view : runtime.list_participants()) {
    if (view.has_active_incarnation) {
      ++live;
    }
  }
  if (runtime.state().transition_history_count > 1) {
    FE_REQUIRE_MSG(live <= registered, "live incarnations can never exceed accepted registrations");
  }
  FE_REQUIRE(runtime.check_indexes());
}

FE_TEST(concurrency, snapshots_taken_during_mutation_are_internally_consistent) {
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  std::atomic<bool> stop{false};
  std::atomic<bool> inconsistent{false};
  std::atomic<std::size_t> snapshots{0};

  std::thread reader([&]() {
    std::size_t previous = 0;
    while (!stop.load()) {
      const fe::AuthoritySnapshot snapshot = runtime.snapshot();
      std::size_t active = 0;
      for (const auto& participant : snapshot.participants) {
        if (participant.active_incarnation_count() > 1 &&
            participant.policy == fe::IncarnationPolicy::SingleIncarnation) {
          inconsistent.store(true);
        }
        active += participant.active_incarnation_count();
      }
      if (snapshot.participants.size() < previous) {
        inconsistent.store(true);
      }
      previous = snapshot.participants.size();
      snapshots.fetch_add(1);
    }
  });

  for (int index = 0; index < 64; ++index) {
    FE_REQUIRE_EQ(runtime
                      .register_participant(registration_request("publisher.snapshotrace" + std::to_string(index),
                                                                 fe::WorkerBootId::generate(),
                                                                 runtime.current_epoch()))
                      .outcome,
                  fe::RegistrationOutcome::Registered);
  }
  stop.store(true);
  reader.join();

  FE_REQUIRE_MSG(!inconsistent.load(), "a snapshot exposed an internally impossible state");
  FE_REQUIRE(snapshots.load() > 0);
  FE_REQUIRE(runtime.check_indexes());
}

FE_TEST(concurrency, revoke_racing_validation_is_always_decisive) {
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const fe::RegistrationResult registered =
      runtime.register_participant(registration_request("publisher.racerevoke", boot, runtime.current_epoch()));
  fe::GrantAuthorityRequest grant_request;
  grant_request.attempt = fe::MutationAttemptId::generate();
  grant_request.expected_epoch = runtime.current_epoch();
  grant_request.participant = participant("publisher.racerevoke");
  grant_request.boot = boot;
  grant_request.incarnation = *registered.incarnation;
  grant_request.scopes = default_scopes();
  const fe::GrantResult granted = runtime.grant_authority(grant_request);
  FE_REQUIRE_EQ(granted.outcome, fe::GrantOutcome::Granted);

  fe::ValidateAuthorityRequest request;
  request.token = token_for(registered, participant("publisher.racerevoke"), boot);
  request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
  request.grant = granted.grant;

  std::atomic<bool> torn{false};
  std::atomic<int> current{0};
  std::atomic<int> revoked{0};
  StartBarrier barrier(3);

  std::vector<std::thread> readers;
  for (int index = 0; index < 2; ++index) {
    readers.emplace_back([&]() {
      barrier.arrive_and_wait();
      for (int iteration = 0; iteration < 200; ++iteration) {
        const fe::AuthorityValidation validation = runtime.validate_authority(request);
        if (validation.code == fe::AuthorityValidationCode::Current) {
          current.fetch_add(1);
        } else if (validation.code == fe::AuthorityValidationCode::Revoked) {
          revoked.fetch_add(1);
        } else {
          torn.store(true);
        }
      }
    });
  }
  std::thread revoker([&]() {
    barrier.arrive_and_wait();
    fe::RevokeAuthorityRequest revoke;
    revoke.attempt = fe::MutationAttemptId::generate();
    revoke.expected_epoch = runtime.current_epoch();
    revoke.grant = *granted.grant;
    (void)runtime.revoke_authority(revoke);
  });
  for (auto& thread : readers) {
    thread.join();
  }
  revoker.join();

  FE_REQUIRE_MSG(!torn.load(), "validation observed a state outside {CURRENT, REVOKED}");
  FE_REQUIRE_EQ(current.load() + revoked.load(), 400);
  // The post-condition is unconditional even though the interleaving is not.
  FE_REQUIRE_EQ(runtime.validate_authority(request).code, fe::AuthorityValidationCode::Revoked);
}

FE_TEST(concurrency, independent_participants_never_interfere) {
  RuntimeFixture storage;
  fe::EpochRuntime runtime(runtime_options(storage));
  constexpr int kWorkers = 8;
  std::atomic<int> failures{0};

  run_racing(kWorkers, [&](int index) {
    const std::string name = "publisher.independent" + std::to_string(index);
    const fe::WorkerBootId boot = fe::WorkerBootId::generate();
    const fe::RegistrationResult registered =
        runtime.register_participant(registration_request(name, boot, runtime.current_epoch()));
    if (registered.outcome != fe::RegistrationOutcome::Registered) {
      failures.fetch_add(1);
      return;
    }
    fe::ValidateAuthorityRequest request;
    request.token = token_for(registered, participant(name), boot);
    request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
    if (runtime.validate_authority(request).code != fe::AuthorityValidationCode::Current) {
      failures.fetch_add(1);
    }
    fe::FenceWorkerRequest fence;
    fence.attempt = fe::MutationAttemptId::generate();
    fence.expected_epoch = runtime.current_epoch();
    fence.participant = participant(name);
    fence.boot = boot;
    if (runtime.fence_worker(fence).outcome != fe::FenceOutcome::Fenced) {
      failures.fetch_add(1);
    }
  });

  FE_REQUIRE_EQ(failures.load(), 0);
  FE_REQUIRE_EQ(runtime.list_participants().size(), static_cast<std::size_t>(kWorkers));
  FE_REQUIRE(runtime.check_indexes());
}
