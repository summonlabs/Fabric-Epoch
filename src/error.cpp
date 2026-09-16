// Fabric Epoch 1.0.0 - Summon Software Labs
#include "fabric_epoch/error.hpp"

#include <string_view>

namespace fabric_epoch {

std::string_view to_string(ErrorCategory category) noexcept {
  switch (category) {
    case ErrorCategory::MalformedEncoding:
      return "MALFORMED_ENCODING";
    case ErrorCategory::UnsupportedVersion:
      return "UNSUPPORTED_VERSION";
    case ErrorCategory::IntegrityFailure:
      return "INTEGRITY_FAILURE";
    case ErrorCategory::CorruptState:
      return "CORRUPT_STATE";
    case ErrorCategory::IoFailure:
      return "IO_FAILURE";
    case ErrorCategory::ResourceLimit:
      return "RESOURCE_LIMIT";
    case ErrorCategory::Overflow:
      return "OVERFLOW";
    case ErrorCategory::InvalidArgument:
      return "INVALID_ARGUMENT";
    case ErrorCategory::ProtocolViolation:
      return "PROTOCOL_VIOLATION";
    case ErrorCategory::InternalInvariant:
      return "INTERNAL_INVARIANT";
  }
  return "UNKNOWN";
}

EpochError::EpochError(ErrorCategory category, std::string message)
    : std::runtime_error(std::move(message)), category_(category) {}

std::string EpochError::render() const {
  std::string out;
  out.append(to_string(category_));
  out.append(": ");
  out.append(what());
  return out;
}

EpochError malformed(std::string message) {
  return EpochError(ErrorCategory::MalformedEncoding, std::move(message));
}
EpochError corrupt(std::string message) {
  return EpochError(ErrorCategory::CorruptState, std::move(message));
}
EpochError io_failure(std::string message) {
  return EpochError(ErrorCategory::IoFailure, std::move(message));
}
EpochError resource_limit(std::string message) {
  return EpochError(ErrorCategory::ResourceLimit, std::move(message));
}
EpochError overflow(std::string message) {
  return EpochError(ErrorCategory::Overflow, std::move(message));
}
EpochError invalid_argument(std::string message) {
  return EpochError(ErrorCategory::InvalidArgument, std::move(message));
}
EpochError protocol_violation(std::string message) {
  return EpochError(ErrorCategory::ProtocolViolation, std::move(message));
}
EpochError invariant(std::string message) {
  return EpochError(ErrorCategory::InternalInvariant, std::move(message));
}

}  // namespace fabric_epoch
