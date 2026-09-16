// Fabric Epoch 1.0.0 - Summon Software Labs
// Independent consumer of the installed package.
//
// It exercises exactly the documented consumption surface: find the package,
// link the exported target, create epoch state, register a participant,
// validate authority, fence the worker, and observe the stale rejection that
// follows. Every step is asserted; the process exits non-zero on any failure.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "fabric_epoch/fabric_epoch.hpp"

namespace fe = fabric_epoch;

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "consumer failed: %s\n", message);
    std::exit(1);
  }
}

std::filesystem::path make_workspace() {
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
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  require(!error, "cannot resolve the temporary directory");
  const std::filesystem::path path = base / ("fabric_epoch_consumer_" + suffix);
  std::filesystem::create_directories(path, error);
  require(!error, "cannot create the consumer workspace");
  return path;
}

}  // namespace

int main() {
  std::printf("fabric epoch consumer built against %s\n", std::string(fe::version_string).c_str());

  const std::filesystem::path workspace = make_workspace();
  fe::EpochRuntimeOptions options;
  options.store_path = workspace / "epoch.store";
  options.provenance = fe::BoundedText::from_validated("consumer");

  // Create epoch state.
  fe::EpochRuntime runtime(options);
  const fe::CoordinatorEpoch epoch = runtime.current_epoch();
  require(epoch.value() > 0, "no epoch was established");
  require(runtime.coordinator_boot().is_nil() == false, "no coordinator boot was established");

  // Register a participant.
  const fe::WorkerBootId boot = fe::WorkerBootId::generate();
  const auto scope = fe::AuthorityScope::parse(fe::ScopeKind::Fabric, "fabric-main");
  require(scope.has_value(), "the documented scope could not be constructed");
  fe::ScopeSet scopes;
  require(scopes.insert(*scope, 8), "the scope set rejected its only element");

  fe::RegisterParticipantRequest registration;
  registration.attempt = fe::MutationAttemptId::generate();
  registration.expected_epoch = epoch;
  const auto participant = fe::ParticipantId::parse("downstream.consumer");
  require(participant.has_value(), "the participant identifier was rejected");
  registration.participant = *participant;
  registration.boot = boot;
  registration.scopes = scopes;
  const fe::RegistrationResult registered = runtime.register_participant(registration);
  require(registered.outcome == fe::RegistrationOutcome::Registered, "registration was refused");

  // Validate authority.
  fe::ValidateAuthorityRequest validation;
  validation.token.epoch = registered.epoch;
  validation.token.participant = *participant;
  validation.token.boot = boot;
  validation.token.incarnation = *registered.incarnation;
  validation.token.registration_generation = *registered.registration_generation;
  validation.scope = *scope;
  require(runtime.validate_authority(validation).code == fe::AuthorityValidationCode::Current,
          "a current authority token was rejected");

  // Fence the worker.
  fe::FenceWorkerRequest fence;
  fence.attempt = fe::MutationAttemptId::generate();
  fence.expected_epoch = runtime.current_epoch();
  fence.participant = *participant;
  fence.boot = boot;
  fence.reason = fe::FenceReason::ExplicitOperatorFence;
  require(runtime.fence_worker(fence).outcome == fe::FenceOutcome::Fenced, "the fence was refused");

  // An exact replay of the same request returns the recorded outcome and
  // advances nothing.
  const fe::EpochGeneration after_fence = runtime.generation();
  const fe::FenceResult replay = runtime.fence_worker(fence);
  require(replay.outcome == fe::FenceOutcome::Fenced, "an exact fence replay was not recognised");
  require(runtime.generation() == after_fence, "an exact fence replay advanced a generation");

  // A genuinely new request for the same already-fenced boot is idempotent.
  fe::FenceWorkerRequest repeat = fence;
  repeat.attempt = fe::MutationAttemptId::generate();
  require(runtime.fence_worker(repeat).outcome == fe::FenceOutcome::AlreadyFenced,
          "the fence was not idempotent");

  // Observe the stale rejection.
  const fe::AuthorityValidation stale = runtime.validate_authority(validation);
  require(stale.code == fe::AuthorityValidationCode::StaleWorkerBoot, "the fenced boot stayed current");
  require(stale.currentness == fe::AuthorityCurrentness::Fenced, "the currentness reason was lost");

  // Durable restart semantics through the installed interface.
  const fe::AuthoritySnapshot snapshot = runtime.snapshot();
  require(snapshot.participants.size() == 1, "the snapshot lost the participant");
  runtime.flush();

  fe::StoreOptions store_options;
  store_options.path = options.store_path;
  fe::EpochStore store(store_options);
  require(store.load().recent_fences.size() == 1, "the durable fence was not persisted");

  std::printf("consumer ok: epoch=%s incarnation=%s digest=%s\n", epoch.to_string().c_str(),
              registered.incarnation->to_string().c_str(), snapshot.digest.to_string().c_str());

  std::error_code error;
  std::filesystem::remove_all(workspace, error);
  return 0;
}
