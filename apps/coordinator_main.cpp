// Fabric Epoch 1.0.0 - Summon Software Labs
// Real coordinator process. Establishes a fresh coordinator authority
// incarnation, advances the durable epoch, and serves the framed protocol.
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include "arguments.hpp"
#include "fabric_epoch/error.hpp"
#include "fabric_epoch/runtime.hpp"
#include "fabric_epoch/server.hpp"
#include "fabric_epoch/version.hpp"

namespace {

void print_usage() {
  std::cout << "usage: fabric-epoch-coordinator --store <path> [options]\n"
               "  --store <path>              durable epoch store (created when absent)\n"
               "  --bind <address>            IPv4 bind address (default 127.0.0.1)\n"
               "  --port <port>               TCP port, 0 selects an ephemeral port\n"
               "  --replacement-policy <p>    REQUIRE_EXPLICIT_FENCE | REPLACE_INCUMBENT\n"
               "  --allow-multi-incarnation   permit multi-incarnation participants\n"
               "  --no-advance-on-open        open the durable epoch without advancing\n"
               "  --provenance <text>         provenance recorded on epoch transitions\n"
               "  --version                   print the version and exit\n"
               "  --help                      print this message and exit\n";
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
      std::cout << "fabric-epoch-coordinator " << version_full() << "\n";
      return 0;
    }
    if (arguments.positional.size() > 0) {
      throw invalid_argument("unexpected positional argument");
    }

    EpochRuntimeOptions runtime_options;
    runtime_options.store_path = arguments.require("store");
    runtime_options.provenance =
        BoundedText::from_validated(arguments.get_or("provenance", "fabric-epoch-coordinator"));
    runtime_options.allow_multi_incarnation = arguments.has("allow-multi-incarnation");
    runtime_options.advance_epoch_on_open = !arguments.has("no-advance-on-open");
    const std::string policy = arguments.get_or("replacement-policy", "REQUIRE_EXPLICIT_FENCE");
    if (policy == "REQUIRE_EXPLICIT_FENCE") {
      runtime_options.replacement_policy = IncarnationReplacementPolicy::RequireExplicitFence;
    } else if (policy == "REPLACE_INCUMBENT") {
      runtime_options.replacement_policy = IncarnationReplacementPolicy::ReplaceIncumbent;
    } else {
      throw invalid_argument("unknown --replacement-policy value");
    }

    EpochRuntime runtime(std::move(runtime_options));

    CoordinatorServerOptions server_options;
    server_options.bind_address = arguments.get_or("bind", "127.0.0.1");
    const auto port = arguments.get_unsigned("port");
    server_options.port = port.has_value() ? static_cast<std::uint16_t>(*port) : 0;

    CoordinatorServer server(runtime, std::move(server_options));
    server.start();

    std::cout << "coordinator ready address=" << arguments.get_or("bind", "127.0.0.1")
              << " port=" << server.port()
              << " epoch=" << runtime.current_epoch().to_string()
              << " coordinator_boot=" << runtime.coordinator_boot().to_string()
              << " store=" << runtime.store_path().string() << std::endl;

    server.wait_for_shutdown_request();
    server.stop();
    runtime.flush();
    std::cout << "coordinator stopped epoch=" << runtime.current_epoch().to_string() << std::endl;
    return 0;
  } catch (const EpochError& error) {
    std::cerr << "error " << error.render() << std::endl;
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "error " << error.what() << std::endl;
    return 3;
  }
}
