// Fabric Epoch 1.0.0 - Summon Software Labs
// Shared scaffolding for the shipped examples. Examples use the public library
// API only.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "fabric_epoch/fabric_epoch.hpp"

namespace example {

inline void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "example failed: " << message << std::endl;
    std::exit(1);
  }
}

/// Isolated working directory for one example run. Uses operating-system
/// entropy so concurrent example runs never share state.
class Workspace {
 public:
  Workspace() {
    std::array<std::byte, 16> entropy{};
    fabric_epoch::detail::fill_random_16(entropy.data());
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string suffix;
    suffix.reserve(32);
    for (const std::byte value : entropy) {
      const auto raw = static_cast<unsigned>(value);
      suffix.push_back(kDigits[(raw >> 4) & 0xFu]);
      suffix.push_back(kDigits[raw & 0xFu]);
    }
    std::error_code error;
    const std::filesystem::path base = std::filesystem::temp_directory_path(error);
    require(!error, "cannot resolve the temporary directory");
    path_ = base / ("fabric_epoch_example_" + suffix);
    std::filesystem::create_directories(path_, error);
    require(!error, "cannot create the example workspace");
  }

  ~Workspace() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  Workspace(const Workspace&) = delete;
  Workspace& operator=(const Workspace&) = delete;

  [[nodiscard]] std::filesystem::path store(const std::string& name = "epoch.store") const {
    return path_ / name;
  }

 private:
  std::filesystem::path path_;
};

inline fabric_epoch::AuthorityScope scope(fabric_epoch::ScopeKind kind, const std::string& subject) {
  const auto parsed = fabric_epoch::AuthorityScope::parse(kind, subject);
  require(parsed.has_value(), "malformed example scope");
  return *parsed;
}

inline fabric_epoch::ScopeSet scopes(std::initializer_list<fabric_epoch::AuthorityScope> values) {
  fabric_epoch::ScopeSet set;
  for (const auto& value : values) {
    require(set.insert(value, 32), "example scope set overflow");
  }
  return set;
}

inline fabric_epoch::ParticipantId participant(const std::string& name) {
  const auto parsed = fabric_epoch::ParticipantId::parse(name);
  require(parsed.has_value(), "malformed example participant id");
  return *parsed;
}

inline fabric_epoch::EpochRuntimeOptions options(const std::filesystem::path& store_path) {
  fabric_epoch::EpochRuntimeOptions runtime_options;
  runtime_options.store_path = store_path;
  runtime_options.provenance = fabric_epoch::BoundedText::from_validated("example");
  return runtime_options;
}

inline fabric_epoch::RegisterParticipantRequest registration(const std::string& name,
                                                             const fabric_epoch::WorkerBootId& boot,
                                                             fabric_epoch::CoordinatorEpoch epoch) {
  fabric_epoch::RegisterParticipantRequest request;
  request.attempt = fabric_epoch::MutationAttemptId::generate();
  request.expected_epoch = epoch;
  request.participant = participant(name);
  request.boot = boot;
  request.scopes = scopes({scope(fabric_epoch::ScopeKind::Fabric, "fabric-main")});
  request.provenance = fabric_epoch::BoundedText::from_validated("example-registration");
  return request;
}

}  // namespace example
