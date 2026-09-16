// Fabric Epoch 1.0.0 - Summon Software Labs
// Real worker/publisher process. Generates a fresh WorkerBootId, registers
// under the current coordinator epoch, then holds its session until the
// connection ends.
#include <chrono>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "arguments.hpp"
#include "fabric_epoch/client.hpp"
#include "fabric_epoch/error.hpp"
#include "fabric_epoch/version.hpp"

namespace {

void print_usage() {
  std::cout << "usage: fabric-epoch-worker --port <port> --participant <id> --scope <KIND:SUBJECT> [options]\n"
               "  --host <address>          coordinator address (default 127.0.0.1)\n"
               "  --port <port>             coordinator TCP port (required)\n"
               "  --participant <id>        stable participant identity\n"
               "  --scope <KIND:SUBJECT>    authority scope; repeatable, at least one required\n"
               "  --boot <hex|generate>     worker boot identifier (default generate)\n"
               "  --policy <p>              SINGLE_INCARNATION | MULTI_INCARNATION\n"
               "  --idle                    remain attached without registering\n"
               "  --version                 print the version and exit\n"
               "  --help                    print this message and exit\n";
}

}  // namespace

int main(int argc, char** argv) {
  using namespace fabric_epoch;
  using namespace fabric_epoch::app;

  try {
    const Arguments arguments = parse_arguments(argc, argv);
    if (arguments.has("help")) {
      print_usage();
      return 0;
    }
    if (arguments.has("version")) {
      std::cout << "fabric-epoch-worker " << version_full() << "\n";
      return 0;
    }

    ClientOptions client_options;
    client_options.host = arguments.get_or("host", "127.0.0.1");
    const auto port = arguments.get_unsigned("port");
    if (!port.has_value()) {
      throw invalid_argument("--port is required");
    }
    client_options.port = static_cast<std::uint16_t>(*port);

    CoordinatorClient client(std::move(client_options));
    client.connect();
    const HelloAckPayload hello = client.hello("worker-session");

    if (arguments.has("idle")) {
      std::cout << "worker attached epoch=" << hello.epoch.to_string()
                << " session=" << hello.session.to_string() << std::endl;
      client.wait_for_disconnect();
      return 0;
    }

    const ParticipantId participant = [&]() {
      const auto parsed = ParticipantId::parse(arguments.require("participant"));
      if (!parsed.has_value()) {
        throw invalid_argument("--participant is not a valid identifier");
      }
      return *parsed;
    }();

    ScopeSet scopes;
    for (const auto& text : arguments.get_all("scope")) {
      const auto scope = parse_scope(text);
      if (!scope.has_value()) {
        throw invalid_argument("--scope must be KIND:SUBJECT");
      }
      if (!scopes.insert(*scope, 32)) {
        throw invalid_argument("too many scopes for one registration");
      }
    }
    if (arguments.get_all("scope").empty()) {
      throw invalid_argument("at least one --scope is required");
    }

    WorkerBootId boot = WorkerBootId::generate();
    if (const auto text = arguments.get("boot"); text.has_value()) {
      const auto parsed = parse_boot(*text);
      if (!parsed.has_value()) {
        throw invalid_argument("--boot must be 32 hexadecimal characters or 'generate'");
      }
      boot = *parsed;
    }

    RegisterParticipantRequest request;
    request.attempt = MutationAttemptId::generate();
    request.expected_epoch = hello.epoch;
    request.participant = participant;
    request.boot = boot;
    request.scopes = scopes;
    request.provenance = BoundedText::from_validated(arguments.get_or("provenance", "worker"));

    const std::string policy = arguments.get_or("policy", "SINGLE_INCARNATION");
    const auto parsed_policy = parse_incarnation_policy(policy);
    if (!parsed_policy.has_value()) {
      throw invalid_argument("unknown --policy value");
    }
    request.policy = *parsed_policy;

    const RegistrationResult result = client.register_participant(request, hello.session);
    std::cout << "worker registration outcome=" << to_string(result.outcome)
              << " participant=" << participant.to_string()
              << " boot=" << boot.to_string()
              << " epoch=" << result.epoch.to_string()
              << " incarnation="
              << (result.incarnation.has_value() ? result.incarnation->to_string() : std::string("NONE"))
              << " registration_generation="
              << (result.registration_generation.has_value() ? result.registration_generation->to_string()
                                                             : std::string("NONE"))
              << std::endl;
    if (result.outcome != RegistrationOutcome::Registered &&
        result.outcome != RegistrationOutcome::Idempotent) {
      std::cerr << "error registration rejected: " << result.explanation.render() << std::endl;
      return 4;
    }

    client.wait_for_disconnect();
    return 0;
  } catch (const EpochError& error) {
    std::cerr << "error " << error.render() << std::endl;
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "error " << error.what() << std::endl;
    return 3;
  }
}
