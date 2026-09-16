// Fabric Epoch 1.0.0 - Summon Software Labs
// Shared fixtures for the Fabric Epoch test suites.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "fabric_epoch/fabric_epoch.hpp"
#include "test_harness.hpp"

namespace fe_test {

inline fabric_epoch::AuthorityScope scope(fabric_epoch::ScopeKind kind, const std::string& subject) {
  const auto parsed = fabric_epoch::AuthorityScope::parse(kind, subject);
  FE_REQUIRE_MSG(parsed.has_value(), "test fixture produced a malformed scope");
  return *parsed;
}

inline fabric_epoch::ScopeSet scopes(std::initializer_list<fabric_epoch::AuthorityScope> values) {
  fabric_epoch::ScopeSet set;
  for (const auto& value : values) {
    FE_REQUIRE(set.insert(value, 64));
  }
  return set;
}

inline fabric_epoch::ParticipantId participant(const std::string& text) {
  const auto parsed = fabric_epoch::ParticipantId::parse(text);
  FE_REQUIRE_MSG(parsed.has_value(), "test fixture produced a malformed participant id");
  return *parsed;
}

inline fabric_epoch::ScopeSet default_scopes() {
  return scopes({scope(fabric_epoch::ScopeKind::Fabric, "fabric-main")});
}

struct RuntimeFixture {
  TempDirectory directory;
  std::filesystem::path store_path;

  explicit RuntimeFixture(const std::string& name = "epoch.store")
      : store_path(directory.file(name)) {}
};

inline fabric_epoch::EpochRuntimeOptions runtime_options(const RuntimeFixture& fixture) {
  fabric_epoch::EpochRuntimeOptions options;
  options.store_path = fixture.store_path;
  options.provenance = fabric_epoch::BoundedText::from_validated("test");
  return options;
}

inline fabric_epoch::RegisterParticipantRequest registration_request(
    const std::string& participant_text, const fabric_epoch::WorkerBootId& boot,
    fabric_epoch::CoordinatorEpoch epoch,
    std::optional<fabric_epoch::ScopeSet> scope_set = std::nullopt) {
  fabric_epoch::RegisterParticipantRequest request;
  request.attempt = fabric_epoch::MutationAttemptId::generate();
  request.expected_epoch = epoch;
  request.participant = participant(participant_text);
  request.boot = boot;
  request.scopes = scope_set.has_value() ? *scope_set : default_scopes();
  request.provenance = fabric_epoch::BoundedText::from_validated("test-registration");
  return request;
}

inline fabric_epoch::AuthorityToken token_for(const fabric_epoch::RegistrationResult& result,
                                              const fabric_epoch::ParticipantId& id,
                                              const fabric_epoch::WorkerBootId& boot) {
  fabric_epoch::AuthorityToken token;
  token.epoch = result.epoch;
  token.participant = id;
  token.boot = boot;
  token.incarnation = result.incarnation.value_or(fabric_epoch::IncarnationSequence(0));
  token.registration_generation =
      result.registration_generation.value_or(fabric_epoch::RegistrationGeneration(0));
  return token;
}

/// Asserts that constructing a runtime over the supplied options fails. A
/// runtime is neither copyable nor movable, so this cannot be expressed as a
/// single throwing expression.
inline void expect_runtime_construction_fails(const fabric_epoch::EpochRuntimeOptions& options) {
  bool threw = false;
  try {
    fabric_epoch::EpochRuntime probe(options);
  } catch (const fabric_epoch::EpochError&) {
    threw = true;
  }
  FE_REQUIRE_MSG(threw, "runtime construction over an unusable store did not fail");
}

/// Reads an unsigned decimal value out of a "key=value" token list.
inline std::optional<std::uint64_t> field_value(const std::string& line, const std::string& key) {
  const std::string needle = key + "=";
  std::size_t position = line.find(needle);
  while (position != std::string::npos) {
    const bool at_boundary = position == 0 || line[position - 1] == ' ' || line[position - 1] == '(' ||
                             line[position - 1] == ',';
    if (at_boundary) {
      const std::size_t begin = position + needle.size();
      std::size_t end = begin;
      while (end < line.size() && line[end] >= '0' && line[end] <= '9') {
        ++end;
      }
      if (end > begin) {
        return std::stoull(line.substr(begin, end - begin));
      }
      return std::nullopt;
    }
    position = line.find(needle, position + 1);
  }
  return std::nullopt;
}

inline std::optional<std::string> field_text(const std::string& line, const std::string& key) {
  const std::string needle = key + "=";
  std::size_t position = line.find(needle);
  while (position != std::string::npos) {
    const bool at_boundary = position == 0 || line[position - 1] == ' ' || line[position - 1] == '(' ||
                             line[position - 1] == ',';
    if (at_boundary) {
      const std::size_t begin = position + needle.size();
      std::size_t end = begin;
      while (end < line.size() && line[end] != ' ' && line[end] != ')' && line[end] != ',') {
        ++end;
      }
      return line.substr(begin, end - begin);
    }
    position = line.find(needle, position + 1);
  }
  return std::nullopt;
}

}  // namespace fe_test
