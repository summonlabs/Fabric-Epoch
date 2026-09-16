// Fabric Epoch 1.0.0 - Summon Software Labs
#include "arguments.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>

#include "fabric_epoch/error.hpp"

namespace fabric_epoch::app {

bool Arguments::has(std::string_view key) const {
  return std::any_of(options.begin(), options.end(),
                     [key](const auto& entry) { return entry.first == key; });
}

std::optional<std::string> Arguments::get(std::string_view key) const {
  for (const auto& entry : options) {
    if (entry.first == key) {
      return entry.second;
    }
  }
  return std::nullopt;
}

std::string Arguments::require(std::string_view key) const {
  const auto value = get(key);
  if (!value.has_value()) {
    throw invalid_argument("missing required option --" + std::string(key));
  }
  return *value;
}

std::vector<std::string> Arguments::get_all(std::string_view key) const {
  std::vector<std::string> out;
  for (const auto& entry : options) {
    if (entry.first == key) {
      out.push_back(entry.second);
    }
  }
  return out;
}

std::string Arguments::get_or(std::string_view key, std::string fallback) const {
  const auto value = get(key);
  return value.has_value() ? *value : std::move(fallback);
}

std::optional<std::uint64_t> Arguments::get_unsigned(std::string_view key) const {
  const auto value = get(key);
  if (!value.has_value()) {
    return std::nullopt;
  }
  if (value->empty()) {
    throw invalid_argument("option --" + std::string(key) + " requires a numeric value");
  }
  std::uint64_t out = 0;
  for (const char c : *value) {
    if (c < '0' || c > '9') {
      throw invalid_argument("option --" + std::string(key) + " requires a numeric value");
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (out > (UINT64_MAX - digit) / 10u) {
      throw invalid_argument("option --" + std::string(key) + " overflows");
    }
    out = out * 10u + digit;
  }
  return out;
}

Arguments parse_arguments(int argc, char** argv) {
  Arguments parsed;
  for (int index = 1; index < argc; ++index) {
    const std::string token = argv[index];
    if (token.size() >= 2 && token[0] == '-' && token[1] == '-') {
      const std::string key = token.substr(2);
      if (key.empty()) {
        throw invalid_argument("empty option name");
      }
      if (index + 1 < argc) {
        const std::string next = argv[index + 1];
        if (!(next.size() >= 2 && next[0] == '-' && next[1] == '-')) {
          parsed.options.emplace_back(key, next);
          ++index;
          continue;
        }
      }
      parsed.options.emplace_back(key, std::string{});
    } else {
      parsed.positional.push_back(token);
    }
  }
  return parsed;
}

std::optional<ScopeKind> parse_scope_kind(std::string_view text) {
  static const std::array<std::pair<std::string_view, ScopeKind>, 6> kinds = {{
      {"FABRIC", ScopeKind::Fabric},
      {"SITE", ScopeKind::Site},
      {"DOMAIN", ScopeKind::Domain},
      {"ENTITY_CLASS", ScopeKind::EntityClass},
      {"ENTITY", ScopeKind::Entity},
      {"OPERATION_FAMILY", ScopeKind::OperationFamily},
  }};
  for (const auto& entry : kinds) {
    if (entry.first == text) {
      return entry.second;
    }
  }
  return std::nullopt;
}

std::optional<AuthorityScope> parse_scope(std::string_view text) {
  const std::size_t separator = text.find(':');
  if (separator == std::string_view::npos) {
    return std::nullopt;
  }
  const auto kind = parse_scope_kind(text.substr(0, separator));
  if (!kind.has_value()) {
    return std::nullopt;
  }
  return AuthorityScope::parse(*kind, text.substr(separator + 1));
}

std::optional<FenceReason> parse_fence_reason(std::string_view text) {
  static const std::array<std::pair<std::string_view, FenceReason>, 8> reasons = {{
      {"PROCESS_LOSS", FenceReason::ProcessLoss},
      {"REINCARNATION", FenceReason::Reincarnation},
      {"COORDINATOR_EPOCH_ADVANCE", FenceReason::CoordinatorEpochAdvance},
      {"ADMINISTRATIVE_REVOKE", FenceReason::AdministrativeRevoke},
      {"SESSION_PROTOCOL_VIOLATION", FenceReason::SessionProtocolViolation},
      {"EXPLICIT_OPERATOR_FENCE", FenceReason::ExplicitOperatorFence},
      {"AUTHORITY_REPLACED", FenceReason::AuthorityReplaced},
      {"SHUTDOWN", FenceReason::Shutdown},
  }};
  for (const auto& entry : reasons) {
    if (entry.first == text) {
      return entry.second;
    }
  }
  return std::nullopt;
}

std::optional<IncarnationPolicy> parse_incarnation_policy(std::string_view text) {
  if (text == "SINGLE_INCARNATION") {
    return IncarnationPolicy::SingleIncarnation;
  }
  if (text == "MULTI_INCARNATION") {
    return IncarnationPolicy::MultiIncarnation;
  }
  return std::nullopt;
}

std::optional<WorkerBootId> parse_boot(std::string_view text) {
  if (text == "generate") {
    return WorkerBootId::generate();
  }
  return WorkerBootId::parse(text);
}

std::string format_scopes(const ScopeSet& scopes) { return scopes.to_string(); }

}  // namespace fabric_epoch::app
