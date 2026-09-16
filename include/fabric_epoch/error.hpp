// Fabric Epoch 1.0.0 - Summon Software Labs
#pragma once

#include <stdexcept>
#include <string>

namespace fabric_epoch {

/// Categories of infrastructure failure. These are *not* authority decisions:
/// authority decisions are always reported as structured outcome values.
enum class ErrorCategory {
  MalformedEncoding,
  UnsupportedVersion,
  IntegrityFailure,
  CorruptState,
  IoFailure,
  ResourceLimit,
  Overflow,
  InvalidArgument,
  ProtocolViolation,
  InternalInvariant,
};

[[nodiscard]] std::string_view to_string(ErrorCategory category) noexcept;

/// Thrown for unrecoverable infrastructure failures: unusable persistence,
/// failed durability barriers, exhausted resources, protocol violations that
/// cannot be answered, and internal invariant breaks. Authority outcomes
/// (stale epoch, fenced boot, missing scope, ...) are never exceptions.
class EpochError : public std::runtime_error {
 public:
  EpochError(ErrorCategory category, std::string message);

  [[nodiscard]] ErrorCategory category() const noexcept { return category_; }
  [[nodiscard]] std::string render() const;

 private:
  ErrorCategory category_;
};

[[nodiscard]] EpochError malformed(std::string message);
[[nodiscard]] EpochError corrupt(std::string message);
[[nodiscard]] EpochError io_failure(std::string message);
[[nodiscard]] EpochError resource_limit(std::string message);
[[nodiscard]] EpochError overflow(std::string message);
[[nodiscard]] EpochError invalid_argument(std::string message);
[[nodiscard]] EpochError protocol_violation(std::string message);
[[nodiscard]] EpochError invariant(std::string message);

}  // namespace fabric_epoch
