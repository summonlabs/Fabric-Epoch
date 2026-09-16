// Fabric Epoch 1.0.0 - Summon Software Labs
// Deterministic, script-friendly inspection and control client.
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "arguments.hpp"
#include "fabric_epoch/client.hpp"
#include "fabric_epoch/error.hpp"
#include "fabric_epoch/store.hpp"
#include "fabric_epoch/version.hpp"

namespace {

using namespace fabric_epoch;
using namespace fabric_epoch::app;

void print_usage() {
  std::cout <<
      "usage: fabric-epoch-cli [--host <address>] --port <port> <command> [options]\n"
      "commands:\n"
      "  version\n"
      "  show-epoch\n"
      "  advance-epoch [--cause <CAUSE>]\n"
      "  list-participants\n"
      "  show-participant --participant <id>\n"
      "  list-active-workers\n"
      "  list-fences\n"
      "  list-grants\n"
      "  validate-authority --epoch <n> --participant <id> --boot <hex> --incarnation <n>\n"
      "                      --scope <KIND:SUBJECT> [--grant <hex>]\n"
      "  fence-worker --participant <id> --boot <hex> [--reason <REASON>]\n"
      "  revoke-grant --grant <hex> [--reason <REASON>]\n"
      "  retire-participant --participant <id>\n"
      "  snapshot\n"
      "  explain --participant <id> --boot <hex>\n"
      "  shutdown\n"
      "  inspect-store --store <path>\n";
}

[[nodiscard]] WorkerBootId require_boot(const Arguments& arguments) {
  const auto parsed = parse_boot(arguments.require("boot"));
  if (!parsed.has_value()) {
    throw invalid_argument("--boot must be 32 hexadecimal characters or 'generate'");
  }
  return *parsed;
}

[[nodiscard]] ParticipantId require_participant(const Arguments& arguments) {
  const auto parsed = ParticipantId::parse(arguments.require("participant"));
  if (!parsed.has_value()) {
    throw invalid_argument("--participant is not a valid identifier");
  }
  return *parsed;
}

[[nodiscard]] AuthorityScope require_scope(const Arguments& arguments) {
  const auto parsed = parse_scope(arguments.require("scope"));
  if (!parsed.has_value()) {
    throw invalid_argument("--scope must be KIND:SUBJECT");
  }
  return *parsed;
}

[[nodiscard]] std::uint64_t require_unsigned(const Arguments& arguments, const char* key) {
  const auto value = arguments.get_unsigned(key);
  if (!value.has_value()) {
    throw invalid_argument(std::string("--") + key + " is required");
  }
  return *value;
}

int inspect_store(const Arguments& arguments) {
  const std::string path = arguments.require("store");
  StoreOptions options;
  options.path = path;
  options.create_if_missing = false;
  EpochStore store(options);
  if (!store.exists()) {
    std::cout << "store present=false path=" << path << std::endl;
    return 0;
  }
  const DurableState state = store.load();
  std::cout << "store present=true path=" << path << std::endl;
  std::cout << "current_epoch=" << state.current_epoch.to_string() << std::endl;
  std::cout << "coordinator_boot=" << state.coordinator_boot.to_string() << std::endl;
  std::cout << "generation=" << state.generation.to_string() << std::endl;
  std::cout << "registration_floor=" << state.registration_floor.to_string() << std::endl;
  std::cout << "fence_floor=" << state.fence_floor.to_string() << std::endl;
  std::cout << "participants=" << state.participants.size() << std::endl;
  std::cout << "retained_fences=" << state.recent_fences.size() << std::endl;
  std::cout << "grants=" << state.grants.size() << std::endl;
  std::cout << "transitions=" << state.transitions.size() << std::endl;
  std::cout << "watermark_epoch=" << store.highest_persisted_epoch().to_string() << std::endl;
  for (const auto& transition : state.transitions) {
    std::cout << "transition from=" << transition.from_epoch.to_string()
              << " to=" << transition.to_epoch.to_string()
              << " cause=" << to_string(transition.cause)
              << " boot=" << transition.coordinator_boot.to_string() << std::endl;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Arguments arguments = parse_arguments(argc, argv);
    if (arguments.positional.empty()) {
      print_usage();
      return arguments.has("help") ? 0 : 1;
    }
    const std::string command = arguments.positional.front();

    if (command == "version") {
      std::cout << "fabric-epoch-cli " << version_full() << std::endl;
      return 0;
    }
    if (command == "inspect-store") {
      return inspect_store(arguments);
    }
    if (command == "help") {
      print_usage();
      return 0;
    }

    ClientOptions client_options;
    client_options.host = arguments.get_or("host", "127.0.0.1");
    const auto port = arguments.get_unsigned("port");
    if (!port.has_value()) {
      throw invalid_argument("--port is required for coordinator commands");
    }
    client_options.port = static_cast<std::uint16_t>(*port);

    CoordinatorClient client(std::move(client_options));
    client.connect();
    const HelloAckPayload hello = client.hello("cli");
    std::cout << "hello epoch=" << hello.epoch.to_string()
              << " coordinator_boot=" << hello.coordinator_boot.to_string()
              << " session=" << hello.session.to_string()
              << " product=" << hello.product_version << std::endl;

    if (command == "show-epoch") {
      std::cout << client.epoch_state().render() << std::endl;
      return 0;
    }
    if (command == "retire-participant") {
      RetireParticipantRequest request;
      request.attempt = MutationAttemptId::generate();
      request.expected_epoch = client.epoch_state().epoch;
      request.participant = require_participant(arguments);
      const RetireResult result = client.retire_participant(request);
      std::cout << "retire outcome=" << to_string(result.outcome)
                << " epoch=" << result.epoch.to_string()
                << " incarnations_fenced=" << result.incarnations_fenced << std::endl;
      std::cout << "explanation " << result.explanation.render() << std::endl;
      return (result.outcome == RetireOutcome::Retired || result.outcome == RetireOutcome::AlreadyRetired)
                 ? 0
                 : 1;
    }
    if (command == "advance-epoch") {
      EpochAdvanceRequest request;
      request.attempt = MutationAttemptId::generate();
      request.expected_epoch = hello.epoch;
      request.cause = EpochTransitionCause::ExplicitOperatorAdvance;
      if (const auto cause = arguments.get("cause"); cause.has_value()) {
        if (*cause == "COORDINATOR_START") {
          request.cause = EpochTransitionCause::CoordinatorStart;
        } else if (*cause == "COORDINATOR_RESTART") {
          request.cause = EpochTransitionCause::CoordinatorRestart;
        } else if (*cause == "EXPLICIT_OPERATOR_ADVANCE") {
          request.cause = EpochTransitionCause::ExplicitOperatorAdvance;
        } else if (*cause == "DURABLE_RECOVERY") {
          request.cause = EpochTransitionCause::DurableRecovery;
        } else {
          throw invalid_argument("unknown --cause value");
        }
      }
      const EpochAdvanceResult result = client.advance_epoch(request);
      std::cout << "advance outcome=" << to_string(result.outcome)
                << " epoch=" << result.epoch.to_string()
                << " coordinator_boot=" << result.coordinator_boot.to_string()
                << " replayed=" << (result.replayed ? "true" : "false") << std::endl;
      std::cout << "explanation " << result.explanation.render() << std::endl;
      return result.outcome == EpochAdvanceOutcome::Advanced ? 0 : 1;
    }
    if (command == "list-participants") {
      const AuthoritySnapshot snapshot = client.snapshot();
      std::cout << "snapshot epoch=" << snapshot.epoch.to_string()
                << " generation=" << snapshot.generation.to_string()
                << " digest=" << snapshot.digest.to_string() << std::endl;
      for (const auto& participant : snapshot.participants) {
        std::cout << participant.render() << std::endl;
      }
      return 0;
    }
    if (command == "list-active-workers") {
      const AuthoritySnapshot snapshot = client.snapshot();
      for (const auto& participant : snapshot.participants) {
        if (participant.has_active_incarnation) {
          std::cout << "active participant=" << participant.id.to_string()
                    << " boot=" << participant.active_boot.to_string()
                    << " incarnation=" << participant.active_incarnation.to_string() << std::endl;
        }
        for (const auto& boot : participant.active_boots) {
          std::cout << "active participant=" << participant.id.to_string()
                    << " boot=" << boot.to_string() << std::endl;
        }
      }
      return 0;
    }
    if (command == "show-participant") {
      const auto view = client.query_participant(require_participant(arguments));
      if (!view.has_value()) {
        std::cout << "participant found=false" << std::endl;
        return 1;
      }
      std::cout << view->render() << std::endl;
      return 0;
    }
    if (command == "list-fences") {
      const AuthoritySnapshot snapshot = client.snapshot();
      for (const auto& fence : snapshot.fences) {
        std::cout << "fence boot=" << fence.boot.to_string()
                  << " participant=" << fence.participant.to_string()
                  << " reason=" << to_string(fence.reason)
                  << " generation=" << fence.generation.to_string()
                  << " epoch=" << fence.epoch.to_string()
                  << " incarnation=" << fence.incarnation.to_string() << std::endl;
      }
      std::cout << "fence_floor=" << snapshot.fence_floor.to_string() << std::endl;
      return 0;
    }
    if (command == "list-grants") {
      const AuthoritySnapshot snapshot = client.snapshot();
      for (const auto& grant : snapshot.grants) {
        std::cout << "grant id=" << grant.id.to_string()
                  << " epoch=" << grant.epoch.to_string()
                  << " participant=" << grant.participant.to_string()
                  << " boot=" << grant.boot.to_string()
                  << " generation=" << grant.generation.to_string()
                  << " revoked=" << (grant.revoked ? "true" : "false")
                  << " delegated=" << (grant.delegated ? "true" : "false")
                  << " scopes=" << grant.scopes.to_string() << std::endl;
      }
      return 0;
    }
    if (command == "validate-authority") {
      ValidateAuthorityRequest request;
      request.token.epoch = CoordinatorEpoch(require_unsigned(arguments, "epoch"));
      request.token.participant = require_participant(arguments);
      request.token.boot = require_boot(arguments);
      request.token.incarnation = IncarnationSequence(require_unsigned(arguments, "incarnation"));
      request.scope = require_scope(arguments);
      if (const auto grant = arguments.get("grant"); grant.has_value()) {
        const auto parsed = AuthorityGrantId::parse(*grant);
        if (!parsed.has_value()) {
          throw invalid_argument("--grant must be 32 hexadecimal characters");
        }
        request.grant = *parsed;
      }
      const AuthorityValidation validation = client.validate_authority(request);
      std::cout << "validation " << validation.render() << std::endl;
      std::cout << "explanation " << validation.explanation.render() << std::endl;
      return validation.code == AuthorityValidationCode::Current ? 0 : 1;
    }
    if (command == "fence-worker") {
      FenceWorkerRequest request;
      request.attempt = MutationAttemptId::generate();
      request.expected_epoch = client.epoch_state().epoch;
      request.participant = require_participant(arguments);
      request.boot = require_boot(arguments);
      const std::string reason_text = arguments.get_or("reason", "EXPLICIT_OPERATOR_FENCE");
      const auto reason = parse_fence_reason(reason_text);
      if (!reason.has_value()) {
        throw invalid_argument("unknown --reason value");
      }
      request.reason = *reason;
      const FenceResult result = client.fence_worker(request);
      std::cout << "fence outcome=" << to_string(result.outcome)
                << " epoch=" << result.epoch.to_string()
                << " fence_generation="
                << (result.fence_generation.has_value() ? result.fence_generation->to_string()
                                                        : std::string("NONE"))
                << std::endl;
      std::cout << "explanation " << result.explanation.render() << std::endl;
      return (result.outcome == FenceOutcome::Fenced || result.outcome == FenceOutcome::AlreadyFenced) ? 0
                                                                                                      : 1;
    }
    if (command == "revoke-grant") {
      RevokeAuthorityRequest request;
      request.attempt = MutationAttemptId::generate();
      request.expected_epoch = client.epoch_state().epoch;
      const auto grant = AuthorityGrantId::parse(arguments.require("grant"));
      if (!grant.has_value()) {
        throw invalid_argument("--grant must be 32 hexadecimal characters");
      }
      request.grant = *grant;
      const auto reason = parse_fence_reason(arguments.get_or("reason", "ADMINISTRATIVE_REVOKE"));
      if (!reason.has_value()) {
        throw invalid_argument("unknown --reason value");
      }
      request.reason = *reason;
      const RevokeResult result = client.revoke_authority(request);
      std::cout << "revoke outcome=" << to_string(result.outcome)
                << " epoch=" << result.epoch.to_string()
                << " derived_grants_revoked=" << result.derived_grants_revoked << std::endl;
      std::cout << "explanation " << result.explanation.render() << std::endl;
      return (result.outcome == RevokeOutcome::Revoked || result.outcome == RevokeOutcome::AlreadyRevoked)
                 ? 0
                 : 1;
    }
    if (command == "snapshot") {
      const AuthoritySnapshot snapshot = client.snapshot();
      std::cout << snapshot.render() << std::endl;
      for (const auto& participant : snapshot.participants) {
        std::cout << "  " << participant.render() << std::endl;
      }
      return 0;
    }
    if (command == "explain") {
      const Explanation explanation =
          client.explain(require_participant(arguments), require_boot(arguments));
      std::cout << "explanation " << explanation.render() << std::endl;
      return 0;
    }
    if (command == "shutdown") {
      client.request_shutdown();
      std::cout << "shutdown requested=true" << std::endl;
      return 0;
    }

    std::cerr << "error unknown command: " << command << std::endl;
    print_usage();
    return 1;
  } catch (const EpochError& error) {
    std::cerr << "error " << error.render() << std::endl;
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "error " << error.what() << std::endl;
    return 3;
  }
}
