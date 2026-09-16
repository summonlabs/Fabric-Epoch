// Fabric Epoch 1.0.0 - Summon Software Labs
// Durable recovery: inspect the on-disk image, reload it, and confirm that a
// corrupt image is refused rather than partially trusted.
#include <fstream>
#include <iostream>
#include <vector>

#include "example_support.hpp"

int main() {
  using namespace fabric_epoch;

  example::Workspace workspace;
  const WorkerBootId boot = WorkerBootId::generate();
  {
    EpochRuntime runtime(example::options(workspace.store()));
    example::require(runtime.register_participant(
                         example::registration("publisher.theta", boot, runtime.current_epoch()))
                         .outcome == RegistrationOutcome::Registered,
                     "registration was refused");
    runtime.flush();
  }

  StoreOptions store_options;
  store_options.path = workspace.store();
  EpochStore store(store_options);
  const DurableState state = store.load();
  std::cout << "current_epoch=" << state.current_epoch.to_string()
            << " generation=" << state.generation.to_string()
            << " participants=" << state.participants.size()
            << " fencing_floor=" << state.fence_floor.to_string() << std::endl;
  example::require(state.participants.size() == 1, "the durable image lost its participant");
  example::require(state.participants.front().active_boot == boot, "the active boot was not persisted");

  // Reopening the runtime recovers the durable identity but not live authority.
  EpochRuntime restarted(example::options(workspace.store()));
  const auto view = restarted.query_participant(example::participant("publisher.theta"));
  example::require(view.has_value(), "the participant did not survive recovery");
  example::require(!view->has_active_incarnation, "live authority survived recovery");
  std::cout << "recovered: " << view->render() << std::endl;

  // A corrupted image is refused outright.
  std::vector<char> raw;
  {
    std::ifstream in(workspace.store(), std::ios::binary);
    raw.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  example::require(raw.size() > 40, "the durable image is implausibly small");
  raw[raw.size() / 2] = static_cast<char>(raw[raw.size() / 2] ^ 0x5A);
  {
    std::ofstream out(workspace.store(), std::ios::binary | std::ios::trunc);
    out.write(raw.data(), static_cast<std::streamsize>(raw.size()));
  }

  bool rejected = false;
  try {
    const DurableState ignored = store.load();
    (void)ignored;
  } catch (const EpochError& error) {
    rejected = true;
    std::cout << "corrupt image rejected: " << error.render() << std::endl;
  }
  example::require(rejected, "a corrupted durable image was accepted");
  return 0;
}
