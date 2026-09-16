// Fabric Epoch 1.0.0 - Summon Software Labs
// Minimal deterministic command-line parsing shared by the Fabric Epoch tools.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fabric_epoch/fence.hpp"
#include "fabric_epoch/participant.hpp"
#include "fabric_epoch/scope.hpp"

namespace fabric_epoch::app {

struct Arguments {
  std::vector<std::string> positional;
  std::vector<std::pair<std::string, std::string>> options;

  [[nodiscard]] bool has(std::string_view key) const;
  [[nodiscard]] std::optional<std::string> get(std::string_view key) const;
  [[nodiscard]] std::string require(std::string_view key) const;
  [[nodiscard]] std::vector<std::string> get_all(std::string_view key) const;
  [[nodiscard]] std::string get_or(std::string_view key, std::string fallback) const;
  [[nodiscard]] std::optional<std::uint64_t> get_unsigned(std::string_view key) const;
};

/// Parses "--key value", "--flag" and positional arguments. A malformed
/// invocation throws EpochError(InvalidArgument).
[[nodiscard]] Arguments parse_arguments(int argc, char** argv);

/// Parses "KIND:SUBJECT" where KIND is one of FABRIC, SITE, DOMAIN,
/// ENTITY_CLASS, ENTITY, OPERATION_FAMILY.
[[nodiscard]] std::optional<AuthorityScope> parse_scope(std::string_view text);
[[nodiscard]] std::optional<ScopeKind> parse_scope_kind(std::string_view text);
[[nodiscard]] std::optional<FenceReason> parse_fence_reason(std::string_view text);
[[nodiscard]] std::optional<IncarnationPolicy> parse_incarnation_policy(std::string_view text);
[[nodiscard]] std::optional<WorkerBootId> parse_boot(std::string_view text);

[[nodiscard]] std::string format_scopes(const ScopeSet& scopes);

}  // namespace fabric_epoch::app
