// Fabric Epoch 1.0.0 - Summon Software Labs
// Real distributed proofs: independent operating-system processes, real
// loopback sockets, real process termination, real durable restart.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "fabric_epoch/client.hpp"
#include "fabric_epoch/error.hpp"
#include "test_executables.hpp"
#include "test_support.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#if defined(_MSC_VER)
// See src/socket.cpp: ws2tcpip.h contains an inline body the static analyzer
// reports against. The suppression is scoped to this system header and covers
// no first-party test code.
#pragma warning(push)
#pragma warning(disable : 6101)
#endif
#include <ws2tcpip.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#pragma comment(lib, "ws2_32.lib")
#endif

namespace fe = fabric_epoch;
using namespace fe_test;
using namespace fabric_epoch;

namespace {

/// Budget for observing an external process. Exceeding it produces an explicit
/// assertion failure with a diagnostic; it is never a silent pass.
constexpr std::uint32_t kObserveBudgetMilliseconds = 30000;

struct CoordinatorHandle {
  std::unique_ptr<ChildProcess> process;
  std::uint16_t port = 0;
  fe::CoordinatorEpoch epoch;
  fe::CoordinatorBootId boot;
};

CoordinatorHandle start_coordinator(const std::filesystem::path& store,
                                    const std::vector<std::string>& extra = {}) {
  ChildProcessOptions options;
  options.executable = FABRIC_EPOCH_COORDINATOR_EXE;
  options.arguments = {"--store", store.string(), "--port", "0"};
  for (const auto& argument : extra) {
    options.arguments.push_back(argument);
  }
  CoordinatorHandle handle;
  handle.process = std::make_unique<ChildProcess>(options);
  const auto line = handle.process->try_read_line(kObserveBudgetMilliseconds);
  FE_REQUIRE_MSG(line.has_value(), "the coordinator process never reported readiness");
  const auto port = field_value(*line, "port");
  const auto epoch = field_value(*line, "epoch");
  FE_REQUIRE_MSG(port.has_value(), "the readiness line carried no port: " + *line);
  FE_REQUIRE_MSG(epoch.has_value(), "the readiness line carried no epoch: " + *line);
  const auto boot = field_text(*line, "coordinator_boot");
  FE_REQUIRE_MSG(boot.has_value(), "the readiness line carried no coordinator boot: " + *line);
  const auto parsed_boot = fe::CoordinatorBootId::parse(*boot);
  FE_REQUIRE(parsed_boot.has_value());
  handle.port = static_cast<std::uint16_t>(*port);
  handle.epoch = fe::CoordinatorEpoch(*epoch);
  handle.boot = *parsed_boot;
  return handle;
}

struct ClientHandle {
  std::unique_ptr<CoordinatorClient> client;
  fe::HelloAckPayload hello;
};

ClientHandle connect_client(std::uint16_t port, const char* role = "test") {
  ClientOptions options;
  options.port = port;
  ClientHandle handle;
  handle.client = std::make_unique<CoordinatorClient>(options);
  handle.client->connect();
  handle.hello = handle.client->hello(role);
  return handle;
}

struct WorkerHandle {
  std::unique_ptr<ChildProcess> process;
  fe::WorkerBootId boot;
  fe::IncarnationSequence incarnation;
  fe::RegistrationGeneration registration_generation;
};

WorkerHandle start_worker(std::uint16_t port, const std::string& participant_name,
                          const fe::WorkerBootId& boot) {
  ChildProcessOptions options;
  options.executable = FABRIC_EPOCH_WORKER_EXE;
  options.arguments = {"--port",     std::to_string(port),
                       "--participant", participant_name,
                       "--scope",    "FABRIC:fabric-main",
                       "--scope",    "OPERATION_FAMILY:publish",
                       "--boot",     boot.to_string()};
  WorkerHandle handle;
  handle.process = std::make_unique<ChildProcess>(options);
  handle.boot = boot;
  const auto line = handle.process->try_read_line(kObserveBudgetMilliseconds);
  FE_REQUIRE_MSG(line.has_value(), "the worker process never reported its registration: " + participant_name);
  FE_REQUIRE_MSG(line->find("outcome=REGISTERED") != std::string::npos, *line);
  const auto incarnation = field_value(*line, "incarnation");
  const auto generation = field_value(*line, "registration_generation");
  FE_REQUIRE_MSG(incarnation.has_value(), "the worker line carried no incarnation: " + *line);
  FE_REQUIRE_MSG(generation.has_value(), "the worker line carried no registration generation: " + *line);
  handle.incarnation = fe::IncarnationSequence(*incarnation);
  handle.registration_generation = fe::RegistrationGeneration(*generation);
  return handle;
}

fe::AuthorityValidation validate(CoordinatorClient& client, fe::CoordinatorEpoch epoch,
                                 const std::string& participant_name, const fe::WorkerBootId& boot,
                                 fe::IncarnationSequence incarnation) {
  fe::ValidateAuthorityRequest request;
  request.token.epoch = epoch;
  request.token.participant = participant(participant_name);
  request.token.boot = boot;
  request.token.incarnation = incarnation;
  request.scope = scope(fe::ScopeKind::Fabric, "fabric-main");
  return client.validate_authority(request);
}

/// Polls until the observed validation code stops being CURRENT, then returns
/// it. The budget is a proof obligation, not a timer that hides a hang.
fe::AuthorityValidation wait_until_not_current(CoordinatorClient& client, fe::CoordinatorEpoch epoch,
                                               const std::string& participant_name,
                                               const fe::WorkerBootId& boot,
                                               fe::IncarnationSequence incarnation) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(kObserveBudgetMilliseconds);
  fe::AuthorityValidation last;
  for (;;) {
    last = validate(client, epoch, participant_name, boot, incarnation);
    if (last.code != fe::AuthorityValidationCode::Current) {
      return last;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      FE_REQUIRE_MSG(false, "the coordinator never observed the loss of " + participant_name);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

#if defined(_WIN32)
/// Raw socket used to feed the coordinator deliberately malformed traffic.
class RawPeer {
 public:
  explicit RawPeer(std::uint16_t port) {
    WSADATA data{};
    FE_REQUIRE(::WSAStartup(MAKEWORD(2, 2), &data) == 0);
    handle_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    FE_REQUIRE(handle_ != INVALID_SOCKET);
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = ::htons(port);
    FE_REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr) == 1);
    FE_REQUIRE(::connect(handle_, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0);
  }
  ~RawPeer() {
    if (handle_ != INVALID_SOCKET) {
      ::shutdown(handle_, 2);
      ::closesocket(handle_);
    }
    ::WSACleanup();
  }
  RawPeer(const RawPeer&) = delete;
  RawPeer& operator=(const RawPeer&) = delete;

  void send_bytes(const std::vector<std::byte>& bytes) {
    int sent = ::send(handle_, reinterpret_cast<const char*>(bytes.data()),
                      static_cast<int>(bytes.size()), 0);
    FE_REQUIRE(sent == static_cast<int>(bytes.size()));
  }

  /// Reads until the peer closes, with an explicit budget.
  [[nodiscard]] std::vector<std::byte> read_until_close() {
    std::vector<std::byte> received;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kObserveBudgetMilliseconds);
    for (;;) {
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(handle_, &readable);
      timeval budget{};
      budget.tv_sec = 0;
      budget.tv_usec = 50000;
      const int ready = ::select(0, &readable, nullptr, nullptr, &budget);
      if (ready > 0) {
        std::byte buffer[512];
        const int got = ::recv(handle_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
        if (got <= 0) {
          return received;
        }
        received.insert(received.end(), buffer, buffer + got);
        continue;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        FE_REQUIRE_MSG(false, "the coordinator never closed the malformed peer connection");
      }
    }
  }

 private:
  SOCKET handle_ = INVALID_SOCKET;
};
#endif

int run_cli(const std::vector<std::string>& arguments, std::string& output) {
  ChildProcessOptions options;
  options.executable = FABRIC_EPOCH_CLI_EXE;
  options.arguments = arguments;
  ChildProcess process(options);
  output.clear();
  for (;;) {
    const auto line = process.try_read_line(kObserveBudgetMilliseconds);
    if (!line.has_value()) {
      break;
    }
    output += *line;
    output += '\n';
  }
  const auto code = process.try_wait(kObserveBudgetMilliseconds);
  FE_REQUIRE_MSG(code.has_value(), "the CLI process never exited");
  if (*code != 0) {
    // Diagnostics for the failing command; the assertion that consumes the
    // return value reports the failure itself.
    std::fprintf(stderr, "[cli] exited %d\n%s", *code, output.c_str());
  }
  return *code;
}

}  // namespace

FE_TEST(distributed, worker_death_is_detected_and_permanently_fenced) {
  TempDirectory directory;
  const std::filesystem::path store = directory.file("epoch.store");
  CoordinatorHandle coordinator = start_coordinator(store);
  ClientHandle observer = connect_client(coordinator.port, "observer");

  const std::string victim_name = "publisher.victim";
  const std::string bystander_name = "publisher.bystander";
  WorkerHandle victim = start_worker(coordinator.port, victim_name, fe::WorkerBootId::generate());
  WorkerHandle bystander = start_worker(coordinator.port, bystander_name, fe::WorkerBootId::generate());

  FE_REQUIRE_EQ(validate(*observer.client, coordinator.epoch, victim_name, victim.boot, victim.incarnation).code,
                fe::AuthorityValidationCode::Current);
  FE_REQUIRE_EQ(
      validate(*observer.client, coordinator.epoch, bystander_name, bystander.boot, bystander.incarnation).code,
      fe::AuthorityValidationCode::Current);

  // 5. Kill Worker A through an actual operating-system termination path. The
  //    worker is asserted to be alive first, which proves the loss below was
  //    caused by the termination and not by a worker that gave up on its own.
  FE_REQUIRE_MSG(victim.process->running(), "the worker exited before it was killed");
  FE_REQUIRE_MSG(bystander.process->running(), "the bystander exited before it was killed");
  victim.process->terminate();
  FE_REQUIRE(!victim.process->running());

  // 6-9. The control path observes the loss and fences Boot A1 permanently.
  const fe::AuthorityValidation fenced = wait_until_not_current(
      *observer.client, coordinator.epoch, victim_name, victim.boot, victim.incarnation);
  FE_REQUIRE_MSG(fenced.code == fe::AuthorityValidationCode::StaleWorkerBoot,
                 std::string("unexpected rejection code ") + std::string(fe::to_string(fenced.code)));
  FE_REQUIRE_EQ(fenced.currentness, fe::AuthorityCurrentness::Fenced);
  FE_REQUIRE_EQ(fenced.superseded_by, std::optional<fe::WorkerBootId>());

  // 10. Replaying the old incarnation's traffic is rejected.
  for (int repeat = 0; repeat < 4; ++repeat) {
    FE_REQUIRE_EQ(
        validate(*observer.client, coordinator.epoch, victim_name, victim.boot, victim.incarnation).code,
        fe::AuthorityValidationCode::StaleWorkerBoot);
  }

  // 11-13. Worker A' restarts as the same publisher with a brand new boot.
  const fe::WorkerBootId reincarnated_boot = fe::WorkerBootId::generate();
  WorkerHandle reincarnated = start_worker(coordinator.port, victim_name, reincarnated_boot);
  FE_REQUIRE(reincarnated.incarnation > victim.incarnation);
  FE_REQUIRE(reincarnated.registration_generation > victim.registration_generation);
  FE_REQUIRE_EQ(validate(*observer.client, coordinator.epoch, victim_name, reincarnated_boot,
                         reincarnated.incarnation)
                    .code,
                fe::AuthorityValidationCode::Current);

  // 14. The fenced incarnation stays stale forever.
  FE_REQUIRE_EQ(
      validate(*observer.client, coordinator.epoch, victim_name, victim.boot, victim.incarnation).code,
      fe::AuthorityValidationCode::StaleWorkerBoot);

  // 15. The unrelated worker is untouched.
  FE_REQUIRE_EQ(validate(*observer.client, coordinator.epoch, bystander_name, bystander.boot,
                         bystander.incarnation)
                    .code,
                fe::AuthorityValidationCode::Current);

  // The fence was durable: a fresh coordinator over the same store still
  // refuses the fenced boot.
  observer.client->request_shutdown();
  observer.client->wait_for_disconnect();
  FE_REQUIRE_EQ(coordinator.process->try_wait(kObserveBudgetMilliseconds), std::optional<int>(0));

  reincarnated.process->terminate();
  bystander.process->terminate();

  CoordinatorHandle restarted = start_coordinator(store);
  FE_REQUIRE(restarted.epoch > coordinator.epoch);
  ClientHandle after = connect_client(restarted.port, "observer");
  const fe::AuthorityValidation after_restart =
      validate(*after.client, restarted.epoch, victim_name, victim.boot, victim.incarnation);
  FE_REQUIRE_EQ(after_restart.code, fe::AuthorityValidationCode::StaleWorkerBoot);
  FE_REQUIRE_EQ(after_restart.currentness, fe::AuthorityCurrentness::Fenced);
  after.client->request_shutdown();
  after.client->wait_for_disconnect();
  FE_REQUIRE_EQ(restarted.process->try_wait(kObserveBudgetMilliseconds), std::optional<int>(0));
}

FE_TEST(distributed, coordinator_hard_restart_advances_the_epoch_monotonically) {
  TempDirectory directory;
  const std::filesystem::path store = directory.file("epoch.store");

  fe::CoordinatorEpoch previous_epoch(0);
  std::vector<std::uint64_t> observed;
  std::vector<fe::WorkerBootId> pre_restart_boots;
  fe::RegistrationResult pre_restart_registration;
  std::uint16_t first_port = 0;

  for (int generation = 0; generation < 4; ++generation) {
    CoordinatorHandle coordinator = start_coordinator(store);
    if (generation == 0) {
      first_port = coordinator.port;
    }
    // 1-3. Register participants and persist durable epoch state.
    // One session per worker incarnation: every publisher owns its own
    // transport, and the sessions stay open until the coordinator is killed.
    // A publisher that hung up would fence its own incarnation, which is the
    // documented session-loss model and not what this proof is measuring.
    std::vector<ClientHandle> publishers;
    publishers.reserve(3);
    for (int index = 0; index < 3; ++index) {
      const std::string name = "publisher.durable" + std::to_string(index);
      const fe::WorkerBootId boot = fe::WorkerBootId::generate();
      ClientHandle publisher = connect_client(coordinator.port, "registrar");
      fe::RegisterParticipantRequest request = registration_request(name, boot, coordinator.epoch);
      const fe::RegistrationResult result =
          publisher.client->register_participant(request, publisher.hello.session);
      FE_REQUIRE_MSG(result.outcome == fe::RegistrationOutcome::Registered ||
                         result.outcome == fe::RegistrationOutcome::AlreadyActive,
                     std::string("unexpected registration outcome ") +
                         std::string(fe::to_string(result.outcome)));
      if (generation == 0) {
        pre_restart_boots.push_back(boot);
        pre_restart_registration = result;
      }
      publishers.push_back(std::move(publisher));
    }

    // 4. Kill the coordinator with no graceful shutdown at all.
    const auto exit = coordinator.process->try_wait(kObserveBudgetMilliseconds);
    FE_REQUIRE_MSG(!exit.has_value(), "the coordinator exited before it was killed");
    coordinator.process->terminate();
    FE_REQUIRE(!coordinator.process->running());
    observed.push_back(coordinator.epoch.value());
    previous_epoch = coordinator.epoch;
  }

  for (std::size_t index = 1; index < observed.size(); ++index) {
    FE_REQUIRE_MSG(observed[index] == observed[index - 1] + 1,
                   "epoch progression was not monotonic: " + std::to_string(observed[index - 1]) + " -> " +
                       std::to_string(observed[index]));
  }

  // 5-7. A fresh coordinator loads the durable state and is at a higher epoch.
  CoordinatorHandle fresh = start_coordinator(store);
  FE_REQUIRE(fresh.epoch > previous_epoch);
  ClientHandle client = connect_client(fresh.port, "inspector");

  // 9. Old epoch traffic rejects over the real wire.
  const fe::AuthorityValidation stale_epoch = validate(
      *client.client, previous_epoch, "publisher.durable0",
      pre_restart_boots.empty() ? fe::WorkerBootId::generate() : pre_restart_boots.front(),
      pre_restart_registration.incarnation.value_or(fe::IncarnationSequence(1)));
  FE_REQUIRE_EQ(stale_epoch.code, fe::AuthorityValidationCode::StaleEpoch);

  // 10 and 14. The durable participant identity survived while live authority
  // did not.
  const auto view = client.client->query_participant(participant("publisher.durable0"));
  FE_REQUIRE(view.has_value());
  FE_REQUIRE(!view->retired);
  FE_REQUIRE_EQ(view->active_incarnation_count(), std::size_t{0});
  FE_REQUIRE(view->revalidation_required);

  // 11-13. Fresh incarnations re-establish authority under the new epoch.
  WorkerHandle fresh_worker = start_worker(fresh.port, "publisher.durable0", fe::WorkerBootId::generate());
  FE_REQUIRE_EQ(validate(*client.client, fresh.epoch, "publisher.durable0", fresh_worker.boot,
                         fresh_worker.incarnation)
                    .code,
                fe::AuthorityValidationCode::Current);
  FE_REQUIRE(fresh_worker.registration_generation > *pre_restart_registration.registration_generation);
  fresh_worker.process->terminate();

  client.client->request_shutdown();
  client.client->wait_for_disconnect();
  FE_REQUIRE_EQ(fresh.process->try_wait(kObserveBudgetMilliseconds), std::optional<int>(0));

  // The first coordinator listened on an ephemeral port that is now free; the
  // proof does not reuse a fixed port anywhere.
  FE_REQUIRE(first_port != 0);
}

FE_TEST(distributed, cli_reports_the_same_authority_state) {
  TempDirectory directory;
  const std::filesystem::path store = directory.file("epoch.store");
  CoordinatorHandle coordinator = start_coordinator(store);
  WorkerHandle worker = start_worker(coordinator.port, "publisher.cli", fe::WorkerBootId::generate());

  std::string output;
  FE_REQUIRE_EQ(run_cli({"--port", std::to_string(coordinator.port), "show-epoch"}, output), 0);
  FE_REQUIRE_MSG(output.find("epoch=" + coordinator.epoch.to_string()) != std::string::npos, output);
  FE_REQUIRE_MSG(output.find("coordinator_boot=" + coordinator.boot.to_string()) != std::string::npos, output);

  FE_REQUIRE_EQ(run_cli({"--port", std::to_string(coordinator.port), "list-active-workers"}, output), 0);
  FE_REQUIRE_MSG(output.find(worker.boot.to_string()) != std::string::npos, output);

  FE_REQUIRE_EQ(run_cli({"--port", std::to_string(coordinator.port), "validate-authority", "--epoch",
                         coordinator.epoch.to_string(), "--participant", "publisher.cli", "--boot",
                         worker.boot.to_string(), "--incarnation", worker.incarnation.to_string(),
                         "--scope", "FABRIC:fabric-main"},
                        output),
                0);
  FE_REQUIRE_MSG(output.find("code=CURRENT") != std::string::npos, output);

  FE_REQUIRE_EQ(run_cli({"--port", std::to_string(coordinator.port), "list-participants"}, output), 0);
  FE_REQUIRE_MSG(output.find("participant=publisher.cli") != std::string::npos, output);

  FE_REQUIRE_EQ(run_cli({"--port", std::to_string(coordinator.port), "explain", "--participant",
                         "publisher.cli", "--boot", worker.boot.to_string()},
                        output),
                0);
  FE_REQUIRE_MSG(output.find("AUTHORITY_CURRENT") != std::string::npos, output);

  FE_REQUIRE_EQ(run_cli({"--port", std::to_string(coordinator.port), "snapshot"}, output), 0);
  FE_REQUIRE_MSG(output.find("digest=") != std::string::npos, output);

  worker.process->terminate();
  FE_REQUIRE_EQ(run_cli({"--port", std::to_string(coordinator.port), "fence-worker", "--participant",
                         "publisher.cli", "--boot", worker.boot.to_string(), "--reason",
                         "EXPLICIT_OPERATOR_FENCE"},
                        output),
                0);
  FE_REQUIRE_MSG(output.find("outcome=") != std::string::npos, output);

  FE_REQUIRE_EQ(run_cli({"--port", std::to_string(coordinator.port), "list-fences"}, output), 0);
  FE_REQUIRE_MSG(output.find("fence_floor=") != std::string::npos, output);

  FE_REQUIRE_EQ(run_cli({"--port", std::to_string(coordinator.port), "retire-participant", "--participant",
                         "publisher.cli"},
                        output),
                0);
  FE_REQUIRE_MSG(output.find("outcome=RETIRED") != std::string::npos, output);

  FE_REQUIRE_EQ(run_cli({"inspect-store", "--store", store.string()}, output), 0);
  FE_REQUIRE_MSG(output.find("current_epoch=" + coordinator.epoch.to_string()) != std::string::npos, output);

  FE_REQUIRE_EQ(run_cli({"--port", std::to_string(coordinator.port), "shutdown"}, output), 0);
  FE_REQUIRE_EQ(coordinator.process->try_wait(kObserveBudgetMilliseconds), std::optional<int>(0));
  std::string version_output;
  FE_REQUIRE_EQ(run_cli({"version"}, version_output), 0);
  FE_REQUIRE_MSG(version_output.find("1.0.0") != std::string::npos, version_output);
}

FE_TEST(distributed, malformed_peers_are_refused_and_the_coordinator_survives) {
#if defined(_WIN32)
  TempDirectory directory;
  const std::filesystem::path store = directory.file("epoch.store");
  CoordinatorHandle coordinator = start_coordinator(store);

  // 1. A peer that connects and immediately disappears.
  {
    RawPeer peer(coordinator.port);
  }

  // 2. A peer whose first frame is not HELLO but a structurally valid frame.
  {
    RawPeer peer(coordinator.port);
    const std::vector<std::byte> payload = fe::encode_snapshot_request(fe::default_limits());
    const std::vector<std::byte> frame = fe::encode_frame(
        fe::MessageType::SnapshotRequest, 1, fe::CoordinatorEpoch(1), fe::MutationAttemptId::generate(),
        payload, fe::default_limits());
    peer.send_bytes(frame);
    const std::vector<std::byte> response = peer.read_until_close();
    FE_REQUIRE_MSG(!response.empty(), "the coordinator answered a non-HELLO first frame with nothing");
  }

  // 3. A peer that sends garbage where a frame header belongs.
  {
    RawPeer peer(coordinator.port);
    peer.send_bytes(std::vector<std::byte>(64, std::byte{0x5A}));
    const std::vector<std::byte> response = peer.read_until_close();
    FE_REQUIRE_MSG(!response.empty(), "the coordinator answered garbage with nothing");
  }

  // 4. A peer that declares an enormous payload length and then stops talking.
  {
    RawPeer peer(coordinator.port);
    std::vector<std::byte> header(fe::frame_header_size, std::byte{0});
    header[0] = std::byte{0x31};
    header[1] = std::byte{0x50};
    header[2] = std::byte{0x45};
    header[3] = std::byte{0x46};
    header[4] = std::byte{0x01};
    header[6] = std::byte{0x01};
    header[10] = std::byte{0xFF};
    header[11] = std::byte{0xFF};
    header[12] = std::byte{0xFF};
    header[13] = std::byte{0xFF};
    peer.send_bytes(header);
    const std::vector<std::byte> response = peer.read_until_close();
    FE_REQUIRE_MSG(!response.empty(), "the coordinator answered an oversized declaration with nothing");
  }

  // The coordinator is still healthy and still serves well-formed peers.
  ClientHandle client = connect_client(coordinator.port, "after-abuse");
  FE_REQUIRE_EQ(client.client->epoch_state().epoch, coordinator.epoch);
  client.client->request_shutdown();
  client.client->wait_for_disconnect();
  FE_REQUIRE_EQ(coordinator.process->try_wait(kObserveBudgetMilliseconds), std::optional<int>(0));
#endif
}

FE_TEST(distributed, repeated_worker_start_stop_leaves_no_orphans_or_leaked_sessions) {
  TempDirectory directory;
  const std::filesystem::path store = directory.file("epoch.store");
  CoordinatorHandle coordinator = start_coordinator(store);
  ClientHandle client = connect_client(coordinator.port, "supervisor");

  for (int cycle = 0; cycle < 6; ++cycle) {
    WorkerHandle worker =
        start_worker(coordinator.port, "publisher.churn", fe::WorkerBootId::generate());
    FE_REQUIRE_EQ(validate(*client.client, coordinator.epoch, "publisher.churn", worker.boot,
                           worker.incarnation)
                      .code,
                  fe::AuthorityValidationCode::Current);
    worker.process->terminate();
    const fe::AuthorityValidation fenced = wait_until_not_current(
        *client.client, coordinator.epoch, "publisher.churn", worker.boot, worker.incarnation);
    FE_REQUIRE_EQ(fenced.currentness, fe::AuthorityCurrentness::Fenced);
  }

  const fe::EpochState state = client.client->epoch_state();
  FE_REQUIRE_EQ(state.active_worker_count, std::size_t{0});
  FE_REQUIRE_EQ(state.participant_count, std::size_t{1});

  client.client->request_shutdown();
  client.client->wait_for_disconnect();
  FE_REQUIRE_EQ(coordinator.process->try_wait(kObserveBudgetMilliseconds), std::optional<int>(0));
}
